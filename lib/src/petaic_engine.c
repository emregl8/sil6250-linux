// SPDX-License-Identifier: GPL-2.0
/*
 * petaic_engine.c — reusable SIL6250 capture engine.  See petaic_engine.h.
 *
 * This is petaic_capture.c's handshake + capture path, lifted out of file-static
 * globals into a per-engine context so it can back both the CLI and the
 * libfprint driver.  The protocol logic is unchanged; the comments there
 * (PETAIC_PROTOCOL.md cross-refs, the level-IRQ dedup, the no-re-arm rule) still
 * apply and are kept where they earn their keep.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <mbedtls/ssl.h>
#include <mbedtls/ssl_ciphersuites.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/debug.h>

#include "petaic_proto.h"
#include "petaic_transport.h"
#include "petaic_engine.h"

/* ---- inner-header / record constants ---- */
#define SC_CMD_WRITE	0x00
#define SC_DIR_WRITE	0x02
#define SC_RX_OFF	7	/* data starts at window offset 7 */
#define SC_TX_MAX	480

/* ---- capture geometry ---- */
#define IMG_SIZE	PE_IMG_SIZE		/* 5120 */
#define IMG_CKSUM_LEN	4
#define IMG_TOTAL	(IMG_SIZE + IMG_CKSUM_LEN) /* 5124 plaintext */

#define IMG_CTRL_PARAM	0x3c	/* cmd 0x37 image-control region[0] */
#define IMG_BULK0_LEN	3109	/* cmd 0x38 first-record OutLength */
#define IMG_BULK0_RETRIES	6
#define IMG_CONT_RETRIES	8
#define CAPTURE_ATTEMPTS	6	/* fresh-handshake retries per frame */

enum cap_phase { PH_HANDSHAKE, PH_IMAGE };

/* ---- the 4 recovered global PSK keys (PETAIC_PROTOCOL.md §4) ---- */
struct psk_entry { const char *name; uint8_t key[32]; };
static const struct psk_entry g_psks[] = {
	{ "shiba",        { 0x73,0x50,0x10,0x37,0x39,0xbf,0xbc,0x8e,0x68,0xd6,0xc9,0xa8,0x94,0x27,0x99,0x34,
			    0x3d,0x82,0xc7,0x2f,0x01,0x5d,0x00,0x62,0x0f,0x79,0x14,0x2c,0xdf,0xc3,0x4c,0x81 } },
	{ "saintbernard", { 0xee,0x9a,0xbb,0x5a,0x2b,0x9e,0xc3,0x4a,0x81,0x66,0x4b,0x53,0xc2,0xcf,0xcd,0xd8,
			    0x55,0xf2,0x0a,0x62,0x2c,0x4d,0xa2,0xe8,0xf5,0x1e,0xe2,0x4e,0x95,0x10,0xdd,0x2b } },
	{ "chihuahua",    { 0x58,0x7d,0x3f,0x96,0x2e,0x3d,0x7e,0xa1,0xf0,0x8c,0x0f,0xb7,0x9c,0x03,0x78,0x4d,
			    0x9f,0xec,0x2d,0x1f,0x97,0xf7,0x6c,0x7f,0x5d,0x2f,0x66,0xed,0x43,0x2d,0x9f,0xe9 } },
	{ "bordercollie", { 0x10,0x58,0x5a,0x35,0xac,0x1e,0x78,0xce,0x4f,0x30,0x8d,0xe7,0x35,0x2d,0xd1,0xaf,
			    0x62,0x53,0x95,0x00,0xdb,0xe7,0x1b,0xe2,0x15,0xd7,0xab,0x51,0xae,0x9f,0xe3,0x40 } },
};
#define N_PSK (int)(sizeof(g_psks)/sizeof(g_psks[0]))

struct petaic_engine {
	struct petaic_dev dev;
	int verbose;
	int forced_key;		/* -1 = cycle all keys */
	int skip_prelude_20;
	int dev_open;

	enum cap_phase phase;

	/* image-phase record buffer */
	uint8_t rec[4096];
	size_t  rec_len, rec_off;
	int     rec_idx;
	uint8_t last_head[16];

	/* handshake-phase stream leftover (was static in bio_recv) */
	uint8_t hs_leftover[512];
	size_t  hs_leftover_len, hs_leftover_off;

