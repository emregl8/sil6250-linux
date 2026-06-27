use std::fs::{self, DirBuilder, OpenOptions};
use std::io::{self, Read, Write};
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};

use crate::engine::Features;

const STORAGE_DIR: &str = "/var/lib/open-fprintd/sil6250";

/// Ensure the storage root directory exists and is only accessible to root.
pub fn init_storage() -> io::Result<()> {
    DirBuilder::new()
        .recursive(true)
        .mode(0o700)
        .create(STORAGE_DIR)?;

    let mut perms = fs::metadata(STORAGE_DIR)?.permissions();
    perms.set_mode(0o700);
    fs::set_permissions(STORAGE_DIR, perms)?;
    Ok(())
}

fn finger_path(username: &str, finger: &str) -> PathBuf {
    Path::new(STORAGE_DIR)
        .join(username)
        .join(finger)
        .join("features.bin")
}

/// Persist extracted feature sets to disk.
///
/// Each enrolled frame is stored as a length-prefixed serialized `Features`
/// blob so verification can load them directly without re-extracting from
/// raw sensor images.
pub fn save_features(username: &str, finger: &str, features: &[Features]) -> io::Result<()> {
    let path = finger_path(username, finger);
    let parent = path.parent().unwrap();
    DirBuilder::new()
        .recursive(true)
        .mode(0o700)
        .create(parent)?;
    let mut f = OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .mode(0o600)
        .open(&path)?;
    for feat in features {
        let bytes = feat.serialize();
        f.write_all(&(bytes.len() as u32).to_le_bytes())?;
        f.write_all(&bytes)?;
    }
    Ok(())
}

/// Load stored extracted feature sets. Returns an empty vec if nothing stored.
pub fn load_features(username: &str, finger: &str) -> io::Result<Vec<Features>> {
    let path = finger_path(username, finger);
    let mut f = match std::fs::File::open(&path) {
        Ok(f) => f,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(vec![]),
        Err(e) => return Err(e),
    };
    let mut buf = Vec::new();
    f.read_to_end(&mut buf)?;

    let mut features = Vec::new();
    let mut off = 0usize;
    while off < buf.len() {
        if off + 4 > buf.len() {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "truncated length"));
        }
        let len = u32::from_le_bytes(buf[off..off + 4].try_into().unwrap()) as usize;
        off += 4;
        if off + len > buf.len() {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "truncated features"));
        }
        let feat = Features::deserialize(&buf[off..off + len]).ok_or_else(|| {
            io::Error::new(io::ErrorKind::InvalidData, "invalid feature data")
        })?;
        features.push(feat);
        off += len;
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
        let features_path = entry.path().join("features.bin");
        features_path.exists().then_some(name)
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
