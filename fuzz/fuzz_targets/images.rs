// SPDX-License-Identifier: GPL-2.0
#![no_main]

use libfuzzer_sys::fuzz_target;
use sil6250::{sift::match_features, Features, Frame, IMG_SIZE};

fuzz_target!(|data: &[u8]| {
    let mut raw = [0; IMG_SIZE];
    for (pixel, value) in raw.iter_mut().zip(data.iter().cycle()) {
        *pixel = *value;
    }
    let frame = Frame::from_raw(&raw);
    let quality = frame.quality();
    assert!(quality.is_finite() && (0.0..=1.0).contains(&quality));
    let features = Features::extract(&frame);
    let decoded = Features::deserialize(&features.serialize()).unwrap();
    let score = match_features(&features, &decoded);
    assert!((0..=200).contains(&score));
});
