// The embedded game payload.
//
// The launcher is a single self-contained executable: the compiled 32-bit
// `residentevil` game binary and its bundled 32-bit libraries are packed
// *inside* this file as a gzip-compressed container. On first run the launcher
// materialises them beside itself (this is unavoidable - the OS loader needs
// real files on disk to exec a binary and to satisfy DT_NEEDED), then spawns
// the game with LD_LIBRARY_PATH pointed at the extracted libs.
//
// Capcom's game data is NEVER embedded here. The player rips their own from a
// copy they own; the launcher imports it. Only the compiled game binary and
// its libraries ride inside the launcher.
//
// Container layout (before gzip), all integers little-endian:
//
//   magic      8 bytes  b"REDCONT1"
//   count      u32      number of files
//   entries    count times:
//       path_len u32
//       path     path_len bytes (utf-8, posix separators, e.g. "lib/libSDL2.so")
//       data_len u64
//   data       the file contents, concatenated in entry order
//
// The whole thing is gzip-compressed by tools/pack_payload.py and read back
// here with flate2. An empty (zero-byte) payload means "nothing embedded" -
// a plain dev build - and the launcher simply falls back to looking for a
// residentevil binary beside itself.

use std::io::Read;
use std::path::{Path, PathBuf};

/// The payload packed in by the release build via include_bytes!. A dev build
/// has no packed payload, so build.rs leaves an empty file here and the
/// launcher degrades to the sibling-binary layout. Linux and Windows carry
/// different payloads (a 32-bit ELF + .so libs vs a PE + DLLs), selected by
/// the target being compiled.
#[cfg(windows)]
static PAYLOAD: &[u8] = include_bytes!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/embed/ruined_payload.windows.bin.gz"
));
#[cfg(not(windows))]
static PAYLOAD: &[u8] = include_bytes!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/embed/ruined_payload.bin.gz"
));

const MAGIC: &[u8; 8] = b"REDCONT1";

/// The name of the game executable inside the payload, per target.
#[cfg(windows)]
pub fn exe_name() -> &'static str {
    "residentevil.exe"
}
#[cfg(not(windows))]
pub fn exe_name() -> &'static str {
    "residentevil"
}

/// One file recovered from the container.
struct Entry {
    path: String,
    data: Vec<u8>,
}

/// Parse and gunzip the embedded payload. Returns None when nothing is packed
/// (empty payload) or the data is malformed.
fn decode(payload: &[u8]) -> Option<Vec<Entry>> {
    if payload.is_empty() {
        return None;
    }
    let mut raw = Vec::new();
    flate2::read::GzDecoder::new(payload)
        .read_to_end(&mut raw)
        .ok()?;
    if raw.len() < 12 || &raw[..8] != MAGIC {
        return None;
    }
    let mut pos = 8usize;
    let count = u32::from_le_bytes(raw[pos..pos + 4].try_into().ok()?) as usize;
    pos += 4;

    // First pass: read the entry headers to learn each blob's length.
    let mut spans: Vec<(String, usize)> = Vec::with_capacity(count);
    for _ in 0..count {
        let path_len =
            u32::from_le_bytes(raw.get(pos..pos + 4)?.try_into().ok()?) as usize;
        pos += 4;
        let path = std::str::from_utf8(raw.get(pos..pos + path_len)?).ok()?.to_string();
        pos += path_len;
        let data_len =
            u64::from_le_bytes(raw.get(pos..pos + 8)?.try_into().ok()?) as usize;
        pos += 8;
        spans.push((path, data_len));
    }

    // Second pass: slice the concatenated blobs out of the tail.
    let mut entries = Vec::with_capacity(count);
    for (path, len) in spans {
        let data = raw.get(pos..pos + len)?.to_vec();
        pos += len;
        entries.push(Entry { path, data });
    }
    Some(entries)
}

/// A stable-enough fingerprint of the payload so we only re-extract when the
/// packed content actually changes between builds (FNV-1a).
fn fingerprint(payload: &[u8]) -> u64 {
    let mut h: u64 = 0xcbf29ce484222325;
    for &b in payload {
        h ^= b as u64;
        h = h.wrapping_mul(0x100000001b3);
    }
    h
}

/// Extract the embedded game binary and libraries into `dest` (the launcher's
/// own folder) if they are missing or the packed payload has changed. Returns
/// the path to the game binary, or None when nothing is embedded.
pub fn ensure_extracted(dest: &Path) -> Option<PathBuf> {
    let entries = decode(PAYLOAD)?;
    let stamp_path = dest.join(".ruined_embed");
    let want = fingerprint(PAYLOAD).to_string();

    let bin = dest.join(exe_name());
    if let Ok(cur) = std::fs::read_to_string(&stamp_path) {
        if cur.trim() == want && bin.exists() {
            return Some(bin); // already extracted and still current
        }
    }

    for e in &entries {
        let rel = Path::new(&e.path);
        let out = dest.join(rel);
        if let Some(parent) = out.parent() {
            let _ = std::fs::create_dir_all(parent);
        }
        if std::fs::write(&out, &e.data).is_err() {
            return None; // read-only location; caller reports the failure
        }
        // The game binary must be executable; data files just need read.
        let mode = if e.path == exe_name() { 0o755 } else { 0o644 };
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let _ = std::fs::set_permissions(&out, std::fs::Permissions::from_mode(mode));
        }
        #[cfg(not(unix))]
        let _ = mode;
    }

    let _ = std::fs::write(&stamp_path, want);
    Some(bin)
}

/// The directory holding the extracted libraries, for LD_LIBRARY_PATH.
pub fn lib_dir(dest: &Path) -> PathBuf {
    dest.join("lib")
}
