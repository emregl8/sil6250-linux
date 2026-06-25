/* petaic_sift.c — see petaic_sift.h.
 *
 * Clean-room reimplementation of the Silead matcher's algorithm class, recovered
 * from DllSiMatcher (MATCHER_ANALYSIS.md §B5-B7) and rebuilt from public CV
 * primitives only (all patent-free: SIFT expired ~2020, Harris 1988, RANSAC 1981):
 *
 *   1. Harris keypoint detection over a small scale set, NMS, top-200 by response.
 *   2. Per-keypoint dominant orientation (36-bin gradient histogram).
 *   3. Canonical SIFT-128 descriptor (4x4 spatial x 8 orientation, trilinear,
 *      unit-norm + 0.2 clamp + renorm).
 *   4. Match: nearest-neighbour + Lowe ratio test -> tentative correspondences.
 *   5. Geometric verify: translation RANSAC; the consistent-inlier count is the
 *      score (the DLL accepts at inliers >= 5).
 *
 * Pure C + libm. Operates on the destriped float frame from pm_destripe.
 */
#include "petaic_sift.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ */
/* Separable Gaussian blur on a PM_H x PM_W float image (zero-padded). */
static void gaussian_blur(const float *in, float *out, float sigma)
{
    int r = (int)(3.0f * sigma + 0.5f);
    if (r < 1) r = 1;
    int klen = 2 * r + 1;
    float k[64];
    float sum = 0.0f;
    for (int i = -r; i <= r; i++) {
        float v = expf(-(float)(i * i) / (2.0f * sigma * sigma));
        k[i + r] = v;
        sum += v;
    }
    for (int i = 0; i < klen; i++) k[i] /= sum;

    static float tmp[PM_N];
    /* horizontal */
    for (int y = 0; y < PM_H; y++)
        for (int x = 0; x < PM_W; x++) {
            float s = 0.0f;
            for (int i = -r; i <= r; i++) {
                int xx = x + i;
                if (xx < 0) xx = 0; else if (xx >= PM_W) xx = PM_W - 1;
                s += k[i + r] * in[y * PM_W + xx];
            }
            tmp[y * PM_W + x] = s;
        }
    /* vertical */
    for (int y = 0; y < PM_H; y++)
        for (int x = 0; x < PM_W; x++) {
            float s = 0.0f;
            for (int i = -r; i <= r; i++) {
                int yy = y + i;
                if (yy < 0) yy = 0; else if (yy >= PM_H) yy = PM_H - 1;
                s += k[i + r] * tmp[yy * PM_W + x];
            }
            out[y * PM_W + x] = s;
        }
}

/* Gradient (central difference) of a float image. */
static void gradients(const float *img, float *gx, float *gy)
{
    for (int y = 0; y < PM_H; y++)
        for (int x = 0; x < PM_W; x++) {
            int xl = x > 0 ? x - 1 : x, xr = x < PM_W - 1 ? x + 1 : x;
            int yu = y > 0 ? y - 1 : y, yd = y < PM_H - 1 ? y + 1 : y;
            gx[y * PM_W + x] = 0.5f * (img[y * PM_W + xr] - img[y * PM_W + xl]);
            gy[y * PM_W + x] = 0.5f * (img[yd * PM_W + x] - img[yu * PM_W + x]);
        }
}

/* ------------------------------------------------------------------ */
/* Keypoint detection: Harris response at a given smoothing scale. */
#define PS_HARRIS_K   0.04f
#define PS_NMS_RADIUS 1        /* suppression radius in px */
#define PS_EDGE       3        /* drop keypoints within this of the border */
#define PS_RESP_FRAC  0.0006f  /* response threshold as a fraction of max */

/* Candidate keypoint before description. */
typedef struct { float x, y, scale, resp; } ps_cand;

static int cmp_cand_desc(const void *a, const void *b)
{
    float ra = ((const ps_cand *)a)->resp, rb = ((const ps_cand *)b)->resp;
    return (ra < rb) - (ra > rb);   /* descending */
}

