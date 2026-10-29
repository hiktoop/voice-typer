/*
 * xpaste - 通过「拥有 CLIPBOARD 选区 + 模拟 Ctrl+V」把文本粘贴到焦点窗口
 *
 * 为什么不用逐字符 XTEST 重映射：
 *   那套做法要把每个字符的 keysym 临时绑到一个 keycode 上。真实应用（尤其开了
 *   输入法、或有自己的键盘映射缓存的程序）在事件处理时才去查映射表，而此时我们
 *   可能已经换成下一个字符的映射了 —— 结果就是通知里文字正确、输入框里却是乱的。
 *   剪贴板粘贴没有这个问题：文本一次性传输，与键盘映射、输入法状态完全无关。
 *
 * 同样用 dlopen 解析 libX11 / libXtst，无需开发包。
 *
 * 用法：
 *   xpaste "要粘贴的文字"
 *   echo 文字 | xpaste --stdin
 *   xpaste --ctrl-shift "文字"     # 终端等需要 Ctrl+Shift+V 的场景
 *   xpaste --hold 3000 "文字"      # 选区保持 3 秒（默认 2000ms）
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <time.h>

typedef struct _XDisplay Display;
typedef unsigned long Window;
typedef unsigned long Atom;
typedef unsigned long Time_t;
typedef unsigned long KeySym;
typedef unsigned char KeyCode;

/* ---- libX11 ---- */
static Display *(*pXOpenDisplay)(const char *);
static int (*pXCloseDisplay)(Display *);
static Window (*pXDefaultRootWindow)(Display *);
static Window (*pXCreateSimpleWindow)(Display *, Window, int, int, unsigned, unsigned,
                                      unsigned, unsigned long, unsigned long);
static Atom (*pXInternAtom)(Display *, const char *, int);
static int (*pXSetSelectionOwner)(Display *, Atom, Window, Time_t);
static Window (*pXGetSelectionOwner)(Display *, Atom);
static int (*pXChangeProperty)(Display *, Window, Atom, Atom, int, int,
                               const unsigned char *, int);
static int (*pXSendEvent)(Display *, Window, int, long, void *);
static int (*pXSelectInput)(Display *, Window, long);
static int (*pXNextEvent)(Display *, void *);
static int (*pXPending)(Display *);
static int (*pXFlush)(Display *);
static int (*pXSync)(Display *, int);
static KeyCode (*pXKeysymToKeycode)(Display *, KeySym);
static int (*pXDestroyWindow)(Display *, Window);

/* ---- libXtst ---- */
static int (*pXTestFakeKeyEvent)(Display *, unsigned int, int, unsigned long);
static int (*pXTestQueryExtension)(Display *, int *, int *, int *, int *);

static void die(const char *m) { fprintf(stderr, "xpaste: %s\n", m); exit(1); }
static void *sym(void *h, const char *n) {
	void *p = dlsym(h, n);
	if (!p) { fprintf(stderr, "xpaste: 缺少符号 %s\n", n); exit(1); }
	return p;
}

/* Xlib 事件结构（x86-64 ABI，手写以避开开发头文件） */
#define SEL_REQUEST 30
#define SEL_NOTIFY  31
#define PROP_NEW    0
#define ATOM_ATOM   4

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
} XAnyEvent;

