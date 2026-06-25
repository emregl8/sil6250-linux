/* petaic_engine.h — reusable SIL6250 capture engine (HANDOFF.md §7 A1).
 *
 * Wraps the proven handshake + capture path from petaic_capture.c behind a
 * small API so both the CLI tools and the libfprint FpDevice driver bind to one
 * surface. The engine owns the TLS-PSK session, the mailbox record framing, and
 * the 0x11->0x37->0x38 capture loop, and transparently re-handshakes on a moving
 * finger (a desynced GCM stream is unrecoverable, but a fresh handshake is not).
 *
 * Blocking API: each call drives the mailbox synchronously. The libfprint driver
 * runs these on a worker thread and marshals completion back to the main loop.
 */
#ifndef PETAIC_ENGINE_H
#define PETAIC_ENGINE_H

#include <stdint.h>

#define PE_IMG_W    64
#define PE_IMG_H    80
#define PE_IMG_SIZE (PE_IMG_W * PE_IMG_H)   /* 5120 */

typedef struct petaic_engine petaic_engine;

/* Allocate / free an engine. engine_new returns NULL on OOM. */
petaic_engine *engine_new(void);
void           engine_free(petaic_engine *e);

void engine_set_verbose(petaic_engine *e, int v);
/* Force a specific PSK by name ("shiba" etc.); default cycles all four. */
int  engine_set_key(petaic_engine *e, const char *name);

/* Open the kernel device (e.g. "/dev/sil6250"). No handshake yet. 0 / -errno. */
int  engine_open(petaic_engine *e, const char *devpath);

/* Capture one 5120-byte raw image. Waits up to finger_ms for a finger (polling
 * cmd 0x11), arms the sensor and pulls the image, lazily (re)establishing the
 * TLS session and retrying on a fresh handshake if the finger moves. Returns 0
 * on success (img filled, checksum verified) or -errno (-ETIMEDOUT = no finger /
 * gave up). */
int  engine_capture_frame(petaic_engine *e, uint8_t img[PE_IMG_SIZE],
                          unsigned finger_ms);

/* Wait for the finger to be lifted (poll cmd 0x11 until it reports up), up to
 * timeout_ms. Returns 1 once the finger is up (or was already up), 0 on timeout.
 * Used between enroll captures so each stored frame is a fresh placement rather
 * than a burst of near-identical frames from one continuous touch. Does not
 * disturb the TLS session (0x11 is an out-of-band std poll). */
int  engine_wait_finger_up(petaic_engine *e, unsigned timeout_ms);

/* Tear down the TLS session and close the device. Safe to call on a NULL or
 * never-opened engine. */
void engine_close(petaic_engine *e);

#endif /* PETAIC_ENGINE_H */
