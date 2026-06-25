/*
 * Petaic / Silead SIL6250 fingerprint driver (match-on-host)
 *
 * The SIL6250 (Huawei MateBook X Pro 2024) is a 64x80 touch sensor reached over
 * an EC-arbitrated shared-memory mailbox, not USB/SPI.  A companion kernel
 * module (sil6250.ko) brokers the mailbox as /dev/sil6250; all protocol logic
 * (TLS-PSK secure channel, framing, the 0x11->0x37->0x38 capture loop) lives in
 * userspace in petaic_engine.  Matching also runs on the host — the sensor only
 * streams raw images — using a correlation matcher (petaic_match), because the
 * 3.2x4mm patch yields too few minutiae for the NBIS pipeline (see HANDOFF.md).
 *
 * Hence this is an FpDevice (not FpImageDevice, whose NBIS path fails here): we
 * implement enroll/verify/identify ourselves, store the destriped reference
 * frames in the FpPrint (FPI_PRINT_RAW), and score with best-shift NCC.
 *
 * Discovery: the mailbox device is neither hidraw nor spidev, so it is not
 * auto-discovered by libfprint's udev backend.  For now it registers as a
 * virtual-type device keyed on the FP_SIL6250 environment variable, whose value
 * is the device node path (e.g. FP_SIL6250=/dev/sil6250).  Proper udev discovery
 * is a later enhancement (needs the kernel module to expose a recognised node).
 *
 * The engine API is blocking; libfprint is single-threaded async on a GMainLoop.
 * Each interactive action runs on a GTask worker thread; the final completion is
 * marshalled back to the device's main context by GTask, and intermediate enroll
 * progress is posted via g_main_context_invoke.
 */

#define FP_COMPONENT "sil6250"

#include "fpi-device.h"
#include "fpi-print.h"
#include "fpi-log.h"

#include "petaic_engine.h"
#include "petaic_match.h"
#include "petaic_sift.h"

/* Verify/identify scoring is the clean-room SIFT matcher (petaic_sift): a
 * keypoint + SIFT-128 descriptor + translation-RANSAC inlier count, recovered
 * from DllSiMatcher (MATCHER_ANALYSIS.md §B5-B7). The score is the geometric
 * inlier count; accept at >= 5 (the DLL's own len_reject_fa_Inlinenum5 rule).
 * This replaces best-shift NCC, whose genuine/impostor distributions overlapped
 * on real hardware (0.218 vs 0.233); offline ROC on the finger1/finger2 PGM set
 * separates cleanly (overlapping-genuine 5-25 inliers, impostor ceiling 2). */
#define SIL6250_SIFT_THRESHOLD 5

/* NCC is still used for the enroll-diversity gate (cheap whole-frame overlap
 * test to spread the stored gallery across the pad). */
#define SIL6250_MAX_SHIFT     14
#define SIL6250_MIN_OVERLAP   1200
#define SIL6250_ENROLL_STAGES 12
#define SIL6250_FINGER_MS     15000
#define SIL6250_MAX_FRAMES    64

/* Enroll-diversity gate (HANDOFF.md §7 immediate-action #1): reject a freshly
 * captured frame that is near-identical to one already kept, so the finger has
 * to move and the stored gallery spreads across the pad. Without this, fast
 * same-region enroll frames cluster and an off-region verify barely overlaps
 * (live genuine score sat at 0.3229 vs a 0.30 threshold). A frame is redundant
 * when its best-shift NCC against any kept frame exceeds this. */
#define SIL6250_ENROLL_MAX_NCC 0.95f
/* Bound on redundant (rejected) captures before we accept whatever we have, so
 * a user who simply holds still still completes enroll rather than hanging. */
#define SIL6250_ENROLL_MAX_REDUNDANT (SIL6250_ENROLL_STAGES * 4)
/* After accepting an enroll frame, wait up to this long for the finger to lift
 * before the next capture, so each stage is a distinct fresh placement rather
 * than a burst from one continuous touch. */
#define SIL6250_LIFT_MS 4000

