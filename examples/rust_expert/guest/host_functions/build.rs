//! Runs generate.py over the shared host_functions.json, producing the Rust
//! module that lib.rs includes. The JSON is the single source of truth for
//! both sides, so touching it rebuilds the guest and nothing else has to be
//! kept in step by hand.

use std::path::PathBuf;
use std::process::Command;

fn main() {
    let example_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .and_then(|guest| guest.parent())
        .expect("host_functions crate must live in <example>/guest/")
        .to_path_buf();

    let json = example_dir.join("host_functions.json");
    let generator = example_dir.join("generate.py");
    let output = PathBuf::from(std::env::var("OUT_DIR").expect("OUT_DIR is set by cargo"))
        .join("host_functions.rs");

    println!("cargo:rerun-if-changed={}", json.display());
    println!("cargo:rerun-if-changed={}", generator.display());

    let python = std::env::var("PYTHON").unwrap_or_else(|_| "python3".into());
    let status = Command::new(&python)
        .arg(&generator)
        .arg("-j")
        .arg(&json)
        .arg("-o")
        .arg(&output)
        .status()
        .unwrap_or_else(|e| panic!("Could not run {} {}: {e}", python, generator.display()));

    assert!(status.success(), "{} failed: {status}", generator.display());
}