/* Detect Harris corners at one scale, append to cand[] (cap n_cap). */
static int detect_scale(const float *img, float sigma, ps_cand *cand,
                        int n_have, int n_cap)
{
    static float sm[PM_N], gx[PM_N], gy[PM_N];
    static float a[PM_N], b[PM_N], c[PM_N];   /* structure-tensor terms */
    static float resp[PM_N];

    gaussian_blur(img, sm, sigma);
    gradients(sm, gx, gy);
    for (int i = 0; i < PM_N; i++) {
        a[i] = gx[i] * gx[i];
        b[i] = gx[i] * gy[i];
        c[i] = gy[i] * gy[i];
    }
    /* window the structure tensor (integration scale ~1.5*sigma) */
    gaussian_blur(a, a, 1.5f * sigma);
    gaussian_blur(b, b, 1.5f * sigma);
    gaussian_blur(c, c, 1.5f * sigma);

    float maxr = 0.0f;
    for (int i = 0; i < PM_N; i++) {
        float det = a[i] * c[i] - b[i] * b[i];
        float tr = a[i] + c[i];
        float r = det - PS_HARRIS_K * tr * tr;
        resp[i] = r;
        if (r > maxr) maxr = r;
    }
    if (maxr <= 0.0f) return n_have;
    float thresh = PS_RESP_FRAC * maxr;

    int n = n_have;
    for (int y = PS_EDGE; y < PM_H - PS_EDGE; y++)
        for (int x = PS_EDGE; x < PM_W - PS_EDGE; x++) {
            float r = resp[y * PM_W + x];
            if (r < thresh) continue;
            /* 3x3 non-max */
            int is_max = 1;
            for (int dy = -1; dy <= 1 && is_max; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    if (resp[(y + dy) * PM_W + (x + dx)] > r) { is_max = 0; break; }
                }
            if (!is_max) continue;
            if (n >= n_cap) continue;
            cand[n].x = (float)x;
            cand[n].y = (float)y;
            cand[n].scale = sigma;
            cand[n].resp = r;
            n++;
        }
    return n;
}

/* Greedy spatial NMS across all scales: keep strongest, drop weaker within
 * PS_NMS_RADIUS. Assumes cand[] sorted by response descending. Returns kept #. */
static int spatial_nms(ps_cand *cand, int n, ps_cand *out, int out_cap)
{
    int kept = 0;
    for (int i = 0; i < n && kept < out_cap; i++) {
        int ok = 1;
        for (int j = 0; j < kept; j++) {
            float dx = cand[i].x - out[j].x, dy = cand[i].y - out[j].y;
            if (dx * dx + dy * dy < (float)(PS_NMS_RADIUS * PS_NMS_RADIUS)) {
                ok = 0; break;
            }
        }
        if (ok) out[kept++] = cand[i];
    }
    return kept;
}

/* ------------------------------------------------------------------ */
/* Dominant orientation: 36-bin gaussian-weighted gradient histogram. */
#define PS_ORI_BINS 36
static float dominant_orientation(const float *gx, const float *gy,
                                  float cx, float cy, float scale)
{
    float hist[PS_ORI_BINS];
    memset(hist, 0, sizeof(hist));
    int radius = (int)(4.0f * scale + 0.5f);
    float sig = 1.5f * scale;
    float expden = 2.0f * sig * sig;
    int xi = (int)(cx + 0.5f), yi = (int)(cy + 0.5f);

    for (int dy = -radius; dy <= radius; dy++)
        for (int dx = -radius; dx <= radius; dx++) {
            int x = xi + dx, y = yi + dy;
            if (x < 0 || x >= PM_W || y < 0 || y >= PM_H) continue;
            float gxv = gx[y * PM_W + x], gyv = gy[y * PM_W + x];
            float mag = sqrtf(gxv * gxv + gyv * gyv);
            float w = expf(-(float)(dx * dx + dy * dy) / expden);
            float ang = atan2f(gyv, gxv);            /* -pi..pi */
            if (ang < 0) ang += 2.0f * (float)M_PI;
            int bin = (int)(ang / (2.0f * (float)M_PI) * PS_ORI_BINS);
            if (bin >= PS_ORI_BINS) bin = PS_ORI_BINS - 1;
            hist[bin] += w * mag;
        }
    /* smooth the histogram (circular, 3-tap) once */
    float sm[PS_ORI_BINS];
    for (int i = 0; i < PS_ORI_BINS; i++) {
        int l = (i - 1 + PS_ORI_BINS) % PS_ORI_BINS, r = (i + 1) % PS_ORI_BINS;
        sm[i] = 0.25f * hist[l] + 0.5f * hist[i] + 0.25f * hist[r];
    }
    int peak = 0;
    for (int i = 1; i < PS_ORI_BINS; i++) if (sm[i] > sm[peak]) peak = i;
    /* parabolic interpolation around the peak */
    int l = (peak - 1 + PS_ORI_BINS) % PS_ORI_BINS, r = (peak + 1) % PS_ORI_BINS;
    float denom = sm[l] - 2.0f * sm[peak] + sm[r];
    float off = denom != 0.0f ? 0.5f * (sm[l] - sm[r]) / denom : 0.0f;
    float bin = (float)peak + off;
    return bin / PS_ORI_BINS * 2.0f * (float)M_PI;   /* radians */
}

