// SPDX-License-Identifier: GPL-2.0
/*
 * petaic_capture - grab one fingerprint image from the SIL6250 sensor.
 *
 * This is step 5 of PETAIC_PROTOCOL.md: the post-handshake capture loop.  It
 * stands up the same TLS-PSK session as petaic_tls (host = server), then:
 *
 *   1. cmd 0x37  RequestEncryptImageCmd  -> arms capture (useage-1 ack, no IRQ)
 *   2. wait for the finger-down IRQ
 *   3. cmd 0x38  bulk TLS app-data record (first chunk, ~3109 wire bytes)
 *      cmd 0x00  useage-3 READ_ONLY continuation(s) for the remainder
 *   4. mbedtls_ssl_read() decrypts the records into 5124 plaintext bytes
 *      = 5120-byte image (64x80) + a 4-byte trailing one's-complement checksum
 *   5. verify the checksum, strip it, write the image as a PGM
 *
 * Capture transaction decoded byte-for-byte from petaic_decoded_trace.log
 * (lines 238-496):
 *   0x37 req: f0 13 .. 5a 37 00 04 00 00 00 00  3c 00 00 00 00 00 00 2e ff ff ff
 *   0x38 req: f0 13 .. 5a 38 01 04 00 00 00 25  0c 00 00 00 00 00 00 37 ff ff ff
 *             (be32=0x25, region[0]=0x0c -> LE16 0x0c25 = 3109 = OutLength)
 *   0x38 rsp: 5a 38 05 [len_lo] [len_hi] 00 00 | 17 03 03 .. (TLS record @ off 7)
 *   2nd rsp:  pulled with cmd 0x0 useage 3, NO request frame (READ_ONLY).
 *
 * The two records decrypt to 3080 + 2044 = 5124 plaintext bytes.  Note the
 * Windows driver additionally runs PetaicCryptSendEnimgKey (cmd 0x27) before
 * capture on a *keyed* unit; that is template/engine key provisioning, not the
 * TLS transport, and is not required to pull a raw image.  --sendkey is left as
 * a TODO hook if a unit turns out to NAK 0x37 without it.
 *
 * Build:  make petaic_capture   (links -lmbedtls -lmbedx509 -lmbedcrypto)
 * Usage:  sudo ./petaic_capture [--dev /dev/sil6250] [--key shiba|all]
 *                               [--out img.pgm] [--finger-ms 15000] [-v]
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

/* ---- inner-header / record constants ---- */
#define SC_CMD_WRITE	0x00
#define SC_DIR_WRITE	0x02
#define SC_CMD_READ	0x22
#define SC_DIR_READ	0x04
#define SC_RX_OFF	7	/* data starts at window offset 7 */
#define SC_TX_MAX	480

/* ---- capture geometry (from cmd 0x14 sensor info: 64x80) ---- */
#define IMG_W		64
#define IMG_H		80
#define IMG_SIZE	(IMG_W * IMG_H)		/* 5120 */
#define IMG_CKSUM_LEN	4
#define IMG_TOTAL	(IMG_SIZE + IMG_CKSUM_LEN) /* 5124 plaintext */

/* cmd 0x37 image-control parameter byte (region[0] in the trace). */
#define IMG_CTRL_PARAM	0x3c
/* cmd 0x38 first-record OutLength (fixed constant the Windows driver uses). */
#define IMG_BULK0_LEN	3109
/* re-issue the first 0x38 read this many times before giving up on a capture
 * (a moving finger aborts the one-shot capture; we fail fast and re-handshake). */
#define IMG_BULK0_RETRIES	6
/* whole-capture attempts, each on a fresh handshake (clean TLS session). */
#define CAPTURE_ATTEMPTS	6
/* re-wait the continuation read this many times to skip stale level-IRQ re-reads. */
#define IMG_CONT_RETRIES	8

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

