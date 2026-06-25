// SPDX-License-Identifier: GPL-2.0
/*
 * petaic_record - drive one raw Petaic mailbox record over /dev/sil6250.
 *
 * Userspace successor to gxfp_ref/tools/petaic_record.c: instead of the kernel
 * GXFP_IOCTL_PETAIC_RECORD ioctl (which built the frame in-kernel), the framing
 * and the whole strobe/wait/read transaction now run here via petaic_transport.
 *
 * Build:  make            (in petaic_ref/)
 * Usage:  sudo ./petaic_record --cmd 0x37 --dir 1 --width 4 --outer 0x13 \
 *                 --be32 0 --tx 0c00000000000000 --rxcap 64
 *         sudo ./petaic_record --read-only --rxcap 64     # pull a continuation
 *         sudo ./petaic_record --std --cmd 0x1b --rxcap 4  # standard bring-up
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "petaic_proto.h"
#include "petaic_transport.h"

static int parse_hex(const char *s, uint8_t *out, size_t cap, size_t *out_len)
{
	size_t n = 0;

	while (*s) {
		int hi, lo;

		while (*s == ' ' || *s == ':')
			s++;
		if (!*s)
			break;
		if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1]))
			return -1;
		hi = (s[0] <= '9') ? s[0] - '0' : (tolower(s[0]) - 'a' + 10);
		lo = (s[1] <= '9') ? s[1] - '0' : (tolower(s[1]) - 'a' + 10);
		if (n >= cap)
			return -1;
		out[n++] = (uint8_t)((hi << 4) | lo);
		s += 2;
	}
	*out_len = n;
	return 0;
}

static void hexdump(const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i += 16) {
		printf("  %04zx  ", i);
		for (size_t j = 0; j < 16; j++) {
			if (i + j < len)
				printf("%02x ", buf[i + j]);
			else
				printf("   ");
		}
		printf(" |");
		for (size_t j = 0; j < 16 && i + j < len; j++) {
			uint8_t c = buf[i + j];

			putchar((c >= 0x20 && c < 0x7f) ? c : '.');
		}
		printf("|\n");
	}
}

static void usage(const char *p)
{
	fprintf(stderr,
		"usage: %s [--dev /dev/sil6250] [--std] [--read-only] --cmd 0xNN\n"
		"          [--dir N] [--width N] [--outer 0xNN] [--be32 N]\n"
		"          [--tx HEX] [--rxcap N] [--tries N] [--timeout MS] [-v]\n"
		"\n"
		"  --std        use the standard-command path (offset-7 + checksum)\n"
		"  --read-only  pull a sensor->host continuation (no TX frame)\n", p);
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/sil6250";
	struct petaic_dev d;
	uint8_t tx[PETAIC_TX_BUF_SIZE];
	uint8_t *rx;
	size_t tx_len = 0, rx_len = 0;
	long rx_cap = 64;
	unsigned int cmd = 0, dir = 1, width = 4, outer = 0x13;
	unsigned int be32 = 0, tries = 0, timeout = 0;
	int std = 0, read_only = 0, have_cmd = 0, verbose = 0;
	int rc;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;

#define NEEDVAL() do { if (!v) { usage(argv[0]); return 2; } i++; } while (0)
		if (!strcmp(a, "--dev")) { NEEDVAL(); dev = v; }
		else if (!strcmp(a, "--std")) { std = 1; }
		else if (!strcmp(a, "--read-only")) { read_only = 1; }
		else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) { verbose = 1; }
		else if (!strcmp(a, "--cmd")) { NEEDVAL(); cmd = strtoul(v, NULL, 0); have_cmd = 1; }
		else if (!strcmp(a, "--dir")) { NEEDVAL(); dir = strtoul(v, NULL, 0); }
		else if (!strcmp(a, "--width")) { NEEDVAL(); width = strtoul(v, NULL, 0); }
		else if (!strcmp(a, "--outer")) { NEEDVAL(); outer = strtoul(v, NULL, 0); }
		else if (!strcmp(a, "--be32")) { NEEDVAL(); be32 = strtoul(v, NULL, 0); }
		else if (!strcmp(a, "--tries")) { NEEDVAL(); tries = strtoul(v, NULL, 0); }
		else if (!strcmp(a, "--timeout")) { NEEDVAL(); timeout = strtoul(v, NULL, 0); }
		else if (!strcmp(a, "--rxcap")) { NEEDVAL(); rx_cap = strtol(v, NULL, 0); }
		else if (!strcmp(a, "--tx")) {
			NEEDVAL();
			if (parse_hex(v, tx, sizeof(tx), &tx_len)) {
				fprintf(stderr, "bad --tx hex\n");
				return 2;
			}
		} else { usage(argv[0]); return 2; }
#undef NEEDVAL
	}

	if (!have_cmd && !read_only) {
		usage(argv[0]);
		return 2;
	}
	if (rx_cap <= 0 || rx_cap > PETAIC_READ_CHUNK_MAX) {
		fprintf(stderr, "--rxcap must be 1..%u\n", PETAIC_READ_CHUNK_MAX);
		return 2;
	}

	rx = calloc(1, (size_t)rx_cap);
	if (!rx) {
		perror("calloc");
		return 1;
	}

	rc = petaic_open(&d, dev);
	if (rc) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(-rc));
		free(rx);
		return 1;
	}
	d.verbose = verbose;

	printf("TX cmd=0x%02x dir=%u width=%u outer=0x%02x be32=%u tx_len=%zu rx_cap=%ld%s%s\n",
	       cmd, dir, width, outer, be32, tx_len, rx_cap,
	       std ? " [std]" : "", read_only ? " [read-only]" : "");
	if (tx_len)
		hexdump(tx, tx_len);

	if (std)
		rc = petaic_xfer(&d, cmd, dir, width, tx_len ? tx : NULL, tx_len,
				 rx, rx_cap, &rx_len, rx_cap, tries, timeout);
	else
		rc = petaic_xfer_raw(&d, cmd, dir, width, outer, be32,
				     tx_len ? tx : NULL, tx_len,
				     rx, rx_cap, &rx_len, tries, timeout,
				     read_only ? PETAIC_XFER_READ_ONLY : 0);

	if (rc) {
		fprintf(stderr, "xfer failed: %s\n", strerror(-rc));
		petaic_close(&d);
		free(rx);
		return 1;
	}

	printf("RX rx_len=%zu\n", rx_len);
	hexdump(rx, rx_len);

	petaic_close(&d);
	free(rx);
	return 0;
}
