/*
 * xfocus - 读取/设置 X11 输入焦点（测试与调试用）
 *
 * 用法:
 *   xfocus                 打印当前焦点窗口
 *   xfocus <窗口ID>        把输入焦点设到该窗口
 *   xfocus save           保存当前焦点到 $XDG_RUNTIME_DIR/xfocus.saved
 *   xfocus restore        从保存文件还原焦点
 *
 * 同样用 dlopen 解析 libX11 符号，无需开发包。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>

typedef struct _XDisplay Display;
typedef unsigned long Window;

static Display *(*pXOpenDisplay)(const char *);
static int (*pXCloseDisplay)(Display *);
static int (*pXSetInputFocus)(Display *, Window, int, unsigned long);
static Window (*pXGetInputFocus)(Display *, Window *, int *);
static int (*pXSync)(Display *, int);

static void *sym(void *h, const char *n) {
	void *p = dlsym(h, n);
	if (!p) { fprintf(stderr, "xfocus: 缺少符号 %s\n", n); exit(1); }
	return p;
}

static const char *save_path(void) {
	static char buf[512];
	const char *rt = getenv("XDG_RUNTIME_DIR");
	/* 优先 XDG_RUNTIME_DIR，但沙箱/权限可能禁止写入，则回退到 /tmp */
	if (rt && *rt && access(rt, W_OK) == 0) {
		snprintf(buf, sizeof buf, "%s/xfocus.saved", rt);
		FILE *t = fopen(buf, "a");
		if (t) { fclose(t); return buf; }
	}
	snprintf(buf, sizeof buf, "/tmp/xfocus.saved");
	return buf;
}

int main(int argc, char **argv) {
	void *h = dlopen("libX11.so.6", RTLD_NOW);
	if (!h) { fprintf(stderr, "xfocus: 无法加载 libX11.so.6\n"); return 1; }
	pXOpenDisplay  = (Display *(*)(const char *))sym(h, "XOpenDisplay");
	pXCloseDisplay = (int (*)(Display *))sym(h, "XCloseDisplay");
	pXSetInputFocus= (int (*)(Display *, Window, int, unsigned long))sym(h, "XSetInputFocus");
	pXGetInputFocus= (Window (*)(Display *, Window *, int *))sym(h, "XGetInputFocus");
	pXSync         = (int (*)(Display *, int))sym(h, "XSync");

	Display *d = pXOpenDisplay(NULL);
	if (!d) { fprintf(stderr, "xfocus: 无法连接 X 显示\n"); return 1; }

	Window cur; int rev;
	pXGetInputFocus(d, &cur, &rev);

	if (argc < 2) {
		printf("0x%lx\n", (unsigned long)cur);
		pXCloseDisplay(d);
		return 0;
	}

	if (strcmp(argv[1], "save") == 0) {
		FILE *f = fopen(save_path(), "w");
		if (!f) { fprintf(stderr, "xfocus: 无法写入 %s\n", save_path()); return 1; }
		fprintf(f, "0x%lx\n", (unsigned long)cur);
		fclose(f);
		printf("已保存焦点 0x%lx\n", (unsigned long)cur);
	} else if (strcmp(argv[1], "restore") == 0) {
		FILE *f = fopen(save_path(), "r");
		if (!f) { fprintf(stderr, "xfocus: 没有保存的焦点\n"); return 1; }
		unsigned long w = 0;
		if (fscanf(f, "0x%lx", &w) != 1) { fclose(f); fprintf(stderr, "xfocus: 保存文件损坏\n"); return 1; }
		fclose(f);
		pXSetInputFocus(d, (Window)w, 2 /* RevertToParent */, 0 /* CurrentTime */);
		pXSync(d, 0);
		printf("已还原焦点 0x%lx\n", w);
	} else {
		unsigned long w = strtoul(argv[1], NULL, 0);
		pXSetInputFocus(d, (Window)w, 2, 0);
		pXSync(d, 0);
		printf("已设置焦点 0x%lx\n", w);
	}

	pXCloseDisplay(d);
	dlclose(h);
	return 0;
}
