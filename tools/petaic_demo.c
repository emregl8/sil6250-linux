/* petaic_demo.c — end-to-end Track-A demo: engine capture + correlation matcher,
 * no libfprint.  Proves the enroll/verify pipeline the FpDevice driver will wrap.
 *
 *   petaic_demo enroll --out finger.gal [--n 12] [--key shiba] [-v]
 *   petaic_demo verify --gal finger.gal [--thresh 0.30] [--key shiba] [-v]
 *
 * Gallery file format: "PGAL" magic, u32 count, then count * 5120 raw frames
 * (raw so the destripe/match front-end can evolve without re-enrolling).
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "petaic_engine.h"
#include "petaic_match.h"

#define GAL_MAGIC "PGAL"
#define MAX_GAL   64
#define FINGER_MS 15000

static int gal_save(const char *path, const uint8_t frames[][PM_N], uint32_t n)
{
	FILE *f = fopen(path, "wb");
	if (!f) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); return -1; }
	fwrite(GAL_MAGIC, 1, 4, f);
	fwrite(&n, sizeof(n), 1, f);
	for (uint32_t i = 0; i < n; i++)
		fwrite(frames[i], 1, PM_N, f);
	fclose(f);
	return 0;
}

static int gal_load(const char *path, uint8_t frames[][PM_N], uint32_t *n)
{
	FILE *f = fopen(path, "rb");
	char magic[4];
	if (!f) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); return -1; }
	if (fread(magic, 1, 4, f) != 4 || memcmp(magic, GAL_MAGIC, 4)) {
		fprintf(stderr, "%s: bad magic\n", path); fclose(f); return -1;
	}
	if (fread(n, sizeof(*n), 1, f) != 1 || *n == 0 || *n > MAX_GAL) {
		fprintf(stderr, "%s: bad count\n", path); fclose(f); return -1;
	}
	for (uint32_t i = 0; i < *n; i++)
		if (fread(frames[i], 1, PM_N, f) != PM_N) {
			fprintf(stderr, "%s: short read\n", path); fclose(f); return -1;
		}
	fclose(f);
	return 0;
}

static petaic_engine *open_engine(const char *key, int verbose)
{
	petaic_engine *e = engine_new();
	if (!e) { fprintf(stderr, "OOM\n"); return NULL; }
	engine_set_verbose(e, verbose);
	if (key && engine_set_key(e, key)) {
		fprintf(stderr, "unknown key '%s'\n", key); engine_free(e); return NULL;
	}
	int rc = engine_open(e, "/dev/sil6250");
	if (rc) {
		fprintf(stderr, "engine_open: %s\n", strerror(-rc));
		engine_free(e); return NULL;
	}
	return e;
}

static int cmd_enroll(const char *out, int n, const char *key, int verbose)
{
	static uint8_t frames[MAX_GAL][PM_N];
	if (n < 1 || n > MAX_GAL) n = 12;

	petaic_engine *e = open_engine(key, verbose);
	if (!e) return 1;

	int got = 0;
	for (int i = 0; i < n; i++) {
		fprintf(stderr, "[enroll] frame %d/%d — place/roll your finger...\n", i + 1, n);
		int rc = engine_capture_frame(e, frames[got], FINGER_MS);
		if (rc) {
			fprintf(stderr, "[enroll] frame %d failed (%s); skipping\n",
				i + 1, strerror(-rc));
			continue;
		}
		got++;
		fprintf(stderr, "[enroll] captured %d/%d — LIFT finger, then place again\n", got, n);
	}
	engine_free(e);

	if (got < 2) { fprintf(stderr, "[enroll] too few frames (%d)\n", got); return 1; }
	if (gal_save(out, frames, got)) return 1;
	fprintf(stderr, "[enroll] saved %d frames -> %s\n", got, out);
	return 0;
}

static int cmd_verify(const char *gal, float thresh, const char *key, int verbose)
{
	static uint8_t raw[MAX_GAL][PM_N];
	static pm_frame gallery[MAX_GAL];
	uint32_t n = 0;

	if (gal_load(gal, raw, &n)) return 1;
	for (uint32_t i = 0; i < n; i++)
		pm_destripe(raw[i], &gallery[i]);

	petaic_engine *e = open_engine(key, verbose);
	if (!e) return 1;

	uint8_t probe_raw[PM_N];
	fprintf(stderr, "[verify] place your finger...\n");
	int rc = engine_capture_frame(e, probe_raw, FINGER_MS);
	engine_free(e);
	if (rc) { fprintf(stderr, "[verify] capture failed: %s\n", strerror(-rc)); return 1; }

	pm_frame probe;
	pm_destripe(probe_raw, &probe);
	float score = pm_verify(&probe, gallery, (int)n, 14, 14, 1200);

	printf("score %.4f  threshold %.4f  -> %s\n",
	       score, thresh, score >= thresh ? "ACCEPT" : "REJECT");
	return score >= thresh ? 0 : 2;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage:\n"
			"  %s enroll --out finger.gal [--n 12] [--key shiba] [-v]\n"
			"  %s verify --gal finger.gal [--thresh 0.30] [--key shiba] [-v]\n",
			argv[0], argv[0]);
		return 2;
	}
	const char *out = "finger.gal", *gal = "finger.gal", *key = "shiba";
	int n = 12, verbose = 0;
	float thresh = 0.30f;

	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
		else if (!strcmp(argv[i], "--gal") && i + 1 < argc) gal = argv[++i];
		else if (!strcmp(argv[i], "--key") && i + 1 < argc) key = argv[++i];
		else if (!strcmp(argv[i], "--n") && i + 1 < argc) n = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--thresh") && i + 1 < argc) thresh = atof(argv[++i]);
		else if (!strcmp(argv[i], "-v")) verbose = 1;
		else { fprintf(stderr, "bad arg %s\n", argv[i]); return 2; }
	}

	if (!strcmp(argv[1], "enroll"))
		return cmd_enroll(out, n, key, verbose);
	if (!strcmp(argv[1], "verify"))
		return cmd_verify(gal, thresh, key, verbose);
	fprintf(stderr, "unknown command '%s'\n", argv[1]);
	return 2;
}
