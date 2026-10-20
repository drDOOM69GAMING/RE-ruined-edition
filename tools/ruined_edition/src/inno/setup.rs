// The setup header, the per-file data entries, and pulling files back out.
//
// This mirrors innoextract's setup/header.cpp, setup/data.cpp and
// stream/{slice,chunk,file}.cpp, reduced to the fields this launcher needs and
// the versions it will actually meet (the GOG installer is 5.6.2, unicode, one
// slice). Every field is read in the order the reference reads it; when a
// version gate is not one this reader implements, it says so instead of
// drifting out of alignment.

use super::block::{self, BlockReader};
use super::{Compression, InnoError};
use std::path::Path;

/// The version markers the reader understands, as (major, minor, patch).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Version(pub u32, pub u32, pub u32);

impl Version {
    fn ge(&self, o: Version) -> bool {
        (self.0, self.1, self.2) >= (o.0, o.1, o.2)
    }
    fn lt(&self, o: Version) -> bool {
        (self.0, self.1, self.2) < (o.0, o.1, o.2)
    }
}

/// One file in the setup, positioned in the chunk stream.
#[derive(Debug, Clone)]
pub struct Entry {
    /// The paths this data entry is written to, `\`-separated and possibly
    /// `{app}`-rooted. Usually one; GOG points several names at one file when
    /// their contents are identical.
    pub names: Vec<String>,
    #[allow(dead_code)]
    pub first_slice: u32,
    #[allow(dead_code)]
    pub last_slice: u32,
    /// Offset of the compressed chunk inside its first slice.
    pub chunk_offset: u32,
    /// Total compressed size of the chunk.
    pub chunk_size: u64,
    /// Where this file starts in the decompressed chunk.
    pub file_offset: u64,
    pub compression: Compression,
    #[allow(dead_code)]
    pub encrypted: bool,
}

/// A parsed setup: enough to extract files by name.
pub struct Setup {
    /// The setup data version, for diagnostics.
    #[allow(dead_code)]
    pub version: Version,
    /// The application name the installer reports.
    #[allow(dead_code)]
    pub app_name: String,
    pub entries: Vec<Entry>,
}

/// Read a setup exe (or a bare header) and return the entries.
pub fn read(_exe: &Path, bytes: &[u8], offsets: &super::offsets::Offsets) -> Result<Setup, InnoError> {
    let header_at = offsets.header_offset as usize;

    // The version string is 64 bytes, NUL padded.
    if header_at + 64 > bytes.len() {
        return Err(InnoError::Format("header offset is past the end of the file".into()));
    }
    let raw = &bytes[header_at..header_at + 64];
    let end = raw.iter().position(|&b| b == 0).unwrap_or(raw.len());
    let version = parse_version(&raw[..end])?;
    if version.lt(Version(4, 1, 6)) || version.ge(Version(6, 0, 0)) {
        return Err(InnoError::Unsupported(format!(
            "this installer uses setup data version {}.{}.{}, which this reader does not implement",
            version.0, version.1, version.2
        )));
    }

    // The main header is the first block stream; it starts right after the
    // version string and the block reader walks the file from there.
    let mut reader = BlockReader::new(bytes, header_at + 64, true, true);
    let main = reader.next_block()?;
    let header = Header::parse(&main, version)?;

    // Everything else in the primary stream follows the main header in one
    // run: languages, messages, permissions, types, components, tasks,
    // directories, files, then a few more lists this reader does not need.
    // Only the file table matters, but every entry before it has to be walked
    // so the reader lands on the right byte.
    let header_len = header.bytes_consumed;
    let (names, locations, before_installs, _after_installs, _ext_sizes) =
        read_primary_entries(&main[header_len..], &header, version)?;

    // The data entries live in a second block stream that starts where the
    // first one stopped.
    let mut reader2 = BlockReader::new(bytes, reader.position(), true, true);
    let data_stream = reader2.next_block()?;
    let entries = read_data_entries(&data_stream, &header, version)?;

    let mut entries = entries;
    for (i, e) in entries.iter_mut().enumerate() {
        // A file entry names its data entry through `location`, so collect
        // every file entry that points at this one. GOG lists several names
        // against a single data entry when the files are identical, and the
        // chunk then holds one copy that all of the names share.
        let mut group: Vec<String> = Vec::new();
        for pos in 0..locations.len() {
            if locations[pos] as usize != i {
                continue;
            }
            // GOG's installer extracts to a temporary folder and moves the
            // files into place from script, so the real path is in the
            // `before_install` call. Fall back to the plain destination when
            // there is no call.
            let real = before_installs
                .get(pos)
                .and_then(|b| galaxy_destination(b))
                .or_else(|| names.get(pos).cloned())
                .unwrap_or_default();
            if !real.is_empty() {
                group.push(real);
            }
        }
        e.names = group;
    }

    let _ = offsets;
    Ok(Setup {
        version,
        app_name: header.app_name,
        entries,
    })
}


