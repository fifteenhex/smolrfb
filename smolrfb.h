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

#endif /* _SMOLRFB_H */
