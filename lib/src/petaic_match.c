/* petaic_match.c — see petaic_match.h. Pure C, no deps. */
#include "petaic_match.h"
#include <math.h>
#include <stddef.h>

/* Separable box blur, radius r, matching numpy convolve(k=ones/(2r+1), "same")
 * with zero-padding at the edges (so edge pixels see a partial, darkened sum).
 * Replicates calibrate.py:local_normalize's blur exactly. */
static void box_blur(const float *in, float *out, int r)
{
    int x, y, k;
    float tmp[PM_N];
    float inv = 1.0f / (2 * r + 1);

    /* along y (axis 0) */
    for (x = 0; x < PM_W; x++)
        for (y = 0; y < PM_H; y++) {
            double s = 0.0;
            for (k = -r; k <= r; k++) {
                int yy = y + k;
                if (yy >= 0 && yy < PM_H)
                    s += in[yy * PM_W + x];
            }
            tmp[y * PM_W + x] = (float)(s * inv);
        }
    /* along x (axis 1) */
    for (y = 0; y < PM_H; y++)
        for (x = 0; x < PM_W; x++) {
            double s = 0.0;
            for (k = -r; k <= r; k++) {
                int xx = x + k;
                if (xx >= 0 && xx < PM_W)
                    s += tmp[y * PM_W + xx];
            }
            out[y * PM_W + x] = (float)(s * inv);
        }
}

/* Local contrast normalization (calibrate.py:local_normalize, sigma=6):
 * zero local mean, unit local std. Bandpass that suppresses the low-frequency
 * touch-blob (which correlates across *different* fingers) and equalizes ridge
 * contrast across the frame. r = int(sigma). */
#define PM_LN_R 6
static void local_normalize(const float *in, float *out)
{
    float mean[PM_N], cen[PM_N], var[PM_N], sq[PM_N];
    int i;

    box_blur(in, mean, PM_LN_R);
    for (i = 0; i < PM_N; i++) {
        cen[i] = in[i] - mean[i];
        sq[i] = cen[i] * cen[i];
    }
    box_blur(sq, var, PM_LN_R);
    for (i = 0; i < PM_N; i++) {
        float sd = sqrtf(var[i] > 1e-6f ? var[i] : 1e-6f);
        out[i] = cen[i] / sd;
    }
}

void pm_destripe(const uint8_t raw[PM_N], pm_frame *out)
{
    float tmp[PM_N], ds[PM_N];
    int x, y;

    /* per-column mean subtraction (vertical-coherent FPN) */
    for (x = 0; x < PM_W; x++) {
        double sum = 0.0;
        for (y = 0; y < PM_H; y++)
            sum += raw[y * PM_W + x];
        float cmean = (float)(sum / PM_H);
        for (y = 0; y < PM_H; y++)
            tmp[y * PM_W + x] = (float)raw[y * PM_W + x] - cmean;
    }

    /* per-row mean subtraction (row offset) */
    for (y = 0; y < PM_H; y++) {
        double sum = 0.0;
        for (x = 0; x < PM_W; x++)
            sum += tmp[y * PM_W + x];
        float rmean = (float)(sum / PM_W);
        for (x = 0; x < PM_W; x++)
            ds[y * PM_W + x] = tmp[y * PM_W + x] - rmean;
    }

    /* local contrast normalization — the matcher front-end (HANDOFF §6/§8) */
    local_normalize(ds, out->px);
}

/* NCC over the overlap region for probe shifted by (dx,dy) vs ref:
 * probe pixel (x,y) is compared against ref pixel (x+dx, y+dy). */
static float ncc_at_shift(const pm_frame *probe, const pm_frame *ref,
                          int dx, int dy, int min_overlap)
{
    int x, y, count = 0;
    double sa = 0.0, sb = 0.0;

    /* valid probe-x range so that x+dx is in [0,PM_W) */
    int x0 = (dx < 0) ? -dx : 0;
    int x1 = (dx > 0) ? PM_W - dx : PM_W;
    int y0 = (dy < 0) ? -dy : 0;
    int y1 = (dy > 0) ? PM_H - dy : PM_H;

    if (x1 <= x0 || y1 <= y0)
        return -1.0f;
    count = (x1 - x0) * (y1 - y0);
    if (count < min_overlap)
        return -1.0f;

    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            sa += probe->px[y * PM_W + x];
            sb += ref->px[(y + dy) * PM_W + (x + dx)];
        }
    double ma = sa / count, mb = sb / count;

    double num = 0.0, da = 0.0, db = 0.0;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            double a = probe->px[y * PM_W + x] - ma;
            double b = ref->px[(y + dy) * PM_W + (x + dx)] - mb;
            num += a * b;
            da += a * a;
            db += b * b;
        }
    double denom = sqrt(da * db);
    if (denom < 1e-9)
        return 0.0f;
    return (float)(num / denom);
}

float pm_best_shift_ncc(const pm_frame *probe, const pm_frame *ref,
                        int max_dx, int max_dy, int min_overlap,
                        int *best_dx, int *best_dy)
{
    float best = -1.0f;
    int bdx = 0, bdy = 0, dx, dy;

    for (dy = -max_dy; dy <= max_dy; dy++)
        for (dx = -max_dx; dx <= max_dx; dx++) {
            float s = ncc_at_shift(probe, ref, dx, dy, min_overlap);
            if (s > best) {
                best = s;
                bdx = dx;
                bdy = dy;
            }
        }
    if (best_dx) *best_dx = bdx;
    if (best_dy) *best_dy = bdy;
    return best;
}

float pm_verify(const pm_frame *probe, const pm_frame *gallery, int n,
                int max_dx, int max_dy, int min_overlap)
{
    float best = -1.0f;
    int i;
    for (i = 0; i < n; i++) {
        float s = pm_best_shift_ncc(probe, &gallery[i], max_dx, max_dy,
                                    min_overlap, NULL, NULL);
        if (s > best)
            best = s;
    }
    return best;
}