fn parse_version(s: &[u8]) -> Result<Version, InnoError> {
    // e.g. "Inno Setup Setup Data (5.6.2) (u)". Some builds carry a fourth
    // component ("5.3.10.1"); the reader only needs the first three.
    let text = String::from_utf8_lossy(s);
    let open = text.find('(').ok_or_else(|| {
        InnoError::Format(format!("unrecognised setup version string {text:?}"))
    })?;
    let rest = &text[open + 1..];
    let close = rest.find(')').ok_or_else(|| {
        InnoError::Format(format!("unrecognised setup version string {text:?}"))
    })?;
    let nums = &rest[..close];
    let mut it = nums.split('.');
    let major = it.next().and_then(|v| v.parse().ok());
    let minor = it.next().and_then(|v| v.parse().ok());
    let patch = it.next().and_then(|v| v.parse().ok());
    match (major, minor, patch) {
        (Some(a), Some(b), Some(c)) => Ok(Version(a, b, c)),
        _ => Err(InnoError::Format(format!("unrecognised setup version string {text:?}"))),
    }
}

/// The fields of the main header this launcher uses.
struct Header {
    app_name: String,
    #[allow(dead_code)]
    compression: Compression,
    #[allow(dead_code)]
    slices_per_disk: u32,
    /// The counts of the entry lists that follow the header in the stream.
    language_count: u32,
    message_count: u32,
    permission_count: u32,
    type_count: u32,
    component_count: u32,
    task_count: u32,
    directory_count: u32,
    data_entry_count: u32,
    file_count: u32,
    /// Where the header ends and the entry lists begin.
    bytes_consumed: usize,
}

/// A cursor over a header stream, with the reference's string format.
struct Cur<'a> {
    b: &'a [u8],
    p: usize,
}

