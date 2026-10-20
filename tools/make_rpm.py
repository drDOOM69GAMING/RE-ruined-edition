#!/usr/bin/env python3
"""Write a binary .rpm without rpmbuild.

The rpm container is a lead, a signature header, a header and a compressed cpio
payload. Its header format is documented ("RPM v3 File Format") and simple
enough to emit directly: a magic, a count of index entries, the store size, the
entries themselves (tag, type, offset, count) and then the store. This script
does exactly that, so packaging works on a machine that has no rpm tooling -
which is the case for the machine that builds these artifacts.

Only what is needed for a data-only, no-script package is emitted:

    lead          the legacy 96-byte lead (mostly ignored by modern rpm)
    signature     SIZE, MD5 (of the header+payload), SHA1, SHA256, PAYLOADSIZE
    header        NAME, VERSION, RELEASE, ARCH, SUMMARY, DESCRIPTION, ...
    payload       a gzip'd cpio "newc" archive of the file tree

Usage:
    make_rpm.py --tree DIR --name N --version V --out FILE [options]
"""

import argparse
import gzip
import hashlib
import io
import os
import stat
import struct
import sys
import time

# --- rpm header tags --------------------------------------------------------
# From /usr/include/rpm/rpmtag.h. Only the ones a data package needs.

RPMTAG_NAME = 1000
RPMTAG_VERSION = 1001
RPMTAG_RELEASE = 1002
RPMTAG_SUMMARY = 1004
RPMTAG_DESCRIPTION = 1005
RPMTAG_BUILDTIME = 1006
RPMTAG_BUILDHOST = 1007
RPMTAG_SIZE = 1009
RPMTAG_VENDOR = 1011
RPMTAG_LICENSE = 1014
RPMTAG_PACKAGER = 1015
RPMTAG_GROUP = 1016
RPMTAG_URL = 1020
RPMTAG_OS = 1021
RPMTAG_ARCH = 1022
RPMTAG_FILESIZES = 1028
RPMTAG_FILEMODES = 1030
RPMTAG_FILERDEVS = 1033
RPMTAG_FILEMTIMES = 1034
RPMTAG_FILEDIGESTS = 1035
RPMTAG_FILELINKTOS = 1036
RPMTAG_FILEFLAGS = 1037
RPMTAG_FILEUSERNAME = 1039
RPMTAG_FILEGROUPNAME = 1040
RPMTAG_SOURCERPM = 1044
RPMTAG_FILEVERIFYFLAGS = 1045
RPMTAG_PROVIDENAME = 1047
RPMTAG_REQUIRENAME = 1049
RPMTAG_REQUIREVERSION = 1050
RPMTAG_REQUIREFLAGS = 1048
RPMTAG_DIRINDEXES = 1116
RPMTAG_BASENAMES = 1117
RPMTAG_DIRNAMES = 1118
RPMTAG_PAYLOADFORMAT = 1124
RPMTAG_PAYLOADCOMPRESSOR = 1125
RPMTAG_PAYLOADFLAGS = 1126
RPMTAG_PAYLOADDIGEST = 5092
RPMTAG_PAYLOADDIGESTALGO = 5093
RPMTAG_RPMVERSION = 1064
RPMTAG_COOKIE = 1094
RPMTAG_ENCODING = 5062

# signature tags
RPMSIGTAG_SIZE = 1000
RPMSIGTAG_MD5 = 1004
RPMSIGTAG_SHA1 = 269
RPMSIGTAG_SHA256 = 273
RPMSIGTAG_PAYLOADSIZE = 1007
RPMSIGTAG_RESERVEDSPACE = 1008

# types
RPM_NULL = 0
RPM_CHAR = 1
RPM_INT8 = 2
RPM_INT16 = 3
RPM_INT32 = 4
RPM_INT64 = 5
RPM_STRING = 6
RPM_BIN = 7
RPM_STRING_ARRAY = 8
RPM_I18NSTRING = 9