/* ------------------------------------------------------------------ */
/* SIFT-128 descriptor: 4x4 spatial cells, 8 orientation bins, trilinear. */
#define PS_D 4          /* spatial cells per axis */
#define PS_OBINS 8      /* orientation bins */
#define PS_MAGFAC 3.0f  /* cell width = PS_MAGFAC * scale (pixels) */

static void compute_descriptor(const float *gx, const float *gy,
                               ps_keypoint *kp)
{
    float hist[PS_D][PS_D][PS_OBINS];
    memset(hist, 0, sizeof(hist));

    float cell = PS_MAGFAC * kp->scale;          /* px per spatial cell */
    float ct = cosf(kp->ori), st = sinf(kp->ori);
    /* sample radius covers the 4x4 grid plus interpolation margin */
    float radius_f = cell * (PS_D + 1) * 0.5f * 1.41421356f;
    int radius = (int)(radius_f + 0.5f);
    int xi = (int)(kp->x + 0.5f), yi = (int)(kp->y + 0.5f);
    float expden = 2.0f * (0.5f * PS_D) * (0.5f * PS_D);   /* gaussian over cells */

    for (int dy = -radius; dy <= radius; dy++)
        for (int dx = -radius; dx <= radius; dx++) {
            int x = xi + dx, y = yi + dy;
            if (x < 0 || x >= PM_W || y < 0 || y >= PM_H) continue;
            /* rotate sample offset into the keypoint frame, scale to cell units */
            float rx = ( ct * dx + st * dy) / cell;
            float ry = (-st * dx + ct * dy) / cell;
            /* cell coords centred so grid spans [-D/2, D/2]; shift to [0,D) bins */
            float cbx = rx + PS_D * 0.5f - 0.5f;
            float cby = ry + PS_D * 0.5f - 0.5f;
            if (cbx <= -1.0f || cbx >= PS_D || cby <= -1.0f || cby >= PS_D)
                continue;

            float gxv = gx[y * PM_W + x], gyv = gy[y * PM_W + x];
            float mag = sqrtf(gxv * gxv + gyv * gyv);
            float w = expf(-(rx * rx + ry * ry) / expden);
            float ang = atan2f(gyv, gxv) - kp->ori;
            while (ang < 0) ang += 2.0f * (float)M_PI;
            while (ang >= 2.0f * (float)M_PI) ang -= 2.0f * (float)M_PI;
            float obin = ang / (2.0f * (float)M_PI) * PS_OBINS;

            float wmag = w * mag;
            /* trilinear distribution into (cy,cx,obin) */
            int x0 = (int)floorf(cbx), y0 = (int)floorf(cby), o0 = (int)floorf(obin);
            float fx = cbx - x0, fy = cby - y0, fo = obin - o0;
            for (int iy = 0; iy <= 1; iy++) {
                int yy = y0 + iy;
                if (yy < 0 || yy >= PS_D) continue;
                float wy = iy ? fy : 1.0f - fy;
                for (int ix = 0; ix <= 1; ix++) {
                    int xx = x0 + ix;
                    if (xx < 0 || xx >= PS_D) continue;
                    float wx = ix ? fx : 1.0f - fx;
                    for (int io = 0; io <= 1; io++) {
                        int oo = (o0 + io) % PS_OBINS;
                        float wo = io ? fo : 1.0f - fo;
                        hist[yy][xx][oo] += wmag * wy * wx * wo;
                    }
                }
            }
        }

    /* flatten + SIFT normalization (unit norm, clamp 0.2, renorm) */
    float *d = kp->desc;
    int idx = 0;
    for (int iy = 0; iy < PS_D; iy++)
        for (int ix = 0; ix < PS_D; ix++)
            for (int io = 0; io < PS_OBINS; io++)
                d[idx++] = hist[iy][ix][io];

    float norm = 0.0f;
    for (int i = 0; i < PS_DESC_DIM; i++) norm += d[i] * d[i];
    norm = sqrtf(norm);
    if (norm < 1e-9f) norm = 1.0f;
    for (int i = 0; i < PS_DESC_DIM; i++) {
        d[i] /= norm;
        if (d[i] > 0.2f) d[i] = 0.2f;
    }
    norm = 0.0f;
    for (int i = 0; i < PS_DESC_DIM; i++) norm += d[i] * d[i];
    norm = sqrtf(norm);
    if (norm < 1e-9f) norm = 1.0f;
    for (int i = 0; i < PS_DESC_DIM; i++) d[i] /= norm;
}

