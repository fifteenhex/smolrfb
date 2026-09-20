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

/* Returns bytes consumed, 0 if the message is incomplete. */
static inline size_t smolrfb_handle_message(struct smolrfb *s,
					    struct smolrfb_client *c,
					    const uint8_t *p, size_t n)
{
	switch (p[0]) {
	case SMOLRFB_MSG_SET_PIXEL_FORMAT:
		if (n < 20)
			return 0;
		/* Palette formats need a colour map we do not have */
		if (!p[7]) {
			fprintf(stderr, "smolrfb: client wants a palette format "
				"(%u bpp), only true colour works here\n", p[4]);
			c->st = SMOLRFB_C_DEAD;
			return 20;
		}
		if (p[4] != 8 && p[4] != 16 && p[4] != 32) {
			fprintf(stderr, "smolrfb: client wants %u bpp, only 8, "
				"16 and 32 work here\n", p[4]);
			c->st = SMOLRFB_C_DEAD;
			return 20;
		}
		c->pf_bpp = p[4];
		c->pf_depth = p[5];
		c->pf_big = p[6];
		c->pf_true = p[7];
		c->pf_rmax = smolrfb_get16(p + 8);
		c->pf_gmax = smolrfb_get16(p + 10);
		c->pf_bmax = smolrfb_get16(p + 12);
		c->pf_rsh = p[14];
		c->pf_gsh = p[15];
		c->pf_bsh = p[16];
		c->pf_native = (c->pf_bpp == 32 && c->pf_big == 0 &&
				c->pf_rmax == 255 && c->pf_gmax == 255 &&
				c->pf_bmax == 255 && c->pf_rsh == 16 &&
				c->pf_gsh == 8 && c->pf_bsh == 0);
		fprintf(stderr, "smolrfb: client wants %u bpp, depth %u, %s, "
			"max %u/%u/%u shift %u/%u/%u%s\n",
			c->pf_bpp, c->pf_depth,
			c->pf_big ? "big-endian" : "little-endian",
			c->pf_rmax, c->pf_gmax, c->pf_bmax,
			c->pf_rsh, c->pf_gsh, c->pf_bsh,
			c->pf_native ? " (ours)" : " -- converting");
		/* Everything the client has is now in the wrong format */
		smolrfb_client_reset_damage(c);
		smolrfb_client_damage(c, 0, 0, s->w, s->h);
		return 20;

	case SMOLRFB_MSG_SET_ENCODINGS: {
		unsigned int cnt;
		size_t need;

		if (n < 4)
			return 0;
		cnt = smolrfb_get16(p + 2);
		need = 4 + (size_t) cnt * 4;
		if (n < need)
			return 0;
		/* Ignored: raw is all we send and clients must take it */
		return need;
	}

	case SMOLRFB_MSG_FB_UPDATE_REQ: {
		int inc, x, y, w, h;

		if (n < 10)
			return 0;
		inc = p[1];
		x = smolrfb_get16(p + 2);
		y = smolrfb_get16(p + 4);
		w = smolrfb_get16(p + 6);
		h = smolrfb_get16(p + 8);
		/* Non-incremental means the client has nothing at all */
		if (!inc)
			smolrfb_client_damage(c, x, y, x + w, y + h);
		c->req_pending = 1;
		c->req_incremental = inc;
		return 10;
	}

	case SMOLRFB_MSG_KEY:
		if (n < 8)
			return 0;
		if (s->input.key)
			s->input.key(s->input.user, smolrfb_get32(p + 4),
				     p[1] != 0);
		return 8;

	case SMOLRFB_MSG_POINTER:
		if (n < 6)
			return 0;
		if (s->input.pointer)
			s->input.pointer(s->input.user, smolrfb_get16(p + 2),
					 smolrfb_get16(p + 4), p[1]);
		return 6;

	case SMOLRFB_MSG_CUT_TEXT: {
		uint32_t len;
		size_t have;

		if (n < 8)
			return 0;
		len = smolrfb_get32(p + 4);
		/* Take the header, drop the body as it arrives; see `drop` */
		have = n - 8 < len ? n - 8 : len;
		c->drop = len - (uint32_t) have;
		return 8 + have;
	}

	default: {
		/* No length to skip past, so the stream is lost */
		static char msg[72];

		snprintf(msg, sizeof(msg),
			 "unknown client message type %u", p[0]);
		smolrfb_die(c, msg);
		return 0;
	}
	}
}

static inline void smolrfb_client_read(struct smolrfb *s,
				       struct smolrfb_client *c)
{
	ssize_t r;

	r = read(c->fd, c->in + c->in_n, sizeof(c->in) - c->in_n);
	if (r == 0) {
		smolrfb_die(c, "client closed the connection");
		return;
	}
	if (r < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			smolrfb_die(c, strerror(errno));
		return;
	}
	c->in_n += (size_t) r;

