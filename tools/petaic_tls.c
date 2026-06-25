// SPDX-License-Identifier: GPL-2.0
/*
 * petaic_tls - drive a TLS-PSK handshake with the SIL6250 sensor.
 *
 * The host is the TLS *server*; the sensor MCU is the client. This harness
 * stands up an mbedTLS PSK server, points its BIO at the userspace mailbox
 * transport (petaic_xfer_raw over /dev/sil6250), and runs the handshake. It
 * confirms two things only live hardware can:
 *   1. the cmd-0x00 (write) / cmd-0x22 (read) record framing, and
 *   2. which of the 4 recovered PSK keys the sensor's 32-byte identity selects.
 *
 * Port of gxfp_ref/tools/petaic_tls.c: the kernel GXFP_IOCTL_PETAIC_RECORD
 * ioctl is replaced by petaic_xfer_raw(), which now builds the frame and drives
 * the strobe/wait/read dance in userspace (see petaic_transport.c). All the
 * TLS / BIO / PSK / record-splitting logic below is unchanged.
 *
 * Framing decoded from petaic_decoded_trace.log (PETAIC_PROTOCOL.md §4a):
 *   write chunk (host->sensor): cmd 0x00, dir 0x02
 *       inner: 5a 00 02 [datalen] 00 00 00 [data0] data1.. + 4 zero pad + cksum
 *       outer[1] = inner_len = datalen + 15
 *   read chunk (sensor->host):   cmd 0x22, dir 0x04 (pulled read-only)
 *     response window carries the data at offset 7.
 *
 * Build:  make petaic_tls   (links -lmbedtls -lmbedx509 -lmbedcrypto)
 * Usage:  sudo ./petaic_tls [--dev /dev/sil6250] [--key shiba|all] [-v]
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

#include "petaic_transport.h"

/* ---- inner-header / record constants ---- */
#define SC_CMD_WRITE   0x00
#define SC_DIR_WRITE   0x02
#define SC_CMD_READ    0x22
#define SC_DIR_READ    0x04
#define SC_RX_OFF      7      /* response data starts at window offset 7 */
#define SC_TX_MAX      480
#define SC_RX_MAX      PETAIC_READ_CHUNK_MAX

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

static struct petaic_dev g_dev;
static int g_verbose = 0;
static int g_forced_key = -1;
static int g_skip_prelude_20 = 0;

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

static void msleep(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

/*
 * One raw record round-trip. Returns rx_len (>=0) or -errno. Wraps
 * petaic_xfer_raw, which builds the 0x5A frame, writes the window, strobes
 * write_done, waits for the RX-ready IRQ, reads the raw window and strobes
 * read_done -- the whole transaction the kernel used to do.
 */
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

/*
 * Handshake prelude (PETAIC_PROTOCOL.md §5): right before
 * TLS_StartExchangeKeyThread the Windows driver issues cmd 0x20 (read 128 B)
 * then cmd 0x28 (read the 64-byte key-id blob). cmd 0x28 kicks the sensor into
 * key-exchange mode; after that we send a cmd-0x22 request and sc_read_chunk()
 * pulls the ClientHello stream.
 */
static void sc_prelude(void)
{
	uint8_t tx[11] = { 0 };
	uint8_t rx[256];
	int r;

	if (!g_skip_prelude_20) {
		r = sc_xfer(0x20, 0x01, 0x04, 0x13, 128, tx, 7, rx, sizeof(rx));
		fprintf(stderr, "[prelude] cmd 0x20 -> rc=%d\n", r);
		if (r > 0) hexdump("0x20 window", rx, r < 48 ? r : 48);
	}

	static const uint8_t reg01[] = { 0x01, 0x08, 0xff, 0x03, 0x03, 0x00, 0x00,
					 0x00, 0x00, 0x00, 0x00 };
	r = sc_xfer(0x01, 0x00, 0x08, 0x17, 0x08, reg01, sizeof(reg01), rx, sizeof(rx));
	fprintf(stderr, "[prelude] cmd 0x01 (init reg) -> rc=%d\n", r);
	if (r > 0) hexdump("0x01 window", rx, r < 64 ? r : 64);

	r = sc_xfer(0x28, 0x01, 0x04, 0x13, 64, tx, 7, rx, sizeof(rx));
	fprintf(stderr, "[prelude] cmd 0x28 (key-id) -> rc=%d\n", r);
	if (r > 0) hexdump("0x28 window", rx, r < 80 ? r : 80);

	r = sc_xfer(0x22, 0x01, 0x04, 0x13, 1, tx, 7, rx, sizeof(rx));
	fprintf(stderr, "[prelude] cmd 0x22 request -> rc=%d\n", r);
	if (r > 0) hexdump("0x22 window", rx, r < 80 ? r : 80);
}

/*
 * Send one secure-channel data chunk (host->sensor) via cmd 0x00.
 *   inner[3]=datalen, data[0] at offset 7 (be32 low byte), data[1..] as payload,
 *   then a 4-byte zero pad; outer_type = datalen + 15.
 */
static int sc_write_chunk(const uint8_t *data, size_t datalen)
{
	uint8_t tx[SC_TX_MAX];
	uint8_t rx[64];
	size_t payload_len;
	int r;

	if (datalen == 0 || datalen > 255)
		return -1;
	payload_len = (datalen - 1) + 4;	/* data[1..] + fixed 4-byte zero pad */
	if (payload_len > sizeof(tx))
		return -1;
	if (datalen > 1)
		memcpy(tx, data + 1, datalen - 1);
	memset(tx + (datalen - 1), 0, 4);

	if (g_verbose)
		fprintf(stderr, "[tx] cmd=0x00 datalen=%zu data0=0x%02x\n", datalen, data[0]);
	hexdump("tx data", data, datalen);

	r = sc_xfer(SC_CMD_WRITE, SC_DIR_WRITE, (uint8_t)datalen,
		    (uint8_t)(datalen + 15), data[0],
		    tx, (uint32_t)payload_len, rx, sizeof(rx));
	if (r < 0) {
		fprintf(stderr, "[tx] xfer failed: %s\n", strerror(-r));
		return r;
	}
	hexdump("tx ack window", rx, (size_t)r);
	return 0;
}

/*
 * Read one sensor->host continuation packet (read-only). The transport waits
 * for the EC's RX-ready IRQ, reads the raw window, then strobes read_done to
 * request the next packet. Data is taken from window offset 7 (inner[3] reports
 * how many bytes the sensor delivered). A leading 1-byte 0x5A marker
 * continuation is skipped transparently.
 */
static int sc_read_chunk(uint8_t *out, size_t want)
{
	uint8_t rx[SC_RX_MAX];
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
		hexdump("rx window", rx, (size_t)r);

		if (r < SC_RX_OFF || rx[0] == 0xff || rx[0] != 0x5a)
			return 0;

		navail = rx[3];			/* inner datalen */
		if (navail <= 0)
			return 0;

		if (navail == 1 && rx[SC_RX_OFF] == 0x5a) {
			if (g_verbose)
				fprintf(stderr, "[rx] skipping 0x5A marker continuation\n");
			continue;
		}

		if ((size_t)navail > want)
			navail = (int)want;
		if (SC_RX_OFF + navail > r)
			navail = r - SC_RX_OFF;
		memcpy(out, rx + SC_RX_OFF, (size_t)navail);
		if (g_verbose)
			fprintf(stderr, "[rx] cmd=0x22 want=%zu got=%d\n", want, navail);
		return navail;
	}
}