/* Capture-quality gate (MATCHER_ANALYSIS.md §B4 / ps_quality): a weak, dry, or
 * partial press carries little coherent ridge flow and matches poorly. Rather
 * than let it become a hard false-reject, reject it at capture time and ask for
 * a re-press. On the clean v../w.. set the only two genuine FRR frames score
 * q~0.48 while every other genuine press is >=0.55 and every impostor <=0.47, so
 * gating below ~0.52 turns the 13% FRR into a 13% "lift & retry" with FRR ~0%.
 * Default overridable via the SIL6250_QUALITY_MIN env var (0 disables). */
#define SIL6250_QUALITY_MIN_DEFAULT PS_QUALITY_MIN
/* Re-press attempts before a frame is accepted regardless, so a user whose
 * finger is simply dry/light still completes rather than the gate looping. */
#define SIL6250_QUALITY_MAX_RETRY 3

/* Multi-frame verify (MATCHER_ANALYSIS.md §B8 step 2): capture several frames in
 * a single verify/identify touch and take the BEST inlier count over them. A
 * single 64x80 frame is intrinsically marginal on separate presses (genuine
 * 4-9 inliers, impostor ceiling 4-5 overlap), but the best of a few jittered
 * frames from one touch overlaps the gallery far more reliably. FAR does not
 * rise: an impostor's every frame is geometrically inconsistent with the
 * gallery, so its max stays at the impostor ceiling. Override via
 * SIL6250_VERIFY_FRAMES (1 disables multi-frame). */
#define SIL6250_VERIFY_FRAMES 3

struct _FpiDeviceSil6250
{
  FpDevice       parent;
  petaic_engine *engine;
  GMainContext  *main_ctx;   /* device's thread-default context, for progress */
};

G_DECLARE_FINAL_TYPE (FpiDeviceSil6250, fpi_device_sil6250, FPI, DEVICE_SIL6250, FpDevice)
G_DEFINE_TYPE (FpiDeviceSil6250, fpi_device_sil6250, FP_TYPE_DEVICE)

/* ---- gallery <-> FpPrint (FPI_PRINT_RAW) serialization ---- */

/* Pack n raw 5120-byte frames into the print's fpi-data as a flat byte array. */
static void
sil6250_print_set_frames (FpPrint *print, const guint8 *frames, guint n)
{
  GVariant *data = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                              frames, (gsize) n * PM_N, 1);

  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, FALSE);
  g_object_set (print, "fpi-data", data, NULL);
}

/* Recover and destripe the reference frames from a stored print. Returns the
 * frame count, or 0 if the print carries no usable data. */
static guint
sil6250_print_get_gallery (FpPrint *print, pm_frame *out, guint max)
{
  g_autoptr(GVariant) data = NULL;
  const guint8 *raw;
  gsize len = 0;
  guint n;

  g_object_get (print, "fpi-data", &data, NULL);
  if (!data)
    return 0;

  raw = g_variant_get_fixed_array (data, &len, 1);
  if (!raw || len < PM_N)
    return 0;

  n = (guint) (len / PM_N);
  if (n > max)
    n = max;
  for (guint i = 0; i < n; i++)
    pm_destripe (raw + (gsize) i * PM_N, &out[i]);
  return n;
}

/* Recover a stored print's gallery and extract SIFT features for each frame.
 * Returns the frame count (0 if the print has no usable data). */
static guint
sil6250_print_get_sift_gallery (FpPrint *print, ps_features *out, guint max)
{
  g_autofree pm_frame *frames = g_new0 (pm_frame, max);
  guint n = sil6250_print_get_gallery (print, frames, max);

  for (guint i = 0; i < n; i++)
    ps_extract (&frames[i], &out[i]);
  return n;
}

/* ---- progress marshaling (worker thread -> main context) ---- */

typedef struct
{
  FpDevice *dev;
  gint      stage;
} ProgressMsg;

static gboolean
post_enroll_progress (gpointer user_data)
{
  ProgressMsg *m = user_data;

  fpi_device_enroll_progress (m->dev, m->stage, NULL, NULL);
  return G_SOURCE_REMOVE;
}

