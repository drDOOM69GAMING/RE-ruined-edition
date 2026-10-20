#!/usr/bin/env python3
"""Pack a staging directory into the launcher's embedded payload.

Reads every regular file under SRC (the compiled 32-bit `residentevil` binary
plus its bundled libraries under SRC/lib) and writes a single gzip-compressed
container to DST. The launcher (tools/ruined_edition/src/embed.rs) unpacks this
at run time. Capcom's game data is NEVER packed here - only the compiled game
binary and its libraries.

Container layout (before gzip), all integers little-endian:

    magic      8 bytes  b"REDCONT1"
    count      u32      number of files
    entries    count times:
        path_len u32
        path     path_len bytes (utf-8, posix path relative to SRC)
        data_len u64
    data       the file contents, concatenated in entry order

Files are packed in sorted path order so the output is deterministic for a
given set of inputs.
"""

import gzip
import os
import struct
import sys

MAGIC = b"REDCONT1"


def collect(src: str):
    """Yield (relpath, abspath) for every regular file under src, sorted."""
    out = []
    for root, _dirs, files in os.walk(src):
        for name in files:
            abspath = os.path.join(root, name)
            if not os.path.isfile(abspath) or os.path.islink(abspath) and not os.path.exists(abspath):
                continue
            rel = os.path.relpath(abspath, src).replace(os.sep, "/")
            out.append((rel, abspath))
    out.sort(key=lambda p: p[0])
    return out


def pack(src: str, dst: str) -> int:
    files = collect(src)
    header = bytearray()
    header += MAGIC
    header += struct.pack("<I", len(files))
    blobs = []
    for rel, abspath in files:
        with open(abspath, "rb") as f:
            data = f.read()
        rel_b = rel.encode("utf-8")
        header += struct.pack("<I", len(rel_b))
        header += rel_b
        header += struct.pack("<Q", len(data))
        blobs.append(data)

    raw = bytes(header) + b"".join(blobs)
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    # mtime=0 keeps the output byte-identical across rebuilds of the same files.
    with open(dst, "wb") as f:
        f.write(gzip.compress(raw, compresslevel=9, mtime=0))
    total = sum(len(b) for b in blobs)
    print(f"pack_payload: {len(files)} files, {total} bytes raw -> {dst}")
    return len(files)


def main(argv):
    if len(argv) != 3:
        print("usage: pack_payload.py SRC_DIR OUT.bin.gz", file=sys.stderr)
        return 2
    return 0 if pack(argv[1], argv[2]) >= 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
