/* SPDX-License-Identifier: GPL-2.0 */
#ifndef PETAIC_TRANSPORT_H
#define PETAIC_TRANSPORT_H

/*
 * Userspace Petaic mailbox transport over /dev/sil6250.
 *
 * This is the half that moved out of the kernel: it owns the whole
 *   shm_write -> strobe write_done -> wait_irq -> shm_read -> strobe read_done
 * transaction, the 0xF0/0x5A framing and the checksum. The sil6250 module
 * provides only the raw window (mmap), the two strobe GPIOs and the RX-ready
 * IRQ. Port of gxfp_ref/transport/gxfp_petaic.c.
 */

#include <stddef.h>
#include <stdint.h>

struct petaic_dev {
	int fd;
	volatile uint8_t *window;	/* mmap'd mailbox window */
	size_t window_size;
	uint16_t seq;			/* transaction sequence (per-command) */
	int verbose;			/* dump TX/RX to stderr */
};

/* Mailbox geometry (matches the firmware contract). */
#define PETAIC_RX_OFFSET	0x200	/* EC response region within the window */
#define PETAIC_READ_CHUNK_MAX	0xe00
#define PETAIC_TX_BUF_SIZE	0x200

/* petaic_xfer_raw flags. */
#define PETAIC_XFER_READ_ONLY	(1u << 0) /* skip TX frame; pull a continuation */
#define PETAIC_XFER_NO_IRQ	(1u << 1) /* useage-1 ack: read after a settle delay, no IRQ wait (cmd 0x37) */

int  petaic_open(struct petaic_dev *d, const char *path);
void petaic_close(struct petaic_dev *d);

/* Low-level primitives (exposed for probing / custom sequences). */
int  petaic_strobe_write_done(struct petaic_dev *d);
int  petaic_strobe_read_done(struct petaic_dev *d);
int  petaic_wait_irq(struct petaic_dev *d, unsigned int timeout_ms);
int  petaic_shm_write(struct petaic_dev *d, const uint8_t *buf, size_t len);
int  petaic_shm_read(struct petaic_dev *d, uint8_t *buf, size_t len);

/*
 * Standard command round-trip (cmd 0x1b/0x14/0x11 bring-up path): builds a
 * 27-byte frame, performs the transaction, extracts the win-style offset-7
 * OutputBuffer and verifies the checksum. Returns 0 and fills out_rx_len, or a
 * negative errno.
 */
int petaic_xfer(struct petaic_dev *d, uint8_t cmd, uint8_t dir, uint8_t width,
		const uint8_t *tx, size_t tx_len,
		uint8_t *rx, size_t rx_cap, size_t *out_rx_len,
		size_t expected_rx_len, unsigned int tries, unsigned int timeout_ms);

/*
 * Raw secure-channel round-trip (cmd 0x00 TLS records, 0x37/0x38 image): a dumb
 * pipe -- caller controls the full inner header + TX payload, and the raw RX
 * window (from +0x200) is returned with no offset-7 extraction or checksum
 * verify. With PETAIC_XFER_READ_ONLY the TX frame is skipped and a continuation
 * packet is pulled (wait_irq -> read -> strobe read_done).
 */
int petaic_xfer_raw(struct petaic_dev *d, uint8_t cmd, uint8_t dir, uint8_t width,
		    uint8_t outer_type, uint32_t be32,
		    const uint8_t *tx, size_t tx_len,
		    uint8_t *rx, size_t rx_cap, size_t *out_rx_len,
		    unsigned int tries, unsigned int timeout_ms, unsigned int flags);

#endif /* PETAIC_TRANSPORT_H */