static void
report_enroll_progress (FpiDeviceSil6250 *self, gint stage)
{
  ProgressMsg *m = g_new0 (ProgressMsg, 1);

  m->dev = FP_DEVICE (self);
  m->stage = stage;
  g_main_context_invoke_full (self->main_ctx, G_PRIORITY_DEFAULT,
                              post_enroll_progress, m, g_free);
}

/* ---- open / close (fast; run inline on the main thread) ---- */

static void
sil6250_open (FpDevice *device)
{
  FpiDeviceSil6250 *self = FPI_DEVICE_SIL6250 (device);
  const gchar *path;
  int rc;

  path = fpi_device_get_virtual_env (device);
  if (!path || path[0] == '\0')
    path = "/dev/sil6250";

  self->engine = engine_new ();
  if (!self->engine)
    {
      fpi_device_open_complete (device,
                                fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                          "out of memory"));
      return;
    }
  engine_set_key (self->engine, "shiba");

  rc = engine_open (self->engine, path);
  if (rc)
    {
      engine_free (self->engine);
      self->engine = NULL;
      fpi_device_open_complete (device,
                                fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                          "cannot open %s: %s",
                                                          path, g_strerror (-rc)));
      return;
    }

  self->main_ctx = g_main_context_ref_thread_default ();
  fpi_device_open_complete (device, NULL);
}

static void
sil6250_close (FpDevice *device)
{
  FpiDeviceSil6250 *self = FPI_DEVICE_SIL6250 (device);

  g_clear_pointer (&self->engine, engine_free);
  g_clear_pointer (&self->main_ctx, g_main_context_unref);
  fpi_device_close_complete (device, NULL);
}

/* ---- capture helper (worker thread) ---- */

/* Capture-quality acceptance floor, read once from the environment so it can be
 * tuned without a rebuild (SIL6250_QUALITY_MIN; 0 disables the gate). */
static float
sil6250_quality_min (void)
{
  static float q = -1.0f;

  if (q < 0.0f)
    {
      const char *env = g_getenv ("SIL6250_QUALITY_MIN");
      q = env ? (float) g_ascii_strtod (env, NULL) : SIL6250_QUALITY_MIN_DEFAULT;
    }
  return q;
}

/* Number of frames to capture per verify/identify touch, read once from the
 * environment so it can be tuned without a rebuild (SIL6250_VERIFY_FRAMES). */
static guint
sil6250_verify_frames (void)
{
  static guint n = 0;

  if (n == 0)
    {
      const char *env = g_getenv ("SIL6250_VERIFY_FRAMES");
      gint64 v = env ? g_ascii_strtoll (env, NULL, 10) : SIL6250_VERIFY_FRAMES;
      n = (v >= 1 && v <= SIL6250_MAX_FRAMES) ? (guint) v : SIL6250_VERIFY_FRAMES;
    }
  return n;
}

/* Capture one destriped frame, gating on capture quality: a press whose ridge
 * coherence is below the floor is rejected and re-requested (lift & retry) up to
 * SIL6250_QUALITY_MAX_RETRY times, after which the last frame is accepted so a
 * persistently dry/light finger still completes. This converts the matcher's
 * weak-press false-rejects into a re-press prompt. */
static gboolean
capture_destriped (FpiDeviceSil6250 *self, pm_frame *out,
                   guint8 *raw_out, GError **error)
{
  guint8 raw[PM_N];
  pm_frame local;
  float qmin = sil6250_quality_min ();

  for (int attempt = 0; ; attempt++)
    {
      int rc = engine_capture_frame (self->engine, raw, SIL6250_FINGER_MS);
      if (rc)
        {
          /* A capture miss (no finger, or a transient handshake/mailbox
           * failure right after boot) is recoverable: a re-handshake on the
           * next attempt usually succeeds. Report it in the retry domain so the
           * caller re-prompts rather than aborting the operation, which would
           * otherwise leave the fprintd claim dangling ("Device was already
           * claimed"). See verify_done/identify_done. */
          g_set_error (error, FP_DEVICE_RETRY, FP_DEVICE_RETRY_GENERAL,
                       "capture failed: %s", g_strerror (-rc));
          return FALSE;
        }

      pm_destripe (raw, &local);
      float q = ps_quality (&local);
      gboolean ok = (q >= qmin) || (attempt >= SIL6250_QUALITY_MAX_RETRY);
      fp_dbg ("capture quality %.3f (min %.3f, attempt %d)%s",
              q, qmin, attempt, ok ? "" : " -> retry");
      if (!ok)
        {
          /* Poor press: require a real lift before re-sampling. */
          engine_wait_finger_up (self->engine, SIL6250_LIFT_MS);
          continue;
        }

      if (raw_out)
        memcpy (raw_out, raw, PM_N);
      if (out)
        *out = local;
      return TRUE;
    }
}

