// SPDX-License-Identifier: GPL-2.0
/*
 * Petaic/SIL6250 frame builder + parser (userspace port).
 *
 * Byte-for-byte equivalent of gxfp_ref/proto/gxfp_petaic_proto.c; see that file
 * and windows_extract/PETAIC_PROTOCOL.md for the derivation of the framing.
 */
#include <errno.h>
#include <string.h>

#include "petaic_proto.h"

#define PETAIT_INNER_MAGIC	0x5A
#define PETAIT_INNER_HEADER	8
#define PETAIT_OUTER_HEADER	8
#define PETAIT_CHECKSUM_SIZE	4

#define ALIGN8(x)	(((x) + 7u) & ~(size_t)7u)

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p)
{
	return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

uint32_t petaic_checksum(const uint8_t *buf, size_t len)
{
	uint32_t sum = 0;
	size_t i;

	if (!buf || len == 0)
		return 0;

	for (i = 0; i < len; i++)
		sum += buf[i];

	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);

	return ~sum;
}

int petaic_build_frame_ex(uint8_t cmd, uint8_t dir, uint8_t access_width,
			  const uint8_t *payload, size_t payload_len,
			  uint32_t be32_field, size_t min_region,
			  uint8_t seq, uint8_t outer_type,
			  uint8_t *out, size_t out_cap)
{
	size_t inner_len, frame_len, padded_len, region_len;
	uint32_t cksum;
	uint8_t *inner;

	if (!out || out_cap < PETAIT_OUTER_HEADER + PETAIT_INNER_HEADER + PETAIT_CHECKSUM_SIZE)
		return -EINVAL;
	if (payload_len && !payload)
		return -EINVAL;
	if (payload_len > 0xFFFFFFu)
		return -EOVERFLOW;

	region_len = payload_len > min_region ? payload_len : min_region;
	inner_len = PETAIT_INNER_HEADER + region_len + PETAIT_CHECKSUM_SIZE;
	frame_len = PETAIT_OUTER_HEADER + inner_len;
	padded_len = ALIGN8(frame_len);
	if (padded_len > out_cap)
		return -EMSGSIZE;

	memset(out, 0, padded_len);

	/* Outer wrapper. */
	out[0] = 0xF0;
	out[1] = outer_type;
	out[7] = seq;

	/* Inner frame. */
	inner = out + PETAIT_OUTER_HEADER;
	inner[0] = PETAIT_INNER_MAGIC;
	inner[1] = cmd;
	inner[2] = dir;
	inner[3] = access_width;
	put_be32(inner + 4, be32_field);

	if (payload_len)
		memcpy(inner + PETAIT_INNER_HEADER, payload, payload_len);

	/* Checksum over inner header + full (zero-padded) region, stored LE32. */
	cksum = petaic_checksum(inner, inner_len - PETAIT_CHECKSUM_SIZE);
	put_le32(inner + inner_len - PETAIT_CHECKSUM_SIZE, cksum);

	return (int)padded_len;
}

int petaic_build_frame(uint8_t cmd, uint8_t dir, uint8_t access_width,
		       const uint8_t *payload, size_t payload_len,
		       size_t out_length, uint8_t seq, uint8_t outer_type,
		       uint8_t *out, size_t out_cap)
{
	if (out_length > 0xFFFFFFu)
		return -EOVERFLOW;

	return petaic_build_frame_ex(cmd, dir, access_width, payload, payload_len,
				     (uint32_t)out_length, PETAIT_STD_PAYLOAD_REGION,
				     seq, outer_type, out, out_cap);
}

static bool try_parse_inner(const uint8_t *inner, size_t inner_len,
			    size_t payload_len, struct petaic_frame *out)
{
	uint32_t cksum, calc;

	if (inner_len < PETAIT_INNER_HEADER + payload_len + PETAIT_CHECKSUM_SIZE)
		return false;

	cksum = get_le32(inner + PETAIT_INNER_HEADER + payload_len);
	calc = petaic_checksum(inner, PETAIT_INNER_HEADER + payload_len);
	if (cksum != calc)
		return false;

	out->cmd = inner[1];
	out->dir = inner[2];
	out->access_width = inner[3];
	out->declared_len = get_be32(inner + 4);
	out->payload_len = (uint32_t)payload_len;
	out->payload = payload_len ? inner + PETAIT_INNER_HEADER : NULL;
	out->checksum = cksum;
	out->checksum_ok = true;
	out->valid = true;
	return true;
}

static bool parse_inner(const uint8_t *inner, size_t inner_len,
			struct petaic_frame *out)
{
	static const size_t candidates[] = { 0, 1, 2, 4, 8, 12, 16, 20, 24, 32,
					     48, 64, 128, 256 };
	size_t payload_len, i;

	if (!out || !inner || inner_len < PETAIT_INNER_HEADER + PETAIT_CHECKSUM_SIZE)
		return false;

	memset(out, 0, sizeof(*out));
	if (inner[0] != PETAIT_INNER_MAGIC)
		return false;

	/* First trust the declared BE32 length. */
	payload_len = get_be32(inner + 4);
	if (payload_len <= 0x1000 &&
	    try_parse_inner(inner, inner_len, payload_len, out))
		return true;

	/* Fall back to candidate lengths. */
	for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		if (candidates[i] == payload_len)
			continue;
		if (try_parse_inner(inner, inner_len, candidates[i], out))
			return true;
	}
	return false;
}

bool petaic_parse_frame(const uint8_t *buf, size_t buf_len,
			struct petaic_frame *out)
{
	const uint8_t *inner;
	size_t inner_len;

	if (!out || !buf || buf_len < PETAIT_INNER_HEADER + PETAIT_CHECKSUM_SIZE)
		return false;

	/* Responses start with 0x5A directly; requests carry the 0xF0 wrapper. */
	if (buf[0] == PETAIT_INNER_MAGIC) {
		inner = buf;
		inner_len = buf_len;
	} else if (buf[0] == 0xF0 &&
		   buf_len >= PETAIT_OUTER_HEADER + PETAIT_INNER_HEADER + PETAIT_CHECKSUM_SIZE) {
		inner = buf + PETAIT_OUTER_HEADER;
		inner_len = buf_len - PETAIT_OUTER_HEADER;
	} else {
		return false;
	}

	return parse_inner(inner, inner_len, out);
}
