/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef SIL6250_FFI_H
#define SIL6250_FFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIL6250_IMAGE_SIZE 5120
#define SIL6250_MAX_FEATURE_SIZE 106412

typedef struct Sil6250Engine Sil6250Engine;
typedef struct Sil6250Gallery Sil6250Gallery;
typedef struct Sil6250Probe Sil6250Probe;
typedef int32_t (*Sil6250Cancelled)(void *user_data);

Sil6250Engine *sil6250_engine_new(const char *path);
void sil6250_engine_free(Sil6250Engine *engine);
bool sil6250_engine_set_control(Sil6250Engine *engine, uint32_t timeout_ms,
                                Sil6250Cancelled cancelled, void *user_data);
int32_t sil6250_engine_capture(Sil6250Engine *engine, uint32_t timeout_ms,
                               uint8_t *raw, size_t raw_len);
bool sil6250_engine_wait_lift(Sil6250Engine *engine, uint32_t timeout_ms);
float sil6250_frame_quality(const uint8_t *raw, size_t raw_len);
float sil6250_frame_ncc(const uint8_t *raw, const uint8_t *reference,
                        size_t raw_len);
ssize_t sil6250_frame_extract(const uint8_t *raw, size_t raw_len,
                              uint8_t *output, size_t capacity);
int32_t sil6250_frame_score(const uint8_t *raw, size_t raw_len,
                            const uint8_t *gallery, size_t gallery_len);
int32_t sil6250_frame_matches(const uint8_t *raw, size_t raw_len,
                              const uint8_t *gallery, size_t gallery_len,
                              int32_t threshold);
bool sil6250_gallery_valid(const uint8_t *gallery, size_t gallery_len);
Sil6250Gallery *sil6250_gallery_new(const uint8_t *data, size_t len);
void sil6250_gallery_free(Sil6250Gallery *gallery);
Sil6250Probe *sil6250_probe_new(const uint8_t *raw, size_t len);
void sil6250_probe_free(Sil6250Probe *probe);
int32_t sil6250_probe_matches(const Sil6250Probe *probe,
                              const Sil6250Gallery *gallery, int32_t threshold);

#ifdef __cplusplus
}
#endif

#endif
