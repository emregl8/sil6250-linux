// SPDX-License-Identifier: GPL-2.0
#![no_main]

use libfuzzer_sys::fuzz_target;
use sil6250::{proto, psk, Features};

fuzz_target!(|data: &[u8]| {
    if let Some(features) = Features::deserialize(data) {
        assert!(Features::deserialize(&features.serialize()).is_some());
    }
    let _ = proto::parse_frame(data);
    let _ = psk::client_psk_identity(data);
    let _ = psk::key_id_from_response(data);
    let _ = psk::is_set_psk_ack(data);
    if data.len() >= 8 {
        let region = u32::from_le_bytes(data[..4].try_into().unwrap()) as usize;
        let length = u32::from_le_bytes(data[4..8].try_into().unwrap());
        let mut output = [0xa5; 512];
        let _ = proto::build_frame_ex(0, 0, 0, &data[8..], length, region, 0, 0, &mut output);
    }
});
