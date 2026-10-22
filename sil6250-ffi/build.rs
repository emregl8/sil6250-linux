// SPDX-License-Identifier: LGPL-2.1-or-later

fn main() {
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("linux") {
        println!("cargo:rustc-cdylib-link-arg=-Wl,-soname,libsil6250_ffi.so.0");
    }
}
