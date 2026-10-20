// ISO9660 reader for the Japanese MediaKite disc.
//
// The MediaKite release is a plain PC data disc: the game assets live under
// `horr/JPN/` with the same subfolder names the port resolves (DATA, ENEMY,
// STAGE1..7, ...). We walk the ISO9660 directory records directly, so no
// mounting, no root, no external tools - the player just picks the .iso.
//
// ISO9660 is a stable, fully public format; only the primary volume descriptor
// and the directory record layout are needed here, and both are fixed.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

const SECTOR: u64 = 2048;
const PVD_LBA: u64 = 16;

struct Iso {
    file: File,
}

#[derive(Clone)]
struct Entry {
    name: String,
    lba: u64,
    size: u64,
    is_dir: bool,
}

impl Iso {
    fn open(path: &Path) -> Result<Iso, String> {
        let mut file = File::open(path).map_err(|e| e.to_string())?;
        file.seek(SeekFrom::Start(PVD_LBA * SECTOR)).map_err(|e| e.to_string())?;
        let mut vd = [0u8; 2048];
        file.read_exact(&mut vd).map_err(|e| e.to_string())?;
        if vd[0] != 1 || &vd[1..6] != b"CD001" {
            return Err("not an ISO9660 image".into());
        }
        Ok(Iso { file })
    }

    fn root(&mut self) -> Result<Entry, String> {
        self.file.seek(SeekFrom::Start(PVD_LBA * SECTOR)).map_err(|e| e.to_string())?;
        let mut vd = [0u8; 2048];
        self.file.read_exact(&mut vd).map_err(|e| e.to_string())?;
        let rec = &vd[156..190];
        Ok(Entry {
            name: "/".into(),
            lba: le32(&rec[2..6]) as u64,
            size: le32(&rec[10..14]) as u64,
            is_dir: true,
        })
    }

    fn read_dir(&mut self, dir: &Entry) -> Result<Vec<Entry>, String> {
        self.file.seek(SeekFrom::Start(dir.lba * SECTOR)).map_err(|e| e.to_string())?;
        let mut data = vec![0u8; dir.size as usize];
        self.file.read_exact(&mut data).map_err(|e| e.to_string())?;

        let mut out = Vec::new();
        let mut o = 0usize;
        while o < data.len() {
            let len = data[o] as usize;
            if len == 0 {
                // Records never straddle a sector boundary; skip the padding.
                o = (o / SECTOR as usize + 1) * SECTOR as usize;
                continue;
            }
            let rec = &data[o..o + len];
            let name_len = rec[32] as usize;
            let raw = &rec[33..33 + name_len];
            let flags = rec[25];
            let is_dir = flags & 2 != 0;
            // 0x00 and 0x01 are "." and "..".
            if !(name_len == 1 && (raw[0] == 0 || raw[0] == 1)) {
                let name = decode_name(raw);
                out.push(Entry {
                    name,
                    lba: le32(&rec[2..6]) as u64,
                    size: le32(&rec[10..14]) as u64,
                    is_dir,
                });
            }
            o += len;
        }
        Ok(out)
    }

    /// Follow a `/`-separated path from the root, case-insensitively.
    fn find(&mut self, path: &str) -> Result<Entry, String> {
        let mut cur = self.root()?;
        for part in path.split('/').filter(|s| !s.is_empty()) {
            let entries = self.read_dir(&cur)?;
            cur = entries
                .into_iter()
                .find(|e| e.name.eq_ignore_ascii_case(part))
                .ok_or_else(|| format!("{part} not found on the disc"))?;
        }
        Ok(cur)
    }
}

