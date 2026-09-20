// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef _SMOLRFB_H
#define _SMOLRFB_H

/*
 * smolrfb - a small RFB (VNC) server for putting a framebuffer on a wire.
 *
 * A single header that builds with nolibc. Upstream nolibc has no
 * sockets so those come from nolibc-extensions; force include both
 * before this header:
 *
 *	-include $(NOLIBCDIR)/nolibc.h
 *	-include $(NOLIBCEXTDIR)/include/nolibc-extensions.h
 *
 * RFB 3.3/3.7/3.8, security type 1 (none), raw encoding only, several
 * clients at once. The framebuffer is 32bpp 0x00RRGGBB in host order and
 * pixels are converted to whatever true colour format the client asks
 * for. Damage is tracked per client as one bounding box. Key and pointer
 * events go to callbacks, or are dropped.
 *
 * No zlib, because nolibc has none, so everything goes raw; a full
 * 1024x768 frame is 3MB, which loopback shrugs at. No palette formats.
 * No encryption or authentication: bind to loopback and tunnel over ssh.
 *
 * Not threaded. One thread calls smolrfb_poll() in a loop.
 */

#include <linux/in.h>
#include <linux/tcp.h>

/* The kernel defines it the same way, but not in a uapi header */
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK O_NONBLOCK
#endif

#define SMOLRFB_MAX_CLIENTS	8
#define SMOLRFB_INBUF_SZ	512
#define SMOLRFB_OUTBUF_MIN	(1 << 16)
#define SMOLRFB_NAME_SZ		128
#define SMOLRFB_MAX_DIM		16384

/* client-to-server message types we care about */
#define SMOLRFB_MSG_SET_PIXEL_FORMAT	0
#define SMOLRFB_MSG_SET_ENCODINGS	2
#define SMOLRFB_MSG_FB_UPDATE_REQ	3
#define SMOLRFB_MSG_KEY			4
#define SMOLRFB_MSG_POINTER		5
#define SMOLRFB_MSG_CUT_TEXT		6

#define SMOLRFB_ENC_RAW			0

/*
 * The socket is non-blocking and a client can dribble the version string
 * a byte at a time, so the handshake has to be a state machine.
 */
enum smolrfb_cstate {
	SMOLRFB_C_VERSION,
	SMOLRFB_C_SECURITY,
	SMOLRFB_C_INIT,
	SMOLRFB_C_READY,
	SMOLRFB_C_DEAD,
};

/* Input from a viewer. Either callback may be NULL. */
struct smolrfb_input {
	void (*key)(void *user, uint32_t keysym, int down);
	/* buttons is the RFB mask: bit 0 left, 1 middle, 2 right, 3/4 wheel */
	void (*pointer)(void *user, int x, int y, unsigned int buttons);
	void *user;
};

struct smolrfb_client {
	int fd;
	enum smolrfb_cstate st;
	int rfb_minor;		/* 3, 7 or 8 */

	uint8_t in[SMOLRFB_INBUF_SZ];
	size_t in_n;

	uint8_t *out;
	size_t out_n, out_cap;

	/* What the client asked for, which is not necessarily what we have */
	uint8_t pf_bpp, pf_depth, pf_big, pf_true;
	uint16_t pf_rmax, pf_gmax, pf_bmax;
	uint8_t pf_rsh, pf_gsh, pf_bsh;
	int pf_native;		/* exactly our 0x00RRGGBB little-endian */

	/* Pending damage, one bounding box, plus an unanswered request */
	int dmg_x0, dmg_y0, dmg_x1, dmg_y1;
	int req_pending;
	int req_incremental;

	/*
	 * Bytes of an over-long ClientCutText still to be discarded. A
	 * clipboard bigger than `in` never fits, and waiting for it wedges
	 * the parser: read() gets a zero-length request and returns 0,
	 * which is indistinguishable from the client hanging up.
	 */
	uint32_t drop;

	const char *why;	/* why it died, for the log */
};

/*
 * Mark a client dead and say why. "client gone" on its own cannot tell
 * a clean disconnect from a protocol refusal from a bug in here.
 */
