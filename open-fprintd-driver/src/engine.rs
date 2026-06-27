use std::ffi::CString;
use std::io;

use crate::ffi::{self, PE_IMG_SIZE, PM_N};

// ---- raw-image frame (5120 bytes) ----------------------------------------

pub const FRAME_BYTES: usize = PM_N;

/// A destriped, normalized frame ready for matching.
pub struct Frame(pub Box<ffi::PmFrame>);

impl Frame {
    fn from_raw(raw: &[u8; PE_IMG_SIZE]) -> Self {
        let mut out = Box::new(ffi::PmFrame { px: [0.0; PM_N] });
        unsafe { ffi::pm_destripe(raw.as_ptr(), &raw mut *out) };
        Frame(out)
    }

    pub fn quality(&self) -> f32 {
        unsafe { ffi::ps_quality(&*self.0) }
    }

    pub fn ncc_vs(&self, reference: &Frame, max_shift: i32, min_overlap: i32) -> f32 {
        unsafe {
            ffi::pm_best_shift_ncc(
                &*self.0,
                &*reference.0,
                max_shift,
                max_shift,
                min_overlap,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
            )
        }
    }
}

// ---- SIFT features -------------------------------------------------------

pub struct Features(pub Box<ffi::PsFeatures>);

impl Features {
    pub fn extract(frame: &Frame) -> Self {
        // SAFETY: PsKeypoint contains only floats; zero-init is valid.
        let mut out = Box::new(ffi::PsFeatures {
            n: 0,
            kp: unsafe { std::mem::zeroed() },
        });
        unsafe { ffi::ps_extract(&*frame.0, &raw mut *out) };
        Features(out)
    }

    /// Returns the gallery inlier count against `gallery` (an array of feature sets).
    pub fn verify_against(&self, gallery: &[Features]) -> i32 {
        if gallery.is_empty() {
            return 0;
        }
        // Build a C-layout array of PsFeatures from the slice.
        let raw: Vec<ffi::PsFeatures> = gallery
            .iter()
            .map(|f| ffi::PsFeatures {
                n: f.0.n,
                kp: f.0.kp,
            })
            .collect();
        unsafe { ffi::ps_verify(&*self.0, raw.as_ptr(), raw.len() as i32) }
    }
}

// ---- Engine --------------------------------------------------------------

/// Owns the petaic_engine handle. Not Send by itself; callers move it into
/// spawn_blocking closures.
pub struct Engine(*mut ffi::PetaicEngine);

// SAFETY: the underlying C engine is self-contained; we never share the
// pointer across threads simultaneously.
unsafe impl Send for Engine {}

impl Drop for Engine {
    fn drop(&mut self) {
        if !self.0.is_null() {
            unsafe { ffi::engine_free(self.0) };
        }
    }
}

impl Engine {
    pub fn open(devpath: &str) -> io::Result<Self> {
        let e = unsafe { ffi::engine_new() };
        if e.is_null() {
            return Err(io::Error::from(io::ErrorKind::OutOfMemory));
        }
        let key = CString::new("shiba").unwrap();
        unsafe { ffi::engine_set_key(e, key.as_ptr()) };

        let path = CString::new(devpath).map_err(|_| {
            io::Error::new(io::ErrorKind::InvalidInput, "devpath contains NUL")
        })?;
        let rc = unsafe { ffi::engine_open(e, path.as_ptr()) };
        if rc != 0 {
            unsafe { ffi::engine_free(e) };
            return Err(io::Error::from_raw_os_error(-rc));
        }
        Ok(Engine(e))
    }

    /// Capture one frame, with a quality gate. Returns `None` on timeout/error.
    ///
    /// `quality_min` – reject frames below this and require a lift (up to
    /// `max_retry` times); 0.0 disables the gate.
    pub fn capture(
        &self,
        finger_ms: u32,
        quality_min: f32,
        max_retry: u32,
    ) -> Option<(Frame, Box<[u8; PE_IMG_SIZE]>)> {
        let mut raw = Box::new([0u8; PE_IMG_SIZE]);
        for attempt in 0..=max_retry {
            let rc =
                unsafe { ffi::engine_capture_frame(self.0, raw.as_mut_ptr(), finger_ms) };
            if rc != 0 {
                return None;
            }
            let frame = Frame::from_raw(&raw);
            let q = frame.quality();
            let ok = q >= quality_min || attempt >= max_retry;
            tracing::debug!(attempt, q, quality_min, ok, "capture quality");
            if ok {
                return Some((frame, raw));
            }
            self.wait_finger_up(4000);
        }
        None
    }

    pub fn wait_finger_up(&self, timeout_ms: u32) -> bool {
        unsafe { ffi::engine_wait_finger_up(self.0, timeout_ms) != 0 }
    }
}