/* ---- enroll ---- */

static void
enroll_thread (GTask        *task,
               gpointer      source,
               gpointer      task_data,
               GCancellable *cancellable)
{
  FpiDeviceSil6250 *self = source;
  g_autofree guint8 *frames = g_malloc0 ((gsize) SIL6250_ENROLL_STAGES * PM_N);
  g_autofree pm_frame *kept = g_new0 (pm_frame, SIL6250_ENROLL_STAGES);
  GError *error = NULL;
  guint got = 0;
  guint redundant = 0;

  (void) task_data;
  while (got < SIL6250_ENROLL_STAGES)
    {
      pm_frame cand;
      gboolean too_similar = FALSE;

      if (g_task_return_error_if_cancelled (task))
        return;

      if (!capture_destriped (self, &cand, frames + (gsize) got * PM_N, &error))
        {
          /* A single failed frame (finger moved/timed out) is not fatal; retry
           * this stage a couple of times before giving up. */
          g_clear_error (&error);
          if (g_cancellable_is_cancelled (cancellable))
            continue;
          if (++redundant >= SIL6250_ENROLL_MAX_REDUNDANT)
            break;    /* runaway guard */
          continue;
        }

      /* Enroll-diversity gate: drop the frame if it overlaps an already-kept
       * one too closely, so the finger must move and the gallery spreads. */
      for (guint i = 0; i < got; i++)
        {
          float ncc = pm_best_shift_ncc (&cand, &kept[i],
                                         SIL6250_MAX_SHIFT, SIL6250_MAX_SHIFT,
                                         SIL6250_MIN_OVERLAP, NULL, NULL);
          if (ncc > SIL6250_ENROLL_MAX_NCC)
            {
              too_similar = TRUE;
              break;
            }
        }
      if (too_similar)
        {
          /* Don't advance; ask the user (implicitly) to reposition. Accept what
           * we have once the budget is spent so a still finger still completes. */
          if (++redundant >= SIL6250_ENROLL_MAX_REDUNDANT)
            break;
        }
      else
        {
          kept[got] = cand;
          got++;
          report_enroll_progress (self, (gint) got);
        }

      /* Whether the frame was kept or dropped as a duplicate, require a real
       * lift + re-press before the next capture. Without this the loop spins on
       * one continuous touch: re-capturing the same still finger, the gate
       * rejecting each as a dup, bursting frames the user can't keep up with. */
      if (got < SIL6250_ENROLL_STAGES)
        engine_wait_finger_up (self->engine, SIL6250_LIFT_MS);
    }

  if (got < 2)
    {
      g_task_return_new_error (task, FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL,
                               "enroll captured too few frames (%u)", got);
      return;
    }

  {
    FpPrint *print = NULL;
    fpi_device_get_enroll_data (FP_DEVICE (self), &print);
    sil6250_print_set_frames (print, frames, got);
    g_task_return_pointer (task, g_object_ref (print), g_object_unref);
  }
}

static void
enroll_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  FpDevice *dev = FP_DEVICE (source);
  GError *error = NULL;
  FpPrint *print = g_task_propagate_pointer (G_TASK (res), &error);

  (void) user_data;
  fpi_device_enroll_complete (dev, print, error);
}

static void
sil6250_enroll (FpDevice *device)
{
  FpiDeviceSil6250 *self = FPI_DEVICE_SIL6250 (device);
  g_autoptr(GTask) task = g_task_new (self, fpi_device_get_cancellable (device),
                                      enroll_done, NULL);

  g_task_run_in_thread (task, enroll_thread);
}

/* ---- verify ---- */

