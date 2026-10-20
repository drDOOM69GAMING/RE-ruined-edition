#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# package_ruined_edition.sh - build every "Resident Evil: Ruined Edition"
# distribution artifact from this repo.
#
# Produces, under dist/ruined-edition/:
#
#   Resident Evil Ruined Edition/          the folder to drop on the desktop
#   Resident Evil Ruined Edition Arch/     the Arch/CachyOS tree (tarballed)
#   Resident Evil Ruined Edition Windows/  the Windows tree (zipped)
#   Resident Evil Ruined Edition Arch.tar.gz     Arch / CachyOS
#   Resident Evil Ruined Edition Debian.deb      Debian / Ubuntu
#   Resident Evil Ruined Edition Fedora.rpm      Fedora / RHEL
#   Resident Evil Ruined Edition Windows.zip     Windows (real .exe + DLLs)
#
# Each platform's artifact is named after that platform so a download is not
# ambiguous. On Linux the compiled game binary and its bundled libraries are
# packed *inside* the Rust launcher, so the folder carries a single
# self-contained executable that unpacks the game beside itself on first run.
# The Windows zip still ships a real cross-compiled residentevil.exe
# (tools/build_windows_mingw.sh) plus its SDL2/ffmpeg DLLs alongside the
# launcher.
#
# GOG / Capcom game data is NEVER bundled. The Rust launcher asks the player
# for their own copy and imports it.
#
# Usage (from the repo root):
#   bash tools/package_ruined_edition.sh [--no-build] [--out DIR]
#
# Needs: the Linux build tree (build/linux), cargo, ar, fakeroot, xz, zstd,
# cpio, tar, and python3. rpmbuild and dpkg are NOT required - the deb and rpm
# containers are written directly, so this runs on a machine that has neither.
# ---------------------------------------------------------------------------
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO/build/linux"
BIN="$BUILD_DIR/residentevil"
OUT_ROOT="$REPO/dist/ruined-edition"
DO_BUILD=1
APP_NAME="Resident Evil Ruined Edition"
PKG_NAME="ruined-edition"
# Per-platform display names. Each platform ships its own artifact named after
# itself so the downloads are not ambiguous:
#   Resident Evil Ruined Edition Arch     (tarball, for Arch and CachyOS)
#   Resident Evil Ruined Edition Debian   (.deb)
#   Resident Evil Ruined Edition Fedora   (.rpm)
#   Resident Evil Ruined Edition Windows  (.zip with a real .exe)
PLATFORM_ARCH_NAME="Resident Evil Ruined Edition Arch"
PLATFORM_DEB_NAME="Resident Evil Ruined Edition Debian"
PLATFORM_RPM_NAME="Resident Evil Ruined Edition Fedora"
PLATFORM_WIN_NAME="Resident Evil Ruined Edition Windows"

while [ $# -gt 0 ]; do
    case "$1" in
        --no-build) DO_BUILD=0 ;;
        --out)      OUT_ROOT="$2"; shift ;;
        -h|--help)  sed -n '3,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

say() { printf '\n==> %s\n' "$*"; }
die() { echo "error: $*" >&2; exit 1; }

# Copy the bundled 32-bit library closure for $1 (the game binary) into $2/lib.
#
# The GL driver stack and the glibc loader family stay the host's: they have to
# match the running kernel and GPU, and glibc is the loader itself. Everything
# else - SDL2, ffmpeg, the X11/Wayland and ALSA/Pulse clients - travels, which
# is what makes the bundle independent of the target's 32-bit package set.
bundle_libs_into() {
    local bin="$1" dest="$2"
    mkdir -p "$dest/lib"
    local host_owned='^(ld-linux|libc\.so|libm\.so|libdl\.so|libpthread\.so|librt\.so|libresolv\.so|libutil\.so|libanl\.so|libnsl\.so|libnss_|libthread_db|libcrypt\.so|libGL|libEGL|libGLX|libOpenGL|libgbm|libdrm|libvulkan|libgallium|libglapi)'
    local lib base
    while read -r lib; do
        [ -n "$lib" ] || continue
        base="$(basename "$lib")"
        [[ "$base" =~ $host_owned ]] && continue
        cp -L "$lib" "$dest/lib/$base"
    done < <(ldd "$bin" | awk '/=>/ && $3 != "" {print $3}' | sort -u)

    # Fail here, not on the player's machine.
    local missing
    missing="$(LD_LIBRARY_PATH="$dest/lib" ldd "$bin" | grep 'not found' || true)"
    [ -z "$missing" ] || die "unresolved deps after bundling:\n$missing"
}

