#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# build_windows_mingw.sh - cross-compile "Resident Evil: Ruined Edition" for
# Windows from Linux, using MinGW-w64.
#
# This builds the GL/SDL backend (the same one the Linux port uses) for the
# Windows target, and links it against the Windows SDL2, ffmpeg and libwebp in
# the user-local Windows toolchain. It does NOT use the MSVC Direct3D backend:
# that path needs MSVC's __asm { } bare-metal stack switch in
# src/game/TaskScheduler.cpp, which GCC cannot compile. On Windows/GCC the
# scheduler uses the Win32 fiber API instead (see the _WIN32 branch there).
#
# Output: dist/windows-build/residentevil.exe plus the DLLs beside it, ready to
# be dropped in the Windows package.
#
# Usage:
#   bash tools/build_windows_mingw.sh
#
# Expects the toolchain laid out under ~/toolchain/win:
#   prefix/   the extracted mingw-w64 gcc/binutils/crt/headers/winpthreads pkgs
#   sdl2/     SDL2-devel-<v>-mingw.tar.gz, unpacked
#   ffmpeg/   the ffmpeg shared win64 build, unpacked
#   webp/     libwebp built for mingw (configure --host=x86_64-w64-mingw32)
# tools/setup_windows_toolchain.sh fetches all four.
# ---------------------------------------------------------------------------
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOL="$HOME/toolchain/win"
TC="$TOOL/prefix/usr/bin"
TRIPLE=i686-w64-mingw32

# 32-bit (i686) throughout. The port is strictly 32-bit: every struct in
# src/game/Types.h and Entities.h has a static_assert pinned to the original
# 1997 memory layout, and pointer values are packed into DWORD. A 64-bit build
# fails those asserts, by design.
SDL="$TOOL/sdl2/SDL2-2.30.9/$TRIPLE"
FF="$TOOL/ffmpeg32"
WEBP="$TOOL/webp32"

OUT="${RE1_WIN_OUT:-$REPO/dist/windows-build}"

die() { echo "error: $*" >&2; exit 1; }

[ -x "$TC/$TRIPLE-g++" ] || die "$TC/$TRIPLE-g++ missing (run tools/setup_windows_toolchain.sh)"
[ -d "$SDL" ]            || die "$SDL missing (SDL2 devel not unpacked)"
[ -d "$FF/lib" ]         || die "$FF missing (32-bit ffmpeg not built)"
[ -d "$WEBP/lib" ]       || die "$WEBP missing (libwebp not built for mingw)"

CXX="$TC/$TRIPLE-g++"

# ---------------------------------------------------------------------------
# Translation units. Shared sources come straight from the Linux list in
# CMakeLists.txt; the platform layer is the Linux/SDL one (audio/input/video/
# config/crash/stubs/main) and the renderer backend is the GL one. crash.cpp is
# left out because it is glibc execinfo; the Windows build gets the game's
# existing Win32 crash handling through the shared code.
# ---------------------------------------------------------------------------
SHARED=(
    src/Globals.cpp
    src/system/AssetPath.cpp
    src/system/AudioFile.cpp
    src/system/PngImage.cpp
    src/system/ConfigFile.cpp
    src/marni/MarniBits.cpp
    src/marni/PSXTexture.cpp
    src/marni/Marni3DObject.cpp
    src/marni/MarniSystem.cpp
    src/video/VideoPlayback.cpp
    src/game/CharacterSelectionScreen.cpp
    src/game/CmdFunctions.cpp
    src/game/CollisionDebug.cpp
    src/game/ComputerLab.cpp
    src/game/DeathScreen.cpp
    src/game/DebugMenu.cpp
    src/game/DebugSaveLoad.cpp
    src/game/DebugScreens.cpp
    src/game/DoorSystem.cpp
    src/game/EffectSprites.cpp
    src/game/EffectSystem.cpp
    src/game/EndingScreen.cpp
    src/game/Ps1EndingCredits.cpp
    src/game/Ps1FmvSubtitles.cpp
    src/game/EntityModelLoader.cpp
    src/game/FadeSprite.cpp
    src/game/FileLoader.cpp
    src/game/GameInit.cpp
    src/game/GameLoop.cpp
    src/game/GameStart.cpp
    src/game/GteMatrix.cpp
    src/game/InputSystem.cpp
    src/game/InteractiveScreen.cpp
    src/game/JpnTextTables.cpp
    src/game/LabSlides.cpp
    src/game/LogosScreen.cpp
    src/game/MainLoop.cpp
    src/game/MainMenu.cpp
    src/game/MenuData.cpp
    src/game/ObjectManager.cpp
    src/game/OptionsMenu.cpp
    src/game/PathTrail.cpp
    src/game/PlayerAnimations.cpp
    src/game/PrintText.cpp
    src/game/Rendering.cpp
    src/game/Room.cpp
    src/game/RoomCollision.cpp
    src/game/RoomEvents.cpp
    src/game/RoomInit.cpp
    src/game/SaveLoadScreen.cpp
    src/game/SoundApi.cpp
    src/game/SoundSystem.cpp
    src/game/SoundTables.cpp
    src/game/SpriteRenderer.cpp
    src/game/TaskScheduler.cpp
    src/game/TextureLoader.cpp
    src/game/TitleScreen.cpp
    src/game/TmdAnimation.cpp
    src/game/TmdRenderer.cpp
    src/game/WeaponDamage.cpp
    src/game/dc/WeaponDamageTables.cpp
    src/game/dc/ItemTables.cpp
    src/game/dc/ItemNames.cpp
    src/game/dc/ItemModels.cpp
    src/game/dc/ItemDescriptions.cpp
    src/game/dc/Items.cpp
    src/game/dc/EntityModels.cpp
    src/game/dc/ArrangeStages.cpp
    src/game/dc/ArrangeSoundRows.cpp
    src/game/entities/Adder.cpp
    src/game/entities/BlackTiger.cpp
    src/game/entities/Cerberus.cpp
    src/game/entities/CharacterNpc.cpp
    src/game/entities/Chimera.cpp
    src/game/entities/ComputerArms.cpp
    src/game/entities/Crow.cpp
    src/game/entities/EntityCommon.cpp
    src/game/entities/Hunter.cpp
    src/game/entities/MonsterPlant.cpp
    src/game/entities/Neptune.cpp
    src/game/entities/Plant42.cpp
    src/game/entities/Plant42Roots.cpp
    src/game/entities/SpiderWeb.cpp
    src/game/entities/Tyrant.cpp
    src/game/entities/Wasp.cpp
    src/game/entities/WebSpinner.cpp
    src/game/entities/Yawn.cpp
    src/game/entities/Zombie.cpp
)