class Header:
    """Accumulates (tag, type, value) entries and serialises to rpm header."""

    def __init__(self):
        self.entries = []

    def add(self, tag, typ, value):
        self.entries.append((tag, typ, value))

    def _encode_value(self, typ, value):
        if typ == RPM_STRING:
            return value.encode("utf-8") + b"\x00", 1
        if typ == RPM_I18NSTRING:
            return value.encode("utf-8") + b"\x00", 1
        if typ == RPM_BIN:
            return value, len(value)
        if typ == RPM_STRING_ARRAY:
            blob = b"".join(v.encode("utf-8") + b"\x00" for v in value)
            return blob, len(value)
        if typ == RPM_INT32:
            vals = [v & 0xFFFFFFFF for v in value]
            return struct.pack(">%dI" % len(vals), *vals), len(vals)
        if typ == RPM_INT16:
            vals = [v & 0xFFFF for v in value]
            return struct.pack(">%dH" % len(vals), *vals), len(vals)
        if typ == RPM_INT8:
            vals = [v & 0xFF for v in value]
            return struct.pack(">%dB" % len(vals), *vals), len(vals)
        if typ == RPM_INT64:
            return struct.pack(">%dQ" % len(value), *value), len(value)
        raise ValueError("unsupported type %r" % typ)

    def render(self, region_tag=None):
        """Serialise. region_tag adds the 62 tag (HEADERIMMUTABLE) wrapper."""
        # The store holds every value, 4-byte aligned.
        store = bytearray()
        index = []
        for tag, typ, value in sorted(self.entries, key=lambda e: e[0]):
            blob, count = self._encode_value(typ, value)
            # Align to a 4-byte boundary; INT16/INT8 still sit on one.
            while len(store) % 4 != 0:
                store.append(0)
            offset = len(store)
            store += blob
            index.append((tag, typ, offset, count))

        entries = b""
        for tag, typ, offset, count in index:
            entries += struct.pack(">IIII", tag, typ, offset, count)

        if region_tag is not None:
            # The immutable region: the tag's data is itself a header whose
            # index entries and value store follow its own 12-byte intro. The
            # offsets in `index` already point at the value store, and the
            # region's store length counts only that store, so a reader that
            # re-parses the blob lands exactly on the values.
            inner_count = len(index) + 1
            hdr = struct.pack(">III", 0x8EADE801, inner_count, len(store))
            hdr += struct.pack(">IIII", region_tag, RPM_BIN, 0, 16)
            hdr += entries
            hdr += store
            return hdr, len(store)

        hdr = struct.pack(">III", 0x8EADE801, len(index), len(store))
        return hdr + entries + bytes(store), len(store)