/* ---- capture phase: handshake (read via 0x22) vs image (read via 0x38/0x0) ---- */
enum cap_phase { PH_HANDSHAKE, PH_IMAGE };

static struct petaic_dev g_dev;
static int g_verbose;
static int g_forced_key = -1;
static int g_skip_prelude_20;
static enum cap_phase g_phase = PH_HANDSHAKE;
static unsigned g_finger_ms = 15000;
static int g_frames = 1;		/* how many images to capture this run */
static int g_background = 0;	/* capture flat-field with NO finger (FPN reference) */
static int g_frame_done = 0;	/* frames captured so far (persists across re-handshakes) */

/* image-phase BIO record buffer + which bulk record we're on */
static uint8_t g_rec[4096];
static size_t  g_rec_len, g_rec_off;
static int     g_rec_idx;	/* 0 = first (cmd 0x38), >0 = continuation */
static uint8_t g_last_head[16];	/* window head of the last accepted record (dedup) */

static void msleep(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

static void hexdump(const char *tag, const uint8_t *b, size_t n)
{
	if (!g_verbose)
		return;
	fprintf(stderr, "  %s (%zu)\n", tag, n);
	for (size_t i = 0; i < n; i += 16) {
		fprintf(stderr, "    %04zx  ", i);
		for (size_t j = 0; j < 16 && i + j < n; j++)
			fprintf(stderr, "%02x ", b[i + j]);
		fprintf(stderr, "\n");
	}
}

static int sc_xfer_f(uint8_t cmd, uint8_t dir, uint8_t width, uint8_t outer,
		     uint32_t be32, const uint8_t *tx, uint32_t tx_len,
		     uint8_t *rx, uint32_t rx_cap, unsigned int flags,
		     unsigned int tries, unsigned int timeout_ms)
{
	size_t rx_len = 0;
	int rc = petaic_xfer_raw(&g_dev, cmd, dir, width, outer, be32,
				 tx_len ? tx : NULL, tx_len,
				 rx, rx_cap, &rx_len, tries, timeout_ms, flags);
	if (rc)
		return rc;
	return (int)rx_len;
}

static int sc_xfer(uint8_t cmd, uint8_t dir, uint8_t width, uint8_t outer,
		   uint32_t be32, const uint8_t *tx, uint32_t tx_len,
		   uint8_t *rx, uint32_t rx_cap)
{
	return sc_xfer_f(cmd, dir, width, outer, be32, tx, tx_len, rx, rx_cap,
			 0, 0, 0);
}

/* ---- handshake prelude (identical to petaic_tls) ---- */
static void sc_prelude(void)
{
	uint8_t tx[11] = { 0 };
	uint8_t rx[256];
	int r;

	if (!g_skip_prelude_20) {
		r = sc_xfer(0x20, 0x01, 0x04, 0x13, 128, tx, 7, rx, sizeof(rx));
		fprintf(stderr, "[prelude] cmd 0x20 -> rc=%d\n", r);
	}

	static const uint8_t reg01[] = { 0x01, 0x08, 0xff, 0x03, 0x03, 0x00, 0x00,
					 0x00, 0x00, 0x00, 0x00 };
	r = sc_xfer(0x01, 0x00, 0x08, 0x17, 0x08, reg01, sizeof(reg01), rx, sizeof(rx));
	fprintf(stderr, "[prelude] cmd 0x01 (init reg) -> rc=%d\n", r);

	r = sc_xfer(0x28, 0x01, 0x04, 0x13, 64, tx, 7, rx, sizeof(rx));
	fprintf(stderr, "[prelude] cmd 0x28 (key-id) -> rc=%d\n", r);
	if (r > 0) hexdump("0x28 window", rx, r < 80 ? r : 80);

	r = sc_xfer(0x22, 0x01, 0x04, 0x13, 1, tx, 7, rx, sizeof(rx));
	fprintf(stderr, "[prelude] cmd 0x22 request -> rc=%d\n", r);
}

/* ---- handshake TX/RX chunks (cmd 0x00 write / cmd 0x22 read) ---- */
static int sc_write_chunk(const uint8_t *data, size_t datalen)
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

	r = sc_xfer(SC_CMD_WRITE, SC_DIR_WRITE, (uint8_t)datalen,
		    (uint8_t)(datalen + 15), data[0],
		    tx, (uint32_t)payload_len, rx, sizeof(rx));
	if (r < 0) {
		fprintf(stderr, "[tx] xfer failed: %s\n", strerror(-r));
		return r;
	}
	return 0;
}

