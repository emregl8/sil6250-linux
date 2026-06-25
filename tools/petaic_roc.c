/* petaic_roc.c — offline ROC for the correlation matcher (HANDOFF.md §7 A0).
 *
 * Builds a gallery from the first finger's frames, then scores genuine probes
 * (same finger, held-out frames) and impostor probes (a different finger) by
 * pm_verify = max best-shift NCC over the gallery. Prints the two score
 * distributions and sweeps a threshold to report FAR/FRR — no libfprint, no
 * virtual-image socket.
 *
 * Usage:
 *   petaic_roc --gallery g1.pgm g2.pgm ... --genuine p.pgm ... --impostor q.pgm ...
 *   petaic_roc --loo finger1_*.pgm --impostor finger2_*.pgm
 * --loo: leave-one-out over the finger1 set — each frame scored against all the
 *   others (the honest gallery-coverage test); impostors scored vs the full set.
 * Options (before the lists): --maxshift N (default 14), --minov N (default 1200).
 *
 * Build: see Makefile (petaic_roc target).
 */
#include "petaic_match.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXF 64

static int read_pgm(const char *path, uint8_t out[PM_N])
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "open %s\n", path); return -1; }
    char magic[3] = {0};
    if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P5") != 0) {
        fprintf(stderr, "%s: not P5\n", path); fclose(f); return -1;
    }
    int w, h, maxv, c;
    /* skip whitespace/comments, then read w h maxv */
    if (fscanf(f, " %d %d %d", &w, &h, &maxv) != 3) {
        fprintf(stderr, "%s: bad header\n", path); fclose(f); return -1;
    }
    if (w != PM_W || h != PM_H) {
        fprintf(stderr, "%s: %dx%d not %dx%d\n", path, w, h, PM_W, PM_H);
        fclose(f); return -1;
    }
    c = fgetc(f);                      /* single whitespace after maxv */
    (void)c;
    if (fread(out, 1, PM_N, f) != PM_N) {
        fprintf(stderr, "%s: short read\n", path); fclose(f); return -1;
    }
    fclose(f);
    return 0;
}

static int load_frames(char **paths, int n, pm_frame *frames)
{
    uint8_t raw[PM_N];
    for (int i = 0; i < n; i++) {
        if (read_pgm(paths[i], raw) < 0)
            return -1;
        pm_destripe(raw, &frames[i]);
    }
    return 0;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    int maxshift = 14, minov = 1200, loo = 0;
    char *gal[MAXF], *gen[MAXF], *imp[MAXF];
    int ng = 0, npos = 0, nimp = 0;
    int mode = 0;   /* 1=gallery 2=genuine 3=impostor */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--gallery"))  { mode = 1; continue; }
        if (!strcmp(argv[i], "--loo"))      { mode = 1; loo = 1; continue; }
        if (!strcmp(argv[i], "--genuine"))  { mode = 2; continue; }
        if (!strcmp(argv[i], "--impostor")) { mode = 3; continue; }
        if (!strcmp(argv[i], "--maxshift")) { maxshift = atoi(argv[++i]); continue; }
        if (!strcmp(argv[i], "--minov"))    { minov = atoi(argv[++i]); continue; }
        switch (mode) {
        case 1: gal[ng++]  = argv[i]; break;
        case 2: gen[npos++] = argv[i]; break;
        case 3: imp[nimp++] = argv[i]; break;
        default: fprintf(stderr, "stray arg %s\n", argv[i]); return 1;
        }
    }
    if (ng == 0 || (npos == 0 && nimp == 0)) {
        fprintf(stderr, "need --gallery and at least one probe list\n");
        return 1;
    }

    static pm_frame gframes[MAXF], pframes[MAXF], iframes[MAXF];
    if (load_frames(gal, ng, gframes) < 0) return 1;
    if (load_frames(gen, npos, pframes) < 0) return 1;
    if (load_frames(imp, nimp, iframes) < 0) return 1;

    printf("gallery=%d genuine=%d impostor=%d  maxshift=%d minov=%d\n\n",
           ng, npos, nimp, maxshift, minov);

    float gscore[MAXF], iscore[MAXF];
    if (loo) {
        npos = ng;
        printf("== genuine (leave-one-out over finger1) ==\n");
        for (int i = 0; i < ng; i++) {
            float best = -1.0f;
            for (int j = 0; j < ng; j++) {
                if (j == i) continue;
                float s = pm_best_shift_ncc(&gframes[i], &gframes[j],
                                            maxshift, maxshift, minov, NULL, NULL);
                if (s > best) best = s;
            }
            gscore[i] = best;
            printf("  %-18s  %.4f\n", gal[i], best);
        }
    } else {
        printf("== genuine (same finger, held out) ==\n");
        for (int i = 0; i < npos; i++) {
            gscore[i] = pm_verify(&pframes[i], gframes, ng, maxshift, maxshift, minov);
            printf("  %-18s  %.4f\n", gen[i], gscore[i]);
        }
    }
    printf("\n== impostor (different finger) ==\n");
    for (int i = 0; i < nimp; i++) {
        iscore[i] = pm_verify(&iframes[i], gframes, ng, maxshift, maxshift, minov);
        printf("  %-18s  %.4f\n", imp[i], iscore[i]);
    }

    /* summary stats + threshold sweep */
    qsort(gscore, npos, sizeof(float), cmp_float);
    qsort(iscore, nimp, sizeof(float), cmp_float);
    printf("\n== separation ==\n");
    if (npos) printf("  genuine  min=%.4f  median=%.4f  max=%.4f\n",
                     gscore[0], gscore[npos/2], gscore[npos-1]);
    if (nimp) printf("  impostor min=%.4f  median=%.4f  max=%.4f\n",
                     iscore[0], iscore[nimp/2], iscore[nimp-1]);

    if (npos && nimp) {
        printf("\n== threshold sweep (FRR=genuine rejected, FAR=impostor accepted) ==\n");
        printf("  thresh   FRR     FAR\n");
        for (float t = 0.20f; t <= 0.951f; t += 0.05f) {
            int fr = 0, fa = 0;
            for (int i = 0; i < npos; i++) if (gscore[i] < t) fr++;
            for (int i = 0; i < nimp; i++) if (iscore[i] >= t) fa++;
            printf("  %.2f   %5.1f%%  %5.1f%%\n",
                   t, 100.0 * fr / npos, 100.0 * fa / nimp);
        }
        /* best separating threshold = midpoint of the gap, if any */
        if (iscore[nimp-1] < gscore[0])
            printf("\n  CLEAN GAP: impostor max %.4f < genuine min %.4f  -> threshold %.4f\n",
                   iscore[nimp-1], gscore[0], 0.5f * (iscore[nimp-1] + gscore[0]));
        else
            printf("\n  overlap: impostor max %.4f >= genuine min %.4f\n",
                   iscore[nimp-1], gscore[0]);
    }
    return 0;
}
