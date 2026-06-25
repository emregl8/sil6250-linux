/* petaic_sift_roc.c — offline ROC for the clean-room SIFT matcher (petaic_sift).
 *
 * Validates inlier-count separation between genuine (finger1) and impostor
 * (finger2) frames against the oracle ground truth in MATCHER_ANALYSIS.md, with
 * no hardware needed. Mirrors petaic_roc.c's CLI.
 *
 *   make petaic_sift_roc
 *   ./petaic_sift_roc --gallery q1 q3 q5 q7 q9 p1 \
 *                     --genuine q2 q4 q6 q8 q10 fp_0 fp_1 fp_2 p2 p3 p4 \
 *                     --impostor g2_0 g2_1 ... g2_9
 *   ./petaic_sift_roc --loo <all-finger1> --impostor <all-finger2>
 *
 * Paths may omit the .pgm suffix.
 */
#include "petaic_sift.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAXF 64

static int read_pgm(const char *path, uint8_t out[PM_N])
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", path);
    /* allow omitting .pgm */
    FILE *f = fopen(buf, "rb");
    if (!f) {
        snprintf(buf, sizeof(buf), "%s.pgm", path);
        f = fopen(buf, "rb");
    }
    if (!f) { fprintf(stderr, "open %s\n", path); return -1; }
    char magic[3] = {0};
    if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P5") != 0) {
        fprintf(stderr, "%s: not P5\n", path); fclose(f); return -1;
    }
    int w, h, maxv;
    if (fscanf(f, " %d %d %d", &w, &h, &maxv) != 3) {
        fprintf(stderr, "%s: bad header\n", path); fclose(f); return -1;
    }
    if (w != PM_W || h != PM_H) {
        fprintf(stderr, "%s: %dx%d not %dx%d\n", path, w, h, PM_W, PM_H);
        fclose(f); return -1;
    }
    fgetc(f);
    if (fread(out, 1, PM_N, f) != PM_N) {
        fprintf(stderr, "%s: short read\n", path); fclose(f); return -1;
    }
    fclose(f);
    return 0;
}

static int load_feats(char **paths, int n, ps_features *feats, float *qual)
{
    uint8_t raw[PM_N];
    pm_frame fr;
    for (int i = 0; i < n; i++) {
        if (read_pgm(paths[i], raw) < 0) return -1;
        pm_destripe(raw, &fr);
        if (qual) qual[i] = ps_quality(&fr);
        ps_extract(&fr, &feats[i]);
    }
    return 0;
}

int main(int argc, char **argv)
{
    char *gal[MAXF], *gen[MAXF], *imp[MAXF];
    int ng = 0, npos = 0, nimp = 0, loo = 0;
    int mode = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--gallery"))  { mode = 1; continue; }
        if (!strcmp(argv[i], "--loo"))      { mode = 1; loo = 1; continue; }
        if (!strcmp(argv[i], "--genuine"))  { mode = 2; continue; }
        if (!strcmp(argv[i], "--impostor")) { mode = 3; continue; }
        switch (mode) {
        case 1: gal[ng++] = argv[i]; break;
        case 2: gen[npos++] = argv[i]; break;
        case 3: imp[nimp++] = argv[i]; break;
        default: fprintf(stderr, "stray arg %s\n", argv[i]); return 1;
        }
    }
    if (ng == 0) { fprintf(stderr, "need --gallery or --loo frames\n"); return 1; }

    /* tunable accept threshold + quality floor (env, for fast sweeps) */
    int   thr  = getenv("PS_THRESH")  ? atoi(getenv("PS_THRESH"))   : 5;
    float qmin = getenv("PS_QMIN")    ? atof(getenv("PS_QMIN"))     : PS_QUALITY_MIN;

    static ps_features galf[MAXF], genf[MAXF], impf[MAXF];
    static float galq[MAXF], genq[MAXF], impq[MAXF];
    if (load_feats(gal, ng, galf, galq) < 0) return 1;
    if (npos && load_feats(gen, npos, genf, genq) < 0) return 1;
    if (nimp && load_feats(imp, nimp, impf, impq) < 0) return 1;

    printf("== keypoint counts + quality (thr=%d qmin=%.2f) ==\n", thr, qmin);
    for (int i = 0; i < ng; i++)
        printf("  gallery %-8s kpts=%-3d q=%.3f%s\n", gal[i], galf[i].n, galq[i],
               galq[i] < qmin ? "  [LOW-Q]" : "");

    if (loo) {
        /* leave-one-out over the gallery set as genuine probes.
         * The quality gate models capture-time rejection: a low-q press is a
         * "lift & retry", NOT a false reject, so it is excluded from FRR. */
        printf("\n== LOO genuine (gallery minus self) ==\n");
        int gmin = 1 << 30, gmax = 0;
        int reject = 0, gated = 0, scored = 0;
        for (int i = 0; i < ng; i++) {
            int best = 0, bj = -1;
            for (int j = 0; j < ng; j++) {
                if (j == i) continue;
                int s = ps_match(&galf[i], &galf[j], NULL, NULL);
                if (s > best) { best = s; bj = j; }
            }
            int low = galq[i] < qmin;
            printf("  %-8s inliers=%-3d (vs %s)%s%s\n", gal[i], best,
                   bj >= 0 ? gal[bj] : "-",
                   best < thr ? "  REJECT" : "",
                   low ? "  [gated->retry]" : "");
            if (low) { gated++; continue; }   /* re-pressed, not counted */
            scored++;
            if (best < thr) reject++;
            if (best < gmin) gmin = best;
            if (best > gmax) gmax = best;
        }
        printf("  genuine (post-gate) inliers: min=%d max=%d\n", gmin, gmax);
        printf("  gated (retry): %d/%d   FRR = %d/%d = %.1f%%\n",
               gated, ng, reject, scored, scored ? 100.0*reject/scored : 0.0);
    }

    if (npos) {
        printf("\n== genuine probes vs full gallery ==\n");
        for (int i = 0; i < npos; i++) {
            int s = ps_verify(&genf[i], galf, ng);
            printf("  %-8s inliers=%d\n", gen[i], s);
        }
    }

    if (nimp) {
        printf("\n== impostor probes vs full gallery ==\n");
        int imax = 0, accept = 0;
        for (int i = 0; i < nimp; i++) {
            int s = ps_verify(&impf[i], galf, ng);
            printf("  %-8s inliers=%-3d q=%.3f%s\n", imp[i], s, impq[i],
                   s >= thr ? "  ACCEPT(FA)" : "");
            if (s > imax) imax = s;
            if (s >= thr) accept++;
        }
        printf("  impostor inlier ceiling: %d   FAR = %d/%d = %.1f%%\n",
               imax, accept, nimp, nimp ? 100.0*accept/nimp : 0.0);
    }
    return 0;
}
