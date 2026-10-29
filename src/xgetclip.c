/*
 * xgetclip - 读取当前 CLIPBOARD 选中内容并打印（用于验证 xpaste / 排错）
 * 用法: xgetclip [--primary]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <time.h>

typedef struct _XDisplay Display;
typedef unsigned long Window, Atom, Time_t;

static void *sym(void *h, const char *n) {
	void *p = dlsym(h, n);
	if (!p) { fprintf(stderr, "xgetclip: 缺少符号 %s\n", n); return NULL; }
	return p;
}

typedef union {
	long pad[24];
	struct {
		int type; int _p0; unsigned long serial; int send_event; int _p1;
		void *display;
		Window owner, requestor;
		Atom selection, target, property;
		Time_t time;
	} req;
	struct {
		int type; int _p0; unsigned long serial; int send_event; int _p1;
		void *display;
		Window requestor;
		Atom selection, target, property;
		Time_t time;
	} notify;
	struct {
		int type; int _p0; unsigned long serial; int send_event; int _p1;
		void *display;
		Window window;
		Atom atom;
		int state;
	} prop;
} XAnyEvent;

static double now_ms(void) {
	struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
	int use_primary = (argc > 1 && !strcmp(argv[1], "--primary"));

	void *h = dlopen("libX11.so.6", RTLD_NOW);
	if (!h) { fprintf(stderr, "xgetclip: 无法加载 libX11\n"); return 1; }

	Display *(*open_)(const char *) = (Display *(*)(const char *))sym(h, "XOpenDisplay");
	int (*close_)(Display *) = (int (*)(Display *))sym(h, "XCloseDisplay");
	Window (*root_)(Display *) = (Window (*)(Display *))sym(h, "XDefaultRootWindow");
	Window (*mkw_)(Display *, Window, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long)
		= (Window (*)(Display *, Window, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long))sym(h, "XCreateSimpleWindow");
	Atom (*atom_)(Display *, const char *, int) = (Atom (*)(Display *, const char *, int))sym(h, "XInternAtom");
	Window (*owner_)(Display *, Atom) = (Window (*)(Display *, Atom))sym(h, "XGetSelectionOwner");
	int (*conv_)(Display *, Atom, Atom, Atom, Window, Time_t) = (int (*)(Display *, Atom, Atom, Atom, Window, Time_t))sym(h, "XConvertSelection");
	int (*sel_)(Display *, Window, long) = (int (*)(Display *, Window, long))sym(h, "XSelectInput");
	int (*next_)(Display *, void *) = (int (*)(Display *, void *))sym(h, "XNextEvent");
	int (*pend_)(Display *) = (int (*)(Display *))sym(h, "XPending");
	int (*flush_)(Display *) = (int (*)(Display *))sym(h, "XFlush");
	int (*sync_)(Display *, int) = (int (*)(Display *, int))sym(h, "XSync");
	int (*getp_)(Display *, Window, Atom, long, long, int, Atom, Atom *, int *, unsigned long *, unsigned long *, unsigned char **)
		= (int (*)(Display *, Window, Atom, long, long, int, Atom, Atom *, int *, unsigned long *, unsigned long *, unsigned char **))sym(h, "XGetWindowProperty");

	Display *d = open_(NULL);
	if (!d) { fprintf(stderr, "xgetclip: 无法连接 X 显示\n"); return 1; }

	Atom CLIP = atom_(d, use_primary ? "PRIMARY" : "CLIPBOARD", 0);
	Atom UTF8 = atom_(d, "UTF8_STRING", 0);
	Atom PROPT = atom_(d, "XSEL_DATA", 0);

	if (!owner_(d, CLIP)) { printf("(剪贴板为空)\n"); return 0; }

	Window root = root_(d);
	Window w = mkw_(d, root, -20, -20, 1, 1, 0, 0, 0);
	if (!w) { fprintf(stderr, "xgetclip: 建窗口失败\n"); return 1; }
	sel_(d, w, 1L << 22 /* PropertyChangeMask */);
	conv_(d, CLIP, UTF8, PROPT, w, 0 /* CurrentTime */);
	flush_(d);

	double deadline = now_ms() + 2000;
	while (now_ms() < deadline) {
		while (pend_(d)) {
			XAnyEvent e; memset(&e, 0, sizeof e);
			next_(d, &e);
			if (e.notify.type != 31) continue;
			if (e.notify.property == 0) { printf("(取回失败：对方拒绝了请求)\n"); return 0; }

			Atom type; int fmt; unsigned long nitems, after; unsigned char *data = NULL;
			getp_(d, w, PROPT, 0, 1024 * 1024, 0, 0 /* AnyPropertyType */, &type, &fmt,
			      &nitems, &after, &data);
			if (data) {
				fwrite(data, 1, nitems, stdout);
				printf("\n");
			}
			sync_(d, 0);
			return 0;
		}
		usleep(20000);
	}
	printf("(超时：没有收到数据)\n");
	return 1;
}