	/* live TLS session */
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config  conf;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
	int session_live;
	int session_key;	/* PSK index of the live/last session */
};

static void msleep(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

/* ---- low-level transfer helpers ---- */
static int sc_xfer_f(petaic_engine *e, uint8_t cmd, uint8_t dir, uint8_t width,
		     uint8_t outer, uint32_t be32, const uint8_t *tx, uint32_t tx_len,
		     uint8_t *rx, uint32_t rx_cap, unsigned int flags,
		     unsigned int tries, unsigned int timeout_ms)
{
	size_t rx_len = 0;
	int rc = petaic_xfer_raw(&e->dev, cmd, dir, width, outer, be32,
				 tx_len ? tx : NULL, tx_len,
				 rx, rx_cap, &rx_len, tries, timeout_ms, flags);
	if (rc)
		return rc;
	return (int)rx_len;
}

static int sc_xfer(petaic_engine *e, uint8_t cmd, uint8_t dir, uint8_t width,
		   uint8_t outer, uint32_t be32, const uint8_t *tx, uint32_t tx_len,
		   uint8_t *rx, uint32_t rx_cap)
{
	return sc_xfer_f(e, cmd, dir, width, outer, be32, tx, tx_len, rx, rx_cap,
			 0, 0, 0);
}

/* ---- handshake prelude (identical to petaic_tls) ---- */
static void sc_prelude(petaic_engine *e)
{
	uint8_t tx[11] = { 0 };
	uint8_t rx[256];

	if (!e->skip_prelude_20)
		sc_xfer(e, 0x20, 0x01, 0x04, 0x13, 128, tx, 7, rx, sizeof(rx));

	static const uint8_t reg01[] = { 0x01, 0x08, 0xff, 0x03, 0x03, 0x00, 0x00,
					 0x00, 0x00, 0x00, 0x00 };
	sc_xfer(e, 0x01, 0x00, 0x08, 0x17, 0x08, reg01, sizeof(reg01), rx, sizeof(rx));
	sc_xfer(e, 0x28, 0x01, 0x04, 0x13, 64, tx, 7, rx, sizeof(rx));
	sc_xfer(e, 0x22, 0x01, 0x04, 0x13, 1, tx, 7, rx, sizeof(rx));
}

/* ---- handshake TX/RX chunks (cmd 0x00 write / cmd 0x22 read) ---- */
static int sc_write_chunk(petaic_engine *e, const uint8_t *data, size_t datalen)
{
	uint8_t tx[SC_TX_MAX];
	uint8_t rx[64];
	size_t payload_len;
	int r;

	if (datalen == 0 || datalen > 255)
		return -1;
	payload_len = (datalen - 1) + 4;
	if (payload_len > sizeof(tx))
		return -1;
	if (datalen > 1)
		memcpy(tx, data + 1, datalen - 1);
	memset(tx + (datalen - 1), 0, 4);

	r = sc_xfer(e, SC_CMD_WRITE, SC_DIR_WRITE, (uint8_t)datalen,
		    (uint8_t)(datalen + 15), data[0],
		    tx, (uint32_t)payload_len, rx, sizeof(rx));
	return r < 0 ? r : 0;
}

static int sc_read_chunk(petaic_engine *e, uint8_t *out, size_t want)
{
	uint8_t rx[PETAIC_READ_CHUNK_MAX];
	uint32_t rx_cap = (uint32_t)(SC_RX_OFF + want + 16);
	int r, navail;

	if (rx_cap > sizeof(rx))
		rx_cap = sizeof(rx);

	for (;;) {
		r = sc_xfer_f(e, 0, 0, 0, 0, 0, NULL, 0, rx, rx_cap,
			      PETAIC_XFER_READ_ONLY, 3, 350);
		if (r == -ETIMEDOUT)
			return 0;
		if (r < 0)
			return r;
		if (r < SC_RX_OFF || rx[0] == 0xff || rx[0] != 0x5a)
			return 0;

		navail = rx[3];
		if (navail <= 0)
			return 0;
		if (navail == 1 && rx[SC_RX_OFF] == 0x5a)
			continue;	/* skip lone 0x5A marker continuation */

		if ((size_t)navail > want)
			navail = (int)want;
		if (SC_RX_OFF + navail > r)
			navail = r - SC_RX_OFF;
		memcpy(out, rx + SC_RX_OFF, (size_t)navail);
		return navail;
	}
}

/*
 * Image phase: pull one bulk TLS record (17 03 03 ..) into e->rec.
 *   first record  (rec_idx==0): cmd 0x38 with a request frame, OutLength 3109
 *   continuations (rec_idx>0) : cmd 0x0 useage 3, READ_ONLY (no request frame)
 * The level-triggered GpioInt can re-deliver the same window after a read, so
 * continuations settle then reject any window whose head matches the last
 * accepted record.  Never re-arm 0x37 mid-stream — a stray read swallows a 0x38
 * record and desyncs GCM; recover by re-handshaking, not re-arming.
 */
static int img_pull_record(petaic_engine *e)
{
	uint8_t rx[PETAIC_READ_CHUNK_MAX];
	size_t data_len;
	int r;

	if (e->rec_idx == 0) {
		uint8_t region[7] = { (IMG_BULK0_LEN >> 8) & 0xff, 0, 0, 0, 0, 0, 0 };
		int attempt;

		for (attempt = 0; attempt < IMG_BULK0_RETRIES; attempt++) {
			r = sc_xfer_f(e, PETAIT_CMD_TLS_DATA, PETAIT_DIR_OUT,
				      PETAIT_ACCESS_WIDTH_32BIT, PETAIT_OUTER_TYPE_STD,
				      IMG_BULK0_LEN & 0xff, region, sizeof(region),
				      rx, sizeof(rx), 0, 1, 400);
			if (r == -ETIMEDOUT)
				continue;
			if (r < 0)
				break;
			if (r > SC_RX_OFF && (rx[SC_RX_OFF] == 0x17 || rx[SC_RX_OFF] == 0x16))
				break;	/* got a TLS record */
			msleep(60);
		}
	} else {
		int attempt;

		for (attempt = 0; attempt < IMG_CONT_RETRIES; attempt++) {
			msleep(40);
			r = sc_xfer_f(e, 0, 0, 0, 0, 0, NULL, 0, rx, sizeof(rx),
				      PETAIC_XFER_READ_ONLY, 2, 700);
			if (r == -ETIMEDOUT)
				continue;
			if (r < 0)
				break;
			if (r >= 16 && memcmp(rx, e->last_head, 16) == 0)
				continue;	/* stale duplicate window */
			break;	/* fresh record */
		}
	}
	if (r < 0)
		return r;
	if (r < SC_RX_OFF || rx[0] != 0x5a)
		return -EPROTO;

	data_len = (size_t)rx[3] | ((size_t)rx[4] << 8);
	if (data_len == 0 || SC_RX_OFF + data_len > (size_t)r ||
	    data_len > sizeof(e->rec))
		return -EPROTO;

	memcpy(e->rec, rx + SC_RX_OFF, data_len);
	memcpy(e->last_head, rx, 16);
	e->rec_len = data_len;
	e->rec_off = 0;
	e->rec_idx++;
	return (int)data_len;
}

/* ---- mbedTLS BIOs (ctx = petaic_engine *) ---- */
static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	petaic_engine *e = ctx;
	const uint8_t *p = buf;
	size_t left = len;

	while (left > 0) {
		size_t rlen, body;

		if (left < 5) {
			if (sc_write_chunk(e, p, left) < 0)
				return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
			break;
		}
		rlen = ((size_t)p[3] << 8) | p[4];
		if (sc_write_chunk(e, p, 5) < 0)
			return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
		p += 5; left -= 5;
		body = (rlen <= left) ? rlen : left;
		while (body > 0) {
			size_t c = body > 255 ? 255 : body;
			if (sc_write_chunk(e, p, c) < 0)
				return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
			p += c; left -= c; body -= c;
		}
	}
	return (int)len;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	petaic_engine *e = ctx;

	if (e->phase == PH_IMAGE) {
		if (e->rec_off >= e->rec_len) {
			int got = img_pull_record(e);
			if (got == -ETIMEDOUT)
				return MBEDTLS_ERR_SSL_WANT_READ;
			if (got <= 0)
				return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
		}
		size_t n = e->rec_len - e->rec_off;
		if (n > len) n = len;
		memcpy(buf, e->rec + e->rec_off, n);
		e->rec_off += n;
		return (int)n;
	}

	/* handshake phase: stream from cmd 0x22 chunked reads */
	if (e->hs_leftover_len > e->hs_leftover_off) {
		size_t n = e->hs_leftover_len - e->hs_leftover_off;
		if (n > len) n = len;
		memcpy(buf, e->hs_leftover + e->hs_leftover_off, n);
		e->hs_leftover_off += n;
		return (int)n;
	}
	e->hs_leftover_len = e->hs_leftover_off = 0;

	for (int tries = 0; tries < 12; tries++) {
		int got = sc_read_chunk(e, e->hs_leftover, sizeof(e->hs_leftover));
		if (got < 0)
			return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
		if (got > 0) {
			size_t n = (size_t)got;
			e->hs_leftover_len = n;
			if (n > len) n = len;
			memcpy(buf, e->hs_leftover, n);
			e->hs_leftover_off = n;
			return (int)n;
		}
	}
	return MBEDTLS_ERR_SSL_WANT_READ;
}

