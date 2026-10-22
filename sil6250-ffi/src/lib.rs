// SPDX-License-Identifier: LGPL-2.1-or-later

use std::ffi::{c_char, CStr};
use std::ptr;

use sil6250::{Engine, Features, Frame, PskMode, IMG_SIZE};

const MAX_FEATURES: usize = 64;
const MAX_GALLERY_BYTES: usize = 2 * 1024 * 1024;

fn parse_psk_mode(value: Option<&str>) -> Option<PskMode> {
    value.map_or(Some(PskMode::Auto), |mode| mode.parse().ok())
}

#[no_mangle]
unsafe extern "C" fn sil6250_engine_new(path: *const c_char) -> *mut Engine {
    if path.is_null() {
        return ptr::null_mut();
    }
    let Ok(path) = CStr::from_ptr(path).to_str() else {
        return ptr::null_mut();
    };
    let mode = match std::env::var("SIL6250_PSK_MODE") {
        Ok(value) => parse_psk_mode(Some(&value)),
        Err(std::env::VarError::NotPresent) => parse_psk_mode(None),
        Err(std::env::VarError::NotUnicode(_)) => None,
    };
    let Some(mode) = mode else {
        return ptr::null_mut();
    };
    let Ok(mut engine) = Engine::open(path) else {
        return ptr::null_mut();
    };
    if engine.set_key(Some("shiba")).is_err() {
        return ptr::null_mut();
    }
    engine.set_psk_mode(mode);
    Box::into_raw(Box::new(engine))
}

#[no_mangle]
unsafe extern "C" fn sil6250_engine_free(engine: *mut Engine) {
    if !engine.is_null() {
        drop(Box::from_raw(engine));
    }
}

#[no_mangle]
unsafe extern "C" fn sil6250_engine_capture(
    engine: *mut Engine,
    timeout_ms: u32,
    raw: *mut u8,
    raw_len: usize,
) -> i32 {
    if engine.is_null() || raw.is_null() || raw_len != IMG_SIZE {
        return -1;
    }
    match (*engine).capture_frame(timeout_ms) {
        Ok(frame) => {
            ptr::copy_nonoverlapping(frame.as_ptr(), raw, IMG_SIZE);
            1
        }
        Err(e) if e.kind() == std::io::ErrorKind::TimedOut => 0,
        Err(_) => -1,
    }
}

#[no_mangle]
unsafe extern "C" fn sil6250_engine_wait_lift(engine: *mut Engine, timeout_ms: u32) -> bool {
    !engine.is_null() && (*engine).wait_finger_up(timeout_ms)
}

fn frame_from_slice(raw: &[u8]) -> Option<Frame> {
    let raw: &[u8; IMG_SIZE] = raw.try_into().ok()?;
    Some(Frame::from_raw(raw))
}

#[no_mangle]
unsafe extern "C" fn sil6250_frame_quality(raw: *const u8, raw_len: usize) -> f32 {
    if raw.is_null() || raw_len != IMG_SIZE {
        return -1.0;
    }
    frame_from_slice(std::slice::from_raw_parts(raw, raw_len))
        .map(|frame| frame.quality())
        .unwrap_or(-1.0)
}

#[no_mangle]
unsafe extern "C" fn sil6250_frame_ncc(
    raw: *const u8,
    reference: *const u8,
    raw_len: usize,
) -> f32 {
    if raw.is_null() || reference.is_null() || raw_len != IMG_SIZE {
        return -1.0;
    }
    let Some(probe) = frame_from_slice(std::slice::from_raw_parts(raw, raw_len)) else {
        return -1.0;
    };
    let Some(reference) = frame_from_slice(std::slice::from_raw_parts(reference, raw_len)) else {
        return -1.0;
    };
    probe.ncc_vs(&reference, 14, 1200)
}

