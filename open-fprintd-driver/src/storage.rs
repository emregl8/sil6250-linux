use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};

use crate::engine::{FRAME_BYTES, Features, Frame};
use crate::ffi;

const STORAGE_DIR: &str = "/var/lib/open-fprintd/sil6250";

fn finger_path(username: &str, finger: &str) -> PathBuf {
    Path::new(STORAGE_DIR)
        .join(username)
        .join(finger)
        .join("frames.bin")
}

/// Persist `raw_frames` (flat array of FRAME_BYTES-byte frames) to disk.
pub fn save_frames(username: &str, finger: &str, raw_frames: &[u8]) -> io::Result<()> {
    assert_eq!(raw_frames.len() % FRAME_BYTES, 0);
    let path = finger_path(username, finger);
    std::fs::create_dir_all(path.parent().unwrap())?;
    let mut f = std::fs::File::create(&path)?;
    f.write_all(raw_frames)?;
    Ok(())
}

/// Load all stored raw frames for `finger` and extract SIFT features.
/// Returns an empty vec if nothing is stored.
pub fn load_features(username: &str, finger: &str) -> io::Result<Vec<Features>> {
    let path = finger_path(username, finger);
    let mut f = match std::fs::File::open(&path) {
        Ok(f) => f,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(vec![]),
        Err(e) => return Err(e),
    };
    let mut buf = Vec::new();
    f.read_to_end(&mut buf)?;

    let n = buf.len() / FRAME_BYTES;
    let mut features = Vec::with_capacity(n);
    for i in 0..n {
        let raw: &[u8; FRAME_BYTES] = buf[i * FRAME_BYTES..(i + 1) * FRAME_BYTES]
            .try_into()
            .unwrap();
        let mut pm = Box::new(ffi::PmFrame { px: [0.0; ffi::PM_N] });
        unsafe { ffi::pm_destripe(raw.as_ptr(), &raw mut *pm) };
        let frame = Frame(pm);
        features.push(Features::extract(&frame));
    }
    Ok(features)
}

/// List finger names with stored data for `username`.
pub fn list_enrolled(username: &str) -> Vec<String> {
    let user_dir = Path::new(STORAGE_DIR).join(username);
    let Ok(rd) = std::fs::read_dir(&user_dir) else {
        return vec![];
    };
    rd.filter_map(|entry| {
        let entry = entry.ok()?;
        let name = entry.file_name().into_string().ok()?;
        let frames_path = entry.path().join("frames.bin");
        frames_path.exists().then_some(name)
    })
    .collect()
}

/// Delete all enrolled fingers for `username`.
pub fn delete_enrolled(username: &str) -> io::Result<()> {
    let user_dir = Path::new(STORAGE_DIR).join(username);
    if user_dir.exists() {
        std::fs::remove_dir_all(&user_dir)?;
    }
    Ok(())
}