/* ------------------------------------------------------------------ */
/* Capture-quality = mean orientation-tensor coherence over textured 8x8 blocks.
 * Clean-room dirScore_q/texturescore_q analogue (see petaic_sift.h, §B4). */
#define PS_Q_BS      8
#define PS_Q_ENERGY  6.0f   /* per-block gradient-energy floor for "textured" */
float ps_quality(const pm_frame *f)
{
    int nbx = PM_W / PS_Q_BS, nby = PM_H / PS_Q_BS;
    int covered = 0;
    double cohsum = 0.0;
    for (int by = 0; by < nby; by++)
        for (int bx = 0; bx < nbx; bx++) {
            double gxx = 0, gyy = 0, gxy = 0, energy = 0;
            for (int yy = 1; yy < PS_Q_BS - 1; yy++)
                for (int xx = 1; xx < PS_Q_BS - 1; xx++) {
                    int x = bx * PS_Q_BS + xx, y = by * PS_Q_BS + yy;
                    float gx = f->px[y*PM_W + x+1] - f->px[y*PM_W + x-1];
                    float gy = f->px[(y+1)*PM_W + x] - f->px[(y-1)*PM_W + x];
                    gxx += gx*gx; gyy += gy*gy; gxy += gx*gy;
                    energy += gx*gx + gy*gy;
                }
            double tr = gxx + gyy;
            if (energy <= PS_Q_ENERGY || tr <= 1e-6) continue;
            double coher = sqrt((gxx-gyy)*(gxx-gyy) + 4*gxy*gxy) / tr;
            covered++;
            cohsum += coher;
        }
    return covered ? (float)(cohsum / covered) : 0.0f;
}

/* ------------------------------------------------------------------ */
void ps_extract(const pm_frame *f, ps_features *out)
{
    static ps_cand cand[PS_MAX_KPTS * 8];
    static ps_cand kept[PS_MAX_KPTS];
    static float gx[PM_N], gy[PM_N];

    /* multi-scale Harris detection (mirrors the DLL's scale pyramid, coarsened) */
    static const float scales[] = { 1.2f, 1.6f, 2.2f, 3.0f };
    int ncand = 0;
    for (unsigned s = 0; s < sizeof(scales) / sizeof(scales[0]); s++)
        ncand = detect_scale(f->px, scales[s], cand, ncand,
                             (int)(sizeof(cand) / sizeof(cand[0])));

    qsort(cand, ncand, sizeof(cand[0]), cmp_cand_desc);
    int nk = spatial_nms(cand, ncand, kept, PS_MAX_KPTS);

    /* gradients on a lightly smoothed image for orientation + description */
    static float sm[PM_N];
    gaussian_blur(f->px, sm, 1.0f);
    gradients(sm, gx, gy);

    out->n = nk;
    for (int i = 0; i < nk; i++) {
        ps_keypoint *kp = &out->kp[i];
        kp->x = kept[i].x;
        kp->y = kept[i].y;
        kp->scale = kept[i].scale;
        kp->resp = kept[i].resp;
        kp->ori = dominant_orientation(gx, gy, kp->x, kp->y, kp->scale);
        compute_descriptor(gx, gy, kp);
    }
}

/* ------------------------------------------------------------------ */
/* Matching: NN + Lowe ratio, then pairwise geometric-consistency clique.
 *
 * A single global translation/affine fit is too brittle for two physical
 * presses (skin deformation + pressure + small rotation collapse the inlier
 * count). Instead, mirror the DLL's createUndirectGraph: verify matches by
 * *pairwise* consistency, which is invariant to translation and rotation and
 * tolerant of mild non-rigid deformation. For two correct matches a:(pa->ra)
 * and b:(pb->rb), the inter-keypoint distance is preserved (|pa-pb| ~ |ra-rb|)
 * and the per-match orientation delta is roughly the same global rotation. The
 * score is the largest set of mutually-consistent matches (greedy max-clique). */
