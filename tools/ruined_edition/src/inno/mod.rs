// Reading a GOG offline installer with no external tools.
//
// The GOG release of Resident Evil ships as an Inno Setup installer: a
// `setup_*.exe` loader plus a `setup_*.bin` slice holding the compressed game
// data. This module decodes that format directly, in Rust, so the launcher is
// the only program the player ever runs. Nothing here shells out to
// `innoextract` or `7z`.
//
// The format is documented by innoextract (zlib licence) and summarised at
// reverseengineering.wiki/Format:Inno_Setup_installer. Three parts matter:
//
//   1. The loader offset table, in the exe, which gives the offsets of the
//      header stream and the file data.
//   2. The header stream: a 64-byte version string followed by CRC-protected
//      4096-byte sub-chunked streams (LZMA1 or zlib), holding the main header
//      and the per-file location entries.
//   3. The file data, as `zlb\x1a` chunks, either inside the exe or in the
//      `.bin` slices that start with `idska16`/`idska32`.
//
// Only the parts this installer actually uses are implemented, and each one is
// checked: a structure this reader does not recognise is an error, never a
// silent guess. A truncated or damaged asset is worse than a clear failure.

use std::path::{Path, PathBuf};

mod block;
mod import;
mod offsets;
mod setup;

pub use import::import_usa;
pub use setup::{Setup, Slice};

/// A compression method named by the setup header.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Compression {
    Stored,
    Zlib,
    Bzip2,
    Lzma1,
    Lzma2,
}

/// Everything the reader could not do, with enough context to say why.
#[derive(Debug)]
pub enum InnoError {
    Io(std::io::Error),
    /// A magic number or field was not what this format uses.
    Format(String),
    /// The file is an Inno installer newer than this reader understands.
    Unsupported(String),
    /// The data is compressed with something not implemented here.
    Compression(String),
    /// A CRC-32 or Adler-32 did not match the stored one.
    Checksum(String),
}

impl std::fmt::Display for InnoError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            InnoError::Io(e) => write!(f, "{e}"),
            InnoError::Format(m) => write!(f, "{m}"),
            InnoError::Unsupported(m) => write!(f, "{m}"),
            InnoError::Compression(m) => write!(f, "{m}"),
            InnoError::Checksum(m) => write!(f, "{m}"),
        }
    }
}

impl std::error::Error for InnoError {}

impl From<std::io::Error> for InnoError {
    fn from(e: std::io::Error) -> Self {
        InnoError::Io(e)
    }
}

/// Read `path` and return everything needed to pull the game files out.
pub fn open(path: &Path) -> Result<Setup, InnoError> {
    offsets::read(path)
}

/// Find the `.bin` slice that belongs to a loader exe. GOG writes it as
/// `<name>-1.bin`; some builds use `<name>-0.bin`, and a bare `<name>.bin`
/// shows up too, so all three are tried before giving up.
pub fn find_slice(exe: &Path) -> Option<PathBuf> {
    let dir = exe.parent()?;
    let stem = exe.file_stem()?.to_string_lossy().to_string();
    for suffix in ["-1.bin", "-0.bin", ".bin"] {
        let cand = dir.join(format!("{stem}{suffix}"));
        if cand.is_file() {
            return Some(cand);
        }
    }
    None
}

