// ---------------------------------------------------------------------------
// pick.rs - "let the player choose their file" dialog.
//
// Shared by both front ends (the window in gui.rs and the terminal menu in
// ui.rs). The dialogs are deliberately not a GUI toolkit, so the app keeps
// building with nothing but the import crates and still runs from a bare TTY:
//
//   Windows: calls GetOpenFileNameA from comdlg32, the operating system's own
//            file dialog. Every Windows install has it.
//   Linux:   runs zenity or kdialog if either is installed. There is no libc
//            file dialog, so a desktop helper program is the only route. If
//            neither is present - a bare TTY, or a minimal install - we say so
//            and the caller falls back to the typed path prompt.
//
// The return value is None both when the player cancels and when no dialog is
// available; ask_file tells the two apart with a status so the caller can print
// the right thing.
// ---------------------------------------------------------------------------

use std::path::PathBuf;

/// The outcome of trying to show a picker.
pub enum Picked {
    /// The player chose this path.
    Chosen(PathBuf),
    /// A dialog appeared and the player cancelled it.
    Cancelled,
    /// No dialog is available on this machine; use the typed prompt.
    Unavailable,
}

/// Ask for a file to open. `filters` is a list of (label, pattern) pairs; the
/// patterns are the "*.iso" style globs to show in the dialog.
pub fn ask_file(title: &str, filters: &[(&str, &str)]) -> Picked {
    #[cfg(windows)]
    {
        return windows_pick(title, filters);
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        return unix_pick(title, filters);
    }
    #[cfg(not(any(windows, all(unix, not(target_os = "macos")))))]
    {
        let _ = (title, filters);
        Picked::Unavailable
    }
}

/// Ask for a folder. Same contract as ask_file.
pub fn ask_folder(title: &str) -> Picked {
    #[cfg(windows)]
    {
        return windows_pick_folder(title);
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        return unix_pick_folder(title);
    }
    #[cfg(not(any(windows, all(unix, not(target_os = "macos")))))]
    {
        let _ = title;
        Picked::Unavailable
    }
}

// ===========================================================================
// Windows: comdlg32
//
// GetOpenFileNameA needs an OPENFILENAMEA filled in, and the filter is the
// awkward part: it is a double-NUL-terminated list of label\0pattern\0 pairs.
// ===========================================================================
#[cfg(windows)]
mod win {
    use std::os::raw::{c_char, c_void};

    pub const MAX_PATH_LEN: usize = 32768;

    #[repr(C)]
    pub struct OpenFileNameA {
        pub l_struct_size: u32,
        pub hwnd_owner: *mut c_void,
        pub h_instance: *mut c_void,
        pub lpstr_filter: *const c_char,
        pub lpstr_custom_filter: *mut c_char,
        pub n_max_cust_filter: u32,
        pub n_filter_index: u32,
        pub lpstr_file: *mut c_char,
        pub n_max_file: u32,
        pub lpstr_file_title: *mut c_char,
        pub n_max_file_title: u32,
        pub lpstr_def_ext: *const c_char,
        pub lpstr_initial_dir: *const c_char,
        pub lpstr_title: *const c_char,
        pub flags: u32,
        pub n_file_offset: u16,
        pub n_file_extension: u16,
        pub lpstr_def_ext2: *const c_char,
        pub pv_reserved: *mut c_void,
        pub dw_reserved: u32,
        pub flags_ex: u32,
    }

    pub const OFN_FILEMUSTEXIST: u32 = 0x0000_1000;
    pub const OFN_PATHMUSTEXIST: u32 = 0x0000_0800;
    pub const OFN_EXPLORER: u32 = 0x0008_0000;
    pub const OFN_NOCHANGEDIR: u32 = 0x0000_0008;

    pub const BIF_RETURNONLYFSDIRS: u32 = 0x0000_0001;
    pub const BIF_NEWDIALOGSTYLE: u32 = 0x0000_0040;

    #[repr(C)]
    pub struct BrowseInfoA {
        pub hwnd_owner: *mut c_void,
        pub pidl_root: *mut c_void,
        pub psz_display_name: *mut c_char,
        pub lpsz_title: *const c_char,
        pub ul_flags: u32,
        pub lpfn: *mut c_void,
        pub l_param: isize,
        pub i_image: i32,
    }

    extern "system" {
        pub fn GetOpenFileNameA(o: *mut OpenFileNameA) -> i32;
        pub fn SHBrowseForFolderA(b: *mut BrowseInfoA) -> *mut c_void;
        pub fn SHGetPathFromIDListA(pidl: *const c_void, path: *mut c_char) -> i32;
        pub fn CoTaskMemFree(p: *mut c_void);
    }
}

