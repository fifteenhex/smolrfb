// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * smolrfb_selftest - an RFB client that checks what smolrfb serves.
 *
 *	smolrfb_selftest <port>
 *
 * A client rather than unit tests on the server's internals, because
 * what matters is the byte stream a real viewer has to parse; a server
 * that is self-consistently wrong passes any test that shares its
 * assumptions.
 *
 * Run it against smolrfb_test, or anything else painting the same
 * gradient and crawling patch. Prints "smolrfb_selftest ok" and exits 0,
 * or says what failed.
 */

#include <linux/in.h>

#define SELFTEST_ENC_RAW	0
#define SELFTEST_IO_TIMEOUT_MS	10000

static int sock = -1;

static void fail(const char *why)
{
	printf("FAIL: %s\n", why);
	exit(1);
}

static void failnum(const char *why, long num)
{
	printf("FAIL: %s (%ld)\n", why, num);
	exit(1);
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t) ((p[0] << 8) | p[1]);
}

static uint32_t get32(const uint8_t *p)
{
	return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
	       ((uint32_t) p[2] << 8) | p[3];
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t) (v >> 8);
	p[1] = (uint8_t) v;
}

/* Read exactly n bytes; a wedged server should fail, not hang */
static void need(void *buf, size_t n)
{
	uint8_t *p = buf;

	while (n) {
		struct pollfd pf = {
			.fd = sock,
			.events = POLLIN,
		};
		ssize_t got;

		if (poll(&pf, 1, SELFTEST_IO_TIMEOUT_MS) <= 0)
			fail("timed out waiting for the server");

		got = read(sock, p, n);
		if (got <= 0)
			fail("server closed the connection");

		p += got;
		n -= (size_t) got;
	}
}

static void put(const void *buf, size_t n)
{
	const uint8_t *p = buf;

	while (n) {
		ssize_t sent = write(sock, p, n);

		if (sent <= 0)
			fail("write to the server failed");
		p += sent;
		n -= (size_t) sent;
	}
}

static unsigned int width, height;
/*
 * Bytes per pixel on the wire: what was last asked for, not what
 * ServerInit advertised. Reading the wrong one hangs the test.
 */
static unsigned int bypp = 4;
static uint8_t *pixels;

struct rect {
	unsigned int x, y, w, h;
	size_t nbytes;
};

/* Ask for the whole screen, take the one rectangle smolrfb sends. */
static void get_update(int incremental, struct rect *r)
{
	uint8_t req[10] = { 3, (uint8_t) incremental };
	uint8_t hdr[4], recthdr[12];
	unsigned int nrect;
	int32_t enc;

	put16(req + 6, (uint16_t) width);
	put16(req + 8, (uint16_t) height);
	put(req, sizeof(req));

	need(hdr, sizeof(hdr));
	if (hdr[0] != 0)
		failnum("expected FramebufferUpdate", hdr[0]);
	nrect = get16(hdr + 2);
	if (nrect != 1)
		failnum("expected one rectangle", nrect);

	need(recthdr, sizeof(recthdr));
	r->x = get16(recthdr);
	r->y = get16(recthdr + 2);
	r->w = get16(recthdr + 4);
	r->h = get16(recthdr + 6);
	enc = (int32_t) get32(recthdr + 8);
	if (enc != SELFTEST_ENC_RAW)
		failnum("unexpected encoding", enc);

	r->nbytes = (size_t) r->w * r->h * bypp;
	if (r->nbytes > (size_t) width * height * 4)
		fail("rectangle larger than the screen");
	need(pixels, r->nbytes);
}