static int sc_read_chunk(uint8_t *out, size_t want)
{
	uint8_t rx[PETAIC_READ_CHUNK_MAX];
	uint32_t rx_cap = (uint32_t)(SC_RX_OFF + want + 16);
	int r, navail;

	if (rx_cap > sizeof(rx))
		rx_cap = sizeof(rx);

	for (;;) {
		r = sc_xfer_f(0, 0, 0, 0, 0, NULL, 0, rx, rx_cap,
			      PETAIC_XFER_READ_ONLY, 3, 350);
		if (r == -ETIMEDOUT)
			return 0;
		if (r < 0) {
			fprintf(stderr, "[rx] xfer failed: %s\n", strerror(-r));
			return r;
		}
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
 * Image phase: pull one bulk TLS record (17 03 03 ..) into g_rec.
 *   first record  (g_rec_idx==0): cmd 0x38 with a request frame, OutLength 3109
 *   continuations (g_rec_idx>0) : cmd 0x0 useage 3, READ_ONLY (no request frame)
 * Both deliver the inner header 5a 38 05 [len_lo][len_hi] 00 00 then the TLS
 * record at window offset 7; the 16-bit LE length lives at inner[3..4].
 */
static int img_pull_record(void)
{
	uint8_t rx[PETAIC_READ_CHUNK_MAX];
	size_t data_len;
	int r;

	if (g_rec_idx == 0) {
		/* be32 = OutLength low byte, region[0] = high byte (0x0c25). */
		uint8_t region[7] = { (IMG_BULK0_LEN >> 8) & 0xff, 0, 0, 0, 0, 0, 0 };
		int attempt;

		/*
		 * The first bulk read can come back as a cmd-0x37 status frame
		 * (e.g. 5a 37 05 04 .. 01 04) while the sensor is still capturing
		 * rather than a TLS record (17 03 03 ..).  Re-issue the 0x38
		 * request a few times until a real record appears.
		 */
		for (attempt = 0; attempt < IMG_BULK0_RETRIES; attempt++) {
			/*
			 * Do NOT re-arm 0x37 here: a moving finger aborts the capture
			 * and 0x38 just times out, but once records start flowing a
			 * stray 0x37 read would swallow a 0x38 record (strobing its
			 * read_done) and desync the GCM stream -> MAC failure.  A
			 * stuck capture is recovered at the top level by re-handshaking
			 * on a fresh (uncorruptible) TLS session.
			 */
			r = sc_xfer_f(PETAIT_CMD_TLS_DATA, PETAIT_DIR_OUT,
				      PETAIT_ACCESS_WIDTH_32BIT, PETAIT_OUTER_TYPE_STD,
				      IMG_BULK0_LEN & 0xff, region, sizeof(region),
				      rx, sizeof(rx), 0, 1, 400);
			if (r == -ETIMEDOUT)
				continue;
			if (r < 0)
				break;
			if (g_verbose) {
				fprintf(stderr, "[img] 0x38 attempt %d head [%d]:", attempt, r);
				for (int i = 0; i < 16 && i < r; i++)
					fprintf(stderr, " %02x", rx[i]);
				fprintf(stderr, "\n");
			}
			if (r > SC_RX_OFF && (rx[SC_RX_OFF] == 0x17 || rx[SC_RX_OFF] == 0x16))
				break;	/* got a TLS record */
			msleep(60);
		}
	} else {
		int attempt;

		/*
		 * The RX-ready GpioInt is LEVEL-triggered: after record N's read
		 * the line can still be asserted until the sensor deasserts it
		 * post-read_done, so a continuation wait_irq may fire immediately
		 * and re-read the *same* window.  Let the line settle, then reject
		 * any window whose head matches the last accepted record.
		 */
		for (attempt = 0; attempt < IMG_CONT_RETRIES; attempt++) {
			msleep(40);
			r = sc_xfer_f(0, 0, 0, 0, 0, NULL, 0, rx, sizeof(rx),
				      PETAIC_XFER_READ_ONLY, 2, 700);
			if (r == -ETIMEDOUT)
				continue;
			if (r < 0)
				break;
			if (g_verbose) {
				fprintf(stderr, "[img] cont record %d attempt %d head [%d]:",
					g_rec_idx, attempt, r);
				for (int i = 0; i < 16 && i < r; i++)
					fprintf(stderr, " %02x", rx[i]);
				fprintf(stderr, "\n");
			}
			if (r >= 16 && memcmp(rx, g_last_head, 16) == 0) {
				if (g_verbose)
					fprintf(stderr, "[img] stale duplicate, re-waiting\n");
				continue;	/* same window as last record */
			}
			break;	/* fresh record */
		}
	}
	if (r < 0) {
		fprintf(stderr, "[img] record %d xfer failed: %s\n",
			g_rec_idx, strerror(-r));
		return r;
	}
	if (r < SC_RX_OFF || rx[0] != 0x5a) {
		fprintf(stderr, "[img] record %d: bad window header\n", g_rec_idx);
		return -EPROTO;
	}

	data_len = (size_t)rx[3] | ((size_t)rx[4] << 8);
	if (data_len == 0 || SC_RX_OFF + data_len > (size_t)r ||
	    data_len > sizeof(g_rec)) {
		fprintf(stderr, "[img] record %d: bad len %zu (window %d)\n",
			g_rec_idx, data_len, r);
		hexdump("img window", rx, 16);
		return -EPROTO;
	}

	memcpy(g_rec, rx + SC_RX_OFF, data_len);
	memcpy(g_last_head, rx, 16);	/* remember for continuation dedup */
	g_rec_len = data_len;
	g_rec_off = 0;
	g_rec_idx++;
	if (g_verbose)
		fprintf(stderr, "[img] record %d: %zu wire bytes (tls %02x %02x %02x len %u)\n",
			g_rec_idx - 1, data_len, g_rec[0], g_rec[1], g_rec[2],
			(unsigned)((g_rec[3] << 8) | g_rec[4]));
	return (int)data_len;
}

/* ---- mbedTLS BIOs ---- */
static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	const uint8_t *p = buf;
	size_t left = len;
	(void)ctx;

	while (left > 0) {
		size_t rlen, body;

		if (left < 5) {
			if (sc_write_chunk(p, left) < 0)
				return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
			break;
		}
		rlen = ((size_t)p[3] << 8) | p[4];
		if (sc_write_chunk(p, 5) < 0)
			return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
		p += 5; left -= 5;
		body = (rlen <= left) ? rlen : left;
		while (body > 0) {
			size_t c = body > 255 ? 255 : body;
			if (sc_write_chunk(p, c) < 0)
				return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
			p += c; left -= c; body -= c;
		}
	}
	return (int)len;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	(void)ctx;

	if (g_phase == PH_IMAGE) {
		if (g_rec_off >= g_rec_len) {
			int got = img_pull_record();
			if (got == -ETIMEDOUT)
				return MBEDTLS_ERR_SSL_WANT_READ;
			if (got <= 0)
				return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
		}
		size_t n = g_rec_len - g_rec_off;
		if (n > len) n = len;
		memcpy(buf, g_rec + g_rec_off, n);
		g_rec_off += n;
		return (int)n;
	}

	/* handshake phase: stream from cmd 0x22 chunked reads */
	static uint8_t leftover[512];
	static size_t leftover_len, leftover_off;
	int got, tries;

	if (leftover_len > leftover_off) {
		size_t n = leftover_len - leftover_off;
		if (n > len) n = len;
		memcpy(buf, leftover + leftover_off, n);
		leftover_off += n;
		return (int)n;
	}
	leftover_len = leftover_off = 0;

	for (tries = 0; tries < 12; tries++) {
		got = sc_read_chunk(leftover, sizeof(leftover));
		if (got < 0)
			return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
		if (got > 0) {
			size_t n = (size_t)got;
			leftover_len = n;
			if (n > len) n = len;
			memcpy(buf, leftover, n);
			leftover_off = n;
			return (int)n;
		}
	}
	return MBEDTLS_ERR_SSL_WANT_READ;
}

static int psk_cb(void *p, mbedtls_ssl_context *ssl,
		  const unsigned char *id, size_t id_len)
{
	int idx = g_forced_key >= 0 ? g_forced_key : 0;
	(void)p; (void)id;

	fprintf(stderr, "[psk] sensor identity (%zu bytes), installing key '%s'\n",
		id_len, g_psks[idx].name);
	return mbedtls_ssl_set_hs_psk(ssl, g_psks[idx].key, sizeof(g_psks[idx].key));
}

static void mbed_dbg(void *ctx, int level, const char *file, int line, const char *str)
{
	(void)ctx; (void)file; (void)line;
	if (level <= 2)
		fprintf(stderr, "  mbedtls[%d]: %s", level, str);
}

/*
 * cmd 0x37 RequestEncryptImageCmd -> arms a capture.  useage-1 ack: the sensor
 * answers immediately in the same window (no IRQ), returning 5a 37 06 01 ..  aa.
 */
static int sc_request_image(void)
{
	uint8_t region[7] = { IMG_CTRL_PARAM, 0, 0, 0, 0, 0, 0 };
	uint8_t rx[64];
	int r = sc_xfer_f(PETAIT_CMD_TLS_CTRL, PETAIT_DIR_IN,
			  PETAIT_ACCESS_WIDTH_32BIT, PETAIT_OUTER_TYPE_STD,
			  0, region, sizeof(region), rx, sizeof(rx),
			  PETAIC_XFER_NO_IRQ, 1, 10);
	if (r < 0) {
		fprintf(stderr, "[cap] cmd 0x37 failed: %s\n", strerror(-r));
		return r;
	}
	fprintf(stderr, "[cap] cmd 0x37 ack: %02x %02x %02x ... data0=0x%02x\n",
		rx[0], rx[1], rx[2], r > SC_RX_OFF ? rx[SC_RX_OFF] : 0);
	return 0;
}

/*
 * Poll cmd 0x11 (finger-detect) until a finger is present.  Standard frame
 * (dir 0x01, width 0x04, OutLength 1); the 1-byte status is 0x01 = finger down,
 * 0x00 = no finger (PETAIC_PROTOCOL.md §3, trace `read_data OutputBuffer: 01`).
 * Returns 1 on finger-down, 0 on timeout.  This replaces the level-IRQ
 * wait_for_finger_down, which fires spuriously on a residual assertion.
 */
static int poll_finger(unsigned timeout_ms)
{
	unsigned waited = 0;
	int last = -1;

	while (waited < timeout_ms) {
		uint8_t st = 0xff;
		size_t got = 0;
		int rc = petaic_xfer(&g_dev, PETAIT_CMD_POLL, PETAIT_DIR_OUT,
				     PETAIT_ACCESS_WIDTH_32BIT, NULL, 0,
				     &st, 1, &got, 1, 2, 10);
		if (rc == 0 && got == 1) {
			if (st != last) {
				fprintf(stderr, "[finger] cmd 0x11 status = 0x%02x\n", st);
				last = st;
			}
			if (st == 0x01)
				return 1;
		} else if (g_verbose) {
			fprintf(stderr, "[finger] 0x11 poll rc=%d got=%zu\n", rc, (size_t)got);
		}
		msleep(40);
		waited += 40;
	}
	return 0;
}

static int write_pgm(const char *path, const uint8_t *img)
{
	FILE *f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return -1;
	}
	fprintf(f, "P5\n%d %d\n255\n", IMG_W, IMG_H);
	fwrite(img, 1, IMG_SIZE, f);
	fclose(f);
	return 0;
}

