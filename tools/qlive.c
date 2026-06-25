/* qlive.c — live capture-quality gate test.
 *
 * Uses the SAME engine capture path as the libfprint driver, computes ps_quality
 * on each press, and prints PASS / GATE against the threshold — so you can press
 * firm vs. light/dry and watch the gate decide in real time. This validates the
 * capture-side quality gate on real hardware before trusting it in enroll/verify.
 *
 *   make qlive
 *   ./qlive                 # default threshold (PS_QUALITY_MIN = 0.52)
 *   ./qlive --thresh 0.50   # override floor
 *   ./qlive --n 10          # stop after 10 presses (default: until Ctrl-C)
 *
 * Press the finger, read the score, lift, repeat. A firm full press should read
 * PASS (q >= thresh); a light/dry/partial press should read GATE.
 */
#include "petaic_engine.h"
#include "petaic_match.h"
#include "petaic_sift.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static volatile sig_atomic_t stop = 0;
static void on_sigint(int sig) { (void) sig; stop = 1; }

int main(int argc, char **argv)
{
    const char *dev = "/dev/sil6250";
    float thresh = PS_QUALITY_MIN;
    int nmax = 0, verbose = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--thresh") && i + 1 < argc) thresh = atof(argv[++i]);
        else if (!strcmp(argv[i], "--n") && i + 1 < argc) nmax = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dev") && i + 1 < argc) dev = argv[++i];
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
    }

    petaic_engine *e = engine_new();
    if (!e) { fprintf(stderr, "engine_new failed\n"); return 1; }
    engine_set_verbose(e, verbose);
    if (engine_open(e, dev) != 0) {
        fprintf(stderr, "engine_open(%s) failed\n", dev);
        engine_free(e);
        return 1;
    }

    signal(SIGINT, on_sigint);
    printf("Live quality gate — threshold %.3f. Press finger; Ctrl-C to stop.\n",
           thresh);

    int count = 0, passed = 0, gated = 0;
    while (!stop && (nmax == 0 || count < nmax)) {
        uint8_t raw[PE_IMG_SIZE];
        int rc = engine_capture_frame(e, raw, 15000);
        if (rc != 0) {
            if (rc == -110) continue;          /* -ETIMEDOUT: no finger, keep waiting */
            fprintf(stderr, "capture failed: %d\n", rc);
            break;
        }

        pm_frame fr;
        pm_destripe(raw, &fr);
        float q = ps_quality(&fr);
        int pass = q >= thresh;
        count++;
        if (pass) passed++; else gated++;
        printf("  press %2d:  q = %.3f   %s\n", count, q,
               pass ? "PASS  (accept)" : "GATE  (lift & retry)");
        fflush(stdout);

        /* require a real lift before the next reading */
        engine_wait_finger_up(e, 4000);
    }

    printf("\n%d presses: %d PASS, %d GATE (threshold %.3f)\n",
           count, passed, gated, thresh);
    engine_close(e);
    engine_free(e);
    return 0;
}
