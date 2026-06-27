//! Raw FFI bindings to libsil6250 (petaic_engine, petaic_match, petaic_sift).

use std::ffi::c_char;

pub const PE_IMG_SIZE: usize = 64 * 80; // 5120
pub const PM_N: usize = 64 * 80;
pub const PS_MAX_KPTS: usize = 200;
pub const PS_DESC_DIM: usize = 128;

#[repr(C)]
pub struct PetaicEngine {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct PmFrame {
    pub px: [f32; PM_N],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct PsKeypoint {
    pub x: f32,
    pub y: f32,
    pub scale: f32,
    pub ori: f32,
    pub resp: f32,
    pub desc: [f32; PS_DESC_DIM],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct PsFeatures {
    pub n: i32,
    pub kp: [PsKeypoint; PS_MAX_KPTS],
}

#[link(name = "sil6250")]
unsafe extern "C" {
    // engine
    pub fn engine_new() -> *mut PetaicEngine;
    pub fn engine_free(e: *mut PetaicEngine);
    pub fn engine_set_key(e: *mut PetaicEngine, name: *const c_char) -> i32;
    pub fn engine_open(e: *mut PetaicEngine, devpath: *const c_char) -> i32;
    pub fn engine_capture_frame(
        e: *mut PetaicEngine,
        img: *mut u8,
        finger_ms: u32,
    ) -> i32;
    pub fn engine_wait_finger_up(e: *mut PetaicEngine, timeout_ms: u32) -> i32;
    #[allow(dead_code)]
    pub fn engine_close(e: *mut PetaicEngine);

    // match
    pub fn pm_destripe(raw: *const u8, out: *mut PmFrame);
    pub fn pm_best_shift_ncc(
        probe: *const PmFrame,
        reference: *const PmFrame,
        max_dx: i32,
        max_dy: i32,
        min_overlap: i32,
        best_dx: *mut i32,
        best_dy: *mut i32,
    ) -> f32;

    // sift
    pub fn ps_quality(f: *const PmFrame) -> f32;
    pub fn ps_extract(f: *const PmFrame, out: *mut PsFeatures);
    pub fn ps_verify(
        probe: *const PsFeatures,
        gallery: *const PsFeatures,
        n: i32,
    ) -> i32;
}