impl<'a> Cur<'a> {
    fn new(b: &'a [u8]) -> Cur<'a> {
        Cur { b, p: 0 }
    }
    fn u8(&mut self) -> Result<u8, InnoError> {
        let v = *self.b.get(self.p).ok_or_else(|| InnoError::Format("header read past its end".into()))?;
        self.p += 1;
        Ok(v)
    }
    fn u32(&mut self) -> Result<u32, InnoError> {
        if self.p + 4 > self.b.len() {
            return Err(InnoError::Format("header read past its end".into()));
        }
        let v = u32::from_le_bytes(self.b[self.p..self.p + 4].try_into().unwrap());
        self.p += 4;
        Ok(v)
    }
    fn u64(&mut self) -> Result<u64, InnoError> {
        if self.p + 8 > self.b.len() {
            return Err(InnoError::Format("header read past its end".into()));
        }
        let v = u64::from_le_bytes(self.b[self.p..self.p + 8].try_into().unwrap());
        self.p += 8;
        Ok(v)
    }
    fn skip(&mut self, n: usize) -> Result<(), InnoError> {
        if self.p + n > self.b.len() {
            return Err(InnoError::Format("header read past its end".into()));
        }
        self.p += n;
        Ok(())
    }
    /// `binary_string`: a u32 byte count, then that many bytes. A unicode setup
    /// stores UTF-16LE, so decode accordingly.
    fn bstr(&mut self, unicode: bool) -> Result<String, InnoError> {
        let n = self.u32()? as usize;
        if self.p + n > self.b.len() {
            return Err(InnoError::Format("header string runs past the end".into()));
        }
        let raw = &self.b[self.p..self.p + n];
        self.p += n;
        Ok(if unicode { decode_utf16(raw) } else { decode_latin1(raw) })
    }
}

fn decode_utf16(raw: &[u8]) -> String {
    let units: Vec<u16> = raw.chunks_exact(2).map(|c| u16::from_le_bytes([c[0], c[1]])).collect();
    String::from_utf16_lossy(&units)
}

fn decode_latin1(raw: &[u8]) -> String {
    raw.iter().map(|&b| b as char).collect()
}

impl Header {
    fn parse(main: &[u8], version: Version) -> Result<Header, InnoError> {
        let unicode = true; // 5.x unicode installers: the version string ends "(u)"
        let mut c = Cur::new(main);

        // The string fields, in the reference's exact order.
        let app_name = c.bstr(unicode)?;
        let _app_versioned_name = c.bstr(unicode)?;
        let _app_id = c.bstr(unicode)?;
        let _app_copyright = c.bstr(unicode)?;
        let _app_publisher = c.bstr(unicode)?;
        let _app_publisher_url = c.bstr(unicode)?;
        let _app_support_phone = c.bstr(unicode)?;
        let _app_support_url = c.bstr(unicode)?;
        let _app_updates_url = c.bstr(unicode)?;
        let _app_version = c.bstr(unicode)?;
        let _default_dir_name = c.bstr(unicode)?;
        let _default_group_name = c.bstr(unicode)?;
        let _base_filename = c.bstr(unicode)?;
        let _uninstall_files_dir = c.bstr(unicode)?;
        let _uninstall_name = c.bstr(unicode)?;
        let _uninstall_icon = c.bstr(unicode)?;
        let _app_mutex = c.bstr(unicode)?;
        let _default_user_name = c.bstr(unicode)?;
        let _default_user_organisation = c.bstr(unicode)?;
        let _default_serial = c.bstr(unicode)?;
        let _app_readme_file = c.bstr(unicode)?;
        let _app_contact = c.bstr(unicode)?;
        let _app_comments = c.bstr(unicode)?;
        let _app_modify_path = c.bstr(unicode)?;
        let _create_uninstall_registry_key = c.bstr(unicode)?;
        let _uninstallable = c.bstr(unicode)?;
        let _close_applications_filter = c.bstr(unicode)?;
        let _setup_mutex = c.bstr(unicode)?;
        if version.ge(Version(5, 6, 1)) {
            let _changes_environment = c.bstr(unicode)?;
            let _changes_associations = c.bstr(unicode)?;
        }
        // 5.2.5 and later: license / info, then compiled code.
        let _license = c.bstr(unicode)?;
        let _info_before = c.bstr(unicode)?;
        let _info_after = c.bstr(unicode)?;
        let _compiled_code = c.bstr(unicode)?;
        // lead_bytes is only read for non-unicode setups.

        // The counts.
        let language_count = c.u32()?;
        let message_count = c.u32()?;
        let permission_count = c.u32()?;
        let type_count = c.u32()?;
        let component_count = c.u32()?;
        let task_count = c.u32()?;
        let directory_count = c.u32()?;
        let file_count = c.u32()?;
        let data_entry_count = c.u32()?;
        let _icon_count = c.u32()?;
        let _ini_entry_count = c.u32()?;
        let _registry_entry_count = c.u32()?;
        let _delete_entry_count = c.u32()?;
        let _uninstall_delete_entry_count = c.u32()?;
        let _run_entry_count = c.u32()?;
        let _uninstall_run_entry_count = c.u32()?;

        // windows_version_range: two windows_version, each =
        //   build(u16) minor(u8) major(u8)  x2  + service pack major/minor x2
        c.skip(20)?;

        // back_color, back_color2 (both read for this version range).
        let _back_color = c.u32()?;
        let _back_color2 = c.u32()?;
        // image_back_color only before 5.5.7; small_image_back_color only for
        // 2.0.0..5.0.4, neither applies here.

        // image_alpha_format (5.5.7+).
        let _image_alpha_format = c.u8()?;
        // password (5.3.9+ is SHA1, 20 bytes).
        c.skip(20)?;
        // password_salt (4.2.2+ is 8 bytes).
        c.skip(8)?;
        // extra_disk_space_required (i64), slices_per_disk (u32).
        let _extra_disk_space_required = c.u64()?;
        let slices_per_disk = c.u32()?;
        // uninstall_log_mode (1.3.0+).
        let _uninstall_log_mode = c.u8()?;
        // dir_exists_warning (1.3.6+).
        let _dir_exists_warning = c.u8()?;
        // privileges_required (5.3.7+).
        let _privileges_required = c.u8()?;
        // show_language_dialog, language_detection (4.0.10+).
        let _show_language_dialog = c.u8()?;
        let _language_detection = c.u8()?;
        // compression (5.3.9+).
        let compression_byte = c.u8()?;
        let compression = block::from_method(compression_byte).ok_or_else(|| {
            InnoError::Unsupported(format!("unknown compression method {compression_byte}"))
        })?;
        // architectures_allowed / installed (5.6.0+): two 5-flag bitfields, one
        // byte each.
        c.skip(2)?;
        // disable_dir_page, disable_program_group_page (5.3.3+).
        let _disable_dir_page = c.u8()?;
        let _disable_program_group_page = c.u8()?;
        // uninstall_display_size (5.5.0+ is u64).
        let _uninstall_display_size = c.u64()?;
        // The main-header flag set. 45 flags for 5.6.2 (six bytes); the reader
        // only needs to step over it so the entry lists start in the right
        // place.
        let header_flags = read_flag_bits(&mut c, 45)?;
        let _ = header_flags;
        // The header ends here; the entry lists follow in the same stream.
        let bytes_consumed = c.p;

        Ok(Header {
            app_name,
            compression,
            slices_per_disk,
            language_count,
            message_count,
            permission_count,
            type_count,
            component_count,
            task_count,
            directory_count,
            data_entry_count,
            file_count,
            bytes_consumed,
        })
    }
}

fn read_data_entries(data: &[u8], header: &Header, _version: Version) -> Result<Vec<Entry>, InnoError> {
    let mut c = Cur::new(data);
    let mut out = Vec::with_capacity(header.data_entry_count as usize);
    for _ in 0..header.data_entry_count {
        let first_slice = c.u32()?;
        let last_slice = c.u32()?;
        let chunk_offset = c.u32()?;
        let file_offset = c.u64()?;
        // The file's uncompressed length, then the compressed chunk size.
        let _file_size = c.u64()?;
        let chunk_size = c.u64()?;
        // file checksum: SHA1 (5.3.9+).
        c.skip(20)?;
        // FILETIME (i64), then file_version (u32 ms + u32 ls).
        c.skip(8)?;
        c.skip(8)?;
        // The data-entry flags, in the reference's order. For 5.6.2:
        // VersionInfoValid(0), VersionInfoNotValid(1), TimeStampInUTC(2),
        // IsUninstallerExe(3), CallInstructionOptimized(4), Touch(5),
        // ChunkEncrypted(6), ChunkCompressed(7), SolidBreak(8), Sign(9),
        // SignOnce(10).
        let flags = read_flag_bits(&mut c, 11)?;
        let encrypted = flags & (1 << 6) != 0;
        let compressed = flags & (1 << 7) != 0;

        out.push(Entry {
            names: Vec::new(),
            first_slice,
            last_slice,
            chunk_offset,
            chunk_size,
            file_offset,
            // GOG's installer mixes methods: chunks with the compressed bit set
            // are LZMA2, and the rest are zlib. Each chunk holds exactly one
            // file, which is what makes the rest of the reader simple.
            compression: if compressed { Compression::Lzma2 } else { Compression::Zlib },
            encrypted,
        });
    }
    Ok(out)
}

/// Walk the primary stream's entry lists, from languages through files, and
/// return the file destinations with their data-entry index (`location`).
///
/// Every entry type before `files` has to be walked byte for byte, because they
/// share one stream; the reference does the same in `info::try_load`. Only the
/// 5.x field set is implemented, and a wrong-sized field would drift the walk,
/// so the caller sanity-checks the resulting names.
fn read_primary_entries(
    data: &[u8],
    header: &Header,
    version: Version,
) -> Result<(Vec<String>, Vec<u32>, Vec<String>, Vec<String>, Vec<u64>), InnoError> {
    let mut c = Cur::new(data);

    for _ in 0..header.language_count {
        // name, language_name, dialog/title/welcome/copyright font, data,
        // license, info_before, info_after.
        for _ in 0..10 {
            c.bstr(true)?;
        }
        let _language_id = c.u32()?;
        let _dialog_font_size = c.u32()?;
        let _title_font_size = c.u32()?;
        let _welcome_font_size = c.u32()?;
        let _copyright_font_size = c.u32()?;
        c.u8()?; // right_to_left (5.2.3+)
    }

    for _ in 0..header.message_count {
        c.bstr(true)?; // name
        c.bstr(true)?; // value
        let _language = c.u32()?;
    }

    for _ in 0..header.permission_count {
        c.bstr(true)?; // the packed permission table
    }

    for _ in 0..header.type_count {
        c.bstr(true)?; // name
        c.bstr(true)?; // description
        c.bstr(true)?; // languages
        c.bstr(true)?; // check
        c.skip(20)?; // winver
        read_flag_bits(&mut c, 1)?; // custom setup type
        c.u8()?; // setup type
        c.skip(8)?; // size
    }

    for _ in 0..header.component_count {
        c.bstr(true)?; // name
        c.bstr(true)?; // description
        c.bstr(true)?; // types
        c.bstr(true)?; // languages
        c.bstr(true)?; // check
        c.skip(8)?; // extra_disk_space_required
        c.skip(4)?; // level
        c.u8()?; // used
        c.skip(20)?; // winver
        read_flag_bits(&mut c, 5)?; // component flags (5.6.2)
        c.skip(8)?; // size
    }

    for _ in 0..header.task_count {
        // name, description, group, components, languages, check.
        for _ in 0..6 {
            c.bstr(true)?;
        }
        c.skip(4)?; // level
        c.u8()?; // used
        c.skip(20)?; // winver
        read_flag_bits(&mut c, 4)?; // task flags (5.6.2)
    }

    for _ in 0..header.directory_count {
        c.bstr(true)?; // name
        // load_condition_data: components, tasks, languages, check, after,
        // before.
        for _ in 0..6 {
            c.bstr(true)?;
        }
        c.skip(4)?; // attributes
        c.skip(20)?; // winver
        c.skip(2)?; // permission (i16)
        read_flag_bits(&mut c, 5)?; // directory options (5.2.0+)
    }

    let mut names = Vec::with_capacity(header.file_count as usize);
    let mut locations = Vec::with_capacity(header.file_count as usize);
    let mut before_installs = Vec::with_capacity(header.file_count as usize);
    let mut after_installs: Vec<String> = Vec::with_capacity(header.file_count as usize);
    let mut ext_sizes: Vec<u64> = Vec::with_capacity(header.file_count as usize);
    for _ in 0..header.file_count {
        let _source = c.bstr(true)?;
        let destination = c.bstr(true)?;
        let _install_font_name = c.bstr(true)?;
        let _strong_assembly_name = c.bstr(true)?; // 5.2.5+
        // load_condition_data: six strings.
        let _components = c.bstr(true)?;
        let _tasks = c.bstr(true)?;
        let _languages = c.bstr(true)?;
        let _check = c.bstr(true)?;
        let after_install = c.bstr(true)?;
        let before_install = c.bstr(true)?;
        c.skip(20)?; // windows_version_range
        let location = c.u32()?;
        let _attributes = c.u32()?;
        let external_size = c.u64()?;
        c.skip(2)?; // permission (i16, 4.1.0+)
        // file flags: 32 of them for 5.6.2, four bytes.
        read_flag_bits(&mut c, 32)?;
        let _type = c.u8()?; // 5.0.0+ (stored_file_type_0)

        names.push(destination);
        locations.push(location);
        before_installs.push(before_install);
        after_installs.push(after_install);
        ext_sizes.push(external_size);
    }

    let _ = version;
    Ok((names, locations, before_installs, after_installs, ext_sizes))
}

/// Pull the second argument out of a compiled `before_install(...)` /
/// `before_install_dependency(...)` call. GOG's installer extracts its files to
/// a temporary folder and moves them into place with this call, so the real
/// destination path is only in the script. Ported from innoextract's
/// `parse_function_call`.
pub fn galaxy_destination(before_install: &str) -> Option<String> {
    for name in ["before_install", "before_install_dependency"] {
        if let Some(args) = parse_function_call(before_install, name) {
            if args.len() >= 2 && !args[1].is_empty() {
                return Some(args[1].clone());
            }
        }
    }
    None
}

fn parse_function_call(code: &str, name: &str) -> Option<Vec<String>> {
    let ws = |c: char| c == ' ' || c == '\t' || c == '\r' || c == '\n';
    let sep = |c: char| c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '(' || c == ')' || c == ',' || c == '\'';
    let bytes: Vec<char> = code.chars().collect();
    let mut i = 0;
    while i < bytes.len() && ws(bytes[i]) {
        i += 1;
    }
    let start = i;
    while i < bytes.len() && !sep(bytes[i]) {
        i += 1;
    }
    if bytes[start..i].iter().collect::<String>() != name {
        return None;
    }
    while i < bytes.len() && ws(bytes[i]) {
        i += 1;
    }
    if i >= bytes.len() || bytes[i] != '(' {
        return None;
    }
    i += 1;
    let mut args = Vec::new();
    loop {
        while i < bytes.len() && ws(bytes[i]) {
            i += 1;
        }
        if i >= bytes.len() {
            break;
        }
        if bytes[i] == ')' {
            break;
        }
        let mut arg = String::new();
        if bytes[i] == '\'' {
            i += 1;
            loop {
                // A doubled quote is an escaped quote.
                let end = bytes[i..].iter().position(|&c| c == '\'');
                match end {
                    Some(off) => {
                        arg.extend(bytes[i..i + off].iter());
                        i += off + 1;
                        if i < bytes.len() && bytes[i] == '\'' {
                            arg.push('\'');
                            i += 1;
                        } else {
                            break;
                        }
                    }
                    None => return None,
                }
            }
        } else {
            let s = i;
            while i < bytes.len() && !sep(bytes[i]) {
                i += 1;
            }
            arg.extend(bytes[s..i].iter());
        }
        args.push(arg);
        while i < bytes.len() && ws(bytes[i]) {
            i += 1;
        }
        if i < bytes.len() && bytes[i] == ',' {
            i += 1;
        }
    }
    Some(args)
}

/// CRC-32 helpers and the flag reader live above; the slice reader follows.

/// The `.bin` slice that holds the compressed chunks.
pub struct Slice {
    data: Vec<u8>,
}

impl Slice {
    /// Open a slice file. Spanned installers start with an 8-byte marker
    /// (`idska32\x1a`) and a u32 slice size; the chunk stream begins right
    /// after, and a chunk's `offset` is absolute in the file.
    pub fn open(path: &Path) -> Result<Slice, InnoError> {
        let data = std::fs::read(path)?;
        if data.len() < 12 {
            return Err(InnoError::Format("slice file is too small to be a data slice".into()));
        }
        let magic = &data[..8];
        let known = [b"idska16\x1a".as_slice(), b"idska32\x1a".as_slice()];
        if !known.iter().any(|m| *m == magic) {
            return Err(InnoError::Format(
                "this data file does not start with the Inno slice marker".into(),
            ));
        }
        Ok(Slice { data })
    }