int main(int argc, char **argv, char **envp)
{
	struct sockaddr_in addr = { 0 };
	uint8_t buf[16], pf[16], name[64];
	unsigned int ntypes, nlen;
	struct rect r;
	uint32_t px;
	int port;
	unsigned int i;

	static const struct {
		uint8_t bpp, depth, big;
		uint16_t rmax;
		uint8_t rsh, gsh, bsh;
	} formats[] = {
		{ 16, 16, 0, 31, 11, 5, 0 },	/* 5-6-5, the common one */
		{ 16, 15, 0, 31, 10, 5, 0 },	/* 5-5-5 */
		{ 8,   8, 0,  7,  5, 2, 0 },	/* 3-3-2 */
		{ 32, 24, 1, 255, 16, 8, 0 },	/* big-endian */
		{ 32, 24, 0, 255,  0, 8, 16 },	/* BGR rather than RGB */
	};

	if (argc < 2) {
		printf("usage: %s <port>\n", argv[0]);
		return 1;
	}
	port = atoi(argv[1]);

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0)
		fail("socket() failed");
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t) port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0)
		fail("could not connect to the server");

	need(buf, 12);
	if (memcmp(buf, "RFB 003.008\n", 12))
		fail("bad version string");
	put("RFB 003.008\n", 12);

	need(buf, 1);
	ntypes = buf[0];
	if (ntypes != 1)
		failnum("security type count", ntypes);
	need(buf, 1);
	if (buf[0] != 1)
		failnum("security type", buf[0]);
	put("\x01", 1);				/* pick None */

	need(buf, 4);
	if (get32(buf) != 0)
		failnum("SecurityResult", get32(buf));

	put("\x01", 1);				/* ClientInit, shared */

	need(buf, 4);
	width = get16(buf);
	height = get16(buf + 2);
	need(pf, 16);				/* ServerInit pixel format */
	if (pf[0] != 32 || pf[1] != 24 || pf[3] != 1)
		fail("pixel format is not 32bpp depth 24 true colour");
	if (get16(pf + 4) != 255 || get16(pf + 6) != 255 || get16(pf + 8) != 255)
		fail("pixel format maxes are not 255/255/255");
	if (pf[10] != 16 || pf[11] != 8 || pf[12] != 0)
		fail("pixel format shifts are not 16/8/0");

	need(buf, 4);
	nlen = get32(buf);
	if (nlen >= sizeof(name))
		failnum("server name too long", nlen);
	need(name, nlen);
	name[nlen] = '\0';
	printf("server: %ux%u  %s  bpp=%u %s-endian\n",
	       width, height, name, pf[0], pf[2] ? "big" : "little");

	pixels = malloc((size_t) width * height * 4);
	if (!pixels)
		fail("out of memory");

	/* SetEncodings: raw */
	put("\x02\x00\x00\x01\x00\x00\x00\x00", 8);

	/* 1. the first, non-incremental update must cover the whole screen */
	get_update(0, &r);
	if (r.x != 0 || r.y != 0 || r.w != width || r.h != height)
		fail("first update did not cover the whole screen");
	px = ((uint32_t) pixels[2] << 16 | pixels[1] << 8 | pixels[0]);
	printf("first update: %ux%u raw, top-left pixel %06x\n", r.w, r.h,
	       (unsigned int) px);

	/* 2. an incremental update should be the patch, not the screen */
	get_update(1, &r);
	if (r.w > 64 || r.h > 64)
		fail("incremental update was not the patch");
	px = ((uint32_t) pixels[2] << 16 | pixels[1] << 8 | pixels[0]);
	printf("incremental: %ux%u at %u,%u, pixel %06x\n",
	       r.w, r.h, r.x, r.y, (unsigned int) px);
	if (px != 0xFF0000)
		fail("patch pixel is not ff0000, the colour order is wrong");

	/* 3. the formats real viewers ask for, checking each length */
	for (i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		uint8_t spf[20] = { 0 };
		uint16_t gmax = formats[i].rmax;

		/* 5-6-5: green gets the extra bit */
		if (formats[i].bpp == 16 && formats[i].depth == 16)
			gmax = 63;

		spf[4] = formats[i].bpp;
		spf[5] = formats[i].depth;
		spf[6] = formats[i].big;
		spf[7] = 1;			/* true colour */
		put16(spf + 8, formats[i].rmax);
		put16(spf + 10, gmax);
		put16(spf + 12, formats[i].rmax);
		spf[14] = formats[i].rsh;
		spf[15] = formats[i].gsh;
		spf[16] = formats[i].bsh;
		put(spf, sizeof(spf));

		bypp = formats[i].bpp / 8u;
		get_update(0, &r);
		if (r.nbytes != (size_t) r.w * r.h * bypp)
			fail("update length does not match the requested depth");
		printf("SetPixelFormat %2u bpp depth %2u %s: %u bytes\n",
		       formats[i].bpp, formats[i].depth,
		       formats[i].big ? "big" : "little",
		       (unsigned int) r.nbytes);
	}

	close(sock);
	printf("smolrfb_selftest ok\n");

	return 0;
}
