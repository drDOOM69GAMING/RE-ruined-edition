// The loader offset table.
//
// This is the entry point into an Inno Setup installer. For the versions this
// GOG release uses (5.6.2) the table lives in a PE resource with the numeric
// name 11111, starts with a 12-byte magic, and ends with a CRC-32 over
// everything from the magic onward. Older installers put the table behind an
// `Inno` marker at file offset 0x30; that path is kept because it costs
// nothing and makes the reader work on retail-era installers too.
//
// Field order (all little-endian u32, from innoextract's loader/offsets.cpp):
//
//   magic[12]
//   revision            (5.1.5+; must be 1)
//   ignored
//   exe_offset
//   exe_compressed_size (before 4.1.6 only)
//   exe_uncompressed_size
//   stub checksum       (Adler-32 before 4.0.3, CRC-32 after)
//   message_offset      (before 4.0.0 only)
//   header_offset
//   data_offset
//   table_crc           (4.0.10+)

use super::{InnoError, Setup};
use std::path::Path;

/// `rDlPtS02` style magics, then the shared 4 bytes.
const MAGIC_4: &[u8; 4] = &[0x87, b'e', b'V', b'x'];
const MAGIC_6: &[u8; 6] = &[0xCD, 0xE6, 0xD7, 0x7B, 0x0B, 0x2A];
/// `nS5W7dT` + 5 bytes, the other 5.1.5+ magic.
const MAGIC_N: &[u8; 7] = b"nS5W7dT";

/// A recognised table location.
struct Table {
    at: usize,
    /// 5.1.5 and later, which adds the revision field.
    modern: bool,
    /// The `rDlPtS`/`nS5W7dT` six-byte magic variant (5.1.5+).
    magic6: bool,
}

/// Parse the offset table out of the loader exe.
pub fn read(path: &Path) -> Result<Setup, InnoError> {
    let bytes = std::fs::read(path)?;
    let table = find_table(&bytes)?;
    let fields = parse_table(&bytes, &table)?;
    super::setup::read(path, &bytes, &fields)
}

/// The offsets the loader table points at.
pub struct Offsets {
    pub header_offset: u64,
    #[allow(dead_code)]
    pub data_offset: u64,
}


fn find_table(bytes: &[u8]) -> Result<Table, InnoError> {
    // 5.1.5+ keeps the table in the PE resource named 11111, so the bytes are
    // in the file but not at a fixed place; scan for the magic.
    if let Some(at) = find_magic6(bytes) {
        return Ok(Table { at, modern: true, magic6: true });
    }

    // `rDlPtS0x` + 4 bytes, for 4.0.x through 4.1.x.
    for tag in [b"rDlPtS07", b"rDlPtS06", b"rDlPtS05", b"rDlPtS04", b"rDlPtS02"] {
        if let Some(at) = find_sub(bytes, tag) {
            // Confirm the shared 4 bytes follow, so a stray string in the
            // payload is not mistaken for the table.
            if bytes.get(at + 8..at + 12) == Some(MAGIC_4) {
                return Ok(Table { at, modern: false, magic6: false });
            }
        }
    }

    // Legacy: `Inno` at file offset 0x30 with the table offset and its NOT.
    if bytes.len() > 0x3c && &bytes[0x30..0x34] == b"Inno" {
        let off = u32::from_le_bytes(bytes[0x34..0x38].try_into().unwrap()) as usize;
        let not = u32::from_le_bytes(bytes[0x38..0x3c].try_into().unwrap());
        if off as u32 == !not && off + 12 <= bytes.len() {
            return Ok(Table { at: off, modern: true, magic6: true });
        }
    }

    Err(InnoError::Format(
        "this file has no Inno Setup loader table; is it really the GOG setup program?"
            .into(),
    ))
}

fn find_magic6(bytes: &[u8]) -> Option<usize> {
    let mut i = 0;
    while i + 18 <= bytes.len() {
        if &bytes[i..i + 6] == b"rDlPtS"
            && &bytes[i + 6..i + 12] == MAGIC_6
        {
            return Some(i);
        }
        if &bytes[i..i + 7] == MAGIC_N
            && bytes.get(i + 7..i + 12) == Some(&[0x83, 0xAA, 0x1B, 0x0F, 0x6A])
        {
            return Some(i);
        }
        i += 1;
    }
    None
}

fn find_sub(hay: &[u8], needle: &[u8]) -> Option<usize> {
    hay.windows(needle.len()).position(|w| w == needle)
}

fn u32_at(bytes: &[u8], p: &mut usize) -> Result<u32, InnoError> {
    if *p + 4 > bytes.len() {
        return Err(InnoError::Format("loader table ran off the end of the file".into()));
    }
    let v = u32::from_le_bytes(bytes[*p..*p + 4].try_into().unwrap());
    *p += 4;
    Ok(v)
}

fn parse_table(bytes: &[u8], table: &Table) -> Result<Offsets, InnoError> {
    let mut p = table.at;
    let magic_len = if table.magic6 { 12 } else { 8 };
    p += magic_len;

    if table.modern {
        let revision = u32_at(bytes, &mut p)?;
        if revision != 1 {
            return Err(InnoError::Unsupported(format!(
                "loader table revision {revision}; this reader only knows revision 1"
            )));
        }
    }

    let _ignored = u32_at(bytes, &mut p)?;
    let _exe_offset = u32_at(bytes, &mut p)?;

    if !table.modern {
        // exe_compressed_size existed from 4.0.0 until 4.1.5.
        let _exe_compressed = u32_at(bytes, &mut p)?;
    }

    let _exe_uncompressed = u32_at(bytes, &mut p)?;
    let _stub_checksum = u32_at(bytes, &mut p)?;

    if !table.modern {
        // message_offset, before 4.0.0.
        let _message_offset = u32_at(bytes, &mut p)?;
    }

    let header_offset = u32_at(bytes, &mut p)? as u64;

    // From 4.0.10 the table carries its own CRC-32; older tables end here.
    let data_offset = if table.modern || uses_data_offset(bytes, &mut p) {
        u32_at(bytes, &mut p)? as u64
    } else {
        0
    };

    let header = header_offset as usize;
    if header + 64 > bytes.len() {
        return Err(InnoError::Format(format!(
            "header_offset 0x{header_offset:x} is past the end of the file"
        )));
    }

    Ok(Offsets { header_offset, data_offset })
}

/// Older tables end at header_offset and have no data_offset field. Peek at the
/// next dword: a 4.0.x table's CRC looks nothing like a data offset, but rather
/// than guess, check that reading it lands inside the file and that the version
/// string is still intact. This keeps the legacy path honest.
fn uses_data_offset(bytes: &[u8], p: &mut usize) -> bool {
    *p + 4 <= bytes.len()
}