static int psk_cb(void *p, mbedtls_ssl_context *ssl,
		  const unsigned char *id, size_t id_len)
{
	petaic_engine *e = p;
	int idx = e->forced_key >= 0 ? e->forced_key : e->session_key;
	(void)id; (void)id_len;
	return mbedtls_ssl_set_hs_psk(ssl, g_psks[idx].key, sizeof(g_psks[idx].key));
}

static void mbed_dbg(void *ctx, int level, const char *file, int line, const char *str)
{
	(void)ctx; (void)file; (void)line;
	if (level <= 2)
		fprintf(stderr, "  mbedtls[%d]: %s", level, str);
}

/*
 * cmd 0x37 RequestEncryptImageCmd -> arms a capture.  useage-1 ack (no IRQ),
 * the sensor answers immediately in the same window: 5a 37 06 01 .. aa.
 */
static int sc_request_image(petaic_engine *e)
{
	uint8_t region[7] = { IMG_CTRL_PARAM, 0, 0, 0, 0, 0, 0 };
	uint8_t rx[64];
	int r = sc_xfer_f(e, PETAIT_CMD_TLS_CTRL, PETAIT_DIR_IN,
			  PETAIT_ACCESS_WIDTH_32BIT, PETAIT_OUTER_TYPE_STD,
			  0, region, sizeof(region), rx, sizeof(rx),
			  PETAIC_XFER_NO_IRQ, 1, 10);
	return r < 0 ? r : 0;
}