#[no_mangle]
unsafe extern "C" fn sil6250_frame_extract(
    raw: *const u8,
    raw_len: usize,
    output: *mut u8,
    capacity: usize,
) -> isize {
    if raw.is_null() || output.is_null() || raw_len != IMG_SIZE {
        return -1;
    }
    let Some(frame) = frame_from_slice(std::slice::from_raw_parts(raw, raw_len)) else {
        return -1;
    };
    let bytes = Features::extract(&frame).serialize();
    if bytes.len() > capacity {
        return -1;
    }
    ptr::copy_nonoverlapping(bytes.as_ptr(), output, bytes.len());
    bytes.len() as isize
}

fn parse_gallery(gallery: &[u8]) -> Option<Vec<Features>> {
    if gallery.len() > MAX_GALLERY_BYTES {
        return None;
    }
    let mut off = 0usize;
    let mut features = Vec::new();
    while off < gallery.len() {
        let end = off.checked_add(4).filter(|&end| end <= gallery.len())?;
        let len = u32::from_le_bytes(gallery[off..end].try_into().ok()?) as usize;
        off = end;
        let end = off.checked_add(len).filter(|&end| end <= gallery.len())?;
        features.push(Features::deserialize(&gallery[off..end])?);
        if features.len() > MAX_FEATURES {
            return None;
        }
        off = end;
    }
    if features.is_empty() {
        return None;
    }
    Some(features)
}

fn gallery_score(probe: &Features, gallery: &[u8]) -> Option<i32> {
    Some(probe.verify_against(&parse_gallery(gallery)?))
}

#[no_mangle]
unsafe extern "C" fn sil6250_gallery_valid(gallery: *const u8, gallery_len: usize) -> bool {
    if gallery.is_null() || gallery_len > MAX_GALLERY_BYTES {
        return false;
    }
    parse_gallery(std::slice::from_raw_parts(gallery, gallery_len)).is_some()
}

#[no_mangle]
unsafe extern "C" fn sil6250_frame_score(
    raw: *const u8,
    raw_len: usize,
    gallery: *const u8,
    gallery_len: usize,
) -> i32 {
    if raw.is_null() || gallery.is_null() || raw_len != IMG_SIZE || gallery_len > MAX_GALLERY_BYTES
    {
        return -1;
    }
    let Some(frame) = frame_from_slice(std::slice::from_raw_parts(raw, raw_len)) else {
        return -1;
    };
    let probe = Features::extract(&frame);
    gallery_score(&probe, std::slice::from_raw_parts(gallery, gallery_len)).unwrap_or(-1)
}

#[cfg(test)]
mod tests {
    use super::*;
    use sil6250::sift::{Keypoint, DESC_DIM};

    #[test]
    fn psk_configuration_uses_upstream_modes() {
        assert_eq!(parse_psk_mode(None), Some(PskMode::Auto));
        for (name, expected) in [
            ("auto", PskMode::Auto),
            ("provision", PskMode::Provision),
            ("replay", PskMode::Replay),
            ("legacy", PskMode::Legacy),
        ] {
            assert_eq!(parse_psk_mode(Some(name)), Some(expected));
        }
        assert_eq!(parse_psk_mode(Some("")), None);
        assert_eq!(parse_psk_mode(Some("unknown")), None);
    }

    fn gallery_entry() -> Vec<u8> {
        let features = Features {
            kp: vec![Keypoint {
                x: 10.0,
                y: 12.0,
                scale: 1.0,
                ori: 0.0,
                resp: 1.0,
                desc: [0.0; DESC_DIM],
            }],
        };
        let bytes = features.serialize();
        let mut entry = Vec::from((bytes.len() as u32).to_le_bytes());
        entry.extend(bytes);
        entry
    }

    #[test]
    fn malformed_gallery_is_rejected() {
        let probe = Features::extract(&Frame::from_raw(&[0; IMG_SIZE]));
        assert!(gallery_score(&probe, &[255, 255, 255, 255]).is_none());
        assert!(gallery_score(&probe, &[]).is_none());
    }

