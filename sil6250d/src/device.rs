use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use tokio::sync::{Mutex, Notify};
use zbus::{interface, object_server::SignalEmitter};

use crate::engine::{Engine, Features, Frame};
use crate::storage;

const ENROLL_STAGES: u32 = 12;
const FINGER_MS: u32 = 15_000;
const LIFT_MS: u32 = 4_000;
const MAX_SHIFT: i32 = 14;
const MIN_OVERLAP: i32 = 1200;
const ENROLL_MAX_NCC: f32 = 0.95;
const ENROLL_MAX_REDUNDANT: u32 = ENROLL_STAGES * 4;
const QUALITY_MIN: f32 = 0.52;
const QUALITY_MAX_RETRY: u32 = 3;
const SIFT_THRESHOLD: i32 = 5;
const VERIFY_FRAMES: u32 = 3;

pub const OBJECT_PATH: &str = "/io/github/uunicorn/Fprint/Device";

#[derive(Default)]
struct State {
    suspended: bool,
}

pub struct DeviceService {
    devpath: String,
    state: Arc<Mutex<State>>,
    cancelled: Arc<AtomicBool>,
    resume_notify: Arc<Notify>,
}

impl DeviceService {
    pub fn new(devpath: String) -> Self {
        DeviceService {
            devpath,
            state: Arc::default(),
            cancelled: Arc::new(AtomicBool::new(false)),
            resume_notify: Arc::new(Notify::new()),
        }
    }

    fn reset_cancel(&self) {
        self.cancelled.store(false, Ordering::Relaxed);
    }

    fn do_cancel(&self) {
        self.cancelled.store(true, Ordering::Relaxed);
    }
}

#[interface(name = "io.github.uunicorn.Fprint.Device")]
impl DeviceService {
    #[zbus(property)]
    fn name(&self) -> &str {
        "sil6250"
    }

    #[zbus(property)]
    fn num_enroll_stages(&self) -> u32 {
        ENROLL_STAGES
    }

    #[zbus(property)]
    fn scan_type(&self) -> &str {
        "press"
    }

    async fn list_enrolled_fingers(&self, username: &str) -> Vec<String> {
        storage::list_enrolled(username)
    }

    async fn delete_enrolled_fingers(&self, username: &str) -> zbus::fdo::Result<()> {
        storage::delete_enrolled(username)
            .map_err(|e| zbus::fdo::Error::Failed(format!("delete failed: {e}")))
    }

    async fn enroll_start(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        username: &str,
        finger_name: &str,
    ) -> zbus::fdo::Result<()> {
        if self.state.lock().await.suspended {
            return Err(zbus::fdo::Error::Failed("suspended".into()));
        }

        self.reset_cancel();

        let devpath = self.devpath.clone();
        let username = username.to_owned();
        let finger_name = finger_name.to_owned();
        let emitter = emitter.to_owned();
        let cancelled = Arc::clone(&self.cancelled);

        tokio::task::spawn(async move {
            let result = tokio::task::spawn_blocking({
                let cancelled = Arc::clone(&cancelled);
                let emitter = emitter.clone();
                let username = username.clone();
                let finger_name = finger_name.clone();
                move || enroll_blocking(&devpath, &username, &finger_name, &cancelled, emitter)
            })
            .await;

            match result {
                Ok(Ok(())) => {
                    let _ = DeviceService::enroll_status(&emitter, "enroll-completed", true).await;
                }
                Ok(Err(e)) => {
                    tracing::error!("enroll error: {e}");
                    let _ = DeviceService::enroll_status(&emitter, "enroll-failed", true).await;
                }
                Err(e) => {
                    tracing::error!("enroll task panicked: {e}");
                    let _ = DeviceService::enroll_status(&emitter, "enroll-failed", true).await;
                }
            }
        });

        Ok(())
    }

    async fn enroll_stop(&self) -> zbus::fdo::Result<()> {
        self.do_cancel();
        Ok(())
    }

    async fn verify_start(
        &self,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        username: &str,
        finger_name: &str,
    ) -> zbus::fdo::Result<()> {
        self.reset_cancel();

        let devpath = self.devpath.clone();
        let username = username.to_owned();
        let finger_name = finger_name.to_owned();
        let cancelled = Arc::clone(&self.cancelled);
        let emitter = emitter.to_owned();

        tokio::task::spawn(async move {
            let result = tokio::task::spawn_blocking(move || {
                verify_blocking(&devpath, &username, &finger_name, &cancelled)
            })
            .await;

            let (status, done) = match result {
                Ok(Ok(true)) => ("verify-match", true),
                Ok(Ok(false)) => ("verify-no-match", true),
                Ok(Err(e)) => {
                    tracing::warn!("verify error: {e}");
                    ("verify-retry-scan", false)
                }
                Err(e) => {
                    tracing::error!("verify task panicked: {e}");
                    ("verify-no-match", true)
                }
            };
            let _ = DeviceService::verify_status(&emitter, status, done).await;
        });

        Ok(())
    }