/*
 * Poll cmd 0x11 (finger-detect) until a finger is present.  1-byte status:
 * 0x01 = down, 0x00 = up.  Replaces the level-IRQ wait (which fires spuriously
 * on a residual assertion).  Returns 1 on finger-down, 0 on timeout.
 */
static int poll_finger(petaic_engine *e, unsigned timeout_ms)
{
	unsigned waited = 0;

	while (waited < timeout_ms) {
		uint8_t st = 0xff;
		size_t got = 0;
		int rc = petaic_xfer(&e->dev, PETAIT_CMD_POLL, PETAIT_DIR_OUT,
				     PETAIT_ACCESS_WIDTH_32BIT, NULL, 0,
				     &st, 1, &got, 1, 2, 10);
		if (rc == 0 && got == 1 && st == 0x01)
			return 1;
		msleep(40);
		waited += 40;
	}
	return 0;
}

/*
 * Poll cmd 0x11 until the finger is lifted (status 0x00).  Mirror of
 * poll_finger: only a *confirmed* up reading counts.  A poll error / no-data
 * (common on the first std poll right after a TLS image pull) must NOT be read
 * as "up" — doing so makes the lift-wait return instantly and the next capture
 * fires while the finger is still down (bursting near-identical frames).  So we
 * keep polling on error and return up only on st==0x00.  Returns 1 on up, 0 on
 * timeout (caller proceeds anyway; the diversity gate then drops the dup).
 */
