// Locating the launcher's own folder. The game's binary, config.ini and asset
// trees all live beside the executable, so everything is resolved against the
// real path of argv[0] rather than the current directory, which may be
// anything when the app is started from a desktop icon.

use std::path::PathBuf;

pub fn exe_dir() -> PathBuf {
    let arg0 = std::env::args_os().next().unwrap_or_default();
    let path = PathBuf::from(&arg0);
    let resolved = std::fs::canonicalize(&path).unwrap_or(path);
    match resolved.parent() {
        Some(p) if !p.as_os_str().is_empty() => p.to_path_buf(),
        _ => std::env::current_dir().unwrap_or_else(|_| PathBuf::from(".")),
    }
}

/// True if the path looks like an Inno Setup installer or a GOG setup exe.
pub fn looks_like_installer(path: &std::path::Path) -> bool {
    use std::io::Read;
    let mut f = match std::fs::File::open(path) {
        Ok(f) => f,
        Err(_) => return false,
    };
    let mut head = [0u8; 512 * 1024];
    let n = f.read(&mut head).unwrap_or(0);
    let hay = &head[..n];
    // GOG offline installers are Inno Setup; the marker lives in the data
    // section, so scan a generous prefix rather than just the PE header.
    contains(hay, b"Inno Setup") || contains(hay, b"goggame-") || contains(hay, b"GOG.com")
}

pub fn looks_like_iso(path: &std::path::Path) -> bool {
    use std::io::{Read, Seek, SeekFrom};
    let mut f = match std::fs::File::open(path) {
        Ok(f) => f,
        Err(_) => return false,
    };
    if f.seek(SeekFrom::Start(0x8001)).is_err() {
        return false;
    }
    let mut magic = [0u8; 5];
    if f.read_exact(&mut magic).is_err() {
        return false;
    }
    // ISO9660 primary volume descriptor at 0x8001: "CD001".
    &magic == b"CD001"
}

fn contains(hay: &[u8], needle: &[u8]) -> bool {
    if needle.len() > hay.len() {
        return false;
    }
    hay.windows(needle.len()).any(|w| w == needle)
}