/// Import the JPN assets. Accepts an ISO image or an already-extracted folder.
pub fn import_jpn(root: &Path, src: &Path) -> Result<usize, String> {
    if !src.exists() {
        return Err(format!("{} does not exist", src.display()));
    }
    let dst = root.join("JPN");

    // A folder that already holds the JPN tree.
    if src.is_dir() {
        let base = if src.join("Stage1").is_dir() || src.join("STAGE1").is_dir() {
            src.to_path_buf()
        } else if src.join("JPN").join("STAGE1").is_dir() {
            src.join("JPN")
        } else {
            return Err(format!(
                "{} has no JPN/STAGE1 - point me at the disc or the extracted folder",
                src.display()
            ));
        };
        return copy_assets(&base, &dst);
    }

    // An ISO image: locate horr/JPN and copy its asset subfolders out.
    if !super::paths::looks_like_iso(src) {
        return Err("that file is not an ISO9660 image".into());
    }
    let mut iso = Iso::open(src)?;

    // The MediaKite disc nests the tree under horr/JPN. Some images ship it at
    // the root instead, so try both.
    let base = match iso.find("horr/JPN") {
        Ok(e) if e.is_dir => e,
        _ => iso
            .find("JPN")
            .or_else(|_| iso.root())
            .map_err(|e| e)?,
    };

    let entries = iso.read_dir(&base)?;
    std::fs::create_dir_all(&dst).map_err(|e| e.to_string())?;
    let mut n = 0;
    for entry in entries {
        let want = classify(&entry.name);
        if !entry.is_dir || !want {
            continue;
        }
        n += copy_iso_dir(&mut iso, &entry, &dst.join(canonical(&entry.name)))?;
    }
    if n == 0 {
        return Err("no asset folders found on the disc (expected horr/JPN)".into());
    }
    Ok(n)
}

/// True if this disc folder name is one of the asset folders the port wants.
fn classify(name: &str) -> bool {
    let u = name.to_ascii_uppercase();
    matches!(
        u.as_str(),
        "DATA" | "EFFSPR" | "ENEMY" | "ITEM_M1" | "ITEM_M2" | "MOVIE" | "OBJSPR"
            | "PLAYERS" | "SOUND" | "VOICE"
    ) || (u.starts_with("STAGE") && u.len() == 6)
}

/// Map a disc folder name onto the capitalisation the port resolves.
fn canonical(name: &str) -> String {
    let u = name.to_ascii_uppercase();
    match u.as_str() {
        "DATA" => "Data",
        "EFFSPR" => "Effspr",
        "ENEMY" => "Enemy",
        "ITEM_M1" => "Item_m1",
        "ITEM_M2" => "Item_m2",
        "MOVIE" => "Movie",
        "OBJSPR" => "Objspr",
        "PLAYERS" => "Players",
        "SOUND" => "Sound",
        "VOICE" => "Voice",
        _ if u.starts_with("STAGE") => {
            // STAGE1 -> Stage1
            let n = &u[5..];
            return format!("Stage{n}");
        }
        _ => return name.to_string(),
    }
    .to_string()
}

fn copy_iso_dir(iso: &mut Iso, dir: &Entry, dst: &Path) -> Result<usize, String> {
    std::fs::create_dir_all(dst).map_err(|e| e.to_string())?;
    let mut n = 0;
    for e in iso.read_dir(dir)? {
        let to = dst.join(&e.name);
        if e.is_dir {
            n += copy_iso_dir(iso, &e, &to)?;
        } else {
            iso.file.seek(SeekFrom::Start(e.lba * SECTOR)).map_err(|x| x.to_string())?;
            let mut buf = vec![0u8; e.size as usize];
            iso.file.read_exact(&mut buf).map_err(|x| x.to_string())?;
            std::fs::write(&to, &buf).map_err(|x| x.to_string())?;
            n += 1;
        }
    }
    Ok(n)
}

fn copy_assets(src: &Path, dst: &Path) -> Result<usize, String> {
    std::fs::create_dir_all(dst).map_err(|e| e.to_string())?;
    let mut n = 0;
    for entry in std::fs::read_dir(src).map_err(|e| e.to_string())? {
        let entry = entry.map_err(|e| e.to_string())?;
        let from = entry.path();
        if !from.is_dir() {
            continue;
        }
        let name = entry.file_name().to_string_lossy().to_string();
        if !classify(&name) {
            continue;
        }
        n += copy_tree(&from, &dst.join(canonical(&name)))?;
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

/// Strip the `;1` version suffix and normalise the name.
fn decode_name(raw: &[u8]) -> String {
    let s: String = raw.iter().map(|&b| b as char).collect();
    match s.split(';').next() {
        Some(x) => x.to_string(),
        None => s,
    }
}

fn le32(b: &[u8]) -> u32 {
    u32::from_le_bytes([b[0], b[1], b[2], b[3]])
}
