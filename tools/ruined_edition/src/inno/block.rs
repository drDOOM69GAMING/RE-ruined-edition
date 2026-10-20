// The header block reader.
//
// A setup header is a sequence of blocks. From 4.0.9 each block is:
//
//   u32 crc32        (over the block's own header fields)
//   u32 stored_size  (the size of the stored data, sub-chunk CRCs included)
//   u8  compressed   (0 = stored, 1 = compressed)
//
// and before 4.0.9 instead:
//
//   u32 crc32
//   u32 compressed_size   (0xFFFFFFFF means stored)
//   u32 uncompressed_size
//
// The stored bytes are then cut into 4096-byte sub-chunks, each preceded by a
// u32 CRC-32 of that chunk. Every CRC is checked, so a damaged installer is
// caught here rather than producing a half-written asset tree.
//
// A compressed block is LZMA1 (from 4.1.6) or zlib (before that). The LZMA1
// stream Inno writes is *not* the .lzma "alone" format: it keeps the properties
// byte and the dictionary size but drops the uncompressed size, so the reader
// decodes until the stream ends. See stream/lzma.cpp in innoextract.

use super::{Compression, InnoError};
use std::io::Read;

const SUBCHUNK: usize = 4096;

pub struct BlockReader<'a> {
    data: &'a [u8],
    pos: usize,
    /// 4.1.6 and later use LZMA1 for compressed blocks; before that, zlib.
    lzma: bool,
    /// 4.0.9 and later use the shorter block header.
    modern: bool,
}

impl<'a> BlockReader<'a> {
    pub fn new(data: &'a [u8], pos: usize, modern: bool, lzma: bool) -> BlockReader<'a> {
        BlockReader { data, pos, modern, lzma }
    }

    pub fn position(&self) -> usize {
        self.pos
    }

    /// Read one block and return the decompressed bytes.
    pub fn next_block(&mut self) -> Result<Vec<u8>, InnoError> {
        let (compressed, stored_size) = self.block_header()?;

        // The stored bytes, cut into sub-chunks that each carry a CRC.
        let mut stored = Vec::with_capacity(stored_size);
        let mut consumed = 0usize;
        while consumed < stored_size {
            if self.pos + 4 > self.data.len() {
                return Err(InnoError::Format("header block ran off the end of the data".into()));
            }
            let crc = u32::from_le_bytes(self.data[self.pos..self.pos + 4].try_into().unwrap());
            self.pos += 4;
            consumed += 4;
            let want = SUBCHUNK.min(stored_size - consumed);
            if self.pos + want > self.data.len() {
                return Err(InnoError::Format("header sub-chunk ran off the end".into()));
            }
            let chunk = &self.data[self.pos..self.pos + want];
            self.pos += want;
            consumed += want;
            let got = crc32(chunk);
            if got != crc {
                return Err(InnoError::Checksum(format!(
                    "header sub-chunk CRC 0x{got:08x} does not match the stored 0x{crc:08x}"
                )));
            }
            stored.extend_from_slice(chunk);
        }

        if !compressed {
            return Ok(stored);
        }
        if self.lzma {
            lzma1_stream(&stored, None)
        } else {
            zlib(&stored)
        }
    }

    fn block_header(&mut self) -> Result<(bool, usize), InnoError> {
        if self.pos + 8 > self.data.len() {
            return Err(InnoError::Format("truncated header block".into()));
        }
        let crc = u32::from_le_bytes(self.data[self.pos..self.pos + 4].try_into().unwrap());
        self.pos += 4;

        if self.modern {
            let stored_size =
                u32::from_le_bytes(self.data[self.pos..self.pos + 4].try_into().unwrap()) as usize;
            self.pos += 4;
            if self.pos >= self.data.len() {
                return Err(InnoError::Format("truncated header block flags".into()));
            }
            let flag = self.data[self.pos];
            self.pos += 1;
            // The CRC covers the two header fields after it.
            let mut hdr = Vec::with_capacity(5);
            hdr.extend_from_slice(&(stored_size as u32).to_le_bytes());
            hdr.push(flag);
            let got = crc32(&hdr);
            if got != crc {
                return Err(InnoError::Checksum(format!(
                    "header block CRC 0x{got:08x} does not match the stored 0x{crc:08x}"
                )));
            }
            Ok((flag != 0, stored_size))
        } else {
            let compressed_size =
                u32::from_le_bytes(self.data[self.pos..self.pos + 4].try_into().unwrap());
            self.pos += 4;
            let uncompressed_size =
                u32::from_le_bytes(self.data[self.pos..self.pos + 4].try_into().unwrap()) as usize;
            self.pos += 4;
            let stored = compressed_size == 0xFFFF_FFFF;
            Ok((!stored, if stored { uncompressed_size } else { compressed_size as usize }))
        }
    }
}

/// zlib, for the header blocks of installers older than 4.1.6.
fn zlib(data: &[u8]) -> Result<Vec<u8>, InnoError> {
    let mut out = Vec::new();
    flate2::read::ZlibDecoder::new(data)
        .read_to_end(&mut out)
        .map_err(|e| InnoError::Compression(format!("zlib: {e}")))?;
    Ok(out)
}

/// A raw LZMA1 stream as Inno writes it: one properties byte, four dictionary
/// size bytes, then the range-coded data. There is no uncompressed size field,
/// so `expected` (when the caller knows it) bounds the output and `None` decodes
/// to the end of the stream.
pub fn lzma1_stream(data: &[u8], expected: Option<u64>) -> Result<Vec<u8>, InnoError> {
    if data.len() < 5 {
        return Err(InnoError::Format("LZMA1 stream is too short for its properties".into()));
    }
    let props = data[0];
    let dict_size = u32::from_le_bytes(data[1..5].try_into().unwrap());
    if dict_size == 0 {
        return Err(InnoError::Compression("LZMA1 dictionary size is zero".into()));
    }
    let mut reader = std::io::Cursor::new(&data[5..]);
    let mut dec = lzma_rust2::LzmaReader::new_with_props(
        &mut reader,
        expected.unwrap_or(u64::MAX),
        props,
        dict_size,
        None,
    )
    .map_err(|e| InnoError::Compression(format!("LZMA1: {e}")))?;
    let mut out = Vec::with_capacity(expected.unwrap_or(1 << 20) as usize);
    let mut buf = vec![0u8; 64 * 1024];
    loop {
        match dec.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => out.extend_from_slice(&buf[..n]),
            Err(e) => return Err(InnoError::Compression(format!("LZMA1: {e}"))),
        }
    }
    if let Some(n) = expected {
        if out.len() as u64 != n {
            return Err(InnoError::Compression(format!(
                "LZMA1 produced {} bytes, expected {n}",
                out.len()
            )));
        }
    }
    Ok(out)
}


/// CRC-32 (IEEE 802.3, the zlib polynomial), used by Inno for the loader table,
/// the block headers and the sub-chunks.
pub fn crc32(data: &[u8]) -> u32 {
    let mut crc = 0xFFFF_FFFFu32;
    for &b in data {
        crc ^= b as u32;
        for _ in 0..8 {
            let mask = (crc & 1).wrapping_neg();
            crc = (crc >> 1) ^ (0xEDB8_8320 & mask);
        }
    }
    !crc
}

/// The compression method a header names, for 5.3.9 and later.
pub fn from_method(v: u8) -> Option<Compression> {
    match v {
        0 => Some(Compression::Stored),
        1 => Some(Compression::Zlib),
        2 => Some(Compression::Bzip2),
        3 => Some(Compression::Lzma1),
        4 => Some(Compression::Lzma2),
        _ => None,
    }
}