/* ---- mbedTLS BIO callbacks ---- */
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
			p += left; left = 0;
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
	static uint8_t leftover[512];
	static size_t leftover_len = 0, leftover_off = 0;
	int got, tries;
	(void)ctx;

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

/* PSK identity callback: log the 32-byte identity and install a key. */
static int psk_cb(void *p, mbedtls_ssl_context *ssl,
		  const unsigned char *id, size_t id_len)
{
	int idx = g_forced_key >= 0 ? g_forced_key : 0;
	(void)p;

	fprintf(stderr, "[psk] sensor identity (%zu bytes):\n  ", id_len);
	for (size_t i = 0; i < id_len; i++)
		fprintf(stderr, "%02x", id[i]);
	fprintf(stderr, "\n[psk] installing key '%s'\n", g_psks[idx].name);

	return mbedtls_ssl_set_hs_psk(ssl, g_psks[idx].key, sizeof(g_psks[idx].key));
}

static void mbed_dbg(void *ctx, int level, const char *file, int line, const char *str)
{
	(void)ctx; (void)file; (void)line;
	if (level <= 2)
		fprintf(stderr, "  mbedtls[%d]: %s", level, str);
}

static int run_handshake(int key_idx)
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
	const char *pers = "petaic_tls";
	int ret;

	g_forced_key = key_idx;

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
	sc_prelude(); /* trigger the sensor into key-exchange mode */
	while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
		if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
		    ret != MBEDTLS_ERR_SSL_WANT_WRITE)
			break;
	}

	if (ret == 0)
		fprintf(stderr, "*** HANDSHAKE OK (cipher=%s) ***\n",
			mbedtls_ssl_get_ciphersuite(&ssl));
	else {
		char eb[128];
		mbedtls_strerror(ret, eb, sizeof(eb));
		fprintf(stderr, "handshake failed: -0x%04x %s\n", -ret, eb);
	}

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
	int key_sel = -1; /* -1 = try all 4 */
	int rc;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dev") && i + 1 < argc) dev = argv[++i];
		else if (!strcmp(argv[i], "-v")) g_verbose = 1;
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
		} else if (!strcmp(argv[i], "--no-prelude-20")) {
			g_skip_prelude_20 = 1;
		} else {
			fprintf(stderr, "usage: %s [--dev /dev/sil6250] [--key shiba|all] [--no-prelude-20] [-v]\n", argv[0]);
			return 2;
		}
	}

	rc = petaic_open(&g_dev, dev);
	if (rc) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(-rc));
		return 1;
	}
	g_dev.verbose = g_verbose;

	if (key_sel >= 0) {
		int r = run_handshake(key_sel);
		petaic_close(&g_dev);
		return r == 0 ? 0 : 1;
	}

	for (int j = 0; j < N_PSK; j++) {
		if (run_handshake(j) == 0) {
			fprintf(stderr, "\n=> sensor uses PSK key '%s'\n", g_psks[j].name);
			petaic_close(&g_dev);
			return 0;
		}
		fprintf(stderr, "--- key '%s' did not complete; trying next ---\n\n",
			g_psks[j].name);
		msleep(200);
	}
	fprintf(stderr, "no key completed the handshake\n");
	petaic_close(&g_dev);
	return 1;
}