def cpio_payload(tree, prefix):
    """Build a gzip'd cpio 'newc' archive of `tree`, rooted at `prefix`."""
    out = io.BytesIO()
    ino = 1
    entries = []

    for dirpath, dirnames, filenames in os.walk(tree):
        dirnames.sort()
        rel = os.path.relpath(dirpath, tree)
        arc = prefix if rel == "." else os.path.join(prefix, rel)
        entries.append((dirpath, arc, True))
        for fn in sorted(filenames):
            entries.append((os.path.join(dirpath, fn), os.path.join(arc, fn), False))

    def write_record(name, mode, data, ino, nlink, mtime):
        namez = name.encode() + b"\x00"
        fields = [
            b"070701",
            b"%08X" % ino,
            b"%08X" % mode,
            b"%08X" % 0,
            b"%08X" % 0,
            b"%08X" % nlink,
            b"%08X" % int(mtime),
            b"%08X" % len(data),
            b"%08X" % 0,
            b"%08X" % 0,
            b"%08X" % 0,
            b"%08X" % 0,
            b"%08X" % len(namez),
            b"%08X" % 0,
        ]
        rec = b"".join(fields) + namez
        # Pad the header+name to a 4-byte boundary, then the data.
        rec += b"\x00" * ((-len(rec)) % 4)
        rec += data
        rec += b"\x00" * ((-len(data)) % 4)
        return rec

    mtime = int(time.time())
    for path, arc, is_dir in entries:
        st = os.lstat(path)
        if is_dir:
            mode = stat.S_IFDIR | (st.st_mode & 0o7777)
            out.write(write_record(arc, mode, b"", ino, 2, mtime))
        elif os.path.islink(path):
            mode = stat.S_IFLNK | 0o777
            out.write(write_record(arc, mode, os.readlink(path).encode(), ino, 1, mtime))
        else:
            mode = stat.S_IFREG | (st.st_mode & 0o7777)
            with open(path, "rb") as fh:
                data = fh.read()
            out.write(write_record(arc, mode, data, ino, 1, mtime))
        ino += 1

    # Trailer: a record named TRAILER!!! with no data.
    out.write(write_record("TRAILER!!!", 0, b"", 0, 1, mtime))
    raw = out.getvalue()
    # Pad to a 512-byte multiple as rpm expects.
    raw += b"\x00" * ((-len(raw)) % 512)
    return gzip.compress(raw, 9), raw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--release", default="1")
    ap.add_argument("--arch", default="i686")
    ap.add_argument("--summary", default="")
    ap.add_argument("--description", default="")
    ap.add_argument("--license", default="")
    ap.add_argument("--url", default="")
    ap.add_argument("--prefix", required=True)
    ap.add_argument("--icon", default=None,
                    help="icon file to declare as the package icon (optional)")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    # --- file list ---------------------------------------------------------
    dirnames = [args.prefix]
    dirindex_of = {args.prefix: 0}
    basenames = []
    dirindexes = []
    filesizes = []
    filemodes = []
    filerdevs = []
    filemtimes = []
    filedigests = []
    filelinktos = []
    fileflags = []
    fileusername = []
    filegroupname = []
    fileverify = []
    total_size = 0

    def add_dir(abs_path, arc):
        # Every parent directory must exist in dirnames too.
        parent = os.path.dirname(arc)
        if parent and parent not in dirindex_of:
            idx = len(dirnames)
            dirindex_of[parent] = idx
            dirnames.append(parent)
        idx = dirindex_of[arc]

    rows = []
    for dirpath, dnames, fnames in os.walk(args.tree):
        dnames.sort()
        rel = os.path.relpath(dirpath, args.tree)
        arc = args.prefix if rel == "." else os.path.join(args.prefix, rel)
        rows.append((dirpath, arc, True))
        for fn in sorted(fnames):
            rows.append((os.path.join(dirpath, fn), os.path.join(arc, fn), False))

    for path, arc, is_dir in rows:
        st = os.lstat(path)
        if is_dir:
            if arc not in dirindex_of:
                idx = len(dirnames)
                dirindex_of[arc] = idx
                dirnames.append(arc)
            # Directories need a parent entry present.
            parent = os.path.dirname(arc)
            if parent and parent not in dirindex_of:
                pidx = len(dirnames)
                dirindex_of[parent] = pidx
                dirnames.append(parent)
            if arc != args.prefix:
                basenames.append(os.path.basename(arc))
                dirindexes.append(dirindex_of[arc])
                filesizes.append(0)
                filemodes.append(stat.S_IFDIR | (st.st_mode & 0o7777))
                filerdevs.append(0)
                filemtimes.append(int(st.st_mtime))
                filedigests.append("")
                filelinktos.append("")
                fileflags.append(0)
                fileusername.append("root")
                filegroupname.append("root")
                fileverify.append(-1)
        else:
            size = st.st_size
            total_size += size
            basenames.append(os.path.basename(arc))
            dirindexes.append(dirindex_of[os.path.dirname(arc)])
            filesizes.append(size)
            if os.path.islink(path):
                filemodes.append(stat.S_IFLNK | 0o777)
                linkto = os.readlink(path)
            else:
                filemodes.append(stat.S_IFREG | (st.st_mode & 0o7777))
                linkto = ""
            filelinktos.append(linkto)
            filerdevs.append(0)
            filemtimes.append(int(st.st_mtime))
            if os.path.islink(path):
                filedigests.append("")
            else:
                h = hashlib.md5()
                with open(path, "rb") as fh:
                    for chunk in iter(lambda: fh.read(1 << 20), b""):
                        h.update(chunk)
                filedigests.append(h.hexdigest())
            fileflags.append(0)
            fileusername.append("root")
            filegroupname.append("root")
            fileverify.append(-1)

    gz, raw = cpio_payload(args.tree, args.prefix)

    # --- header ------------------------------------------------------------
    h = Header()
    h.add(RPMTAG_NAME, RPM_STRING, args.name)
    h.add(RPMTAG_VERSION, RPM_STRING, args.version)
    h.add(RPMTAG_RELEASE, RPM_STRING, args.release)
    h.add(RPMTAG_SUMMARY, RPM_I18NSTRING, args.summary)
    h.add(RPMTAG_DESCRIPTION, RPM_I18NSTRING, args.description)
    h.add(RPMTAG_BUILDTIME, RPM_INT32, [int(time.time())])
    h.add(RPMTAG_BUILDHOST, RPM_STRING, "localhost")
    h.add(RPMTAG_SIZE, RPM_INT32, [total_size])
    h.add(RPMTAG_LICENSE, RPM_STRING, args.license)
    h.add(RPMTAG_GROUP, RPM_I18NSTRING, "Applications/ Games")
    h.add(RPMTAG_URL, RPM_STRING, args.url)
    h.add(RPMTAG_OS, RPM_STRING, "linux")
    h.add(RPMTAG_ARCH, RPM_STRING, args.arch)
    h.add(RPMTAG_SOURCERPM, RPM_STRING, f"{args.name}-{args.version}-{args.release}.src.rpm")
    h.add(RPMTAG_RPMVERSION, RPM_STRING, "4.16.0")
    h.add(RPMTAG_COOKIE, RPM_STRING, "%08x%08x" % (int(time.time()), os.getpid()))
    h.add(RPMTAG_ENCODING, RPM_STRING, "utf-8")
    # Provide the package name so dependency resolvers see it.
    h.add(RPMTAG_PROVIDENAME, RPM_STRING_ARRAY, [args.name])
    h.add(RPMTAG_REQUIRENAME, RPM_STRING_ARRAY, [])
    h.add(RPMTAG_REQUIREVERSION, RPM_STRING_ARRAY, [])
    h.add(RPMTAG_REQUIREFLAGS, RPM_INT32, [])
    # File list
    h.add(RPMTAG_DIRNAMES, RPM_STRING_ARRAY, dirnames)
    h.add(RPMTAG_BASENAMES, RPM_STRING_ARRAY, basenames)
    h.add(RPMTAG_DIRINDEXES, RPM_INT32, dirindexes)
    h.add(RPMTAG_FILESIZES, RPM_INT32, filesizes)
    h.add(RPMTAG_FILEMODES, RPM_INT16, filemodes)
    h.add(RPMTAG_FILERDEVS, RPM_INT16, filerdevs)
    h.add(RPMTAG_FILEMTIMES, RPM_INT32, filemtimes)
    h.add(RPMTAG_FILEDIGESTS, RPM_STRING_ARRAY, filedigests)
    h.add(RPMTAG_FILELINKTOS, RPM_STRING_ARRAY, filelinktos)
    h.add(RPMTAG_FILEFLAGS, RPM_INT32, fileflags)
    h.add(RPMTAG_FILEUSERNAME, RPM_STRING_ARRAY, fileusername)
    h.add(RPMTAG_FILEGROUPNAME, RPM_STRING_ARRAY, filegroupname)
    h.add(RPMTAG_FILEVERIFYFLAGS, RPM_INT32, fileverify)
    h.add(RPMTAG_PAYLOADFORMAT, RPM_STRING, "cpio")
    h.add(RPMTAG_PAYLOADCOMPRESSOR, RPM_STRING, "gzip")
    h.add(RPMTAG_PAYLOADFLAGS, RPM_STRING, "9")
    h.add(RPMTAG_PAYLOADDIGEST, RPM_STRING_ARRAY, [hashlib.sha256(gz).hexdigest()])
    h.add(RPMTAG_PAYLOADDIGESTALGO, RPM_INT32, [8])

    # --- lead --------------------------------------------------------------
    # The legacy 96-byte lead. Modern rpm ignores the contents but wants the
    # length to be exactly 96 so the signature header starts where it expects.
    nname = b"%s-%s-%s" % (args.name.encode(), args.version.encode(), args.release.encode())
    nname = nname[:65]
    lead_prefix = struct.pack(">4sBBhh", b"\xed\xab\xee\xdb", 3, 0, 0, 1)
    lead_prefix += nname + b"\x00" * (66 - len(nname))
    lead_prefix += struct.pack(">hh", 1, 5)   # osnum = 1 (Linux), signature type 5
    lead_prefix += b"\x00" * 16               # reserved
    assert len(lead_prefix) == 96, len(lead_prefix)

    header_bytes, _ = h.render(region_tag=63)

    # The main header is padded so the payload starts on an 8-byte boundary.
    # The signature digests cover exactly those bytes plus the payload, which
    # is what a reader hashes back, so the padding has to be computed first.
    body = header_bytes
    body += b"\x00" * ((-(len(lead_prefix) + len(body))) % 8)

    # --- signature ---------------------------------------------------------
    # SIZE, MD5, SHA1 and SHA256 all cover the header and payload as stored.
    signed = body + gz
    sig = Header()
    sig.add(RPMSIGTAG_SIZE, RPM_INT32, [len(signed)])
    sig.add(RPMSIGTAG_MD5, RPM_BIN, hashlib.md5(signed).digest())
    sig.add(RPMSIGTAG_SHA1, RPM_STRING, hashlib.sha1(signed).hexdigest())
    sig.add(RPMSIGTAG_SHA256, RPM_STRING, hashlib.sha256(signed).hexdigest())
    sig.add(RPMSIGTAG_PAYLOADSIZE, RPM_INT32, [len(raw)])
    # Pad the signature section to an 8-byte boundary using the reserved-space
    # tag, which is what rpm itself does.
    sig_bytes, _ = sig.render()
    align = (-(len(lead_prefix) + len(sig_bytes))) % 8
    if align:
        sig.add(RPMSIGTAG_RESERVEDSPACE, RPM_BIN, b"\x00" * align)
        sig_bytes, _ = sig.render()
    assert (len(lead_prefix) + len(sig_bytes)) % 8 == 0

    # --- lead --------------------------------------------------------------
    # The legacy 96-byte lead. Modern rpm ignores the contents but wants the
    # length to be exactly 96 so the signature header starts where it expects.
    lead = lead_prefix

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as fh:
        fh.write(lead)
        fh.write(sig_bytes)
        fh.write(body)
        fh.write(gz)

    print(f"wrote {args.out} ({os.path.getsize(args.out)} bytes, "
          f"{len(basenames)} files, payload {len(raw)} -> {len(gz)}")


if __name__ == "__main__":
    sys.exit(main())