#define smolrfb_die(_c, _reason)			\
	do {						\
		(_c)->why = (_reason);			\
		(_c)->st = SMOLRFB_C_DEAD;		\
	} while (0)

struct smolrfb {
	int listen_fd;
	int w, h;
	char name[SMOLRFB_NAME_SZ];
	const uint32_t *fb;
	struct smolrfb_input input;
	struct smolrfb_client cl[SMOLRFB_MAX_CLIENTS];
	int nclients;
};

static inline void smolrfb_client_kill(struct smolrfb_client *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;

	free(c->out);
	c->out = NULL;
	c->out_n = c->out_cap = 0;

	c->st = SMOLRFB_C_DEAD;
	fprintf(stderr, "smolrfb: client gone -- %s\n",
		c->why ? c->why : "no reason recorded");
}

/* Wire order is big-endian, and nothing in the messages is aligned */
static inline uint16_t smolrfb_get16(const uint8_t *p)
{
	return (uint16_t) ((p[0] << 8) | p[1]);
}

static inline uint32_t smolrfb_get32(const uint8_t *p)
{
	return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
	       ((uint32_t) p[2] << 8) | p[3];
}

static inline void smolrfb_out_reserve(struct smolrfb_client *c, size_t n)
{
	size_t cap;
	uint8_t *p;

	if (c->out_n + n <= c->out_cap)
		return;

	cap = c->out_cap ? c->out_cap : SMOLRFB_OUTBUF_MIN;
	while (cap < c->out_n + n)
		cap *= 2;

	p = realloc(c->out, cap);
	/* out of memory: drop the client, not the server */
	if (!p) {
		smolrfb_die(c, "out of memory growing the output buffer");
		return;
	}

	c->out = p;
	c->out_cap = cap;
}

static inline void smolrfb_out_put(struct smolrfb_client *c, const void *p,
				   size_t n)
{
	smolrfb_out_reserve(c, n);
	if (c->st == SMOLRFB_C_DEAD)
		return;

	memcpy(c->out + c->out_n, p, n);
	c->out_n += n;
}

static inline void smolrfb_out_u8(struct smolrfb_client *c, uint8_t v)
{
	smolrfb_out_put(c, &v, 1);
}

static inline void smolrfb_out_u16(struct smolrfb_client *c, uint16_t v)
{
	uint8_t b[2] = { (uint8_t) (v >> 8), (uint8_t) v };

	smolrfb_out_put(c, b, 2);
}

static inline void smolrfb_out_u32(struct smolrfb_client *c, uint32_t v)
{
	uint8_t b[4] = { (uint8_t) (v >> 24), (uint8_t) (v >> 16),
			 (uint8_t) (v >> 8), (uint8_t) v };

	smolrfb_out_put(c, b, 4);
}

static inline void smolrfb_client_reset_damage(struct smolrfb_client *c)
{
	c->dmg_x0 = c->dmg_y0 = 1 << 30;
	c->dmg_x1 = c->dmg_y1 = -1;
}

static inline void smolrfb_client_damage(struct smolrfb_client *c,
					 int x0, int y0, int x1, int y1)
{
	if (x0 < c->dmg_x0)
		c->dmg_x0 = x0;
	if (y0 < c->dmg_y0)
		c->dmg_y0 = y0;
	if (x1 > c->dmg_x1)
		c->dmg_x1 = x1;
	if (y1 > c->dmg_y1)
		c->dmg_y1 = y1;
}

static inline int smolrfb_client_has_damage(const struct smolrfb_client *c)
{
	return c->dmg_x1 > c->dmg_x0 && c->dmg_y1 > c->dmg_y0;
}

/*
 * ServerInit's PixelFormat: 16 bytes of 32bpp depth 24 little-endian
 * true colour, shifts 16/8/0, which is what the framebuffer holds.
 */
static inline void smolrfb_put_pixel_format(struct smolrfb_client *c)
{
	smolrfb_out_u8(c, 32);
	smolrfb_out_u8(c, 24);
	smolrfb_out_u8(c, 0);
	smolrfb_out_u8(c, 1);
	smolrfb_out_u16(c, 255);
	smolrfb_out_u16(c, 255);
	smolrfb_out_u16(c, 255);
	smolrfb_out_u8(c, 16);
	smolrfb_out_u8(c, 8);
	smolrfb_out_u8(c, 0);
	/* padding */
	smolrfb_out_u8(c, 0);
	smolrfb_out_u8(c, 0);
	smolrfb_out_u8(c, 0);
}