static void
verify_thread (GTask        *task,
               gpointer      source,
               gpointer      task_data,
               GCancellable *cancellable)
{
  FpiDeviceSil6250 *self = source;
  FpPrint *template = task_data;
  g_autofree ps_features *gallery = g_new0 (ps_features, SIL6250_MAX_FRAMES);
  g_autofree ps_features *probe = g_new0 (ps_features, 1);
  pm_frame probe_frame;
  GError *error = NULL;
  guint n;
  guint nframes = sil6250_verify_frames ();
  int best = -1;
  gboolean got_any = FALSE;

  (void) cancellable;
  n = sil6250_print_get_sift_gallery (template, gallery, SIL6250_MAX_FRAMES);
  if (n == 0)
    {
      g_task_return_new_error (task, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
                               "stored print has no usable frames");
      return;
    }

  /* Multi-frame verify: capture up to nframes frames from this one touch and
   * take the best inlier count. Stop early once a confident match is found so
   * the user need not hold longer than necessary. The frames come from one
   * continuous press (no lift between them), so each capture_destriped call
   * grabs a fresh jittered frame of the same finger. */
  for (guint f = 0; f < nframes; f++)
    {
      int s;

      if (g_task_return_error_if_cancelled (task))
        return;
      if (!capture_destriped (self, &probe_frame, NULL, &error))
        {
          if (!got_any)
            {
              g_task_return_error (task, error);
              return;
            }
          /* Finger lifted/timed out after >=1 good frame: score what we have. */
          g_clear_error (&error);
          break;
        }
      got_any = TRUE;
      ps_extract (&probe_frame, probe);
      s = ps_verify (probe, gallery, (int) n);
      fp_dbg ("verify frame %u/%u inliers %d", f + 1, nframes, s);
      if (s > best)
        best = s;
      if (best >= SIL6250_SIFT_THRESHOLD)
        break;
    }

  fp_dbg ("verify best inliers %d (threshold %d)", best, SIL6250_SIFT_THRESHOLD);
  g_task_return_int (task, best >= SIL6250_SIFT_THRESHOLD ? FPI_MATCH_SUCCESS
                                                          : FPI_MATCH_FAIL);
}

static void
verify_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  FpDevice *dev = FP_DEVICE (source);
  GError *error = NULL;
  gssize result = g_task_propagate_int (G_TASK (res), &error);

  (void) user_data;
  if (error)
    {
      /* Retry-domain errors are per-attempt: report them and complete with no
       * error so fprintd re-prompts and releases the claim cleanly. Any other
       * (hard) error must go through complete() ALONE — reporting AND completing
       * with an error violates the libfprint contract and strands the claim. */
      if (error->domain == FP_DEVICE_RETRY)
        {
          fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL, error);
          fpi_device_verify_complete (dev, NULL);
        }
      else
        {
          fpi_device_verify_complete (dev, error);
        }
      return;
    }

  {
    FpPrint *template = NULL;
    fpi_device_get_verify_data (dev, &template);
    fpi_device_verify_report (dev, (FpiMatchResult) result, template, NULL);
  }
  fpi_device_verify_complete (dev, NULL);
}

static void
sil6250_verify (FpDevice *device)
{
  FpiDeviceSil6250 *self = FPI_DEVICE_SIL6250 (device);
  FpPrint *template = NULL;
  g_autoptr(GTask) task = NULL;

  fpi_device_get_verify_data (device, &template);
  task = g_task_new (self, fpi_device_get_cancellable (device),
                     verify_done, NULL);
  g_task_set_task_data (task, g_object_ref (template), g_object_unref);
  g_task_run_in_thread (task, verify_thread);
}

/* ---- identify ---- */