/* Operating point from the same-core ROC (genuine v0..v14 LOO vs clearly-different
 * impostor w0..w9, MATCHER_ANALYSIS.md §B7): threshold 5 inliers -> FRR 13%, FAR 0%
 * (impostor ceiling 4). Tightened from the initial loose values, which let
 * accidental impostor cliques reach 5. */
#define PS_RATIO      0.92f   /* Lowe ratio (loose; the graph filters falses) */
#define PS_DIST_TOL_A 0.15f   /* distance tolerance, fraction of separation */
#define PS_DIST_TOL_B 2.5f    /* distance tolerance, constant px floor */
#define PS_ORI_TOL    0.45f   /* orientation-delta agreement, radians (~26deg) */
#define PS_MIN_SEP    3.0f    /* ignore match pairs closer than this (ambiguous) */

static float desc_dist2(const float *a, const float *b)
{
    float s = 0.0f;
    for (int i = 0; i < PS_DESC_DIM; i++) {
        float d = a[i] - b[i];
        s += d * d;
    }
    return s;
}

static float ang_wrap(float a)
{
    while (a >  (float)M_PI) a -= 2.0f * (float)M_PI;
    while (a < -(float)M_PI) a += 2.0f * (float)M_PI;
    return a;
}

/* Tunables, overridable via env for offline sweeps (PS_TUNE_*). */
static float g_ratio   = PS_RATIO;
static float g_tol_a   = PS_DIST_TOL_A;
static float g_tol_b   = PS_DIST_TOL_B;
static float g_ori_tol = PS_ORI_TOL;
static float g_min_ext = 0.0f;    /* min clique spatial extent (probe px) */
static float g_max_desc = 1e30f;  /* max absolute squared desc distance */

static void ps_load_tunables(void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    const char *e;
    if ((e = getenv("PS_TUNE_RATIO")))  g_ratio   = (float) atof(e);
    if ((e = getenv("PS_TUNE_TOLA")))   g_tol_a   = (float) atof(e);
    if ((e = getenv("PS_TUNE_TOLB")))   g_tol_b   = (float) atof(e);
    if ((e = getenv("PS_TUNE_ORI")))    g_ori_tol = (float) atof(e);
    if ((e = getenv("PS_TUNE_EXT")))    g_min_ext = (float) atof(e);
    if ((e = getenv("PS_TUNE_MAXD")))   g_max_desc = (float) atof(e);
}

/* one tentative correspondence */
typedef struct { float px, py, rx, ry, dori; } ps_corr;