/* What a client gets until it asks for something else */
static inline void smolrfb_pf_set_native(struct smolrfb_client *c)
{
	c->pf_bpp = 32;
	c->pf_depth = 24;
	c->pf_big = 0;
	c->pf_true = 1;
	c->pf_rmax = c->pf_gmax = c->pf_bmax = 255;
	c->pf_rsh = 16;
	c->pf_gsh = 8;
	c->pf_bsh = 0;
	c->pf_native = 1;
}

/*
 * Scale each channel of 0x00RRGGBB to the client's max, shift it into
 * place and write bpp/8 bytes in the client's byte order.
 */
static inline void smolrfb_pack_pixels(const struct smolrfb_client *c,
				       const uint32_t *src, size_t npix,
				       uint8_t *dst)
{
	unsigned int bytes = c->pf_bpp / 8u;
	size_t i;

	for (i = 0; i < npix; i++) {
		uint32_t v = src[i];
		uint32_t r = (v >> 16) & 0xff, g = (v >> 8) & 0xff, b = v & 0xff;
		uint32_t px = ((r * c->pf_rmax / 255u) << c->pf_rsh)
			    | ((g * c->pf_gmax / 255u) << c->pf_gsh)
			    | ((b * c->pf_bmax / 255u) << c->pf_bsh);
		unsigned int k;

		for (k = 0; k < bytes; k++)
			dst[i * bytes + k] = (uint8_t)
				(px >> (8 * (c->pf_big ? (bytes - 1 - k) : k)));
	}
}

static inline void smolrfb_send_update(struct smolrfb *s,
				       struct smolrfb_client *c)
{
	int x0 = c->dmg_x0 < 0 ? 0 : c->dmg_x0;
	int y0 = c->dmg_y0 < 0 ? 0 : c->dmg_y0;
	int x1 = c->dmg_x1 > s->w ? s->w : c->dmg_x1;
	int y1 = c->dmg_y1 > s->h ? s->h : c->dmg_y1;
	int w = x1 - x0, h = y1 - y0;
	unsigned int bytes = c->pf_bpp / 8u;
	size_t npix, nbytes;
	uint32_t *raw;
	uint8_t *wire;
	int y;

	if (w <= 0 || h <= 0 || !s->fb) {
		smolrfb_client_reset_damage(c);
		return;
	}

	/* Gather the rectangle; fb rows are s->w apart, the wire wants w */
	npix = (size_t) w * h;
	nbytes = npix * bytes;
	raw = malloc(npix * 4);
	if (!raw) {
		smolrfb_client_reset_damage(c);
		return;
	}

	for (y = 0; y < h; y++)
		memcpy(raw + (size_t) y * w,
		       s->fb + (size_t) (y0 + y) * s->w + x0,
		       (size_t) w * 4);

	/* Native is the common case and needs no second buffer */
	if (c->pf_native) {
		wire = (uint8_t *) raw;
	} else {
		wire = malloc(nbytes);
		if (!wire) {
			free(raw);
			smolrfb_client_reset_damage(c);
			return;
		}
		smolrfb_pack_pixels(c, raw, npix, wire);
	}

	smolrfb_out_u8(c, 0);			/* FramebufferUpdate */
	smolrfb_out_u8(c, 0);			/* padding */
	smolrfb_out_u16(c, 1);			/* one rectangle */
	smolrfb_out_u16(c, (uint16_t) x0);
	smolrfb_out_u16(c, (uint16_t) y0);
	smolrfb_out_u16(c, (uint16_t) w);
	smolrfb_out_u16(c, (uint16_t) h);
	smolrfb_out_u32(c, SMOLRFB_ENC_RAW);
	smolrfb_out_put(c, wire, nbytes);

	if (wire != (uint8_t *) raw)
		free(wire);
	free(raw);

	smolrfb_client_reset_damage(c);
	c->req_pending = 0;
}

#endif /* _SMOLRFB_H */