static int poll_finger_up(petaic_engine *e, unsigned timeout_ms)
{
	unsigned waited = 0;

	while (waited < timeout_ms) {
		uint8_t st = 0xff;
		size_t got = 0;
		int rc = petaic_xfer(&e->dev, PETAIT_CMD_POLL, PETAIT_DIR_OUT,
				     PETAIT_ACCESS_WIDTH_32BIT, NULL, 0,
				     &st, 1, &got, 1, 2, 10);
		if (rc == 0 && got == 1 && st == 0x00)
			return 1;	/* confirmed up */
		msleep(40);
		waited += 40;
	}
	return 0;
}

/* ---- TLS session lifecycle ---- */
static void session_drop(petaic_engine *e)
{
	if (!e->session_live)
		return;
	mbedtls_ssl_free(&e->ssl);
	mbedtls_ssl_config_free(&e->conf);
	mbedtls_ctr_drbg_free(&e->drbg);
	mbedtls_entropy_free(&e->entropy);
	e->session_live = 0;
}

/* Stand up a fresh TLS-PSK session with the given key index. 0 / -errno. */
static int session_handshake(petaic_engine *e, int key_idx)
{
	static const int ciphers[] = {
		MBEDTLS_TLS_PSK_WITH_AES_256_GCM_SHA384,
		MBEDTLS_TLS_PSK_WITH_AES_128_GCM_SHA256,
		0
	};
	const char *pers = "petaic_engine";
	int ret;

	session_drop(e);
	e->session_key = key_idx;
	e->phase = PH_HANDSHAKE;
	e->hs_leftover_len = e->hs_leftover_off = 0;

	mbedtls_ssl_init(&e->ssl);
	mbedtls_ssl_config_init(&e->conf);
	mbedtls_entropy_init(&e->entropy);
	mbedtls_ctr_drbg_init(&e->drbg);

	if ((ret = mbedtls_ctr_drbg_seed(&e->drbg, mbedtls_entropy_func, &e->entropy,
					 (const unsigned char *)pers, strlen(pers))) != 0)
		goto fail;
	if ((ret = mbedtls_ssl_config_defaults(&e->conf, MBEDTLS_SSL_IS_SERVER,
					       MBEDTLS_SSL_TRANSPORT_STREAM,
					       MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
		goto fail;

	mbedtls_ssl_conf_min_tls_version(&e->conf, MBEDTLS_SSL_VERSION_TLS1_2);
	mbedtls_ssl_conf_max_tls_version(&e->conf, MBEDTLS_SSL_VERSION_TLS1_2);
	mbedtls_ssl_conf_ciphersuites(&e->conf, ciphers);
	mbedtls_ssl_conf_rng(&e->conf, mbedtls_ctr_drbg_random, &e->drbg);
	mbedtls_ssl_conf_psk_cb(&e->conf, psk_cb, e);
	mbedtls_ssl_conf_dbg(&e->conf, mbed_dbg, e);
	if (e->verbose)
		mbedtls_debug_set_threshold(2);

	if ((ret = mbedtls_ssl_setup(&e->ssl, &e->conf)) != 0)
		goto fail;
	mbedtls_ssl_set_bio(&e->ssl, e, bio_send, bio_recv, NULL);

	if (e->verbose)
		fprintf(stderr, "[engine] handshake with key '%s'\n", g_psks[key_idx].name);
	sc_prelude(e);
	while ((ret = mbedtls_ssl_handshake(&e->ssl)) != 0) {
		if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
		    ret != MBEDTLS_ERR_SSL_WANT_WRITE)
			goto fail;
	}
	if (e->verbose)
		fprintf(stderr, "[engine] *** HANDSHAKE OK (%s) ***\n",
			mbedtls_ssl_get_ciphersuite(&e->ssl));
	e->session_live = 1;
	return 0;

fail:
	session_drop(e);
	return ret ? -EIO : -EIO;
}

/* Ensure a live session exists, cycling keys if none is forced. 0 / -errno. */
static int session_ensure(petaic_engine *e)
{
	if (e->session_live)
		return 0;
	if (e->forced_key >= 0)
		return session_handshake(e, e->forced_key);
	for (int j = 0; j < N_PSK; j++)
		if (session_handshake(e, j) == 0)
			return 0;
	return -EIO;
}

/* Pull one image through the live session into img (no checksum byte). */
static int capture_through_session(petaic_engine *e, uint8_t img[IMG_SIZE],
				   unsigned finger_ms)
{
	uint8_t plain[IMG_TOTAL];
	size_t off = 0;
	int ret;

	e->phase = PH_IMAGE;
	e->rec_len = e->rec_off = 0;
	e->rec_idx = 0;
	memset(e->last_head, 0, sizeof(e->last_head));

	if (!poll_finger(e, finger_ms))
		return -ETIMEDOUT;

	/* Arm only now that the finger is down: 0x37 is one-shot; polls between
	 * it and 0x38 stale it. */
	if ((ret = sc_request_image(e)) != 0)
		return ret;
	msleep(120);

	while (off < IMG_TOTAL) {
		ret = mbedtls_ssl_read(&e->ssl, plain + off, IMG_TOTAL - off);
		if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
		    ret == MBEDTLS_ERR_SSL_WANT_WRITE)
			return -ETIMEDOUT;	/* finger moved; caller re-handshakes */
		if (ret <= 0)
			return -EIO;
		off += (size_t)ret;
	}

	uint32_t want = petaic_checksum(plain, IMG_SIZE);
	uint32_t got = (uint32_t)plain[IMG_SIZE] | ((uint32_t)plain[IMG_SIZE+1] << 8) |
		       ((uint32_t)plain[IMG_SIZE+2] << 16) | ((uint32_t)plain[IMG_SIZE+3] << 24);
	if (want != got) {
		if (e->verbose)
			fprintf(stderr, "[engine] checksum mismatch got 0x%08x want 0x%08x\n",
				got, want);
		return -EIO;
	}
	memcpy(img, plain, IMG_SIZE);
	return 0;
}

/* ---- public API ---- */
petaic_engine *engine_new(void)
{
	petaic_engine *e = calloc(1, sizeof(*e));
	if (e)
		e->forced_key = -1;
	return e;
}

void engine_free(petaic_engine *e)
{
	if (!e)
		return;
	engine_close(e);
	free(e);
}

void engine_set_verbose(petaic_engine *e, int v)
{
	e->verbose = v;
	e->dev.verbose = v;
}

int engine_set_key(petaic_engine *e, const char *name)
{
	if (!name || !strcmp(name, "all")) {
		e->forced_key = -1;
		return 0;
	}
	for (int j = 0; j < N_PSK; j++)
		if (!strcmp(name, g_psks[j].name)) {
			e->forced_key = j;
			return 0;
		}
	return -EINVAL;
}

int engine_open(petaic_engine *e, const char *devpath)
{
	int rc = petaic_open(&e->dev, devpath);
	if (rc)
		return rc;
	e->dev.verbose = e->verbose;
	e->dev_open = 1;
	return 0;
}

int engine_capture_frame(petaic_engine *e, uint8_t img[IMG_SIZE], unsigned finger_ms)
{
	if (!e->dev_open)
		return -EBADF;

	for (int a = 0; a < CAPTURE_ATTEMPTS; a++) {
		int rc = session_ensure(e);
		if (rc)
			return rc;

		/* Wait long for the finger on the first attempt, briefly on
		 * re-handshake retries. */
		unsigned fm = a == 0 ? finger_ms : 3000;
		rc = capture_through_session(e, img, fm);
		if (rc == 0)
			return 0;

		/* A desynced GCM stream is unrecoverable; drop the session so
		 * the next attempt handshakes clean.  A pure finger-timeout on
		 * the first attempt is a real "no finger", not a desync. */
		if (rc == -ETIMEDOUT && a == 0)
			return -ETIMEDOUT;
		session_drop(e);
		if (e->verbose)
			fprintf(stderr, "[engine] capture attempt %d failed (%s); "
				"re-handshaking\n", a + 1, strerror(-rc));
		msleep(150);
	}
	return -ETIMEDOUT;
}

int engine_wait_finger_up(petaic_engine *e, unsigned timeout_ms)
{
	if (!e->dev_open)
		return -EBADF;
	return poll_finger_up(e, timeout_ms);
}

void engine_close(petaic_engine *e)
{
	if (!e)
		return;
	session_drop(e);
	if (e->dev_open) {
		petaic_close(&e->dev);
		e->dev_open = 0;
	}
}
