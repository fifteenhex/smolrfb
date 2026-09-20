// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * smolrfb_fakedrm - scan a fake DRM/KMS card out over VNC.
 *
 *	smolrfb_fakedrm <port> <program> [args...]
 *
 * The fakedrm.h supervisor runs the program against a fake card and
 * whatever it scans out goes over RFB: SETCRTC and PAGE_FLIP damage
 * everything, DIRTYFB damages just its clip rectangles. Keys from the
 * viewer go the other way, into the fake keyboard.
 */

#include "smolrfb.h"
#include "fakedrm.h"

#define MODE_W	640
#define MODE_H	480

static struct smolrfb rfb;

/* RFB keysyms are X keysyms; turn the useful ones into evdev codes */
static uint16_t keysym_key(uint32_t sym)
{
	uint16_t code = fakedrm_char_key((int) sym);

	if (code)
		return code;

	if (sym >= 0xffbe && sym <= 0xffc7)	/* F1..F10 */
		return KEY_F1 + (uint16_t) (sym - 0xffbe);

	switch (sym) {
	case ' ':    return KEY_SPACE;
	case '-':    return KEY_MINUS;
	case '=':    return KEY_EQUAL;
	case ',':    return KEY_COMMA;
	case '.':    return KEY_DOT;
	case '/':    return KEY_SLASH;
	case 0xff08: return KEY_BACKSPACE;
	case 0xff09: return KEY_TAB;
	case 0xff0d: return KEY_ENTER;
	case 0xff1b: return KEY_ESC;
	case 0xff51: return KEY_LEFT;
	case 0xff52: return KEY_UP;
	case 0xff53: return KEY_RIGHT;
	case 0xff54: return KEY_DOWN;
	case 0xffc8: return KEY_F11;
	case 0xffc9: return KEY_F12;
	case 0xffe1: return KEY_LEFTSHIFT;
	case 0xffe2: return KEY_RIGHTSHIFT;
	case 0xffe3: return KEY_LEFTCTRL;
	case 0xffe4: return KEY_RIGHTCTRL;
	case 0xffe9: return KEY_LEFTALT;
	case 0xffea: return KEY_RIGHTALT;
	}

	return 0;
}

static void key(void *user, uint32_t keysym, int down)
{
	uint16_t code = keysym_key(keysym);

	if (code)
		fakedrm_key(user, code, down);
}

static void scanout(void *user, const uint32_t *fb)
{
	smolrfb_framebuffer(&rfb, fb);
	smolrfb_damage_all(&rfb);
}

static void damage(void *user, int x, int y, int w, int h)
{
	smolrfb_damage(&rfb, x, y, w, h);
}

/* Serve VNC clients while the program sleeps in WAIT_VBLANK */
static void idle(void *user, int ms)
{
	smolrfb_poll(&rfb, ms);
}

static const struct fakedrm_ops ops = {
	.scanout = scanout,
	.damage = damage,
	.idle = idle,
};

int main(int argc, char **argv, char **envp)
{
	struct fakedrm __fakedrm_cleanup card = { .arena_fd = -1, .listener = -1 };
	struct smolrfb_input input = {
		.key = key,
		.user = &card,
	};
	int port;
	int ret;

	if (argc < 3) {
		printf("usage: %s <port> <program> [args...]\n", argv[0]);
		return 1;
	}
	port = atoi(argv[1]);

	ret = fakedrm_open(&card, &ops, NULL, MODE_W, MODE_H);
	if (ret) {
		printf("fakedrm_open() failed: %d\n", ret);
		return 1;
	}

	ret = smolrfb_open(&rfb, NULL, port, MODE_W, MODE_H,
			   "smolrfb-fakedrm", &input);
	if (ret) {
		printf("smolrfb_open() failed: %d\n", ret);
		return 1;
	}

	ret = fakedrm_spawn(&card, &argv[2], envp);
	if (ret) {
		printf("fakedrm_spawn() failed: %d\n", ret);
		return 1;
	}

	printf("fakedrm: %dx%d card at %s, keyboard at %s, pid %d, "
	       "vnc on 127.0.0.1:%d\n", MODE_W, MODE_H, FAKEDRM_CARD_PATH,
	       FAKEDRM_KBD_PATH, (int) card.child, port);

	while (!(ret = fakedrm_poll(&card, 10))) {
		ret = smolrfb_poll(&rfb, 0);
		if (ret < 0) {
			printf("smolrfb_poll() failed: %d\n", ret);
			return 1;
		}
	}

	printf("fakedrm: program exited with %d\n",
	       WIFEXITED(card.status) ? WEXITSTATUS(card.status) : 128);

	return WIFEXITED(card.status) ? WEXITSTATUS(card.status) : 1;
}