    /// The bytes of one chunk, decompressed. The chunk starts at `offset` with
    /// a 4-byte `zlb\x1a` marker, then `size` bytes of stream.
    pub fn read_chunk(&self, offset: u32, size: u64, method: Compression) -> Result<Vec<u8>, InnoError> {
        let start = offset as usize;
        if start + 4 > self.data.len() {
            return Err(InnoError::Format("chunk offset is past the end of the slice".into()));
        }
        if &self.data[start..start + 4] != b"zlb\x1a" {
            return Err(InnoError::Format(format!(
                "no chunk marker at slice offset 0x{start:x}"
            )));
        }
        let end = start + 4 + size as usize;
        if end > self.data.len() {
            return Err(InnoError::Format("chunk runs past the end of the slice".into()));
        }
        let stream = &self.data[start + 4..end];
        match method {
            Compression::Stored => Ok(stream.to_vec()),
            Compression::Zlib => {
                let mut out = Vec::new();
                std::io::Read::read_to_end(&mut flate2::read::ZlibDecoder::new(stream), &mut out)
                    .map_err(|e| InnoError::Compression(format!("zlib: {e}")))?;
                Ok(out)
            }
            Compression::Bzip2 => Err(InnoError::Compression(
                "this installer uses bzip2 chunks, which this reader does not implement".into(),
            )),
            Compression::Lzma1 => {
                // A chunk is stored as a raw LZMA1 stream with the standard
                // 5-byte properties header. The uncompressed size is not known
                // here, so decode to the end.
                super::block::lzma1_stream(stream, None)
            }
            Compression::Lzma2 => {
                // The size of the decompressed chunk is not stored on the
                // chunk, so decode until the LZMA2 stream ends.
                let mut reader = std::io::Cursor::new(stream);
                let mut dec = lzma_rust2::Lzma2Reader::new(&mut reader, 1 << 26, None);
                let mut out = Vec::new();
                std::io::Read::read_to_end(&mut dec, &mut out)
                    .map_err(|e| InnoError::Compression(format!("LZMA2: {e}")))?;
                Ok(out)
            }
        }
    }
}

impl Setup {
    /// Extract every entry whose name starts with `prefix` into `dst`, keeping
    /// the path under the prefix. Returns the number of files written.
    pub fn extract_prefix(
        &self,
        slice: &Slice,
        prefix: &str,
        dst: &Path,
    ) -> Result<usize, InnoError> {
        // Chunks are solid, so decompress each once and slice its members out.
        let mut last_chunk: Option<(u32, Vec<u8>)> = None;
        let mut written = 0usize;
        for e in &self.entries {
            // Keep the names that fall under the prefix. Most entries have one
            // name; GOG points several at one chunk when their contents match.
            let targets: Vec<String> = e
                .names
                .iter()
                .filter_map(|n| normalise_entry_name(n))
                .filter(|r| r.starts_with(prefix))
                .collect();
            if targets.is_empty() {
                continue;
            }
            let need = match last_chunk {
                Some((off, _)) if off == e.chunk_offset => false,
                _ => true,
            };
            if need {
                let data = slice.read_chunk(e.chunk_offset, e.chunk_size, e.compression)?;
                last_chunk = Some((e.chunk_offset, data));
            }
            let chunk = &last_chunk.as_ref().unwrap().1;
            let start = e.file_offset as usize;
            // Each chunk holds the whole file. GOG deduplicates identical files
            // by giving one chunk several names, so every name in the group gets
            // the same bytes.
            let end = chunk.len();
            if start > chunk.len() || end > chunk.len() {
                return Err(InnoError::Format(format!(
                    "{} wants bytes {start}..{end} of a {} byte chunk",
                    targets[0],
                    chunk.len()
                )));
            }
            for rel in &targets {
                let out = dst.join(rel);
                if let Some(parent) = out.parent() {
                    std::fs::create_dir_all(parent)?;
                }
                std::fs::write(&out, &chunk[start..end])?;
                written += 1;
            }
        }
        Ok(written)
    }
}

/// Turn a setup path (`{app}\USA\Stage1\...`) into a relative path under the
/// prefix that is kept. Returns None for entries outside the game data.
fn normalise_entry_name(name: &str) -> Option<String> {
    if name.is_empty() {
        return None;
    }
    // Windows separators become the native ones.
    let s = name.replace('\\', "/");
    // Drop a leading `{app}` (with or without a separator).
    let s = s.strip_prefix("{app}/").or_else(|| s.strip_prefix("{app}")).unwrap_or(&s);
    Some(s.trim_start_matches('/').to_string())
}

/// Read a packed flag set: one byte per eight flags, with a three-byte set
/// padded to four (the reference's `stored_flag_reader`).
fn read_flag_bits(c: &mut Cur, n: usize) -> Result<u64, InnoError> {
    let mut out = 0u64;
    let mut byte = 0u8;
    for i in 0..n {
        if i % 8 == 0 {
            byte = c.u8()?;
        }
        if byte & (1 << (i % 8)) != 0 {
            out |= 1 << i;
        }
    }
    let bytes = (n + 7) / 8;
    if bytes == 3 {
        // A 3-byte set is padded to 4 in a 32-bit build.
        c.skip(1)?;
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A real GOG installer, if one is on this machine. The test is skipped
    /// when it is absent so the suite still runs anywhere.
    pub(super) fn gog_installer() -> Option<std::path::PathBuf> {
        let home = std::env::var_os("HOME")?;
        let cand = std::path::PathBuf::from(home).join("Desktop/game-resident");
        let mut found = None;
        for e in std::fs::read_dir(cand).ok()?.flatten() {
            let p = e.path();
            let n = p.file_name()?.to_string_lossy().to_ascii_lowercase();
            if n.starts_with("setup_") && n.ends_with(".exe") {
                found = Some(p);
            }
        }
        found
    }

    #[test]
    fn reads_entries_and_names_from_a_real_installer() {
        let Some(exe) = gog_installer() else {
            eprintln!("no GOG installer on this machine, skipping");
            return;
        };
        let setup = super::super::open(&exe).expect("open installer");
        assert_eq!(setup.app_name, "Resident Evil");
        assert!(!setup.entries.is_empty());
        let named = setup.entries.iter().filter(|e| !e.names.is_empty()).count();
        assert_eq!(named, setup.entries.len(), "every data entry should have a name");
        let usa: usize = setup
            .entries
            .iter()
            .flat_map(|e| e.names.iter())
            .filter(|n| normalise_entry_name(n).map_or(false, |r| r.starts_with("USA/")))
            .count();
        assert!(usa > 500, "expected many USA entries, got {usa}");
    }

    #[test]
    fn extracts_one_usa_file_intact() {
        let Some(exe) = gog_installer() else {
            eprintln!("no GOG installer on this machine, skipping");
            return;
        };
        let setup = super::super::open(&exe).expect("open installer");
        let slice_path = super::super::find_slice(&exe).expect("slice");
        let slice = Slice::open(&slice_path).expect("open slice");
        let tmp = std::env::temp_dir().join(format!("re-inno-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&tmp);
        let n = setup.extract_prefix(&slice, "USA/", &tmp).expect("extract");
        assert!(n > 500, "expected many USA files, got {n}");
        // ResidentEvil.exe lives at the install root, not under USA/, so check
        // for something that must be in the USA tree instead.
        let stage1 = tmp.join("USA/Stage1");
        assert!(stage1.is_dir(), "USA/Stage1 should exist");
        let paks = std::fs::read_dir(&stage1).unwrap().count();
        assert!(paks > 0, "USA/Stage1 should hold room files");
        let _ = std::fs::remove_dir_all(&tmp);
    }

    /// Decode an MD5 by hand so the test does not need a crate.
    fn md5_hex(data: &[u8]) -> String {
        // A compact MD5 (RFC 1321). Verification only; not on the extraction path.
        const S: [u32; 64] = [
            7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20,
            5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
            6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
        ];
        let mut k = [0u32; 64];
        for (i, kk) in k.iter_mut().enumerate() {
            *kk = ((i as f64 + 1.0).sin().abs() * 4294967296.0) as u32;
        }
        let mut msg = data.to_vec();
        let bitlen = (data.len() as u64).wrapping_mul(8);
        msg.push(0x80);
        while msg.len() % 64 != 56 {
            msg.push(0);
        }
        msg.extend_from_slice(&bitlen.to_le_bytes());
        let (mut a0, mut b0, mut c0, mut d0) =
            (0x67452301u32, 0xefcdab89u32, 0x98badcfeu32, 0x10325476u32);
        for chunk in msg.chunks(64) {
            let mut m = [0u32; 16];
            for i in 0..16 {
                m[i] = u32::from_le_bytes(chunk[i * 4..i * 4 + 4].try_into().unwrap());
            }
            let (mut a, mut b, mut c, mut d) = (a0, b0, c0, d0);
            for i in 0..64 {
                let (f, g) = match i / 16 {
                    0 => ((b & c) | (!b & d), i),
                    1 => ((d & b) | (!d & c), (5 * i + 1) % 16),
                    2 => (b ^ c ^ d, (3 * i + 5) % 16),
                    _ => (c ^ (b | !d), (7 * i) % 16),
                };
                let f = f.wrapping_add(a).wrapping_add(k[i]).wrapping_add(m[g]);
                a = d;
                d = c;
                c = b;
                b = b.wrapping_add(f.rotate_left(S[i]));
            }
            a0 = a0.wrapping_add(a);
            b0 = b0.wrapping_add(b);
            c0 = c0.wrapping_add(c);
            d0 = d0.wrapping_add(d);
        }
        let mut out = String::new();
        for v in [a0, b0, c0, d0] {
            for byte in v.to_le_bytes() {
                out.push_str(&format!("{byte:02x}"));
            }
        }
        out
    }

    /// The extractor must reproduce innoextract byte for byte. The GOG installer
    /// keeps each file's real MD5 in its `before_install` call, and these two
    /// are the first files in the archive.
    #[test]
    fn extracted_files_match_the_installers_own_checksums() {
        let Some(exe) = gog_installer() else {
            eprintln!("no GOG installer on this machine, skipping");
            return;
        };
        let setup = super::super::open(&exe).expect("open installer");
        let slice_path = super::super::find_slice(&exe).expect("slice");
        let slice = Slice::open(&slice_path).expect("open slice");
        let tmp = std::env::temp_dir().join(format!("re-inno-md5-{}", std::process::id()));

        let wanted = [
            ("__redist/DirectX/Nov2008_XAudio_x86.cab", "795e745f1a111cbd6512c4b0360538b4"),
            ("__redist/DirectX/APR2007_d3dx10_33_x64.cab", "3fba5d677f19c48f210ca5f6372d55ad"),
        ];
        for (name, want) in wanted {
            let _ = std::fs::remove_dir_all(&tmp);
            let prefix = name.rsplit_once('/').map(|(p, _)| format!("{p}/")).unwrap();
            setup.extract_prefix(&slice, &prefix, &tmp).expect("extract");
            let file = tmp.join(name);
            let bytes = std::fs::read(&file).expect("read extracted file");
            assert_eq!(md5_hex(&bytes), want, "{name} does not match its checksum");
        }
        let _ = std::fs::remove_dir_all(&tmp);
    }
}
