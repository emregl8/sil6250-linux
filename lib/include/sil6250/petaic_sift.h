/* petaic_sift.h — clean-room descriptor matcher for the SIL6250 64x80 sensor.
 *
 * Track B: a from-scratch reimplementation of the Silead DllSiMatcher algorithm
 * *class* (local-feature keypoint detector + SIFT-128 descriptor + geometric
 * RANSAC inlier verification), recovered from the DLL but written clean from a
 * spec (see MATCHER_ANALYSIS.md §B5-B7). This is the path that fixes NCC's
 * genuine/impostor overlap (0.218 vs 0.233): different fingers produce few
 * geometrically-consistent descriptor matches, so the inlier count separates
 * them where whole-frame correlation cannot.
 *
 * Operates on the destriped float frame produced by pm_destripe (petaic_match.h).
 * No external deps; pure C + libm.
 */
#ifndef PETAIC_SIFT_H
#define PETAIC_SIFT_H

#include <stdint.h>
#include "petaic_match.h"   /* pm_frame, PM_W, PM_H, PM_N, pm_destripe */

#define PS_MAX_KPTS   200
#define PS_DESC_DIM   128   /* 4x4 spatial x 8 orientation, SIFT-128 */

typedef struct {
    float x, y;             /* sub-pixel keypoint location */
    float scale;            /* detection scale (sigma) */
    float ori;             /* dominant orientation, radians */
    float resp;             /* detector response (for ranking) */
    float desc[PS_DESC_DIM];/* L2-normalized, 0.2-clamped SIFT descriptor */
} ps_keypoint;

typedef struct {
    int n;
    ps_keypoint kp[PS_MAX_KPTS];
} ps_features;

/* Capture-quality score of a destriped frame, in [0,1]. It is the mean
 * orientation-tensor coherence over textured 8x8 blocks: high where the frame
 * carries clear, directionally-consistent ridge flow, low for weak/dry/partial
 * presses dominated by noise. This is the clean-room analogue of the DLL's
 * dirScore_q / texturescore_q quality gate (MATCHER_ANALYSIS.md §B4). On the
 * clean v../w.. set it cleanly separates the two low-yield genuine presses (v2,
 * v3 ~0.48, the matcher's only FRR frames) from the rest (>=0.55); reject below
 * PS_QUALITY_MIN at capture time to turn a hard reject into a "lift & retry". */
float ps_quality(const pm_frame *f);

/* Default capture-quality acceptance floor (override via PS_QUALITY_MIN env in
 * tools, or SIL6250_QUALITY_MIN in the driver). */
#define PS_QUALITY_MIN 0.52f

/* Extract keypoints + descriptors from a destriped frame. */
void ps_extract(const pm_frame *f, ps_features *out);

/* Match probe features against reference features; returns the geometric
 * inlier count (translation-consistent descriptor matches). This is the score:
 * accept iff inliers >= threshold (the DLL uses >= 5). If best_dx/best_dy are
 * non-NULL they receive the winning translation. */
int ps_match(const ps_features *probe, const ps_features *ref,
             int *best_dx, int *best_dy);

/* Gallery verify: max inlier count of probe over n reference feature sets. */
int ps_verify(const ps_features *probe, const ps_features *gallery, int n);

#endif /* PETAIC_SIFT_H */
