// ---------------------------------------------------------------------------
// probe.rs - "what is this file?" checks before an import copies anything.
//
// The player picks a file, and we want to say what it looks like *before*
// touching their disk. Each probe below mirrors the matching importer in
// inno.rs / iso9660.rs, but only reads enough to classify. If a probe says the
// file is the wrong kind, the launcher says so and asks again; nothing is
// copied.
//
// These are deliberately conservative: a probe returns a confident answer only
// when it has seen the real marker. "Maybe" is reported as Unrecognised so the
// player is never told a wrong file is fine.
// ---------------------------------------------------------------------------

use std::path::Path;

/// What a picked source looks like.
#[derive(PartialEq, Debug)]
pub enum Kind {
    /// A GOG / retail install folder, or a bare USA tree.
    UsaFolder,
    /// A GOG offline installer (Inno Setup).
    GogInstaller,
    /// A Biohazard JPN ISO9660 disc image.
    JpnIso,
    /// An extracted JPN tree (has JPN/STAGE1 or STAGE1).
    JpnFolder,
    /// Exists, but is none of the above.
    Unrecognised,
}

pub struct Report {
    pub kind: Kind,
    /// A sentence for the player, no jargon.
    pub detail: String,
}

/// Classify a path the player selected for the USA import.
pub fn probe_usa(src: &Path) -> Report {
    if !src.exists() {
        return Report {
            kind: Kind::Unrecognised,
            detail: format!("{} does not exist.", src.display()),
        };
    }

    if src.is_dir() {
        // The GOG install root: USA/Stage1, or the tree directly.
        if src.join("Stage1").is_dir() && src.join("Data").is_dir() {
            return Report {
                kind: Kind::UsaFolder,
                detail: "This is a USA asset folder (Stage1 and Data are here).".into(),
            };
        }
        if src.join("USA").join("Stage1").is_dir() {
            return Report {
                kind: Kind::UsaFolder,
                detail: "This is a GOG install folder (the USA tree is inside it).".into(),
            };
        }
        // One level down, the layout some installs use.
        if let Ok(rd) = std::fs::read_dir(src) {
            for e in rd.flatten() {
                let p = e.path();
                if p.is_dir() && p.join("USA").join("Stage1").is_dir() {
                    return Report {
                        kind: Kind::UsaFolder,
                        detail: format!(
                            "This is a GOG install folder (the USA tree is in {}).",
                            p.file_name().map(|n| n.to_string_lossy().to_string())
                                .unwrap_or_default()
                        ),
                    };
                }
                if p.is_dir() && p.join("Stage1").is_dir() && p.join("Data").is_dir() {
                    return Report {
                        kind: Kind::UsaFolder,
                        detail: "This folder holds a USA asset tree.".into(),
                    };
                }
            }
        }
        return Report {
            kind: Kind::Unrecognised,
            detail: "This folder has no USA game data in it. Pick the folder that \
                     contains Stage1 and Data, or the one that contains USA."
                .into(),
        };
    }

    // A file: the game's own exe, an installer, or a loose archive.
    if crate::paths::looks_like_installer(src) {
        return Report {
            kind: Kind::GogInstaller,
            detail: "This is a GOG installer. Its game files can be unpacked directly.".into(),
        };
    }

    // ResidentEvil.exe sitting inside the install root.
    let stem = src
        .file_stem()
        .map(|s| s.to_string_lossy().to_ascii_lowercase())
        .unwrap_or_default();
    if stem == "residentevil" || stem == "biohazard" {
        if let Some(parent) = src.parent() {
            if parent.join("USA").join("Stage1").is_dir() || parent.join("Stage1").is_dir() {
                return Report {
                    kind: Kind::UsaFolder,
                    detail: "This is the game program, and the game data is beside it.".into(),
                };
            }
        }
        return Report {
            kind: Kind::Unrecognised,
            detail: "That looks like the game program, but there is no USA game data \
                     in the folder it sits in."
                .into(),
        };
    }

    Report {
        kind: Kind::Unrecognised,
        detail: "That file is not a GOG folder, a GOG installer, or the game program.".into(),
    }
}

/// Classify a path the player selected for the JPN import.
pub fn probe_jpn(src: &Path) -> Report {
    if !src.exists() {
        return Report {
            kind: Kind::Unrecognised,
            detail: format!("{} does not exist.", src.display()),
        };
    }

    if src.is_dir() {
        if src.join("JPN").join("STAGE1").is_dir()
            || (src.join("STAGE1").is_dir() && src.join("DATA").is_dir())
        {
            return Report {
                kind: Kind::JpnFolder,
                detail: "This is an extracted Biohazard JPN folder.".into(),
            };
        }
        return Report {
            kind: Kind::Unrecognised,
            detail: "That folder has no JPN game data in it.".into(),
        };
    }

    if crate::paths::looks_like_iso(src) {
        // Read the disc label so the player can see it is the right disc.
        let label = iso_label(src);
        return Report {
            kind: Kind::JpnIso,
            detail: match label {
                Some(l) if !l.is_empty() => {
                    format!("This is an ISO9660 disc image, labelled \"{l}\".")
                }
                _ => "This is an ISO9660 disc image.".to_string(),
            },
        };
    }

    Report {
        kind: Kind::Unrecognised,
        detail: "That is not an ISO9660 disc image. Point at the .iso, not a .bin or .cue."
            .into(),
    }
}


/// Read the volume label out of an ISO9660 primary volume descriptor. The label
/// sits at 0x8028 in the 2048-byte sector at offset 0x8000.
fn iso_label(path: &Path) -> Option<String> {
    use std::io::{Read, Seek, SeekFrom};
    let mut f = std::fs::File::open(path).ok()?;
    f.seek(SeekFrom::Start(0x8000 + 0x28)).ok()?;
    let mut buf = [0u8; 32];
    f.read_exact(&mut buf).ok()?;
    let s: String = buf
        .iter()
        .take_while(|&&b| b != 0)
        .map(|&b| if b.is_ascii_graphic() || b == b' ' { b as char } else { ' ' })
        .collect();
    Some(s.trim().to_string())
}
