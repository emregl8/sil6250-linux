// SPDX-License-Identifier: LGPL-2.1-or-later
#![no_main]

use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    sil6250_ffi::fuzzing::gallery(data);
});
