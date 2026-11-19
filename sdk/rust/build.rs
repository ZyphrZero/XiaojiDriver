use std::{env, path::PathBuf};

fn main() {
    assert_eq!(
        env::var("TARGET").unwrap(),
        "x86_64-pc-windows-msvc",
        "The xiaoji client SDK requires the Windows x64 MSVC target"
    );
    println!("cargo:rerun-if-env-changed=XIAOJI_SDK_DIR");
    let sdk = env::var_os("XIAOJI_SDK_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../.."));
    let import_lib = sdk.join("xiaoji_client.lib");
    assert!(
        import_lib.is_file(),
        "Missing {}. Set XIAOJI_SDK_DIR to the directory containing xiaoji_client.lib",
        import_lib.display()
    );
    println!("cargo:rerun-if-changed={}", import_lib.display());
    println!("cargo:rustc-link-search=native={}", sdk.display());
    println!("cargo:rustc-link-lib=dylib=xiaoji_client");
}