/* Pull one 5120-byte image through the live TLS session, write it out. */
static int capture_one(mbedtls_ssl_context *ssl, const char *out_path)
{
	uint8_t plain[IMG_TOTAL];
	size_t off = 0;
	int ret;

	g_phase = PH_IMAGE;
	g_rec_len = g_rec_off = 0;
	g_rec_idx = 0;
	memset(g_last_head, 0, sizeof(g_last_head));

	if (g_background) {
		/* Flat-field reference: arm + read with NO finger on the sensor, to
		 * capture the fixed-pattern background (column FPN) for host-side
		 * subtraction (the engine's `remove_line` step). KEEP THE SENSOR
		 * CLEAR -- do not touch it during this capture. */
		fprintf(stderr, "[cap] BACKGROUND capture: keep the sensor CLEAR (no finger)...\n");
	} else {
		fprintf(stderr, "[cap] PLACE FINGER on the sensor (polling cmd 0x11, up to %u ms)...\n",
			g_finger_ms);
		if (!poll_finger(g_finger_ms)) {
			fprintf(stderr, "[cap] no finger within %u ms; aborting capture\n",
				g_finger_ms);
			return -ETIMEDOUT;
		}
		fprintf(stderr, "[cap] finger present; HOLD STILL... arming + pulling image\n");
	}

	/* Arm the capture only now that the finger is down, immediately before the
	 * bulk read -- 0x37 is a one-shot, so polls between it and 0x38 stale it. */
	if ((ret = sc_request_image()) != 0)
		return ret;
	msleep(120);	/* let the sensor finish capturing before the first bulk read */

	while (off < IMG_TOTAL) {
		ret = mbedtls_ssl_read(ssl, plain + off, IMG_TOTAL - off);
		if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
		    ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
			/*
			 * img_pull_record already exhausts its own retry budget, so a
			 * WANT_READ here means the capture genuinely failed (finger
			 * moved / lifted).  Abort fast; the caller re-handshakes onto
			 * a clean session and tries again.
			 */
			fprintf(stderr, "[cap] capture failed at %zu/%d bytes (finger moved?)\n",
				off, IMG_TOTAL);
			return -ETIMEDOUT;
		}
		if (ret <= 0) {
			char eb[128];
			mbedtls_strerror(ret, eb, sizeof(eb));
			fprintf(stderr, "[cap] ssl_read failed at %zu/%d: -0x%04x %s\n",
				off, IMG_TOTAL, -ret, eb);
			return ret ? ret : -1;
		}
		off += (size_t)ret;
		if (g_verbose)
			fprintf(stderr, "[cap] ssl_read +%d (%zu/%d)\n",
				ret, off, IMG_TOTAL);
	}

	uint32_t want = petaic_checksum(plain, IMG_SIZE);
	uint32_t got = (uint32_t)plain[IMG_SIZE] | ((uint32_t)plain[IMG_SIZE+1] << 8) |
		       ((uint32_t)plain[IMG_SIZE+2] << 16) | ((uint32_t)plain[IMG_SIZE+3] << 24);
	if (want == got)
		fprintf(stderr, "[cap] image checksum OK (0x%08x)\n", got);
	else
		fprintf(stderr, "[cap] image checksum MISMATCH: got 0x%08x want 0x%08x\n",
			got, want);

	if (write_pgm(out_path, plain) == 0)
		fprintf(stderr, "[cap] wrote %dx%d image -> %s\n", IMG_W, IMG_H, out_path);
	return 0;
}

