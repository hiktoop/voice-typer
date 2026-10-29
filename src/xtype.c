/*
 * xtype - 通过 X11 XTEST 扩展向当前焦点窗口注入 Unicode 文本
 *
 * 不依赖 libX11-dev / libXtst-dev：所有符号用 dlopen 在运行时解析，
 * 因此只要系统装有 libX11.so.6 与 libXtst.so.6 即可编译运行。
 *
 * Unicode 注入原理（与 xdotool 相同）：
 *   为每个字符计算对应 keysym，把该 keysym 临时绑定到一个空闲 keycode 上，
 *   用 XTEST 伪造按下/释放，然后恢复原映射。
 *
 * 用法：
 *   xtype "要输入的文字"
 *   echo "文字" | xtype --stdin
 *   xtype --delay 8 "文字"     # 每字符后延迟 8ms
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dlfcn.h>
#include <time.h>

typedef struct _XDisplay Display;
typedef unsigned long KeySym;
typedef unsigned char KeyCode;

/* libX11 */
static Display *(*pXOpenDisplay)(const char *);
static int (*pXCloseDisplay)(Display *);
static KeySym *(*pXGetKeyboardMapping)(Display *, KeyCode, int, int *);
static int (*pXChangeKeyboardMapping)(Display *, int, int, KeySym *, int);
static int (*pXSync)(Display *, int);
static int (*pXFlush)(Display *);
static int (*pXDisplayKeycodes)(Display *, int *, int *);
static int (*pXFree)(void *);

/* libXtst */
static int (*pXTestFakeKeyEvent)(Display *, unsigned int, int, unsigned long);
static int (*pXTestQueryExtension)(Display *, int *, int *, int *, int *);

static void die(const char *msg) {
	fprintf(stderr, "xtype: %s\n", msg);
	exit(1);
}

static void *sym(void *h, const char *name) {
	void *p = dlsym(h, name);
	if (!p) {
		fprintf(stderr, "xtype: 无法解析符号 %s\n", name);
		exit(1);
	}
	return p;
}

/* ---------- UTF-8 解码 ---------- */
/* 返回解码出的码点并推进 *s；非法字节返回 0xFFFD 并推进 1 字节 */
static uint32_t utf8_next(const unsigned char **s) {
	const unsigned char *p = *s;
	unsigned char c = p[0];
	if (c < 0x80) { *s = p + 1; return c; }
	if ((c & 0xE0) == 0xC0) {
		if ((p[1] & 0xC0) != 0x80) { *s = p + 1; return 0xFFFD; }
		*s = p + 2;
		return ((uint32_t)(c & 0x1F) << 6) | (p[1] & 0x3F);
	}
	if ((c & 0xF0) == 0xE0) {
		if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) { *s = p + 1; return 0xFFFD; }
		*s = p + 3;
		return ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
	}
	if ((c & 0xF8) == 0xF0) {
		if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) {
			*s = p + 1; return 0xFFFD;
		}
		*s = p + 4;
		return ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12)
		     | ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
	}
	*s = p + 1;
	return 0xFFFD;
}

/* Unicode 码点 -> keysym */
static KeySym cp_to_keysym(uint32_t cp) {
	if (cp <= 0xFF) return (KeySym)cp;          /* Latin-1 与 ASCII 直接对应 */
	return (KeySym)(0x01000000UL | cp);          /* XK_Unicode 区 */
}

/* 找一个空闲 keycode（该 keycode 上所有 keysym 都为 NoSymbol） */
static int find_spare_keycode(Display *d, int min_kc, int max_kc, int *per_kc) {
	for (int kc = max_kc; kc >= min_kc; kc--) {
		int n = 0;
		KeySym *map = pXGetKeyboardMapping(d, (KeyCode)kc, 1, &n);
		if (!map) continue;
		*per_kc = n;
		int all_empty = 1;
		for (int i = 0; i < n; i++) {
			if (map[i] != 0) { all_empty = 0; break; }
		}
		pXFree(map);
		if (all_empty) return kc;
	}
	return -1;
}

