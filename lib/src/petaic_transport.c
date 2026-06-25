// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace Petaic mailbox transport over /dev/sil6250.
 * Port of gxfp_ref/transport/gxfp_petaic.c + hw/gxfp_gpio.c strobe timings.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "sil6250_uapi.h"
#include "petaic_proto.h"
#include "petaic_transport.h"

/* notify_ec strobe timings from the Windows trace (gxfp_gpio.c). */
#define NOTIFY_HIGH_MS		10
#define WRITE_POST_MS		0
#define READ_POST_MS		20
#define WAIT_IRQ_DEFAULT_MS	500

static void msleep(unsigned int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	if (ms)
		nanosleep(&ts, NULL);
}

static int set_gpio(struct petaic_dev *d, uint32_t line, uint32_t value)
{
	struct sil6250_gpio g = { .line = line, .value = value };

	if (ioctl(d->fd, SIL6250_SET_GPIO, &g) < 0)
		return -errno;
	return 0;
}

int petaic_open(struct petaic_dev *d, const char *path)
{
	uint32_t sz = 0;

	memset(d, 0, sizeof(*d));
	d->fd = open(path, O_RDWR | O_CLOEXEC);
	if (d->fd < 0)
		return -errno;

	if (ioctl(d->fd, SIL6250_GET_WINDOW_SIZE, &sz) < 0 || sz == 0) {
		close(d->fd);
		d->fd = -1;
		return -errno ? -errno : -EINVAL;
	}
	d->window_size = sz;
	d->window = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd, 0);
	if (d->window == MAP_FAILED) {
		int e = -errno;

		close(d->fd);
		d->fd = -1;
		d->window = NULL;
		return e;
	}
	return 0;
}

void petaic_close(struct petaic_dev *d)
{
	if (d->window)
		munmap((void *)d->window, d->window_size);
	if (d->fd >= 0)
		close(d->fd);
	d->window = NULL;
	d->fd = -1;
}

int petaic_strobe_write_done(struct petaic_dev *d)
{
	int rc;

	if (d->verbose)
		fprintf(stderr, "[strobe write_done]\n");
	rc = set_gpio(d, SIL6250_LINE_WRITE_DONE, 1);
	if (rc)
		return rc;
	msleep(NOTIFY_HIGH_MS);
	rc = set_gpio(d, SIL6250_LINE_WRITE_DONE, 0);
	if (rc)
		return rc;
	msleep(WRITE_POST_MS);
	return 0;
}

int petaic_strobe_read_done(struct petaic_dev *d)
{
	int rc;

	if (d->verbose)
		fprintf(stderr, "[strobe read_done]\n");
	rc = set_gpio(d, SIL6250_LINE_READ_DONE, 1);
	if (rc)
		return rc;
	msleep(NOTIFY_HIGH_MS);
	rc = set_gpio(d, SIL6250_LINE_READ_DONE, 0);
	if (rc)
		return rc;
	msleep(READ_POST_MS);
	return 0;
}

int petaic_wait_irq(struct petaic_dev *d, unsigned int timeout_ms)
{
	struct sil6250_wait_irq w = {
		.timeout_ms = timeout_ms ? timeout_ms : WAIT_IRQ_DEFAULT_MS,
	};

	if (ioctl(d->fd, SIL6250_WAIT_IRQ, &w) < 0)
		return -errno;	/* -ETIMEDOUT on no IRQ */
	return 0;
}

/* qword-aligned copy into the TX region (+0x000). */
int petaic_shm_write(struct petaic_dev *d, const uint8_t *buf, size_t len)
{
	volatile uint64_t *dst = (volatile uint64_t *)d->window;
	size_t i;

	if (len % 8 || len > d->window_size)
		return -EINVAL;
	for (i = 0; i < len / 8; i++) {
		uint64_t v;

		memcpy(&v, buf + i * 8, 8);
		dst[i] = v;
	}
	__sync_synchronize();
	return 0;
}

/* qword-aligned copy from the RX region (+0x200). */
int petaic_shm_read(struct petaic_dev *d, uint8_t *buf, size_t len)
{
	volatile uint64_t *src = (volatile uint64_t *)(d->window + PETAIC_RX_OFFSET);
	size_t i;

	if (len % 8 || PETAIC_RX_OFFSET + len > d->window_size)
		return -EINVAL;
	for (i = 0; i < len / 8; i++) {
		uint64_t v = src[i];

		memcpy(buf + i * 8, &v, 8);
	}
	__sync_synchronize();
	return 0;
}

static int dead_window(const uint8_t *rx, size_t len)
{
	size_t i;

	if (len < 8)
		return 0;
	for (i = 0; i < 8; i++)
		if (rx[i] != 0xff)
			return 0;
	return 1;
}

static void dump(const char *tag, const uint8_t *p, size_t n)
{
	size_t i;

	fprintf(stderr, "%s [%zu]:", tag, n);
	for (i = 0; i < n && i < 32; i++)
		fprintf(stderr, " %02x", p[i]);
	fprintf(stderr, "\n");
}