/*
 * Build the per-frame output path.  For a single frame we use out_path as-is;
 * for a multi-frame run we insert the (zero-based) frame index before the file
 * extension, e.g. "fp.pgm" -> "fp_0.pgm", "fp_1.pgm", ...
 */
static void frame_path(char *buf, size_t buflen, const char *out_path, int idx)
{
	if (g_frames <= 1) {
		snprintf(buf, buflen, "%s", out_path);
		return;
	}
	const char *dot = strrchr(out_path, '.');
	const char *slash = strrchr(out_path, '/');
	if (dot && (!slash || dot > slash))
		snprintf(buf, buflen, "%.*s_%d%s",
			 (int)(dot - out_path), out_path, idx, dot);
	else
		snprintf(buf, buflen, "%s_%d", out_path, idx);
}

/*
 * Pull g_frames images through a single live TLS session, one 0x37->0x38
 * capture per frame, without re-handshaking.  This is the enroll/verify
 * enabler (HANDOFF §5.1): Windows captures NumberOfImages frames on one
 * session.  g_frame_done persists across re-handshakes so a finger-moved
 * abort mid-sequence resumes from the next missing frame on a fresh session
 * rather than restarting the whole sequence.
 */
static int capture_session(mbedtls_ssl_context *ssl, const char *out_path)
{
	while (g_frame_done < g_frames) {
		char path[768];
		int ret;

		frame_path(path, sizeof(path), out_path, g_frame_done);
		fprintf(stderr, "=== frame %d/%d (same TLS session) ===\n",
			g_frame_done + 1, g_frames);
		ret = capture_one(ssl, path);
		if (ret != 0)
			return ret;	/* caller re-handshakes; we resume here */

		g_frame_done++;
		fprintf(stderr, "[cap] frame %d/%d complete on session\n",
			g_frame_done, g_frames);
		if (g_frame_done < g_frames) {
			/* Records are exactly consumed (5124 == 3080+2044), so the
			 * TLS stream is at a clean record boundary -- just re-arm
			 * 0x37 for the next frame.  A short settle lets the sensor
			 * deassert RX-ready before the next finger poll. */
			msleep(150);
		}
	}
	return 0;
}