int main(int argc, char **argv) {
	long delay_ms = 4;
	const char *text = NULL;
	int use_stdin = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--delay") == 0 && i + 1 < argc) {
			delay_ms = atol(argv[++i]);
		} else if (strcmp(argv[i], "--stdin") == 0) {
			use_stdin = 1;
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			fprintf(stderr,
				"用法: xtype [--delay MS] \"文字\"\n"
				"      echo 文字 | xtype [--delay MS] --stdin\n");
			return 0;
		} else {
			text = argv[i];
		}
	}

	char *buf = NULL;
	if (use_stdin || !text) {
		size_t cap = 4096, len = 0;
		buf = malloc(cap);
		if (!buf) die("内存不足");
		size_t n;
		while ((n = fread(buf + len, 1, cap - len - 1, stdin)) > 0) {
			len += n;
			if (len + 1 >= cap) {
				cap *= 2;
				char *nb = realloc(buf, cap);
				if (!nb) die("内存不足");
				buf = nb;
			}
		}
		buf[len] = '\0';
		while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
		text = buf;
	}
	if (!text || !*text) return 0;

	void *hx11 = dlopen("libX11.so.6", RTLD_NOW);
	if (!hx11) die("无法加载 libX11.so.6");
	void *hxtst = dlopen("libXtst.so.6", RTLD_NOW);
	if (!hxtst) die("无法加载 libXtst.so.6（需要 X11 环境）");

	pXOpenDisplay          = (Display *(*)(const char *))sym(hx11, "XOpenDisplay");
	pXCloseDisplay         = (int (*)(Display *))sym(hx11, "XCloseDisplay");
	pXGetKeyboardMapping   = (KeySym *(*)(Display *, KeyCode, int, int *))sym(hx11, "XGetKeyboardMapping");
	pXChangeKeyboardMapping= (int (*)(Display *, int, int, KeySym *, int))sym(hx11, "XChangeKeyboardMapping");
	pXSync                 = (int (*)(Display *, int))sym(hx11, "XSync");
	pXFlush                = (int (*)(Display *))sym(hx11, "XFlush");
	pXDisplayKeycodes      = (int (*)(Display *, int *, int *))sym(hx11, "XDisplayKeycodes");
	pXFree                 = (int (*)(void *))sym(hx11, "XFree");
	pXTestFakeKeyEvent     = (int (*)(Display *, unsigned int, int, unsigned long))sym(hxtst, "XTestFakeKeyEvent");
	pXTestQueryExtension   = (int (*)(Display *, int *, int *, int *, int *))sym(hxtst, "XTestQueryExtension");

	Display *d = pXOpenDisplay(NULL);
	if (!d) die("无法连接到 X 显示（检查 DISPLAY）");

	int ev, er, maj, mi;
	if (!pXTestQueryExtension(d, &ev, &er, &maj, &mi)) {
		die("当前 X 服务器不支持 XTEST 扩展");
	}

	int min_kc = 8, max_kc = 255;
	pXDisplayKeycodes(d, &min_kc, &max_kc);

	int per_kc = 0;
	int spare = find_spare_keycode(d, min_kc, max_kc, &per_kc);
	if (spare < 0) die("找不到空闲 keycode，无法注入 Unicode");
	if (per_kc < 1) per_kc = 1;

	/* 保存原始映射以便恢复 */
	int n_orig = 0;
	KeySym *orig = pXGetKeyboardMapping(d, (KeyCode)spare, 1, &n_orig);

	struct timespec ts = { delay_ms / 1000, (delay_ms % 1000) * 1000000L };

	const unsigned char *p = (const unsigned char *)text;
	while (*p) {
		uint32_t cp = utf8_next(&p);
		if (cp == '\n' || cp == '\r') {
			/* 换行 -> Return */
			KeySym ks = 0xFF0D;
			KeySym m[1] = { ks };
			pXChangeKeyboardMapping(d, spare, 1, m, 1);
			pXSync(d, 0);
			pXTestFakeKeyEvent(d, (unsigned)spare, 1, 0);
			pXTestFakeKeyEvent(d, (unsigned)spare, 0, 0);
			pXSync(d, 0);
			if (delay_ms > 0) nanosleep(&ts, NULL);
			continue;
		}
		if (cp == '\t') {
			KeySym m[1] = { 0xFF09 };
			pXChangeKeyboardMapping(d, spare, 1, m, 1);
			pXSync(d, 0);
			pXTestFakeKeyEvent(d, (unsigned)spare, 1, 0);
			pXTestFakeKeyEvent(d, (unsigned)spare, 0, 0);
			pXSync(d, 0);
			if (delay_ms > 0) nanosleep(&ts, NULL);
			continue;
		}
		KeySym ks = cp_to_keysym(cp);
		KeySym m[1] = { ks };
		pXChangeKeyboardMapping(d, spare, 1, m, 1);
		pXSync(d, 0);
		pXTestFakeKeyEvent(d, (unsigned)spare, 1, 0);
		pXTestFakeKeyEvent(d, (unsigned)spare, 0, 0);
		pXSync(d, 0);
		if (delay_ms > 0) nanosleep(&ts, NULL);
	}

	/* 恢复原始键盘映射 */
	if (orig && n_orig > 0) {
		pXChangeKeyboardMapping(d, spare, n_orig, orig, 1);
		pXSync(d, 0);
		pXFree(orig);
	}

	pXCloseDisplay(d);
	free(buf);
	dlclose(hxtst);
	dlclose(hx11);
	return 0;
}
