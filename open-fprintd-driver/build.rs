fn main() {
    let lib = pkg_config::probe_library("libsil6250").expect(
        "libsil6250 not found via pkg-config; build and install sil6250-linux/lib first",
    );
    for path in &lib.link_paths {
        println!("cargo:rustc-link-search=native={}", path.display());
        // Embed the library directory as an RPATH so the installed binary finds
        // libsil6250 at runtime without requiring it in ld.so.conf.
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", path.display());
    }
}