static int run(int key_idx, const char *out_path)
{
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
	static const int ciphers[] = {
		MBEDTLS_TLS_PSK_WITH_AES_256_GCM_SHA384,
		MBEDTLS_TLS_PSK_WITH_AES_128_GCM_SHA256,
		0
	};
	const char *pers = "petaic_capture";
	int ret;

	g_forced_key = key_idx;
	g_phase = PH_HANDSHAKE;

	mbedtls_ssl_init(&ssl);
	mbedtls_ssl_config_init(&conf);
	mbedtls_entropy_init(&entropy);
	mbedtls_ctr_drbg_init(&drbg);

	if ((ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
					 (const unsigned char *)pers, strlen(pers))) != 0)
		goto done;
	if ((ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER,
					       MBEDTLS_SSL_TRANSPORT_STREAM,
					       MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
		goto done;

	mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
	mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
	mbedtls_ssl_conf_ciphersuites(&conf, ciphers);
	mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
	mbedtls_ssl_conf_psk_cb(&conf, psk_cb, NULL);
	mbedtls_ssl_conf_dbg(&conf, mbed_dbg, NULL);
	if (g_verbose)
		mbedtls_debug_set_threshold(2);

	if ((ret = mbedtls_ssl_setup(&ssl, &conf)) != 0)
		goto done;
	mbedtls_ssl_set_bio(&ssl, NULL, bio_send, bio_recv, NULL);

	fprintf(stderr, "=== handshake with key '%s' ===\n", g_psks[key_idx].name);
	sc_prelude();
	while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
		if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
		    ret != MBEDTLS_ERR_SSL_WANT_WRITE)
			break;
	}
	if (ret != 0) {
		char eb[128];
		mbedtls_strerror(ret, eb, sizeof(eb));
		fprintf(stderr, "handshake failed: -0x%04x %s\n", -ret, eb);
		goto done;
	}
	fprintf(stderr, "*** HANDSHAKE OK (cipher=%s) ***\n",
		mbedtls_ssl_get_ciphersuite(&ssl));

	ret = capture_session(&ssl, out_path);

