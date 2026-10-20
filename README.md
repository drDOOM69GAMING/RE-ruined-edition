# Resident Evil: Ruined Edition

Ink version of the 1997 Resident Evil PC release, built on
top of the [Resident Evil 1 PC decompilation](https://github.com/ecruells/resident-evil-pc-decomp)
by ecruells.

The whole game is re-drawn as if it were an ink illustration on paper: flat
outlines, crosshatched shading and a paper ground, with the cast painted as
costumed figures and every creature filled in its own flesh tone. It plays the
real game, with the real rooms, enemies, cutscenes and endings, just drawn in
ink instead of the original textures.

This repository holds the source and the Linux and Windows builds. **No game
data is included and none ever will be.** You point the launcher at your own
legally owned copy.

## What is in the release

Four builds, all from the same source:

| Download | For |
|---|---|
| `Resident Evil Ruined Edition Arch.tar.gz` | Arch and CachyOS, unpack and run `launch.sh` |
| `Resident Evil Ruined Edition Debian.deb` | Debian and Ubuntu, install, then run the launcher |
| `Resident Evil Ruined Edition Fedora.rpm` | Fedora and RHEL, install, then run the launcher |
| `Resident Evil Ruined Edition Windows.zip` | Windows, unzip and run `ruined-edition.exe` |

The launcher opens a folder picker and asks for your own copy of the game the
first time you run it. The Linux builds were packaged and checked on Linux.
**The Windows build has not been tested on real Windows hardware.** It was
cross-compiled with MinGW and it starts under Wine, but nobody has run it on an
actual Windows machine yet, so treat it as untested and expect to have to
fiddle with something. If you build it and it works, please say so; if it does
not, the error message is the useful part.

## How to play

1. Get your own copy of Resident Evil for PC. The GOG release is the easiest,
   it is the same data as the 1997 disc.
2. Run the launcher for your platform.
3. When it asks, point it at the folder that holds your game files. On GOG that
   is the folder with `USA` inside it, for example
   `.../GOG Games/Resident Evil`.
4. The launcher copies your files into place, then starts the game.

HD repainted backgrounds are supported: drop the `.webp` files into the
`hires/bgd` folder beside the app and the game picks them up automatically.

### Keys while you play

| Key | Action |
|---|---|
| `F7` | Turn the ink mode on and off |
| `Shift`+`F7` / `Ctrl`+`Shift`+`F7` | More / less ink detail, `Ctrl`+`R` resets it |
| `F1` | Swap the stick figures in and out, ink mode only |
| `F7` seven times in a row | Swap the stick figures for the real character models, still drawn in ink |
| `F10` or `Ctrl`+`Q` | Quit |

The F7 seven times in a row switch is the one that surprises most people: seven
plain presses in a row, with no other key between them, hands the characters
back their real models while leaving the whole level drawn in ink. Press `F1`
to go back to the stick figures.

## What the ink mode does

The frame is drawn as outlines on a page. An edge pass over the scene produces
the line work, a soft threshold controls how much ink lands, and the paper
ground sits underneath. The `[Display]` keys in `config.ini` set the starting
state:

```ini
[Display]
; Ink mode; F7 toggles it live. 0 = normal picture.
LineArt=1
; How much ink the pass draws, 1..5 (Shift+F7 = more, Ctrl+Shift+F7 = less, Ctrl+R = default).
LineArtDetail=4
; Draw characters as stick figures instead of their models. F1 toggles it live; ink mode only.
StickFigure=1
```

### The cast

Every character is drawn as a costumed figure rather than a bare skeleton. The
player and the other S.T.A.R.S. members wear their own uniforms: Chris in olive
and khaki, Jill in blue, Barry in his crimson vest, Rebecca with her red cross
and bandanna. Legs, boots and sleeves are painted in their real clothing
colours, so a figure reads as a person in a uniform rather than a stick.

The costumes come from the live player state, so the figure on screen always
matches the character you actually picked, whether you started a new game or
loaded a save.

### Monsters and the dead

Red is the infection. No living character ever gets red eyes or red details.
Red belongs to the monsters and the dead:

| Creature | Ink treatment |
|---|---|
| Zombie | Grey-green body, dark mismatched red eyes, jagged mouth |
| Cerberus dog | Pointed ears, red eyes, snout and tail |
| Hunter | Bulging acid-green frog eyes, wide grin, forked tongue, webbed hands and feet |
| Chimera | Antennae, three red compound eyes, mandibles, claws |
| Tyrant | Heavy brow, molten red eyes, three claws per hand |
| Crow, wasp, spider | Beaks, antennae, fangs, red eyes, stingers |
| Snake, Neptune, Plant 42 | Wedge heads, zigzag teeth, thorns, tip maw, red eyes |

Corpses on the floor keep their uniform and get a skin tone head with red X
eyes and a flat mouth, so a body reads as a fallen person, not a monster.

Humanoid monsters are classified by the same joint map as the humans but are
never dressed as one: every monster segment is filled with its own species
flesh tone instead.

### Faces and weapons

The figure's line weight and head ring are sized from the head-to-foot span, so
proportions hold as a character moves toward or away from the camera. The
player's equipped weapon is drawn in the right hand, so the figure holds the
knife, handgun, shotgun, magnum, flamethrower or launcher actually in use.

The ink is written into the scene and copied through by the ink shader, and each
figure is interleaved into the same far-to-near painter walk as the level
geometry. That is what stops a zombie in the next room showing through a wall.

## How to build

The game logic is shared between platforms; only the platform layer and the
renderer backend differ.

### Linux

A native 32-bit binary (`-m32`) built with CMake, SDL2 and OpenGL, with ffmpeg
for the FMV. Requirements and the Arch/CachyOS and Debian/Ubuntu package names
are in the port's own notes. In short:

```
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux -j
```

This produces `build/linux/residentevil`. The packaging script builds the
launcher and the portable bundle:

```
bash tools/package_ruined_edition.sh
```

### Windows

Built with Visual Studio, x86 toolset, from a Developer Command Prompt:

```
build.bat
```

Keep `WholeProgramOptimization` turned off in Release. `/GL` together with
`/LTCG` miscompiles the task scheduler's naked assembly and makes the game crash
now and then when a room loads. The project file already has this right. The
Windows launcher is a Rust program in `tools/ruined_edition/`.

## The decompilation underneath

This is a re-draw, not a rewrite. Under the ink layer it is the same
decompilation of the 1997 executable, rebuilt on a modern rendering layer:

| Platform | Renderer | Audio | FMV |
|---|---|---|---|
| Windows (Win32) | Direct3D 11 | XAudio2 | MCI |
| Linux (SDL2 + OpenGL 3.3) | OpenGL core | SDL2 mixer | ffmpeg (Cinepak AVI) |

Both builds share every line of game logic. Every rewritten function carries
its original address as a comment and every named global documents its original
variable address, so any line in `src/` traces back to the original binary.

The port is feature complete: the entire original game is playable, every room,
enemy, cutscene, FMV, menu and ending. The ink mode, the stick-figure cast and
the F7 seven times switch are additions of this edition; the underlying game is
unchanged.

## Layout

- `src/marni/` - the Marni System compatibility layer, implemented by
  `MarniDX.cpp` (DX11, Windows) and `MarniDX_GL.cpp` (OpenGL, Linux).
- `src/game/` - the game logic, shared by both platforms. The ink and stick
  figure work lives in `TmdRenderer.cpp`.
- `src/platform/` - entry points, input, audio, video, settings and crash
  handling for each platform.
- `tools/ruined_edition/` - the Rust launcher.
- `tools/package_ruined_edition.sh` - the release packaging script.
- `docs/` - architecture and porting notes.

## Credits and license

The decompilation and the Linux port are the work of
[ecruells](https://github.com/ecruells) and contributors to the
[resident-evil-pc-decomp](https://github.com/ecruells/resident-evil-pc-decomp)
project. The Ruined Edition ink renderer, the stick-figure cast and the launcher
are added on top of it here.

This project is licensed under the **GNU General Public License v3.0**, see
[`LICENSE`](LICENSE) for the full text. The license covers only the source in
this repository. It does not and cannot grant any rights over the original game.

### Legal notice

- **No game assets are distributed here.** The repository contains source code
  and compiled launchers only. Running the build requires your own legally
  obtained copy of Resident Evil for PC to supply the data files.
- Resident Evil, its code, assets, characters and trademarks are the property
  of CAPCOM CO., LTD. This project is an independent reverse-engineering and
  preservation effort, not affiliated with, authorized, endorsed or sponsored
  by Capcom in any way.
- The decompiled logic in `src/` is derived from the original executable for
  interoperability, documentation and preservation purposes.