    async fn verify_stop(&self) -> zbus::fdo::Result<()> {
        self.do_cancel();
        Ok(())
    }

    async fn cancel(&self) -> zbus::fdo::Result<()> {
        self.do_cancel();
        Ok(())
    }

    async fn suspend(&self) {
        self.state.lock().await.suspended = true;
        self.do_cancel();
    }

    async fn resume(&self) {
        self.state.lock().await.suspended = false;
        self.resume_notify.notify_waiters();
    }

    async fn run_cmd(&self, _cmd: &str) -> zbus::fdo::Result<String> {
        Err(zbus::fdo::Error::NotSupported("RunCmd not implemented".into()))
    }

    #[zbus(signal)]
    async fn enroll_status(
        emitter: &SignalEmitter<'_>,
        result: &str,
        done: bool,
    ) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn verify_status(
        emitter: &SignalEmitter<'_>,
        result: &str,
        done: bool,
    ) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn verify_finger_selected(
        emitter: &SignalEmitter<'_>,
        finger: &str,
    ) -> zbus::Result<()>;
}

fn enroll_blocking(
    devpath: &str,
    username: &str,
    finger_name: &str,
    cancelled: &AtomicBool,
    emitter: SignalEmitter<'_>,
) -> anyhow::Result<()> {
    let mut engine = Engine::open(devpath)?;
    let rt = tokio::runtime::Handle::current();

    let mut kept_features: Vec<Features> = Vec::new();
    let mut kept_frames: Vec<Frame> = Vec::new();
    let mut got: u32 = 0;
    let mut redundant: u32 = 0;

    while got < ENROLL_STAGES {
        if cancelled.load(Ordering::Relaxed) {
            anyhow::bail!("cancelled");
        }

        let Some((frame, _raw)) = engine.capture(FINGER_MS, QUALITY_MIN, QUALITY_MAX_RETRY) else {
            redundant += 1;
            if redundant >= ENROLL_MAX_REDUNDANT {
                break;
            }
            continue;
        };

        let too_similar = kept_frames
            .iter()
            .any(|kept| frame.ncc_vs(kept, MAX_SHIFT, MIN_OVERLAP) > ENROLL_MAX_NCC);

        if too_similar {
            redundant += 1;
            if redundant >= ENROLL_MAX_REDUNDANT {
                break;
            }
        } else {
            kept_features.push(Features::extract(&frame));
            kept_frames.push(frame);
            got += 1;
            let em = emitter.clone();
            rt.block_on(async move {
                let _ = DeviceService::enroll_status(&em, "enroll-stage-passed", false).await;
            });
        }

        if got < ENROLL_STAGES {
            engine.wait_finger_up(LIFT_MS);
        }
    }

    if got < 2 {
        anyhow::bail!("captured too few frames ({got})");
    }

    storage::save_features(username, finger_name, &kept_features)?;
    Ok(())
}

fn verify_blocking(
    devpath: &str,
    username: &str,
    finger_name: &str,
    cancelled: &AtomicBool,
) -> anyhow::Result<bool> {
    let gallery = storage::load_features(username, finger_name)?;
    if gallery.is_empty() {
        anyhow::bail!("no enrolled data for {username}/{finger_name}");
    }

    let mut engine = Engine::open(devpath)?;
    let mut best = -1i32;
    let mut got_any = false;

    for _ in 0..VERIFY_FRAMES {
        if cancelled.load(Ordering::Relaxed) {
            anyhow::bail!("cancelled");
        }
        let Some((frame, _raw)) = engine.capture(FINGER_MS, QUALITY_MIN, QUALITY_MAX_RETRY)
        else {
            if !got_any {
                anyhow::bail!("capture failed");
            }
            break;
        };
        got_any = true;
        let probe = Features::extract(&frame);
        let score = probe.verify_against(&gallery);
        tracing::debug!(score, "verify frame");
        if score > best {
            best = score;
        }
        if best >= SIFT_THRESHOLD {
            break;
        }
    }

    tracing::debug!(best, threshold = SIFT_THRESHOLD, "verify result");
    Ok(best >= SIFT_THRESHOLD)
}
