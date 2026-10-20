// Getting the USA (English) assets out of whatever the player selected.
//
// Three sources are supported, in order of how sure we are they will work:
//
//   1. An installed GOG / retail folder - the one with `USA/` and
//      `ResidentEvil.exe` sitting in it. We just copy the tree. This always
//      works and needs nothing else on the machine.
//   2. A GOG offline installer (.exe) - an Inno Setup archive. We decode it
//      ourselves, in this crate, and pull `USA/` straight out of it. No
//      `innoextract`, no `7z`, nothing to install.
//   3. A bare `USA/` tree the player already extracted somewhere.
//
// The installer reader lives in the parent module. This file is the part that
// knows what a USA tree looks like and where to put it.

use super::{find_slice, Setup, Slice};
use std::path::{Path, PathBuf};

const ASSET_DIRS: &[&str] = &[
    "Data", "Effspr", "Enemy", "Item_m1", "Item_m2", "Movie", "Objspr",
    "Players", "Sound", "Stage1", "Stage2", "Stage3", "Stage4", "Stage5",
    "Stage6", "Stage7", "Voice",
];

/// Import USA into `root`. Returns the number of files copied.
pub fn import_usa(root: &Path, src: &Path) -> Result<usize, String> {
    if !src.exists() {
        return Err(format!("{} does not exist", src.display()));
    }

    // Case 1/3: a folder, either the GOG install root or a bare USA tree.
    if src.is_dir() {
        let usa = if src.join("Stage1").is_dir() {
            src.to_path_buf()
        } else if src.join("USA").join("Stage1").is_dir() {
            src.join("USA")
        } else {
            return Err(format!(
                "{} has no USA/Stage1 - is that the GOG folder?",
                src.display()
            ));
        };
        return copy_region(&usa, &root.join("USA"));
    }

    // Case 2: a folder-less file. If the player selected the game's own .exe
    // (ResidentEvil.exe / Biohazard.exe, which sits inside the GOG install
    // root) then the assets are beside it, so treat the parent as the folder.
    if let Some(parent) = src.parent() {
        let stem = src
            .file_stem()
            .map(|s| s.to_string_lossy().to_ascii_lowercase())
            .unwrap_or_default();
        if (stem == "residentevil" || stem == "biohazard")
            && (parent.join("USA").join("Stage1").is_dir() || parent.join("Stage1").is_dir())
        {
            let usa = if parent.join("USA").join("Stage1").is_dir() {
                parent.join("USA")
            } else {
                parent.to_path_buf()
            };
            return copy_region(&usa, &root.join("USA"));
        }
    }

    // Case 3: an installer. Decode it here, in Rust.
    import_from_installer(root, src)
}

/// Pull the game data out of an Inno Setup installer (the GOG offline one).
fn import_from_installer(root: &Path, src: &Path) -> Result<usize, String> {
    let setup: Setup = super::open(src).map_err(|e| {
        format!(
            "{src} is not an Inno Setup installer this can read ({e}). \
             Point me at your already-installed GOG folder instead.",
            src = src.display()
        )
    })?;


    // The chunks live in the .bin slice that sits beside the setup exe. A
    // single-file installer keeps them inside the exe, but GOG splits them.
    let slice_path = find_slice(src).ok_or_else(|| {
        format!(
            "the data slice for {} is missing; keep the -1.bin file next to it",
            src.display()
        )
    })?;
    let slice = Slice::open(&slice_path).map_err(|e| e.to_string())?;

    // Write into a staging folder, then move just the asset folders into place
    // so a failure never leaves a half-written USA tree behind.
    let staging = tmp_dir("re-usa")?;
    let n_inno = setup
        .extract_prefix(&slice, "USA/", &staging)
        .map_err(|e| format!("could not read the installer data: {e}"))?;

    let extracted = find_usa(&staging).ok_or("the installer held no USA/ tree")?;
    let n = copy_region(&extracted, &root.join("USA"))?;
    let _ = std::fs::remove_dir_all(&staging);
    if n == 0 {
        return Err(format!("extracted {n_inno} files but none were game assets"));
    }
    Ok(n)
}

/// Search a tree for a folder that looks like a USA asset root.
fn find_usa(base: &Path) -> Option<PathBuf> {
    if base.join("Stage1").is_dir() && base.join("Data").is_dir() {
        return Some(base.to_path_buf());
    }
    if base.join("USA").join("Stage1").is_dir() {
        return Some(base.join("USA"));
    }
    // One more level down covers `<tmp>/Resident Evil/USA`.
    if let Ok(rd) = std::fs::read_dir(base) {
        for e in rd.flatten() {
            let p = e.path();
            if p.is_dir() {
                if p.join("USA").join("Stage1").is_dir() {
                    return Some(p.join("USA"));
                }
                if p.join("Stage1").is_dir() && p.join("Data").is_dir() {
                    return Some(p);
                }
            }
        }
    }
    None
}

/// Copy the asset subfolders from `src` (a USA root) into `dst`.
fn copy_region(src: &Path, dst: &Path) -> Result<usize, String> {
    std::fs::create_dir_all(dst).map_err(|e| e.to_string())?;
    let mut n = 0;
    for name in ASSET_DIRS {
        let from = src.join(name);
        if !from.is_dir() {
            continue;
        }
        n += copy_tree(&from, &dst.join(name))?;
    }
    if n == 0 {
        return Err("no asset folders found to copy".into());
    }
    Ok(n)
}

fn copy_tree(src: &Path, dst: &Path) -> Result<usize, String> {
    std::fs::create_dir_all(dst).map_err(|e| e.to_string())?;
    let mut n = 0;
    for entry in std::fs::read_dir(src).map_err(|e| e.to_string())? {
        let entry = entry.map_err(|e| e.to_string())?;
        let from = entry.path();
        let to = dst.join(entry.file_name());
        if from.is_dir() {
            n += copy_tree(&from, &to)?;
        } else if std::fs::copy(&from, &to).is_ok() {
            n += 1;
        }
    }
    Ok(n)
}

fn tmp_dir(tag: &str) -> Result<PathBuf, String> {
    let base = std::env::temp_dir().join(format!("{tag}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&base);
    std::fs::create_dir_all(&base).map_err(|e| e.to_string())?;
    Ok(base)
}