    #[test]
    fn valid_gallery_can_be_scored() {
        let raw = [128; IMG_SIZE];
        let probe = Features::extract(&Frame::from_raw(&raw));
        let data = probe.serialize();
        let mut gallery = Vec::from((data.len() as u32).to_le_bytes());
        gallery.extend(data);
        assert!(gallery_score(&probe, &gallery).is_some());
        assert!(unsafe { sil6250_gallery_valid(gallery.as_ptr(), gallery.len()) });
        assert!(!unsafe { sil6250_gallery_valid(std::ptr::null(), gallery.len()) });
    }

    #[test]
    fn truncated_gallery_entries_are_rejected() {
        let gallery = gallery_entry();
        assert!(parse_gallery(&gallery).is_some());
        for end in 0..gallery.len() {
            assert!(parse_gallery(&gallery[..end]).is_none(), "prefix {end}");
        }
        let mut trailing = gallery;
        trailing.push(0);
        assert!(parse_gallery(&trailing).is_none());
    }

    #[test]
    fn non_finite_feature_fields_are_rejected() {
        let gallery = gallery_entry();
        for field in 0..5 + DESC_DIM {
            let mut malformed = gallery.clone();
            let start = 4 + 12 + field * 4;
            malformed[start..start + 4].copy_from_slice(&f32::NAN.to_le_bytes());
            assert!(parse_gallery(&malformed).is_none(), "field {field}");
        }
    }

    #[test]
    fn gallery_limits_are_enforced() {
        let empty = Features { kp: Vec::new() }.serialize();
        let mut entry = Vec::from((empty.len() as u32).to_le_bytes());
        entry.extend(empty);
        assert!(parse_gallery(&entry.repeat(MAX_FEATURES)).is_some());
        assert!(parse_gallery(&entry.repeat(MAX_FEATURES + 1)).is_none());
        assert!(parse_gallery(&vec![0; MAX_GALLERY_BYTES + 1]).is_none());
        let byte = 0u8;
        assert!(!unsafe { sil6250_gallery_valid(&byte, MAX_GALLERY_BYTES + 1) });
    }

    #[test]
    fn ffi_rejects_null_and_wrong_sized_inputs() {
        let raw = [0u8; IMG_SIZE];
        let gallery = gallery_entry();
        let mut output = [0xa5u8; 4];
        unsafe {
            assert!(sil6250_engine_new(ptr::null()).is_null());
            sil6250_engine_free(ptr::null_mut());
            assert_eq!(
                sil6250_engine_capture(ptr::null_mut(), 1, output.as_mut_ptr(), 4),
                -1
            );
            assert!(!sil6250_engine_wait_lift(ptr::null_mut(), 1));
            assert_eq!(sil6250_frame_quality(ptr::null(), IMG_SIZE), -1.0);
            assert_eq!(sil6250_frame_quality(raw.as_ptr(), IMG_SIZE - 1), -1.0);
            assert_eq!(sil6250_frame_ncc(raw.as_ptr(), ptr::null(), IMG_SIZE), -1.0);
            assert_eq!(
                sil6250_frame_extract(raw.as_ptr(), IMG_SIZE, output.as_mut_ptr(), 0),
                -1
            );
            assert_eq!(output, [0xa5; 4]);
            assert_eq!(
                sil6250_frame_score(ptr::null(), IMG_SIZE, gallery.as_ptr(), gallery.len()),
                -1
            );
            assert_eq!(
                sil6250_frame_score(raw.as_ptr(), IMG_SIZE, ptr::null(), 0),
                -1
            );
        }
    }

    #[test]
    fn arbitrary_gallery_bytes_do_not_panic() {
        let mut state = 0x1234_5678u32;
        for len in 0..1024 {
            let bytes: Vec<u8> = (0..len)
                .map(|_| {
                    state ^= state << 13;
                    state ^= state >> 17;
                    state ^= state << 5;
                    state as u8
                })
                .collect();
            let _ = parse_gallery(&bytes);
        }
    }
}