done:
	mbedtls_ssl_free(&ssl);
	mbedtls_ssl_config_free(&conf);
	mbedtls_ctr_drbg_free(&drbg);
	mbedtls_entropy_free(&entropy);
	return ret;
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/sil6250";
	const char *out = "fp_image.pgm";
	int key_sel = -1;
	int rc;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dev") && i + 1 < argc) dev = argv[++i];
		else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
		else if (!strcmp(argv[i], "-v")) g_verbose = 1;
		else if (!strcmp(argv[i], "--no-prelude-20")) g_skip_prelude_20 = 1;
		else if (!strcmp(argv[i], "--finger-ms") && i + 1 < argc)
			g_finger_ms = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
			g_frames = (int)strtol(argv[++i], NULL, 0);
			if (g_frames < 1) g_frames = 1;
		}
		else if (!strcmp(argv[i], "--background")) g_background = 1;
		else if (!strcmp(argv[i], "--key") && i + 1 < argc) {
			const char *k = argv[++i];
			if (!strcmp(k, "all")) key_sel = -1;
			else {
				key_sel = -2;
				for (int j = 0; j < N_PSK; j++)
					if (!strcmp(k, g_psks[j].name)) key_sel = j;
				if (key_sel == -2) {
					fprintf(stderr, "unknown key '%s'\n", k);
					return 2;
				}
			}
		} else {
			fprintf(stderr, "usage: %s [--dev /dev/sil6250] [--key shiba|all] "
				"[--out img.pgm] [--frames N] [--finger-ms 15000] "
				"[--background] [--no-prelude-20] [-v]\n",
				argv[0]);
			return 2;
		}
	}

	/* Background (no-finger) capture has nothing to poll, so default it to the
	 * known-good key rather than cycling all four. */
	if (g_background && key_sel < 0)
		key_sel = 0;	/* shiba */

	rc = petaic_open(&g_dev, dev);
	if (rc) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(-rc));
		return 1;
	}
	g_dev.verbose = g_verbose;

	if (key_sel >= 0) {
		unsigned first_finger = g_finger_ms;
		/* Each frame may cost a re-handshake if the finger moves, so scale
		 * the attempt budget with the number of frames requested. */
		int attempts = CAPTURE_ATTEMPTS * g_frames;

		/* Retry on a fresh handshake: a moving finger aborts the capture
		 * and a desynced stream kills the TLS session, but a new handshake
		 * is always clean.  g_frame_done persists, so a re-handshake resumes
		 * from the next missing frame rather than restarting the sequence.
		 * Wait long for the finger on the first try, briefly on retries. */
		for (int a = 0; a < attempts; a++) {
			g_finger_ms = a == 0 ? first_finger : 3000;
			rc = run(key_sel, out);
			if (rc == 0) {
				petaic_close(&g_dev);
				return 0;
			}
			fprintf(stderr, "[cap] attempt %d/%d failed at frame %d/%d; "
				"re-handshaking (hold your finger still)...\n",
				a + 1, attempts, g_frame_done + 1, g_frames);
			msleep(150);
		}
		petaic_close(&g_dev);
		return 1;
	}

	for (int j = 0; j < N_PSK; j++) {
		fprintf(stderr, "--- trying key '%s' ---\n", g_psks[j].name);
		if (run(j, out) == 0) {
			petaic_close(&g_dev);
			return 0;
		}
	}
	petaic_close(&g_dev);
	return 1;
}