#[cfg(windows)]
fn windows_pick(title: &str, filters: &[(&str, &str)]) -> Picked {
    use std::ffi::CString;
    use win::*;

    // "Text files (*.iso)\0*.iso\0All files (*.*)\0*.*\0\0"
    let mut filter = String::new();
    for (label, pat) in filters {
        filter.push_str(label);
        filter.push('\0');
        filter.push_str(pat);
        filter.push('\0');
    }
    filter.push_str("All files (*.*)\0*.*\0");
    filter.push('\0');

    let c_filter = match CString::new(filter) {
        Ok(s) => s,
        Err(_) => return Picked::Unavailable,
    };
    let c_title = CString::new(title).unwrap_or_default();

    let mut buf = vec![0u8; MAX_PATH_LEN];
    let mut ofn: OpenFileNameA = unsafe { std::mem::zeroed() };
    ofn.l_struct_size = std::mem::size_of::<OpenFileNameA>() as u32;
    ofn.lpstr_filter = c_filter.as_ptr();
    ofn.lpstr_file = buf.as_mut_ptr() as *mut std::os::raw::c_char;
    ofn.n_max_file = buf.len() as u32;
    ofn.lpstr_title = c_title.as_ptr();
    ofn.flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;

    let ok = unsafe { GetOpenFileNameA(&mut ofn) };
    if ok == 0 {
        return Picked::Cancelled;
    }
    let len = buf.iter().position(|&b| b == 0).unwrap_or(buf.len());
    match std::str::from_utf8(&buf[..len]) {
        Ok(s) => Picked::Chosen(PathBuf::from(s)),
        Err(_) => Picked::Cancelled,
    }
}

#[cfg(windows)]
fn windows_pick_folder(title: &str) -> Picked {
    use std::ffi::CString;
    use win::*;

    let c_title = CString::new(title).unwrap_or_default();
    let mut display = vec![0u8; MAX_PATH_LEN];

    let mut bi: BrowseInfoA = unsafe { std::mem::zeroed() };
    bi.psz_display_name = display.as_mut_ptr() as *mut std::os::raw::c_char;
    bi.lpsz_title = c_title.as_ptr();
    bi.ul_flags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    let pidl = unsafe { SHBrowseForFolderA(&mut bi) };
    if pidl.is_null() {
        return Picked::Cancelled;
    }
    let mut path = vec![0u8; MAX_PATH_LEN];
    let ok = unsafe { SHGetPathFromIDListA(pidl, path.as_mut_ptr() as *mut std::os::raw::c_char) };
    unsafe { CoTaskMemFree(pidl) };
    if ok == 0 {
        return Picked::Cancelled;
    }
    let len = path.iter().position(|&b| b == 0).unwrap_or(path.len());
    match std::str::from_utf8(&path[..len]) {
        Ok(s) => Picked::Chosen(PathBuf::from(s)),
        Err(_) => Picked::Cancelled,
    }
}

// ===========================================================================
// Linux: zenity / kdialog, whichever is installed.
// ===========================================================================
#[cfg(all(unix, not(target_os = "macos")))]
fn unix_pick(title: &str, filters: &[(&str, &str)]) -> Picked {
    use std::process::Command;

    // Which helper do we have?
    let has = |cmd: &str| {
        Command::new("sh")
            .arg("-c")
            .arg(format!("command -v {cmd}"))
            .output()
            .map(|o| o.status.success())
            .unwrap_or(false)
    };

    if has("zenity") {
        let mut c = Command::new("zenity");
        c.arg("--file-selection")
            .arg("--title")
            .arg(title);
        // zenity takes one --file-filter per entry, "Label | pattern".
        for (label, pat) in filters {
            c.arg("--file-filter").arg(format!("{label} | {pat}"));
        }
        c.arg("--file-filter").arg("All files | *");
        return finish(&mut c);
    }

    if has("kdialog") {
        let mut c = Command::new("kdialog");
        c.arg("--title").arg(title).arg("--getopenfilename").arg(".");
        if let Some((_, pat)) = filters.first() {
            c.arg(pat);
        }
        return finish(&mut c);
    }

    Picked::Unavailable
}

#[cfg(all(unix, not(target_os = "macos")))]
fn unix_pick_folder(title: &str) -> Picked {
    use std::process::Command;

    let has = |cmd: &str| {
        Command::new("sh")
            .arg("-c")
            .arg(format!("command -v {cmd}"))
            .output()
            .map(|o| o.status.success())
            .unwrap_or(false)
    };

    if has("zenity") {
        let mut c = Command::new("zenity");
        c.arg("--file-selection").arg("--directory").arg("--title").arg(title);
        return finish(&mut c);
    }
    if has("kdialog") {
        let mut c = Command::new("kdialog");
        c.arg("--title").arg(title).arg("--getexistingdirectory").arg(".");
        return finish(&mut c);
    }

    Picked::Unavailable
}

/// Run a dialog command and turn its output into a Picked. Cancel is a non-zero
/// exit with no stdout.
#[cfg(all(unix, not(target_os = "macos")))]
fn finish(c: &mut std::process::Command) -> Picked {
    match c.output() {
        Ok(out) if out.status.success() => {
            let s = String::from_utf8_lossy(&out.stdout);
            let t = s.trim();
            if t.is_empty() {
                Picked::Cancelled
            } else {
                Picked::Chosen(PathBuf::from(t))
            }
        }
        Ok(_) => Picked::Cancelled,
        Err(_) => Picked::Unavailable,
    }
}
