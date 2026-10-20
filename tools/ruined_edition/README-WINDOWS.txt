Resident Evil: Ruined Edition, Windows build
============================================
THIS HAS NOT BEEN TESTED ON WINDOWS
-----------------------------------

Read this part first. It is a warning, not filler.

  * There is no compiled .exe in this download. The Windows port builds with
    Microsoft Visual C++, and the machine that packaged this archive runs
    Linux, so it cannot produce the exe for you.
  * Nobody has run this on Windows yet. The port compiles for the USA release
    with MSVC, but the packaged output has never been started. Expect to have
    to fiddle with something.
  * The game data is not here, and it never will be. That belongs to Capcom.
    You point this at your own copy.

If you build it and it works, nice, please say so. If it does not, the build
log is the useful part, not the exe.

What is in here
---------------
  README-WINDOWS.txt   this file
  re.ico               the game icon, ready to use as the exe's icon
  icon.png             the same icon as a PNG

The game data is not in here. Once you have built the exe, run it and it asks
for your own copy of the game, the same way the Linux launcher does.

How to build it
---------------
You need:

  * Windows 10 or 11
  * Visual Studio 2022 (v143) or 2026 (v145) with the "Desktop development
    with C++" workload, x86 toolset
  * The libraries the project's README lists for the Windows backend, which
    means SDL2 and ffmpeg so the feature set matches Linux

From a "Developer Command Prompt for VS":

    git clone <this repo>
    cd <this repo>
    build.bat

build.bat builds Release. build_debug.bat builds Debug.

One important thing: keep WholeProgramOptimization turned off in Release. /GL
together with /LTCG miscompiles the task scheduler's naked assembly and makes
the game crash now and then when a room loads. The project file already has
this right, so please do not turn it back on to "optimise" it.

The exe ends up in the Release output folder. To give it the icon, point the
project's ApplicationIcon at re.ico. In a .vcxproj that is:

    <ApplicationIcon>re.ico</ApplicationIcon>

or add re.ico under Resource Files in the IDE.

Running it
----------
The Windows build reads config.ini next to the exe, with the same keys as
Linux:

    [Display]
    LineArt=1          ; line-art mode, F7 turns it on and off while playing
    LineArtDetail=4    ; 1 to 5, Shift+F7 more and Ctrl+Shift+F7 less
    StickFigure=1      ; stick-figure cast, F1 toggles it, line-art mode only

    [Assets]
    Path=              ; empty means the folder the exe is in
    Version=USA        ; USA or JPN

Put your game data in USA/ next to the exe, and JPN/ too if you want the
Biohazard version. If you already have the Linux build, the USA/ and JPN/
folders are the same files, so you can copy them straight over. HD backgrounds
work the same way, as hires/bgd/*.webp.

Keys while you play: F7 line-art, Shift+F7 and Ctrl+Shift+F7 line detail,
Ctrl+R resets the detail, F1 stick-figure cast, F10 or Ctrl+Q to quit.

Where things stand
------------------
  Linux, the deb, rpm and tarball   tested, works
  Windows                           untested, no binary shipped, build it above

Game data is never bundled, on any platform.