static double now_ms(void) {
	struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
	long hold_ms = 2000;
	int ctrl_shift = 0, use_stdin = 0;
	const char *text = NULL;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--stdin")) use_stdin = 1;
		else if (!strcmp(argv[i], "--ctrl-shift")) ctrl_shift = 1;
		else if (!strcmp(argv[i], "--hold") && i + 1 < argc) hold_ms = atol(argv[++i]);
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			fprintf(stderr, "用法: xpaste [--ctrl-shift] [--hold MS] \"文字\"\n"
			                "      echo 文字 | xpaste --stdin\n");
			return 0;
		}
		else text = argv[i];
	}

	char *buf = NULL;
	if (use_stdin || !text) {
		size_t cap = 8192, len = 0;
		buf = malloc(cap);
		if (!buf) die("内存不足");
		size_t n;
		while ((n = fread(buf + len, 1, cap - len - 1, stdin)) > 0) {
			len += n;
			if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); if (!buf) die("内存不足"); }
		}
		buf[len] = '\0';
		while (len && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
		text = buf;
	}
	if (!text || !*text) return 0;

	void *hx = dlopen("libX11.so.6", RTLD_NOW);
	void *ht = dlopen("libXtst.so.6", RTLD_NOW);
	if (!hx || !ht) die("无法加载 libX11/libXtst");

	pXOpenDisplay         = (Display *(*)(const char *))sym(hx, "XOpenDisplay");
	pXCloseDisplay        = (int (*)(Display *))sym(hx, "XCloseDisplay");
	pXDefaultRootWindow   = (Window (*)(Display *))sym(hx, "XDefaultRootWindow");
	pXCreateSimpleWindow  = (Window (*)(Display *, Window, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long))sym(hx, "XCreateSimpleWindow");
	pXInternAtom          = (Atom (*)(Display *, const char *, int))sym(hx, "XInternAtom");
	pXSetSelectionOwner   = (int (*)(Display *, Atom, Window, Time_t))sym(hx, "XSetSelectionOwner");
	pXGetSelectionOwner   = (Window (*)(Display *, Atom))sym(hx, "XGetSelectionOwner");
	pXChangeProperty      = (int (*)(Display *, Window, Atom, Atom, int, int, const unsigned char *, int))sym(hx, "XChangeProperty");
	pXSendEvent           = (int (*)(Display *, Window, int, long, void *))sym(hx, "XSendEvent");
	pXSelectInput         = (int (*)(Display *, Window, long))sym(hx, "XSelectInput");
	pXNextEvent           = (int (*)(Display *, void *))sym(hx, "XNextEvent");
	pXPending             = (int (*)(Display *))sym(hx, "XPending");
	pXFlush               = (int (*)(Display *))sym(hx, "XFlush");
	pXSync                = (int (*)(Display *, int))sym(hx, "XSync");
	pXKeysymToKeycode     = (KeyCode (*)(Display *, KeySym))sym(hx, "XKeysymToKeycode");
	pXDestroyWindow       = (int (*)(Display *, Window))sym(hx, "XDestroyWindow");
	pXTestFakeKeyEvent    = (int (*)(Display *, unsigned int, int, unsigned long))sym(ht, "XTestFakeKeyEvent");
	pXTestQueryExtension  = (int (*)(Display *, int *, int *, int *, int *))sym(ht, "XTestQueryExtension");

	Display *d = pXOpenDisplay(NULL);
	if (!d) die("无法连接 X 显示");

	int ev, er, maj, mi;
	if (!pXTestQueryExtension(d, &ev, &er, &maj, &mi)) die("X 服务器不支持 XTEST");

	Atom CLIPBOARD = pXInternAtom(d, "CLIPBOARD", 0);
	Atom UTF8      = pXInternAtom(d, "UTF8_STRING", 0);
	Atom TARGETS   = pXInternAtom(d, "TARGETS", 0);
	Atom TEXT      = pXInternAtom(d, "TEXT", 0);
	Atom STRING    = pXInternAtom(d, "STRING", 0);

	Window root = pXDefaultRootWindow(d);
	Window win = pXCreateSimpleWindow(d, root, -20, -20, 1, 1, 0, 0, 0);
	pXSelectInput(d, win, 0);
	pXSetSelectionOwner(d, CLIPBOARD, win, 0 /* CurrentTime */);
	pXSync(d, 0);

	if (pXGetSelectionOwner(d, CLIPBOARD) != win) die("无法取得剪贴板所有权");

	size_t len = strlen(text);

	/* 模拟 Ctrl(+Shift)+V */
	KeyCode kc_ctrl = pXKeysymToKeycode(d, 0xFFE3);  /* XK_Control_L */
	KeyCode kc_shift = pXKeysymToKeycode(d, 0xFFE1); /* XK_Shift_L */
	KeyCode kc_v = pXKeysymToKeycode(d, 0x0076);     /* 'v' */
	if (!kc_ctrl || !kc_v) die("找不到需要的 keycode");

	pXTestFakeKeyEvent(d, kc_ctrl, 1, 0);
	if (ctrl_shift && kc_shift) pXTestFakeKeyEvent(d, kc_shift, 1, 0);
	pXTestFakeKeyEvent(d, kc_v, 1, 0);
	pXTestFakeKeyEvent(d, kc_v, 0, 0);
	if (ctrl_shift && kc_shift) pXTestFakeKeyEvent(d, kc_shift, 0, 0);
	pXTestFakeKeyEvent(d, kc_ctrl, 0, 0);
	pXSync(d, 0);

	/* 保持选区所有权一段时间，等服务对方的 SelectionRequest */
	double deadline = now_ms() + hold_ms;
	while (now_ms() < deadline) {
		while (pXPending(d)) {
			XAnyEvent e;
			memset(&e, 0, sizeof e);
			pXNextEvent(d, &e);
			if (e.req.type != SEL_REQUEST) continue;

			Window requestor = e.req.requestor;
			Atom selection = e.req.selection;
			Atom target = e.req.target;
			Atom prop = e.req.property ? e.req.property : e.req.target;
			Time_t when = e.req.time;

			Atom ok = 0;
			if (target == TARGETS) {
				Atom avail[4] = { UTF8, TEXT, STRING, TARGETS };
				pXChangeProperty(d, requestor, prop, ATOM_ATOM, 32, PROP_NEW,
				                 (const unsigned char *)avail, 4);
				ok = prop;
			} else if (target == UTF8 || target == TEXT || target == STRING) {
				pXChangeProperty(d, requestor, prop, target, 8, PROP_NEW,
				                 (const unsigned char *)text, (int)len);
				ok = prop;
			} else {
				ok = 0;   /* 不支持的类型：property = None 表示拒绝 */
			}

			/* 必须独立构造一个干净的 SelectionNotify。
			 * 复用上面那个 union 会踩到 req 与 notify 的字段偏移差异，
			 * 导致 requestor/time 等字段错位，应用校验不过就会丢弃。 */
			XAnyEvent out;
			memset(&out, 0, sizeof out);
			out.notify.type = SEL_NOTIFY;
			out.notify.serial = e.req.serial;
			out.notify.send_event = 1;
			out.notify.display = d;
			out.notify.requestor = requestor;
			out.notify.selection = selection;
			out.notify.target = target;
			out.notify.property = ok;
			out.notify.time = when;
			pXSendEvent(d, requestor, 0, 0, &out);
			pXFlush(d);
		}
		usleep(5000);
	}

	pXDestroyWindow(d, win);
	pXCloseDisplay(d);
	free(buf);
	return 0;
}