# Pack the compiled game binary and its libraries into a payload that the Rust
# launcher embeds in itself (see tools/ruined_edition/src/embed.rs). Runs before
# the launcher is built, so include_bytes! bakes the payload into the executable.
# Capcom's game data is never packed here - only the compiled game and its libs.
pack_embed_payload() {
    local stage="$OUT_ROOT/embed-stage"
    rm -rf "$stage"
    mkdir -p "$stage"
    cp "$BIN" "$stage/residentevil"
    bundle_libs_into "$stage/residentevil" "$stage"
    python3 "$REPO/tools/pack_payload.py" "$stage" \
        "$REPO/tools/ruined_edition/embed/ruined_payload.bin.gz"
}

# Same, for the Windows launcher. A separate payload file so the two targets do
# not clobber each other. The DLLs sit flat beside the exe (the Windows loader
# searches the exe's own folder), unlike Linux where they travel in lib/.
pack_embed_payload_windows() {
    local win_build="$REPO/dist/windows-build"
    if [ ! -x "$win_build/residentevil.exe" ]; then
        say "no Windows game build (dist/windows-build); skipping the Windows payload"
        return 0
    fi
    local stage="$OUT_ROOT/embed-stage-windows"
    rm -rf "$stage"
    mkdir -p "$stage"
    cp "$win_build/residentevil.exe" "$stage/"
    # Only the runtime DLLs at the top level - not the lib/ or obj/ build dirs.
    local dll
    for dll in "$win_build"/*.dll; do
        [ -f "$dll" ] && cp "$dll" "$stage/"
    done
    python3 "$REPO/tools/pack_payload.py" "$stage" \
        "$REPO/tools/ruined_edition/embed/ruined_payload.windows.bin.gz"
}

# ---------------------------------------------------------------------------
# Build the game and the launcher app.
# ---------------------------------------------------------------------------
if [ "$DO_BUILD" -eq 1 ]; then
    say "building the game (32-bit Linux)"
    cmake -S "$REPO" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$BUILD_DIR" -j"$(nproc)"

    # Pack the game binary + its libraries into the payload the launcher will
    # embed, before the launcher is compiled so include_bytes! bakes it in.
    say "packing the game binary and libraries into the launcher payload"
    pack_embed_payload

    say "building the Ruined Edition launcher (Rust)"
    cargo build --release --manifest-path "$REPO/tools/ruined_edition/Cargo.toml"

    # The Windows launcher, for the Windows package. Needs the MinGW toolchain
    # under ~/toolchain/win; skipped with a note when it is absent, so the Linux
    # packaging still works on a machine without it.
    if [ -x "$HOME/toolchain/win/prefix/usr/bin/i686-w64-mingw32-gcc" ]; then
        say "building the Windows launcher (Rust, i686-pc-windows-gnu)"
        # Pack the Windows game exe + DLLs into the payload the Windows
        # launcher embeds, before the cross-build so include_bytes! bakes it in.
        pack_embed_payload_windows
        rustup target add i686-pc-windows-gnu >/dev/null 2>&1 || true
        PATH="$HOME/toolchain/win/prefix/usr/bin:$PATH" \
        CARGO_TARGET_I686_PC_WINDOWS_GNU_LINKER=i686-w64-mingw32-gcc \
            cargo build --release --target i686-pc-windows-gnu \
            --manifest-path "$REPO/tools/ruined_edition/Cargo.toml" \
            >/dev/null 2>&1 \
            && say "windows launcher built" \
            || say "windows launcher build failed (continuing)"
    else
        say "no MinGW toolchain; skipping the Windows launcher"
    fi
fi

APP="$REPO/tools/ruined_edition/target/release/ruined-edition"
[ -f "$BIN" ] || die "$BIN missing (run without --no-build)"
[ -f "$APP" ] || die "$APP missing (run without --no-build)"

VERSION="$(grep -m1 GAME_VERSION_STRING "$REPO/src/Version.h" | sed 's/.*"\([^"]*\)".*/\1/')"
[ -n "$VERSION" ] || VERSION="1.0.0"
GLIBC="$(ldd --version | awk 'NR==1 {print $NF}')"

# The bundle only makes sense for the 32-bit target.
elfclass="$(od -An -tu1 -j4 -N1 "$BIN" | tr -d ' ')"
elfmachine="$(od -An -tu2 -j18 -N1 "$BIN" | tr -d ' ')"
[ "$elfclass" = "1" ] && [ "$elfmachine" = "3" ] \
    || die "$BIN is not ELF32 i386 (class=$elfclass machine=$elfmachine)"

# ---------------------------------------------------------------------------
# The payload tree. One function builds it so the folder, the tarball, the deb
# and the rpm all carry byte-identical contents.
# ---------------------------------------------------------------------------
stage_tree() {
    local root="$1"
    rm -rf "$root"
    mkdir -p "$root/hires/bgd"
    mkdir -p "$root/SAVE"

    # The compiled game binary and its 32-bit libraries are packed *inside*
    # this launcher (see pack_embed_payload); they are materialised beside it
    # on first run. Only the self-contained launcher is shipped - no loose
    # residentevil binary and no lib/ folder.
    cp "$APP" "$root/ruined-edition"
    chmod +x "$root/ruined-edition"

    # Icon travels with the app so the deb/rpm/desktop folder all have it.
    if [ -f "$REPO/tools/ruined_edition/assets/icon-512.png" ]; then
        cp "$REPO/tools/ruined_edition/assets/icon-512.png" "$root/icon.png"
    fi
}

say "staging the payload tree"
PAYLOAD="$OUT_ROOT/payload"
stage_tree "$PAYLOAD"

# ---------------------------------------------------------------------------
# "mod files go here" placeholder. The game reads hires/bgd/*.webp; the folder
# ships empty and the launcher copies a downloaded pack into it.
# ---------------------------------------------------------------------------
cat > "$PAYLOAD/hires/bgd/README.txt" <<'MODS'
Put your HD background pack in this folder.

The game looks here for .webp backgrounds named after the room camera. So
RC1065.webp is the main hall. Any pack that follows that naming works.

You do not have to copy them by hand. Run ./launch.sh and pick
"3. Import an HD pack", then point it at the folder you downloaded. It copies
every .webp in for you.

If this folder has no .webp files the game just draws the original
backgrounds, so it is safe to leave empty.
MODS

# ---------------------------------------------------------------------------
# Launcher script. It runs the Rust app, which sets up config.ini and then
# spawns the game, so a double-click walks the player through importing their
# own copy the first time.
# ---------------------------------------------------------------------------
cat > "$PAYLOAD/launch.sh" <<'LAUNCH'
#!/usr/bin/env bash
# Resident Evil: Ruined Edition
#
# Starts the setup app. It finds or imports your own copy of the game, copies
# an HD background pack into hires/bgd, lets you turn line-art mode and the
# stick-figure cast on and off, and then starts the game.
#
# Keys while you play: F7 line-art, Shift+F7 and Ctrl+Shift+F7 more and less
# line detail, Ctrl+R resets the detail, F1 stick-figure cast, F10 or
# Ctrl+Q quit.
HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
cd "$HERE" || exit 1

# If this was double clicked from a file manager there is no terminal, and the
# setup app is a menu, so reopen it in one. x-terminal-emulator is Debian's
# generic name, the rest cover the common desktops.
if [ ! -t 0 ] && [ -z "${RE1_NO_TERM:-}" ]; then
    for term in x-terminal-emulator gnome-terminal konsole xfce4-terminal \
                kitty alacritty xterm; do
        if command -v "$term" >/dev/null 2>&1; then
            exec "$term" -e "$HERE/launch.sh" "$@"
        fi
    done
fi

exec "$HERE/ruined-edition" "$@"
LAUNCH
chmod +x "$PAYLOAD/launch.sh"

cat > "$PAYLOAD/README.txt" <<README
Resident Evil: Ruined Edition, version $VERSION
================================================

Double click launch.sh, or run ./launch.sh in a terminal.

What this is
------------
This is the 1996 Resident Evil PC release, decompiled and modded:

  * every character, monster and corpse is a stick figure. The figures only
    show up in line-art mode. F1 turns the cast on and off, F7 turns line-art
    on and off.
  * HD backgrounds, if you drop a pack in (see below)
  * a line-art mode with five levels of detail
  * the stick figures hold the weapons they actually use

Keys while you play:

    F7            line-art mode on and off
    Shift+F7      more line detail
    Ctrl+Shift+F7 less line detail
    Ctrl+R        put the line detail back to the default
    F1            stick-figure cast on and off
    F10 or Ctrl+Q quit

One note on the line detail keys. Use Ctrl+Shift+F7, not Ctrl+F7. On KDE,
Ctrl+F7 is taken by kwin for "Present Windows", so pressing it just throws the
desktop into a grid of little window thumbnails and the game never sees the
key. Ctrl+Shift+F7 is free everywhere. Ctrl+R resets the detail to its default.

Do not use any Ctrl+Alt+F-key chord in this game or anywhere else. Those are
the kernel's console switch keys, and on a fullscreen game they can take the
screen away from the desktop and lock up the session.

If you would rather keep Ctrl+F7, you can clear kwin's binding in System
Settings, Shortcuts, then search for "Present Windows".

First run
---------
The game's data is not in this download. That stuff belongs to Capcom, so we
do not ship it. You point the app at your own copy and it takes care of the
rest:

  1. Run ./launch.sh
  2. Pick "1. Import USA assets" and point it at either
       - your installed GOG folder (the one with USA/ and ResidentEvil.exe), or
       - a GOG offline installer .exe (this one needs the innoextract tool)
     If you play the Japanese PC release instead, pick "2. Import JPN assets"
     and point it at the Biohazard .iso.
  3. Pick q to play.

What is in this folder
----------------------
  launch.sh          start here
  ruined-edition     the setup, import and launcher app - the game and its
                     libraries are packed inside it and unpacked beside it
                     the first time you play
  hires/bgd/         drop an HD background pack here, see below
  USA/ and JPN/      your imported game data ends up here
  SAVE/              your save files

HD backgrounds
--------------
Put a background pack in hires/bgd/, or run ./launch.sh and pick "3. Import an
HD pack". The game loads any .webp named after the room camera, so RC1065.webp
is the main hall. They are drawn at 1280x960. If the folder is empty the game
just draws the original art, so it is fine to leave it that way.

What you need
-------------
A 64-bit Linux with 32-bit support (often called multilib) and glibc $GLIBC or
newer. The OpenGL driver, libGL and mesa, comes from your system. On
Arch and CachyOS that is the lib32-mesa package. On a Steam Deck, add
launch.sh as a non-Steam game rather than launching it through the Steam Linux
Runtime, because that runtime's older glibc is not what this was built against.

Game data is not included. Have fun.
README

# The archive carries the Arch name, so a download is unambiguous.
ARCH_ROOT="$(dirname "$OUT_ROOT")/$PLATFORM_ARCH_NAME"
say "arch folder: $ARCH_ROOT"
rm -rf "$ARCH_ROOT"
mkdir -p "$ARCH_ROOT"
cp -a "$PAYLOAD/." "$ARCH_ROOT/"

# Keep the plain desktop folder name too, for the copy dropped on the desktop.
DESKTOP_ROOT="$(dirname "$OUT_ROOT")/$APP_NAME"
rm -rf "$DESKTOP_ROOT"
mkdir -p "$DESKTOP_ROOT"
cp -a "$PAYLOAD/." "$DESKTOP_ROOT/"

# ---------------------------------------------------------------------------
# Tarball (Arch / generic Linux)
# ---------------------------------------------------------------------------
say "tarball (Arch)"
TARBALL="$OUT_ROOT/$PLATFORM_ARCH_NAME.tar.gz"
mkdir -p "$OUT_ROOT"
tar -C "$(dirname "$ARCH_ROOT")" -czf "$TARBALL" "$PLATFORM_ARCH_NAME"

# ---------------------------------------------------------------------------
# .deb - written directly with ar + tar, so dpkg is not needed.
#
# Layout is the standard one: debian-binary, control.tar.*, data.tar.*.
# Installs to /opt/ruined-edition with a symlink and a menu entry.
# ---------------------------------------------------------------------------
say "deb"
DEB_ROOT="$OUT_ROOT/deb-root"
rm -rf "$DEB_ROOT"
mkdir -p "$DEB_ROOT/DEBIAN"
mkdir -p "$DEB_ROOT/opt/$PKG_NAME"
mkdir -p "$DEB_ROOT/usr/bin"
mkdir -p "$DEB_ROOT/usr/share/applications"
mkdir -p "$DEB_ROOT/usr/share/icons/hicolor/512x512/apps"
mkdir -p "$DEB_ROOT/usr/share/doc/$PKG_NAME"

cp -a "$PAYLOAD/." "$DEB_ROOT/opt/$PKG_NAME/"

ln -sf "/opt/$PKG_NAME/launch.sh" "$DEB_ROOT/usr/bin/$PKG_NAME"

if [ -f "$PAYLOAD/icon.png" ]; then
    cp "$PAYLOAD/icon.png" "$DEB_ROOT/usr/share/icons/hicolor/512x512/apps/$PKG_NAME.png"
fi

cat > "$DEB_ROOT/usr/share/applications/$PKG_NAME.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Resident Evil: Ruined Edition (Debian)
Comment=Resident Evil (1996) PC decomp with stick-figure cast, HD backgrounds and line-art mode
Exec=$PKG_NAME
Path=/opt/$PKG_NAME
Icon=$PKG_NAME
Terminal=true
Categories=Game;
StartupNotify=true
DESKTOP

cat > "$DEB_ROOT/usr/share/doc/$PKG_NAME/copyright" <<'COPY'
Resident Evil: Ruined Edition

The game engine here is a decompilation of the 1997 Resident Evil PC release.
The added bits, meaning the line-art mode, the stick-figure cast, the HD
background support and the launcher, are original work.

There is no game data in this package. The art, models, sound, movies and text
belong to Capcom and are not distributed here. You supply your own legally
owned copy, and the launcher imports it on the first run.
COPY

SIZE="$(du -sk "$DEB_ROOT" | cut -f1)"
cat > "$DEB_ROOT/DEBIAN/control" <<CONTROL
Package: $PKG_NAME
Version: $VERSION
Section: games
Priority: optional
Architecture: i386
Depends: libgl1 | libgl1-mesa-glx, libglx-mesa0
Recommends: innoextract
Maintainer: Resident Evil Ruined Edition <noreply@example.invalid>
Installed-Size: $SIZE
Homepage: https://github.com/opencode/re1-decomp
Description: Resident Evil (1996) PC decomp with a stick-figure cast
 This is the 1996 Resident Evil PC release, decompiled and modded. Every
 character, monster and corpse is a stick figure, drawn in a line-art mode.
 There is HD background support and a launcher that imports your own copy of
 the game.
 .
 Game data is not included. Run ruined-edition to import your GOG install or
 the Japanese Biohazard iso, then play.
CONTROL

# postinst: nothing to compile, just tell the player what to do.
cat > "$DEB_ROOT/DEBIAN/postinst" <<'POST'
#!/bin/sh
set -e
echo "Resident Evil: Ruined Edition installed."
echo "Run 'ruined-edition' (or the menu entry) to import your own copy of the game."
exit 0
POST
chmod 755 "$DEB_ROOT/DEBIAN/postinst"

# md5sums
( cd "$DEB_ROOT" && find . -type f ! -path './DEBIAN/*' -printf '%P\0' \
    | xargs -0 -r md5sum > DEBIAN/md5sums )

DEB="$OUT_ROOT/$PLATFORM_DEB_NAME.deb"
rm -f "$DEB"
WORK="$OUT_ROOT/.deb-work"
rm -rf "$WORK"; mkdir -p "$WORK"
tar --owner=0 --group=0 -C "$DEB_ROOT/DEBIAN" -cJf "$WORK/control.tar.xz" .
tar --owner=0 --group=0 -C "$DEB_ROOT" -cJf "$WORK/data.tar.xz" --exclude=./DEBIAN .
printf '2.0\n' > "$WORK/debian-binary"
ar rc "$DEB" "$WORK/debian-binary" "$WORK/control.tar.xz" "$WORK/data.tar.xz"
rm -rf "$WORK"
echo "    $DEB"

# ---------------------------------------------------------------------------
# .rpm - written by tools/make_rpm.py, which implements the rpm header format
# directly, so rpmbuild is not needed.
# ---------------------------------------------------------------------------
say "rpm"
python3 "$REPO/tools/make_rpm.py" \
    --tree "$PAYLOAD" \
    --name "$PKG_NAME" \
    --version "$VERSION" \
    --release 1 \
    --arch i686 \
    --summary "Resident Evil (1996) PC decomp with a stick-figure cast" \
    --description "This is the 1996 Resident Evil PC release, decompiled and modded. Every character, monster and corpse is a stick figure, drawn in a line-art mode with HD background support and a launcher that imports your own copy of the game. Game data is not included." \
    --license "Decompilation: original work; no Capcom game data included" \
    --url "https://github.com/opencode/re1-decomp" \
    --prefix "/opt/$PKG_NAME" \
    --icon "$PAYLOAD/icon.png" \
    --out "$OUT_ROOT/$PLATFORM_RPM_NAME.rpm"
echo "    $OUT_ROOT/$PLATFORM_RPM_NAME.rpm"

# ---------------------------------------------------------------------------
# Windows: the real cross-compiled build.
#
# tools/build_windows_mingw.sh produces a genuine 32-bit residentevil.exe plus
# the SDL2/ffmpeg/runtime DLLs it needs. This archive ships those, so Windows
# players double-click and play; nothing has to be compiled on their machine.
#
# The native MSVC/Direct3D build is still the "reference" Windows port, and
# tools/build_windows_mingw.sh explains why this one is the GL/SDL backend
# (MSVC's naked-asm task stack switch cannot be built by GCC; the Win32 fiber
# path in TaskScheduler.cpp is used instead).
# ---------------------------------------------------------------------------
say "windows"
WIN_BUILD="$REPO/dist/windows-build"
[ -x "$WIN_BUILD/residentevil.exe" ] || die "$WIN_BUILD/residentevil.exe missing (run tools/build_windows_mingw.sh)"

WIN="$OUT_ROOT/${PLATFORM_WIN_NAME}"
rm -rf "$WIN"
mkdir -p "$WIN"

# The Windows launcher, if it was built. When present it is self-contained:
# the game exe and its DLLs are embedded inside it and extracted beside it on
# first run (see pack_embed_payload_windows). If the cross-build failed we fall
# back to shipping the game exe and DLLs loose, so the tree still runs.
WIN_LAUNCHER=""
for cand in \
    "$REPO/tools/ruined_edition/target/i686-pc-windows-gnu/release/ruined-edition.exe" \
    "$REPO/tools/ruined_edition/target/x86_64-pc-windows-gnu/release/ruined-edition.exe"
do
    if [ -f "$cand" ]; then WIN_LAUNCHER="$cand"; break; fi
done

if [ -n "$WIN_LAUNCHER" ]; then
    cp "$WIN_LAUNCHER" "$WIN/ruined-edition.exe"
else
    say "no Windows launcher; shipping the game exe and DLLs loose"
    cp "$WIN_BUILD/residentevil.exe" "$WIN/"
    cp "$WIN_BUILD"/*.dll "$WIN/"
    chmod +x "$WIN/residentevil.exe"
fi

[ -f "$PAYLOAD/icon.png" ] && cp "$PAYLOAD/icon.png" "$WIN/icon.png"
[ -f "$REPO/tools/ruined_edition/assets/re.ico" ] && cp "$REPO/tools/ruined_edition/assets/re.ico" "$WIN/re.ico"

# hires folder, same convention as Linux. Game data is never included.
mkdir -p "$WIN/hires/bgd"
cp "$PAYLOAD/hires/bgd/README.txt" "$WIN/hires/bgd/" 2>/dev/null || true
mkdir -p "$WIN/SAVE"

# A double-clickable launcher. Windows has no equivalent of launch.sh, so this
# is a .bat beside the exe. If the game exe is already unpacked beside it, start
# it directly; otherwise run the setup app, which unpacks the embedded game and
# walks the player through importing their own copy.
cat > "$WIN/Play Resident Evil.bat" <<'BAT'
@echo off
cd /d "%~dp0"
if exist "residentevil.exe" goto play
if exist "ruined-edition.exe" (
    echo The game is packed inside the launcher. Setting up and starting it.
    echo.
    ruined-edition.exe
    if errorlevel 1 pause
    goto end
)
echo No game or launcher found in this folder.
pause
goto end
:play
start "" residentevil.exe
:end
BAT

cat > "$WIN/README.txt" <<README
Resident Evil: Ruined Edition (Windows), version $VERSION
==========================================================

Double click "Play Resident Evil.bat", or run ruined-edition.exe.

What this is
------------
This is the 1996 Resident Evil PC release, decompiled and modded:

  * every character, monster and corpse is a stick figure, in line-art mode
  * HD backgrounds, if you drop a pack into hires\bgd
  * line-art mode with five levels of detail
  * the stick figures hold the weapons they actually use

Keys while you play:

    F7            line-art mode on and off
    Shift+F7      more line detail
    Ctrl+Shift+F7 less line detail
    Ctrl+R        put the line detail back to the default
    F1            stick-figure cast on and off
    F10 or Ctrl+Q quit

First run
---------
The game's data is not in this download. It belongs to Capcom, so it is not
shipped here. You supply your own copy:

  1. Make a folder for the game data beside residentevil.exe and call it USA
  2. Copy your GOG install's USA folder into it. You want USA\Data,
     USA\Enemy and so on.
  3. Double click "Play Resident Evil.bat"

If you would rather point it somewhere else, edit config.ini in the same
folder and set [Assets] Path.

HD backgrounds
--------------
Put .webp files named after the room camera into hires\bgd. RC1065.webp is the
main hall. They draw at 1280x960. It is fine to leave the folder empty.

What you need
-------------
64-bit or 32-bit Windows 10 or newer. The launcher carries the game and its
SDL2 / ffmpeg DLLs inside itself and unpacks them beside it on the first run;
nothing else is needed.

Game data is not included. Have fun.
README

cat > "$WIN/HOW THIS WAS BUILT.txt" <<'BUILT'
This Windows build is cross-compiled on Linux with MinGW-w64, from the same
source as the Linux version.

It is the SDL2 + OpenGL backend, not the Direct3D one. The Direct3D port needs
Microsoft's compiler, because the game's task scheduler uses MSVC-only naked
assembly. On the GCC/MinGW build the scheduler uses the Win32 fiber API
instead, so the same scheduling behaviour works either way.

To rebuild it yourself:
    bash tools/setup_windows_toolchain.sh     (fetches the compiler and libs)
    bash tools/build_windows_mingw.sh         (builds residentevil.exe)

The toolchain installs into ~/toolchain/win and needs no root. The full steps
are in README-WINDOWS.txt.
BUILT

[ -f "$REPO/tools/ruined_edition/README-WINDOWS.txt" ] && \
    cp "$REPO/tools/ruined_edition/README-WINDOWS.txt" "$WIN/README-WINDOWS.txt"

# Rebuild the zip from scratch - "7z a" adds to an existing archive, which would
# otherwise carry stale files from a previous run.
rm -f "$OUT_ROOT/$PLATFORM_WIN_NAME.zip"
( cd "$OUT_ROOT" && 7z a -tzip -bso0 -bsp0 "$PLATFORM_WIN_NAME.zip" "$PLATFORM_WIN_NAME" >/dev/null )
echo "    $OUT_ROOT/$PLATFORM_WIN_NAME.zip"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
say "done"
echo "Arch folder    : $ARCH_ROOT"
echo "Desktop folder : $DESKTOP_ROOT"
echo "Arch tarball   : $TARBALL"
echo "Debian         : $DEB"
echo "Fedora         : $OUT_ROOT/$PLATFORM_RPM_NAME.rpm"
echo "Windows        : $OUT_ROOT/$PLATFORM_WIN_NAME.zip"
echo

# ---------------------------------------------------------------------------
# Publish copies so the four packages are findable without digging into dist/.
# Desktop gets one folder per platform; Downloads gets all four together.
# ---------------------------------------------------------------------------
publish() {
    local desk="$HOME/Desktop" dl="$HOME/Downloads/Resident Evil Ruined Edition"
    [ -d "$desk" ] || return 0
    for pair in "Arch:tar.gz" "Debian:deb" "Fedora:rpm" "Windows:zip"; do
        local name="${pair%%:*}" ext="${pair##*:}"
        local src="$OUT_ROOT/$APP_NAME $name.$ext"
        [ -f "$src" ] || continue
        mkdir -p "$desk/$APP_NAME $name"
        cp -f "$src" "$desk/$APP_NAME $name/"
        mkdir -p "$dl"
        cp -f "$src" "$dl/"
    done
    cat > "$dl/HOW TO USE THESE.txt" <<'TXT'
Resident Evil Ruined Edition, all four platform builds.

  Resident Evil Ruined Edition Arch.tar.gz    Arch and CachyOS
  Resident Evil Ruined Edition Debian.deb     Debian and Ubuntu
  Resident Evil Ruined Edition Fedora.rpm     Fedora and RHEL
  Resident Evil Ruined Edition Windows.zip    Windows

Linux: unpack or install, then run launch.sh. It opens a file picker and asks
for your own copy of the game. Arch users can extract the tar.gz and run it.

Windows: unzip anywhere and run ruined-edition.exe, or double click
"Play Resident Evil.bat".

HD backgrounds: drop the .webp files into the hires folder that sits beside the
app. The game picks them up automatically.

No Capcom game data is in any of these. The launcher asks for your own copy the
first time you run it.
TXT
    say "published to ~/Desktop and ~/Downloads"
}

publish

echo "Game data is not bundled in any of them."