PLATFORM=(
    src/platform/linux/platform.cpp
    src/platform/linux/input.cpp
    src/platform/linux/audio.cpp
    src/platform/linux/video.cpp
    src/platform/linux/config.cpp
    src/platform/linux/stubs.cpp
    src/platform/linux/main.cpp
    # This hybrid build takes TaskScheduler.cpp's _WIN32 (fiber) switch path,
    # whose OS calls live behind platform.h -- pull in their implementation.
    src/platform/win32/fibers.cpp
    src/marni/MarniDX_GL.cpp
    src/marni/MarniGLFuncs.cpp
)

# The crash reporter is glibc-only on Linux (execinfo). Windows gets the Win32
# unhandled-exception version instead; both write crash.log with the same shape.
PLATFORM+=(tools/crash_win.cpp)

say() { printf '\n==> %s\n' "$*"; }
mkdir -p "$OUT"

ALL=("${SHARED[@]}" "${PLATFORM[@]}")

say "compiling ${#ALL[@]} translation units for Windows (x86-64)"
OBJDIR="$OUT/obj"
rm -rf "$OBJDIR"; mkdir -p "$OBJDIR"

# -w: the port is a faithful transcription and has warning noise on GCC; the
# Linux build uses -w for the same reason. -fno-strict-aliasing: the port type
# puns deliberately (see CMakeLists.txt); without this every TMD vertex comes
# out (0,0,0) at -O2. SDL_MAIN_HANDLED: the game has its own WinMain/ main.
FLAGS=(
    -std=c++17 -O2 -w -fno-strict-aliasing -m32
    -DSDL_MAIN_HANDLED -DNOMINMAX -DWIN32_LEAN_AND_MEAN
    -include "$REPO/tools/win_compat.h"
    -I"$REPO/src"
    -I"$SDL/include"
    -I"$FF/include"
    -I"$WEBP/include"
)

rc=0
for src in "${ALL[@]}"; do
    obj="$OBJDIR/$(echo "$src" | tr '/' '_').o"
    if ! "$CXX" "${FLAGS[@]}" -c "$REPO/$src" -o "$obj" 2>"$OBJDIR/err.txt"; then
        echo "FAILED: $src"
        cat "$OBJDIR/err.txt"
        rc=1
    fi
done
[ "$rc" -eq 0 ] || die "compile errors above"

say "linking residentevil.exe"
# -mwindows: GUI subsystem, no console window. SDL2 and ffmpeg are DLLs and
# travel beside the exe; libwebp is linked statically.
"$CXX" -o "$OUT/residentevil.exe" "$OBJDIR"/*.o \
    -mwindows -m32 \
    -static-libgcc -static-libstdc++ \
    -L"$SDL/lib" -L"$FF/lib" \
    -lmingw32 -lSDL2 \
    -lavformat -lavcodec -lavutil -lswresample \
    "$WEBP/lib/libwebp.a" "$WEBP/lib/libsharpyuv.a" \
    -lwinmm -lole32 -loleaut32 -luuid -lgdi32 -luser32 -lshell32 \
    -lws2_32 -lsetupapi -limm32 -lversion -lbcrypt -lsecur32 -lcfgmgr32 -ldbghelp

say "copying runtime DLLs"
mkdir -p "$OUT/lib"
cp -L "$SDL/bin/SDL2.dll" "$OUT/" 2>/dev/null || cp "$SDL/bin/"*.dll "$OUT/"
cp -L "$FF/bin/"*.dll "$OUT/"

# The GCC runtime the imported DLLs depend on. ffmpeg's DLLs (and the game
# itself) are linked against libgcc_s_dw2-1 and libstdc++-6; without them next
# to the exe Windows refuses to load avcodec/avformat/avutil at all and the game
# exits with a "missing DLL" error before it ever opens a window. These live in
# the mingw triple's bin directory, not in the SDL or ffmpeg folders, so they
# have to be copied explicitly.
for dll in libwinpthread-1.dll libgcc_s_dw2-1.dll libstdc++-6.dll; do
    src="$(ls "$TOOL/prefix/usr/$TRIPLE/bin/$dll" 2>/dev/null | head -1)"
    [ -n "$src" ] && cp -L "$src" "$OUT/" 2>/dev/null || true
done

say "done"
file "$OUT/residentevil.exe"
ls -1 "$OUT"/*.dll 2>/dev/null | sed 's|.*/|    |'
echo "==> windows build complete"