	for (;;) {
		size_t used;

		if (c->st == SMOLRFB_C_VERSION) {
			if (c->in_n < 12)
				return;
			/* "RFB 003.00X\n" */
			c->rfb_minor = c->in[10] - '0';
			if (c->rfb_minor < 3)
				c->rfb_minor = 3;
			if (c->rfb_minor >= 7) {
				smolrfb_out_u8(c, 1);	/* one security type */
				smolrfb_out_u8(c, 1);	/* None */
			} else {
				smolrfb_out_u32(c, 1);	/* 3.3: server picks; None */
			}
			memmove(c->in, c->in + 12, c->in_n - 12);
			c->in_n -= 12;
			c->st = c->rfb_minor >= 7 ? SMOLRFB_C_SECURITY : SMOLRFB_C_INIT;
			continue;
		}
		if (c->st == SMOLRFB_C_SECURITY) {
			if (c->in_n < 1)
				return;
			if (c->in[0] != 1) {
				smolrfb_die(c, "client chose a security type we did not offer");
				return;
			}
			memmove(c->in, c->in + 1, --c->in_n);
			/* SecurityResult, 3.8 only */
			if (c->rfb_minor >= 8)
				smolrfb_out_u32(c, 0);
			c->st = SMOLRFB_C_INIT;
			continue;
		}
		if (c->st == SMOLRFB_C_INIT) {
			size_t nl;

			if (c->in_n < 1)
				return;		/* ClientInit: shared flag */
			memmove(c->in, c->in + 1, --c->in_n);
			smolrfb_out_u16(c, (uint16_t) s->w);
			smolrfb_out_u16(c, (uint16_t) s->h);
			smolrfb_put_pixel_format(c);
			nl = strlen(s->name);
			smolrfb_out_u32(c, (uint32_t) nl);
			smolrfb_out_put(c, s->name, nl);
			c->st = SMOLRFB_C_READY;
			fprintf(stderr, "smolrfb: client ready, RFB 3.%d, %dx%d\n",
				c->rfb_minor, s->w, s->h);
			/* Nothing sent yet, so it is all dirty */
			smolrfb_client_reset_damage(c);
			smolrfb_client_damage(c, 0, 0, s->w, s->h);
			continue;
		}
		/* SMOLRFB_C_READY */
		if (c->in_n == 0)
			return;
		if (c->drop) {		/* still swallowing a long clipboard */
			size_t k = c->drop < c->in_n ? c->drop : c->in_n;

			c->drop -= (uint32_t) k;
			memmove(c->in, c->in + k, c->in_n - k);
			c->in_n -= k;
			continue;
		}
		used = smolrfb_handle_message(s, c, c->in, c->in_n);
		if (c->st == SMOLRFB_C_DEAD)
			return;
		if (used == 0)
			return;		/* incomplete, wait for more */
		memmove(c->in, c->in + used, c->in_n - used);
		c->in_n -= used;
	}
}

static inline void smolrfb_client_write(struct smolrfb_client *c)
{
	while (c->out_n) {
		ssize_t w = write(c->fd, c->out, c->out_n);

		if (w < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
				c->st = SMOLRFB_C_DEAD;
			return;
		}
		memmove(c->out, c->out + w, c->out_n - (size_t) w);
		c->out_n -= (size_t) w;
	}
}

/* A dotted quad to a network order address, what inet_addr() did */
static inline int smolrfb_parse_addr(const char *str, uint32_t *addr)
{
	uint32_t a = 0;
	int octet, val, digits;

	for (octet = 0; octet < 4; octet++) {
		val = 0;
		digits = 0;
		while (*str >= '0' && *str <= '9') {
			val = val * 10 + (*str++ - '0');
			if (val > 255)
				return -EINVAL;
			digits++;
		}
		if (!digits)
			return -EINVAL;
		a = (a << 8) | (uint32_t) val;
		if (octet < 3 && *str++ != '.')
			return -EINVAL;
	}
	if (*str)
		return -EINVAL;

	*addr = htonl(a);

	return 0;
}

static inline int smolrfb_open(struct smolrfb *s, const char *bind_addr,
			       int port, int w, int h, const char *name,
			       const struct smolrfb_input *input)
{
	struct sockaddr_in a = { 0 };
	int one = 1;
	int ret;
	int i;

	if (w <= 0 || h <= 0 || w > SMOLRFB_MAX_DIM || h > SMOLRFB_MAX_DIM)
		return -EINVAL;

	memset(s, 0, sizeof(*s));
	s->w = w;
	s->h = h;
	snprintf(s->name, sizeof(s->name), "%s", name ? name : "smolrfb");
	if (input)
		s->input = *input;
	for (i = 0; i < SMOLRFB_MAX_CLIENTS; i++) {
		s->cl[i].fd = -1;
		s->cl[i].st = SMOLRFB_C_DEAD;
	}

	a.sin_family = AF_INET;
	a.sin_port = htons((uint16_t) port);
	/* Loopback by default, there is no authentication in here */
	if (bind_addr) {
		ret = smolrfb_parse_addr(bind_addr, &a.sin_addr.s_addr);
		if (ret)
			return ret;
	} else {
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	}

	s->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (s->listen_fd < 0)
		return -errno;

	setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	if (bind(s->listen_fd, (struct sockaddr *) &a, sizeof(a)) < 0 ||
	    listen(s->listen_fd, 4) < 0) {
		ret = -errno;
		close(s->listen_fd);
		s->listen_fd = -1;
		return ret;
	}

	return 0;
}

#endif /* _SMOLRFB_H */
