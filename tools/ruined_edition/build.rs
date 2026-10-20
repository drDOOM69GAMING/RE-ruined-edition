// Ensure the embedded-payload path exists at compile time.
//
// The release build writes a real payload to embed/ before invoking cargo (see
// tools/package_ruined_edition.sh). A plain dev build has nothing to embed yet,
// so we drop an empty file there - include_bytes! then yields an empty slice
// and the launcher falls back to a residentevil binary sitting beside itself.
// We never overwrite a real payload. The file name is target-specific: Linux
// and Windows carry different game binaries and libraries.

use std::env;
use std::path::Path;

fn main() {
    let target = env::var("TARGET").unwrap_or_default();
    let name = if target.contains("windows") {
        "ruined_payload.windows.bin.gz"
    } else {
        "ruined_payload.bin.gz"
    };
    let out = Path::new(env!("CARGO_MANIFEST_DIR")).join("embed").join(name);
    if !out.exists() {
        if let Some(parent) = out.parent() {
            let _ = std::fs::create_dir_all(parent);
        }
        let _ = std::fs::write(&out, []);
    }
    // Re-run only if the packed payload changes, so a rebuild picks up a new
    // embedded game without touching anything else.
    println!("cargo:rerun-if-changed={}", out.display());
}
