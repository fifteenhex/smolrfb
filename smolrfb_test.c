// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * smolrfb_test - paint a framebuffer, serve it over VNC.
 *
 *	smolrfb_test [port]
 *
 * A gradient with a red 16x16 patch crawling across it, with only the
 * patch's rectangle damaged as it moves. Point a viewer at it, or run
 * the selftest against it.
 */
#include "smolrfb.h"

#define WIDTH	320
#define HEIGHT	200

static uint32_t framebuffer[WIDTH * HEIGHT];

static void key(void *user, uint32_t keysym, int down)
{
	printf("key 0x%x %s\n", (unsigned int) keysym, down ? "down" : "up");
}

static void pointer(void *user, int x, int y, unsigned int buttons)
{
	if (buttons)
		printf("pointer %d,%d buttons 0x%x\n", x, y, buttons);
}

int main(int argc, char **argv, char **envp)
{
	struct smolrfb __smolrfb_cleanup server = { 0 };
	struct smolrfb_input input = {
		.key = key,
		.pointer = pointer,
	};
	int port = argc > 1 ? atoi(argv[1]) : 5900;
	int tick = 0;
	int x, y;
	int ret;

	for (y = 0; y < HEIGHT; y++)
		for (x = 0; x < WIDTH; x++)
			framebuffer[y * WIDTH + x] = ((x * 255 / WIDTH) << 16) |
						     ((y * 255 / HEIGHT) << 8) |
						     0x40;

	ret = smolrfb_open(&server, NULL, port, WIDTH, HEIGHT, "smolrfb", &input);
	if (ret) {
		printf("smolrfb_open() failed: %d\n", ret);
		return 1;
	}

	smolrfb_framebuffer(&server, framebuffer);

	printf("listening on 127.0.0.1:%d\n", port);

	while (1) {
		int px;

		ret = smolrfb_poll(&server, 100);
		if (ret < 0) {
			printf("smolrfb_poll() failed: %d\n", ret);
			return 1;
		}

		if (++tick % 2)
			continue;

		/* paint just the patch, damage just the patch */
		px = (tick / 2 * 4) % (WIDTH - 16);
		for (y = 0; y < 16; y++)
			for (x = 0; x < 16; x++)
				framebuffer[(20 + y) * WIDTH + px + x] = 0x00FF0000;

		smolrfb_damage(&server, px, 20, 16, 16);
	}

	return 0;
}
