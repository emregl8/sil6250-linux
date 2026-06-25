/* SPDX-License-Identifier: GPL-2.0 */
#ifndef PETAIC_PROTO_H
#define PETAIC_PROTO_H

/*
 * Petaic/SIL6250 mailbox frame builder + parser.
 *
 * De-kernelized port of gxfp_ref/proto/gxfp_petaic_proto.c. Pure byte
 * manipulation, no hardware -- the userspace transport (petaic_transport.c)
 * feeds the built frames to the sil6250 mmap window.
 *
 * Frame layout (16-byte header, payload region, LE32 checksum):
 *   outer: F0 [type] 00 00 00 00 00 [seq]
 *   inner: 5A [cmd] [dir] [width] [BE32 len] [payload region...]
 *   tail:  [LE32 ~ones-complement-sum16 of inner header + region]
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Command codes (Windows WPP trace). */
#define PETAIT_CMD_BULK			0x00 /* TLS handshake / app-data records */
#define PETAIT_CMD_RW			0x02
#define PETAIT_CMD_POLL			0x11 /* finger-detect status */
#define PETAIT_CMD_GET_SENSOR_INFO	0x14 /* 64x80 -> image_size 5120 */
#define PETAIT_CMD_INIT			0x1b /* fw probe -> 0.0.32.15 */
#define PETAIT_CMD_READ_STREAM		0x22 /* sensor->host TLS continuation */
#define PETAIT_CMD_TLS_CTRL		0x37 /* image-transfer control */
#define PETAIT_CMD_TLS_DATA		0x38 /* encrypted-image bulk record */

/* Inner access-width byte. */
#define PETAIT_ACCESS_WIDTH_32BIT	0x04 /* every standard command */
#define PETAIT_ACCESS_WIDTH_TLS		0x05 /* cmd 0x00 TLS records */

/* Inner direction/usage byte (drives EC ec_transfer `useage`). */
#define PETAIT_DIR_IN			0x00 /* control */
#define PETAIT_DIR_OUT			0x01 /* host->sensor request-with-data */
#define PETAIT_DIR_WRITE		0x02 /* host->sensor payload write (TLS u=2) */
#define PETAIT_DIR_READ			0x04 /* sensor->host payload read */

/* Outer wrapper type byte. */
#define PETAIT_OUTER_TYPE_STD		0x13
#define PETAIT_OUTER_TYPE_SHORT		0x10
#define PETAIT_OUTER_TYPE_TLS		0x14

/* Fixed zero payload region in a standard 27-byte command frame. */
#define PETAIT_STD_PAYLOAD_REGION	7

struct petaic_frame {
	uint8_t  cmd;
	uint8_t  dir;
	uint8_t  access_width;
	uint32_t declared_len;	/* BE32 length field from inner header */
	uint32_t payload_len;	/* resolved payload length */
	const uint8_t *payload;	/* points into the parsed buffer */
	uint32_t checksum;
	bool     checksum_ok;
	bool     valid;
};

/* One's-complement folded-16 sum, inverted (the wire stores it LE32). */
uint32_t petaic_checksum(const uint8_t *buf, size_t len);

/*
 * Generalized frame builder. be32_field is written verbatim to the inner BE32
 * slot; min_region forces a minimum (zero-padded) payload region. Returns the
 * 8-byte-padded frame length, or a negative errno.
 */
int petaic_build_frame_ex(uint8_t cmd, uint8_t dir, uint8_t access_width,
			  const uint8_t *payload, size_t payload_len,
			  uint32_t be32_field, size_t min_region,
			  uint8_t seq, uint8_t outer_type,
			  uint8_t *out, size_t out_cap);

/* Standard command: BE32 = OutLength, fixed 7-byte payload region. */
int petaic_build_frame(uint8_t cmd, uint8_t dir, uint8_t access_width,
		       const uint8_t *payload, size_t payload_len,
		       size_t out_length, uint8_t seq, uint8_t outer_type,
		       uint8_t *out, size_t out_cap);

/* Parse a response (with or without the 0xF0 outer wrapper). */
bool petaic_parse_frame(const uint8_t *buf, size_t buf_len,
			struct petaic_frame *out);

#endif /* PETAIC_PROTO_H */
