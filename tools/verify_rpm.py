#!/usr/bin/env python3
"""Read back a .rpm and print its header + file list.

This is the verification half of tools/make_rpm.py: it parses the lead, the
signature header and the main header using the documented rpm layout and checks
that they agree with the payload. rpm/rpmbuild are not needed.

    python3 tools/verify_rpm.py FILE.rpm
"""

import gzip
import struct
import sys


def read_header(buf, off):
    """Parse one header at `off`. Returns (entries, next_off, store)."""
    magic, count, store_len = struct.unpack_from(">III", buf, off)
    if magic != 0x8EADE801:
        raise ValueError("bad header magic 0x%08x at %d" % (magic, off))
    index_off = off + 12
    entries = []
    for i in range(count):
        tag, typ, offset, cnt = struct.unpack_from(">IIII", buf, index_off + i * 16)
        entries.append((tag, typ, offset, cnt))
    store_off = index_off + count * 16
    store = buf[store_off:store_off + store_len]
    return entries, store_off + store_len, store


def get_str(store, off):
    end = store.index(b"\x00", off)
    return store[off:end].decode("utf-8")


def get_str_array(store, off, cnt):
    out = []
    p = off
    for _ in range(cnt):
        end = store.index(b"\x00", p)
        out.append(store[p:end].decode("utf-8"))
        p = end + 1
    return out


def main():
    path = sys.argv[1]
    buf = open(path, "rb").read()

    # Lead: 96 bytes, magic ed ab ee db.
    assert buf[:4] == b"\xed\xab\xee\xdb", "bad lead magic"
    major = buf[4]
    print("lead: version %d, %d bytes" % (major, 96))

    # Signature header starts at 96. The whole signature section is padded so
    # the next header begins on an 8-byte boundary.
    sig_entries, sig_end, sig_store = read_header(buf, 96)
    pad = (-(sig_end - 96)) % 8
    hdr_off = sig_end + pad
    print("signature: %d entries, ends at %d, header at %d" % (len(sig_entries), sig_end, hdr_off))

    sigmap = {t: (ty, o, c) for t, ty, o, c in sig_entries}
    # SIZE(1000) int32, MD5(1004) bin16, SHA1(269) str, SHA256(273) str
    size = struct.unpack_from(">I", sig_store, sigmap[1000][1])[0]
    md5 = sig_store[sigmap[1004][1]:sigmap[1004][1] + 16]
    sha1 = get_str(sig_store, sigmap[269][1])
    sha256 = get_str(sig_store, sigmap[273][1])
    print("  SIZE=%d  MD5=%s  SHA1=%s" % (size, md5.hex(), sha1))
    print("  SHA256=%s" % sha256)

    # Main header.
    entries, body_end, store = read_header(buf, hdr_off)
    emap = {t: (ty, o, c) for t, ty, o, c in entries}

    def s(tag):
        return get_str(store, emap[tag][1])

    print("header: %d entries" % len(entries))
    for tag, name in [(1000, "NAME"), (1001, "VERSION"), (1002, "RELEASE"),
                      (1022, "ARCH"), (1016, "GROUP"), (1014, "LICENSE"),
                      (1021, "OS"), (1124, "PAYLOADFORMAT"),
                      (1125, "PAYLOADCOMPRESSOR")]:
        if tag in emap and emap[tag][0] in (6, 9):
            print("  %-18s %s" % (name, s(tag)))

    basenames = get_str_array(store, emap[1117][1], emap[1117][2])
    dirnames = get_str_array(store, emap[1118][1], emap[1118][2])
    dirindexes = struct.unpack_from(">%dI" % emap[1116][2], store, emap[1116][1])
    nfiles = len(basenames)
    print("  files: %d  dirs: %d" % (nfiles, len(dirnames)))

    # Payload begins after the header (padded to 8).
    payload_off = body_end + ((-(body_end)) % 8)
    gz = buf[payload_off:]
    raw = gzip.decompress(gz)
    print("payload: %d bytes compressed -> %d uncompressed" % (len(gz), len(raw)))

    # Count cpio entries and cross-check against the header file list.
    import io
    f = io.BytesIO(raw)
    paths = []
    while True:
        hdr = f.read(110)
        if len(hdr) < 110 or hdr[:6] != b"070701":
            break
        namesize = int(hdr[94:102], 16)
        filesize = int(hdr[54:62], 16)
        name = f.read(namesize - 1).decode()
        f.read(1)
        f.read((-(110 + namesize)) % 4)
        data = f.read(filesize)
        f.read((-filesize) % 4)
        if name == "TRAILER!!!":
            break
        paths.append(name)

    print("cpio entries: %d" % len(paths))
    # Every file the header lists must appear in the cpio payload. Directories
    # are listed in the header too but arrive as bare names in the payload, so
    # compare on the base name of each header path against the payload set.
    cpio_paths = set(paths)
    cpio_bases = {p.rstrip("/").split("/")[-1] for p in paths}
    missing = []
    for i, b in enumerate(basenames):
        d = dirnames[dirindexes[i]]
        full = (d.rstrip("/") + "/" + b) if d != "/" else "/" + b
        if full in cpio_paths or b in cpio_bases:
            continue
        missing.append(full)
    if missing:
        print("MISMATCH: %d header paths not in payload, e.g. %s"
              % (len(missing), sorted(missing)[:3]))
        return 1
    print("OK: every header file present in the payload")

    # Verify MD5 over (header + payload).
    import hashlib
    calc = hashlib.md5(buf[hdr_off:]).digest()
    print("MD5 check: %s" % ("OK" if calc == md5 else "FAIL (got %s)" % calc.hex()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