int ps_match(const ps_features *probe, const ps_features *ref,
             int *best_dx, int *best_dy)
{
    ps_load_tunables();
    if (best_dx) *best_dx = 0;
    if (best_dy) *best_dy = 0;
    if (probe->n == 0 || ref->n == 0)
        return 0;

    /* For each ref keypoint, precompute its nearest probe (for cross-check). */
    static int ref_nn[PS_MAX_KPTS];
    for (int j = 0; j < ref->n; j++) {
        float best = 1e30f; int bi = -1;
        for (int i = 0; i < probe->n; i++) {
            float d = desc_dist2(probe->kp[i].desc, ref->kp[j].desc);
            if (d < best) { best = d; bi = i; }
        }
        ref_nn[j] = bi;
    }

    /* tentative correspondences: Lowe ratio test + mutual NN cross-check. */
    static ps_corr c[PS_MAX_KPTS];
    int m = 0;
    for (int i = 0; i < probe->n; i++) {
        float best = 1e30f, second = 1e30f;
        int bj = -1;
        for (int j = 0; j < ref->n; j++) {
            float d = desc_dist2(probe->kp[i].desc, ref->kp[j].desc);
            if (d < best) { second = best; best = d; bj = j; }
            else if (d < second) { second = d; }
        }
        if (bj < 0) continue;
        if (ref_nn[bj] != i) continue;                  /* mutual NN cross-check */
        if (best > g_max_desc) continue;                /* absolute distance gate */
        if (best < g_ratio * g_ratio * second) {
            c[m].px = probe->kp[i].x;  c[m].py = probe->kp[i].y;
            c[m].rx = ref->kp[bj].x;   c[m].ry = ref->kp[bj].y;
            c[m].dori = ref->kp[bj].ori - probe->kp[i].ori;
            m++;
        }
    }
    if (m == 0)
        return 0;

    /* pairwise compatibility graph (symmetric): distance preservation +
     * consistent global rotation. */
    static uint8_t comp[PS_MAX_KPTS][PS_MAX_KPTS];
    static int deg[PS_MAX_KPTS];
    for (int i = 0; i < m; i++) { deg[i] = 0; comp[i][i] = 0; }
    for (int i = 0; i < m; i++)
        for (int j = i + 1; j < m; j++) {
            float pdx = c[i].px - c[j].px, pdy = c[i].py - c[j].py;
            float rdx = c[i].rx - c[j].rx, rdy = c[i].ry - c[j].ry;
            float dp = sqrtf(pdx * pdx + pdy * pdy);
            float dr = sqrtf(rdx * rdx + rdy * rdy);
            int ok = 0;
            if (dp >= PS_MIN_SEP && dr >= PS_MIN_SEP) {
                float tol = g_tol_a * (dp > dr ? dp : dr) + g_tol_b;
                if (fabsf(dp - dr) <= tol &&
                    fabsf(ang_wrap(c[i].dori - c[j].dori)) <= g_ori_tol)
                    ok = 1;
            }
            comp[i][j] = comp[j][i] = (uint8_t) ok;
            deg[i] += ok;
            deg[j] += ok;
        }

    /* greedy max-clique approximation seeded from every node: the largest set
     * of mutually-consistent matches is the geometric inlier count. */
    static int clique[PS_MAX_KPTS];
    int best_inliers = 0, bcx = 0, bcy = 0;
    for (int s = 0; s < m; s++) {
        if (deg[s] + 1 <= best_inliers)
            continue;                 /* can't beat current best */
        int cn = 0;
        clique[cn++] = s;
        /* add nodes (in descending degree order, approximated by scan) that are
         * compatible with all current clique members */
        for (int pass = 0; pass < 2; pass++) {
            for (int k = 0; k < m; k++) {
                if (k == s) continue;
                int fits = 1;
                for (int t = 0; t < cn; t++)
                    if (!comp[k][clique[t]]) { fits = 0; break; }
                if (fits) {
                    int dup = 0;
                    for (int t = 0; t < cn; t++) if (clique[t] == k) { dup = 1; break; }
                    if (!dup) clique[cn++] = k;
                }
            }
        }
        /* spatial-extent gate: reject spatially-compact cliques (accidental
         * impostor cliques tend to bunch up; genuine matches spread across the
         * overlap). Measure the probe-side bounding-box diagonal. */
        if (cn > best_inliers && g_min_ext > 0.0f) {
            float xmin = 1e9f, xmax = -1e9f, ymin = 1e9f, ymax = -1e9f;
            for (int t = 0; t < cn; t++) {
                float x = c[clique[t]].px, y = c[clique[t]].py;
                if (x < xmin) xmin = x;
                if (x > xmax) xmax = x;
                if (y < ymin) ymin = y;
                if (y > ymax) ymax = y;
            }
            float ext = sqrtf((xmax - xmin) * (xmax - xmin) +
                              (ymax - ymin) * (ymax - ymin));
            if (ext < g_min_ext)
                continue;
        }
        if (cn > best_inliers) {
            best_inliers = cn;
            float sx = 0, sy = 0;
            for (int t = 0; t < cn; t++) {
                sx += c[clique[t]].rx - c[clique[t]].px;
                sy += c[clique[t]].ry - c[clique[t]].py;
            }
            bcx = (int)(sx / cn + (sx < 0 ? -0.5f : 0.5f));
            bcy = (int)(sy / cn + (sy < 0 ? -0.5f : 0.5f));
        }
    }
    if (best_dx) *best_dx = bcx;
    if (best_dy) *best_dy = bcy;
    return best_inliers;
}

int ps_verify(const ps_features *probe, const ps_features *gallery, int n)
{
    int best = 0;
    for (int i = 0; i < n; i++) {
        int s = ps_match(probe, &gallery[i], NULL, NULL);
        if (s > best) best = s;
    }
    return best;
}