static void
identify_thread (GTask        *task,
                 gpointer      source,
                 gpointer      task_data,
                 GCancellable *cancellable)
{
  FpiDeviceSil6250 *self = source;
  GPtrArray *gallery_prints = task_data;
  g_autofree ps_features *gallery = g_new0 (ps_features, SIL6250_MAX_FRAMES);
  guint nframes = sil6250_verify_frames ();
  g_autofree ps_features *probes = g_new0 (ps_features, nframes);
  pm_frame probe_frame;
  GError *error = NULL;
  gint best_idx = -1;
  int best = -1;
  guint np = 0;

  (void) cancellable;
  /* Multi-frame: capture up to nframes frames from the one touch first, then
   * score each gallery print once against every probe frame (so each gallery is
   * SIFT-extracted only once). The print's best frame-vs-frame inlier count is
   * its score; the overall best over prints decides the identity. */
  for (guint f = 0; f < nframes; f++)
    {
      if (g_task_return_error_if_cancelled (task))
        return;
      if (!capture_destriped (self, &probe_frame, NULL, &error))
        {
          if (np == 0)
            {
              g_task_return_error (task, error);
              return;
            }
          g_clear_error (&error);
          break;
        }
      ps_extract (&probe_frame, &probes[np++]);
    }

  for (guint i = 0; i < gallery_prints->len; i++)
    {
      FpPrint *cand = g_ptr_array_index (gallery_prints, i);
      guint n = sil6250_print_get_sift_gallery (cand, gallery, SIL6250_MAX_FRAMES);

      if (n == 0)
        continue;
      for (guint f = 0; f < np; f++)
        {
          int s = ps_verify (&probes[f], gallery, (int) n);
          if (s > best)
            {
              best = s;
              best_idx = (gint) i;
            }
        }
    }

  fp_dbg ("identify best inliers %d over %u frames (threshold %d)",
          best, np, SIL6250_SIFT_THRESHOLD);
  g_task_return_int (task, best >= SIL6250_SIFT_THRESHOLD ? best_idx : -1);
}

static void
identify_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  FpDevice *dev = FP_DEVICE (source);
  GError *error = NULL;
  gssize idx = g_task_propagate_int (G_TASK (res), &error);

  (void) user_data;
  if (error)
    {
      /* See verify_done: retry-domain errors go through report() + a clean
       * complete(); hard errors through complete() alone. */
      if (error->domain == FP_DEVICE_RETRY)
        {
          fpi_device_identify_report (dev, NULL, NULL, error);
          fpi_device_identify_complete (dev, NULL);
        }
      else
        {
          fpi_device_identify_complete (dev, error);
        }
      return;
    }

  {
    GPtrArray *prints = NULL;
    FpPrint *match = NULL;
    fpi_device_get_identify_data (dev, &prints);
    if (idx >= 0 && (guint) idx < prints->len)
      match = g_ptr_array_index (prints, idx);
    fpi_device_identify_report (dev, match, NULL, NULL);
  }
  fpi_device_identify_complete (dev, NULL);
}

static void
sil6250_identify (FpDevice *device)
{
  FpiDeviceSil6250 *self = FPI_DEVICE_SIL6250 (device);
  GPtrArray *prints = NULL;
  g_autoptr(GTask) task = NULL;

  fpi_device_get_identify_data (device, &prints);
  task = g_task_new (self, fpi_device_get_cancellable (device),
                     identify_done, NULL);
  g_task_set_task_data (task, g_ptr_array_ref (prints),
                        (GDestroyNotify) g_ptr_array_unref);
  g_task_run_in_thread (task, identify_thread);
}

/* ---- class / type ---- */

static void
fpi_device_sil6250_init (FpiDeviceSil6250 *self)
{
  (void) self;
}

static const FpIdEntry sil6250_id_table[] = {
  { .virtual_envvar = "FP_SIL6250" },
  { .virtual_envvar = NULL },
};

static void
fpi_device_sil6250_class_init (FpiDeviceSil6250Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id = "sil6250";
  dev_class->full_name = "Petaic/Silead SIL6250 (match-on-host)";
  dev_class->type = FP_DEVICE_TYPE_VIRTUAL;
  dev_class->id_table = sil6250_id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = SIL6250_ENROLL_STAGES;
  dev_class->temp_hot_seconds = -1;

  dev_class->open = sil6250_open;
  dev_class->close = sil6250_close;
  dev_class->enroll = sil6250_enroll;
  dev_class->verify = sil6250_verify;
  dev_class->identify = sil6250_identify;

  fpi_device_class_auto_initialize_features (dev_class);
}
