#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# setup_windows_toolchain.sh - fetch everything needed to cross-compile the
# Windows build on a Linux host, into ~/toolchain/win.
#
# Installs nothing system-wide and needs no root: the mingw-w64 compiler, the
# Windows SDL2 and ffmpeg libraries and the libwebp source all land under your
# home directory, and only the build script reads them.
#
# Nothing here is distributed with the game. The resulting .exe links against
# SDL2 and ffmpeg runtime DLLs, which the packaging step copies beside it.
#
# Usage:
#   bash tools/setup_windows_toolchain.sh
# ---------------------------------------------------------------------------
set -euo pipefail

TOOL="$HOME/toolchain/win"
SRC="$TOOL/src"
mkdir -p "$SRC" "$TOOL/prefix"
cd "$SRC"

MIRROR="https://geo.mirror.pkgbuild.com/extra/os/x86_64"

say() { printf '\n==> %s\n' "$*"; }

fetch() {  # url outfile
    if [ -s "$2" ]; then echo "have $(basename "$2")"; return 0; fi
    echo "downloading $(basename "$2")"
    curl -fL --retry 3 -o "$2" "$1"
}

# ---------------------------------------------------------------------------
# 1. mingw-w64 toolchain (the Arch packages, extracted, no pacman needed).
#    gcc pulls in the others as build deps in pacman; here each is explicit.
# ---------------------------------------------------------------------------
say "mingw-w64 toolchain"
fetch "$MIRROR/mingw-w64-binutils-2.47-1-x86_64.pkg.tar.zst"        mingw-w64-binutils.pkg.tar.zst
fetch "$MIRROR/mingw-w64-crt-14.0.0-1-any.pkg.tar.zst"              mingw-w64-crt.pkg.tar.zst
fetch "$MIRROR/mingw-w64-headers-14.0.0-1-any.pkg.tar.zst"          mingw-w64-headers.pkg.tar.zst
fetch "$MIRROR/mingw-w64-winpthreads-14.0.0-1-any.pkg.tar.zst"      mingw-w64-winpthreads.pkg.tar.zst
fetch "$MIRROR/mingw-w64-gcc-16.2.0-2-x86_64.pkg.tar.zst"           mingw-w64-gcc.pkg.tar.zst
for f in mingw-w64-*.pkg.tar.zst; do tar --zstd -xf "$f" -C "$TOOL/prefix"; done

# ---------------------------------------------------------------------------
# 2. Windows SDL2 (headers + import lib + DLL).
# ---------------------------------------------------------------------------
say "SDL2 for Windows"
fetch "https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-devel-2.30.9-mingw.tar.gz" SDL2-devel.tar.gz
rm -rf "$TOOL/sdl2"; mkdir -p "$TOOL/sdl2"
tar xzf SDL2-devel.tar.gz -C "$TOOL/sdl2"

# ---------------------------------------------------------------------------
# 3. Windows ffmpeg (shared). The shared build carries the .dll.a import
#    libraries MinGW needs, which the static build does not.
# ---------------------------------------------------------------------------
say "ffmpeg for Windows"
fetch "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-full-shared.7z" ffmpeg.7z
rm -rf "$TOOL/ffmpeg"; mkdir -p "$TOOL/ffmpeg"
7z x -y -o"$TOOL/ffmpeg" ffmpeg.7z >/dev/null

# ---------------------------------------------------------------------------
# 4. libwebp, built for mingw. No mingw package exists, so it is configured and
#    built here. Only the static decode/encode core is needed.
# ---------------------------------------------------------------------------
say "libwebp for Windows"
fetch "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.4.0.tar.gz" libwebp.tar.gz
if [ ! -f "$TOOL/webp/lib/libwebp.a" ]; then
    rm -rf libwebp-1.4.0
    tar xzf libwebp.tar.gz
    cd libwebp-1.4.0
    TC="$TOOL/prefix/usr/bin"
    ./configure --host=x86_64-w64-mingw32 --prefix="$TOOL/webp" \
        --enable-static --disable-shared \
        CC="$TC/x86_64-w64-mingw32-gcc" CXX="$TC/x86_64-w64-mingw32-g++" \
        AR="$TC/x86_64-w64-mingw32-ar" RANLIB="$TC/x86_64-w64-mingw32-ranlib" \
        --disable-libwebpdemux --disable-libwebpmux --disable-libwebpdecoder \
        --disable-cwebp --disable-dwebp --disable-gif2webp --disable-img2webp --disable-webpmux \
        >/tmp/webp-configure.log 2>&1
    make -j"$(nproc)" >/tmp/webp-build.log 2>&1
    make install >/tmp/webp-install.log 2>&1
    cd "$SRC"
fi

say "toolchain ready under $TOOL"
echo "next: bash tools/build_windows_mingw.sh"
