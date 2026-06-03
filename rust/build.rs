//! Build script.
//!
//! When the C sparsemap source is available next to this crate (i.e.
//! when building inside the upstream repository), compile it and set the
//! `c_ffi` cfg so the wire-compatibility tests can round-trip buffers
//! through both implementations.  When the source is absent — notably in
//! a packaged crate published to crates.io — this is a no-op and the
//! crate builds as pure safe Rust.

use std::path::Path;

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=SPARSEMAP_NO_C_FFI");
    // Declare the cfg we may set, so downstream builds don't warn.
    println!("cargo::rustc-check-cfg=cfg(c_ffi)");

    if std::env::var_os("SPARSEMAP_NO_C_FFI").is_some() {
        return;
    }

    let c_src = Path::new("../sm.c");
    let c_hdr = Path::new("../sm.h");
    if !c_src.exists() || !c_hdr.exists() {
        return;
    }

    cc::Build::new()
        .file(c_src)
        .include("..")
        .warnings(false)
        .opt_level(2)
        .compile("sparsemap_c");

    println!("cargo:rerun-if-changed=../sm.c");
    println!("cargo:rerun-if-changed=../sm.h");
    println!("cargo:rustc-cfg=c_ffi");
}