int petaic_xfer(struct petaic_dev *d, uint8_t cmd, uint8_t dir, uint8_t width,
		const uint8_t *tx_payload, size_t tx_len,
		uint8_t *rx_buf, size_t rx_cap, size_t *out_rx_len,
		size_t expected_rx_len, unsigned int tries, unsigned int timeout_ms)
{
	uint8_t tx[PETAIC_TX_BUF_SIZE];
	uint8_t rx[PETAIC_READ_CHUNK_MAX];
	uint8_t seq;
	int frame_len;
	unsigned int t;
	struct petaic_frame frame;

	/* rx_buf only needs to hold expected_rx_len; the window read uses a
	 * separate PETAIC_READ_CHUNK_MAX scratch buffer below. */
	if (!rx_buf || rx_cap == 0)
		return -EINVAL;
	if (tx_len && !tx_payload)
		return -EINVAL;
	if (expected_rx_len > rx_cap)
		return -EINVAL;

	seq = (uint8_t)(++d->seq & 0xFF);
	frame_len = petaic_build_frame(cmd, dir, width, tx_payload, tx_len,
				       expected_rx_len, seq, PETAIT_OUTER_TYPE_STD,
				       tx, sizeof(tx));
	if (frame_len < 0)
		return frame_len;
	if (d->verbose)
		dump("TX", tx, (size_t)frame_len);

	for (t = 0; t < (tries ? tries : 1); t++) {
		int rc = petaic_shm_write(d, tx, (size_t)frame_len);

		if (rc)
			return rc;
		rc = petaic_strobe_write_done(d);
		if (rc)
			return rc;

		/* Standard bring-up path used a fixed settle delay, not the IRQ. */
		msleep(timeout_ms ? timeout_ms : 10);

		rc = petaic_shm_read(d, rx, PETAIC_READ_CHUNK_MAX);
		if (rc)
			return rc;
		if (d->verbose)
			dump("RX", rx, 48);
		if (dead_window(rx, PETAIC_READ_CHUNK_MAX))
			continue;

		/* Win-style success: OutputBuffer at offset 7, own checksum. */
		if (rx[0] == 0x5A && rx[1] == cmd && rx[2] == 0x04 &&
		    expected_rx_len && 7 + expected_rx_len <= PETAIC_READ_CHUNK_MAX) {
			petaic_strobe_read_done(d);
			memcpy(rx_buf, rx + 7, expected_rx_len);
			if (out_rx_len)
				*out_rx_len = expected_rx_len;
			return 0;
		}

		if (!petaic_parse_frame(rx, PETAIC_READ_CHUNK_MAX, &frame))
			continue;
		if (frame.cmd != cmd || !frame.checksum_ok)
			continue;

		petaic_strobe_read_done(d);
		if (frame.payload_len > rx_cap)
			return -EMSGSIZE;
		if (frame.payload_len)
			memcpy(rx_buf, frame.payload, frame.payload_len);
		if (out_rx_len)
			*out_rx_len = frame.payload_len;
		return 0;
	}

	return -ETIMEDOUT;
}

int petaic_xfer_raw(struct petaic_dev *d, uint8_t cmd, uint8_t dir, uint8_t width,
		    uint8_t outer_type, uint32_t be32,
		    const uint8_t *tx_payload, size_t tx_len,
		    uint8_t *rx_buf, size_t rx_cap, size_t *out_rx_len,
		    unsigned int tries, unsigned int timeout_ms, unsigned int flags)
{
	int read_only = flags & PETAIC_XFER_READ_ONLY;
	uint8_t tx[PETAIC_TX_BUF_SIZE];
	uint8_t rx[PETAIC_READ_CHUNK_MAX];
	uint8_t seq;
	int frame_len = 0;
	size_t read_len;
	unsigned int t;

	if (!rx_buf || rx_cap == 0)
		return -EINVAL;
	if (tx_len && !tx_payload)
		return -EINVAL;

	read_len = rx_cap < PETAIC_READ_CHUNK_MAX ? rx_cap : PETAIC_READ_CHUNK_MAX;
	read_len &= ~(size_t)7;
	if (read_len == 0)
		return -EINVAL;

	seq = (uint8_t)(++d->seq & 0xFF);

	if (!read_only) {
		frame_len = petaic_build_frame_ex(cmd, dir, width, tx_payload, tx_len,
						  be32, 0, seq, outer_type,
						  tx, sizeof(tx));
		if (frame_len < 0)
			return frame_len;
		if (d->verbose)
			dump("TX-RAW", tx, (size_t)frame_len);
	}

	for (t = 0; t < (tries ? tries : 1); t++) {
		int rc;

		if (read_only) {
			petaic_wait_irq(d, timeout_ms);
			rc = petaic_shm_read(d, rx, read_len);
			if (rc)
				return rc;
			if (dead_window(rx, read_len))
				continue;
			if (d->verbose)
				dump("RX-RAW(ro)", rx, read_len);
			petaic_strobe_read_done(d);
			memcpy(rx_buf, rx, read_len);
			if (out_rx_len)
				*out_rx_len = read_len;
			return 0;
		}

		rc = petaic_shm_write(d, tx, (size_t)frame_len);
		if (rc)
			return rc;
		rc = petaic_strobe_write_done(d);
		if (rc)
			return rc;

		/*
		 * Primary response: wait for the EC's RX-ready IRQ. useage-1
		 * ack commands (cmd 0x37) answer immediately in the same window
		 * with no IRQ, so just settle instead of waiting (PETAIC_XFER_NO_IRQ).
		 */
		if (flags & PETAIC_XFER_NO_IRQ)
			msleep(timeout_ms ? timeout_ms : 10);
		else if (petaic_wait_irq(d, timeout_ms ? timeout_ms : WAIT_IRQ_DEFAULT_MS))
			continue;

		rc = petaic_shm_read(d, rx, read_len);
		if (rc)
			return rc;
		if (dead_window(rx, read_len))
			continue;
		if (d->verbose)
			dump("RX-RAW", rx, read_len);

		petaic_strobe_read_done(d);
		memcpy(rx_buf, rx, read_len);
		if (out_rx_len)
			*out_rx_len = read_len;
		return 0;
	}

	return -ETIMEDOUT;
}
