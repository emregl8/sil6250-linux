/* petaic_match.h — host-side correlation matcher for the SIL6250 64x80 sensor.
 *
 * The sensor's dominant variance is coherent column fixed-pattern noise, not
 * ridge signal (see HANDOFF.md §6). This library replicates the Windows engine's
 * `remove_line` destripe and then matches frames by best-shift normalized
 * cross-correlation (NCC) — the design the Windows engine uses for single
 * 64x80 frames, where minutiae extraction structurally fails.
 *
 * Frames are raw 8-bit, 64 wide x 80 tall, row-major (5120 bytes).
 */
#ifndef PETAIC_MATCH_H
#define PETAIC_MATCH_H

#include <stdint.h>

#define PM_W 64
#define PM_H 80
#define PM_N (PM_W * PM_H)   /* 5120 */

/* Destriped frame: float per pixel, row-major PM_H x PM_W. */
typedef struct {
    float px[PM_N];
} pm_frame;

/* Matcher front-end: remove_line destripe (per-column then per-row mean
 * subtraction, kills vertical-coherent FPN) followed by local contrast
 * normalization (sigma=6 bandpass). Mirrors calibrate.py destripe +
 * local_normalize — the "plain destripe + local-normalize is best" path. */
void pm_destripe(const uint8_t raw[PM_N], pm_frame *out);

/* Best-shift NCC between two destriped frames. Searches integer shifts of
 * `probe` relative to `ref` in [-max_dx,max_dx] x [-max_dy,max_dy]. Returns the
 * peak NCC over the overlap region (in [-1,1]); writes the winning shift to
 * best_dx and best_dy if non-NULL. Overlaps smaller than `min_overlap` pixels are
 * skipped (avoids spurious peaks from tiny corner overlaps). */
float pm_best_shift_ncc(const pm_frame *probe, const pm_frame *ref,
                        int max_dx, int max_dy, int min_overlap,
                        int *best_dx, int *best_dy);

/* Gallery verify: max best-shift NCC of `probe` over `n` reference frames. */
float pm_verify(const pm_frame *probe, const pm_frame *gallery, int n,
                int max_dx, int max_dy, int min_overlap);

#endif /* PETAIC_MATCH_H */
