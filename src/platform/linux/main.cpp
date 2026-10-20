// main.cpp - Linux entry point.
//
// Counterpart of src/platform/win32/main.cpp: reads config.ini, creates
// the SDL2 window and GL context, brings up the Marni system, then drives
// main_loop() at the engine's 33 ms tick exactly as RunMessageLoop does on
// Windows (see src/platform/win32/main.cpp:706-757 for the
// instruction-for-instruction commentary on that limiter).
#include <SDL2/SDL.h>
#if defined(_WIN32)
#include <SDL2/SDL_syswm.h>   // SDL_SysWMinfo: the real HWND for g_hWnd
#endif

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "Globals.h"
#include "platform/platform.h"
#include "system/AssetPath.h"
#include "system/ConfigFile.h"
#include "marni/MarniDX.h"
#include "marni/MarniGLFuncs.h"
#include "marni/MarniSystem.h"

// 0x00441f26: the original's frame interval, 0x21 == 33 ms == 30 ticks/s. The
// play clock and every scripted wait are built on it.
static const int kFrameIntervalMs = 33;
static const BOOL kFrameLimiterEnabled = TRUE;

// The line-art detail a fresh config starts from, matching the initial value of
// g_iLineArtDetail in Globals.cpp. Ctrl+R restores this.
static const int kLineArtDefaultDetail = 3;

// ---------------------------------------------------------------------------
// DumpCapture - write the game content rectangle as RE1CAP + RGBA8888.
// ---------------------------------------------------------------------------
static void DumpCapture(const char* path, int frame)
{
    MarniDX* dx = Marni_DX();
    void* pixels = NULL;
    DWORD cw = 0, ch = 0;
    if (dx != NULL && dx->CaptureBackbufferToRGBA(&pixels, &cw, &ch) && pixels != NULL) {
        FILE* f = fopen(path, "wb");
        if (f != NULL) {
            fprintf(f, "RE1CAP %u %u\n", (unsigned)cw, (unsigned)ch);
            fwrite(pixels, 1, (size_t)cw * ch * 4, f);
            fclose(f);
            printf("[RE1] captured frame %d -> %s (%ux%u)\n",
                   frame, path, (unsigned)cw, (unsigned)ch);
        }
        free(pixels);
    } else {
        fprintf(stderr, "[RE1] capture failed\n");
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
    // Crash diagnostics and the single-instance guard, in the game root like
    // their Windows counterparts (WinMain does the same, in this order).
    crashlog_install();
    if (!plat_single_instance_check()) {
        fprintf(stderr, "[RE1] RESIDENT EVIL is already running.\n");
        return 3;
    }

    // --capture <file> [frames]: render normally, dump the back buffer to
    // <file> once `frames` have been presented (default 120 ≈ 4 s), then exit.
    // This is the static-frame comparison hook from docs/LINUX_PORT.md 6.7.
    const char* capturePath = NULL;
    int captureFrame = 120;

    // --shots <prefix> <f1,f2,...>: capture several frames in one run, named
    // <prefix>_<frame>.raw, and keep playing until the last one. --capture is
    // still the one-shot form used by the docs.
    const char* shotPrefix = NULL;
    int shotFrames[32];
    int shotCount = 0;

    // --press <scancode> <frame> [hold]: inject one synthetic key press at
    // `frame` (down, then up `hold` frames later; default 3). Lets the boot
    // flow be driven past "PRESS ANY BUTTON" without a keyboard, so
    // interactivity is testable headlessly. SDL_SCANCODE_E == 8 is the default
    // confirm key; a long hold drives held-key behaviour (walking, rotating a
    // model in the item viewer).
    // --pressmod <scancode> <frame> <mod>: the same press with an explicit
    // SDL_Keymod on the keysym, so the modified F7 forms (Shift/Ctrl) can be
    // exercised headlessly. `mod` is a KMOD_* bitmask (SHIFT=0x1, CTRL=0x40).
    // state guards the down/up edges: presented does not advance while a tick
    // is being skipped, so without it the same edge would be re-injected every
    // loop iteration (30-odd times) before the next frame.
    struct PressEvent { int scancode; int frame; int hold; int state; int mod; };
    PressEvent presses[16];
    int pressCount = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            capturePath = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') captureFrame = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--shots") == 0 && i + 2 < argc) {
            shotPrefix = argv[++i];
            char* list = argv[++i];
            for (char* tok = strtok(list, ","); tok != NULL && shotCount < 32;
                 tok = strtok(NULL, ",")) {
                shotFrames[shotCount++] = atoi(tok);
            }
        } else if (strcmp(argv[i], "--press") == 0 && i + 2 < argc && pressCount < 16) {
            presses[pressCount].scancode = atoi(argv[++i]);
            presses[pressCount].frame = atoi(argv[++i]);
            presses[pressCount].hold = 3;
            presses[pressCount].state = 0;
            presses[pressCount].mod = 0;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                presses[pressCount].hold = atoi(argv[++i]);
            }
            ++pressCount;
        } else if (strcmp(argv[i], "--pressmod") == 0 && i + 3 < argc && pressCount < 16) {
            presses[pressCount].scancode = atoi(argv[++i]);
            presses[pressCount].frame = atoi(argv[++i]);
            presses[pressCount].mod = (int)strtol(argv[++i], NULL, 0);
            presses[pressCount].hold = 3;
            presses[pressCount].state = 0;
            ++pressCount;
        }
    }

    // config.ini is the settings store for both builds now: created with
    // documented defaults on first run, then read here. The same file drives
    // the Windows build, so settings travel between them.
    ConfigFile_EnsureExists();
    ConfigFile_Load();

    const int width  = (int)g_dwScreenWidth;
    const int height = (int)g_dwScreenHeight;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;
    SDL_Window* window = SDL_CreateWindow("RESIDENT EVIL", SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED, width, height, flags);
    if (window == NULL) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    if (g_bFullScreen) {
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }

    SDL_GLContext ctx = SDL_GL_CreateContext(window);
    if (ctx == NULL) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(window, ctx);
    SDL_GL_SetSwapInterval(g_bVSync ? 1 : 0);

    if (!MarniGL_LoadFunctions((void* (*)(const char*))SDL_GL_GetProcAddress)) {
        fprintf(stderr, "failed to resolve GL entry points\n");
        SDL_GL_DeleteContext(ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    printf("[RE1] GL %s | %s\n",
           (const char*)glGetString(GL_VERSION),
           (const char*)glGetString(GL_RENDERER));

#if defined(_WIN32)
    // g_hWnd is a Win32 HWND and the sound system needs it, but this is the SDL
    // window. Ask SDL for the underlying HWND rather than casting the pointer.
    {
        SDL_SysWMinfo wmi;
        SDL_VERSION(&wmi.version);
        if (SDL_GetWindowWMInfo(window, &wmi))
            g_hWnd = wmi.info.win.window;
        else
            g_hWnd = NULL;
    }
#else
    g_hWnd = window;
#endif

    // Windows brings the sound system up from WM_CREATE (WindowProc.cpp ->
    // StartSoundSystemAsync). There is no window procedure here, so do it
    // explicitly - without it g_pDirectSound stays NULL and every bank load
    // reports "NOT READY".
    StartSoundSystemAsync(g_hWnd);

    // 0x004414f6: allocate the guard-paged task stacks. the Windows main.cpp calls this
    // in WinMain before anything schedules a task; without it the first
    // TaskSwitch_Start passes a null stack to makecontext.
    TaskScheduler_Init();

    // Bring up CMarniDirect3D + the renderer backend. Same call the Windows
    // build makes; the constructor creates the MarniDX backend internally.
    InitializeMarniSystem();
    if (!IsGraphicsSystemReadyForOperation()) {
        fprintf(stderr, "graphics system failed to initialize\n");
        SDL_GL_DeleteContext(ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    g_bWindowFocused = TRUE;
    g_bWindowActive  = TRUE;
    g_bQuitFlag      = FALSE;

    DWORD lastTick = plat_time_ms();
    g_dwGameTimer1 = lastTick;
    int running = 1;
    int presented = 0;

    // Live modifier state, tracked from the modifier key events themselves
    // rather than trusted to SDL_Keymod on the F7 event. Some X11/Wayland
    // stacks and window managers report an empty `keysym.mod` for a
    // modifier+function-key chord (and a few grab the chord entirely), which
    // silently turned Shift+F7 / Ctrl+F7 into plain F7. Watching LSHIFT/RSHIFT,
    // LCTRL/RCTRL and LALT/RALT down/up is exact and independent of that.
    BOOL modShift = FALSE;
    BOOL modCtrl  = FALSE;

    // Seven plain F7 presses in a row is the hidden cast swap: the stick
    // figures come back as the real character models, line-arted like the
    // level (g_bLineArtRealChars). The presses only count when they arrive
    // within kF7CheatWindowMs of each other, so ordinary on/off toggling
    // during play never trips it by accident.
    enum { kF7CheatCount = 7, kF7CheatWindowMs = 2000 };
    int  f7CheatCount  = 0;
    DWORD f7CheatLastMs = 0;

    while (running) {
        for (int p = 0; p < pressCount; ++p) {
            // Hold for at least 3 frames so the rising edge is always observed.
            if (presented == presses[p].frame && presses[p].state == 0) {
                SDL_Event ev = {};
                ev.type = SDL_KEYDOWN;
                ev.key.type = SDL_KEYDOWN;
                ev.key.state = SDL_PRESSED;
                ev.key.repeat = 0;
                ev.key.keysym.scancode = (SDL_Scancode)presses[p].scancode;
                ev.key.keysym.sym = SDL_GetKeyFromScancode((SDL_Scancode)presses[p].scancode);
                ev.key.keysym.mod = (SDL_Keymod)presses[p].mod;
                SDL_PushEvent(&ev);
                presses[p].state = 1;
            } else if (presented == presses[p].frame + presses[p].hold &&
                       presses[p].state == 1) {
                SDL_Event ev = {};
                ev.type = SDL_KEYUP;
                ev.key.type = SDL_KEYUP;
                ev.key.state = SDL_RELEASED;
                ev.key.keysym.scancode = (SDL_Scancode)presses[p].scancode;
                ev.key.keysym.sym = SDL_GetKeyFromScancode((SDL_Scancode)presses[p].scancode);
                SDL_PushEvent(&ev);
                presses[p].state = 2;
            }
        }

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT:
                running = 0;
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) g_bWindowFocused = TRUE;
                else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) g_bWindowFocused = FALSE;
                else if (e.window.event == SDL_WINDOWEVENT_CLOSE) running = 0;
                break;
            case SDL_KEYDOWN:
                // Track modifier keys by their own events. Do this before the
                // shortcut checks so a modifier pressed just ahead of F7 is
                // seen even if the F7 event's mask is empty.
                switch (e.key.keysym.scancode) {
                case SDL_SCANCODE_LSHIFT:
                case SDL_SCANCODE_RSHIFT: modShift = TRUE; break;
                case SDL_SCANCODE_LCTRL:
                case SDL_SCANCODE_RCTRL:  modCtrl  = TRUE; break;
                default: break;
                }
                // F1 draws the main character as a stick figure (port
                // extension). It is a line-art-mode effect: the figure is ink
                // drawn over the composited page, so with line-art off the flag
                // is remembered but the picture is unchanged. The game maps F1
                // for its debug menu, which is off in Release, and the key is
                // not forwarded, so nothing else sees it.
                if (e.key.keysym.scancode == SDL_SCANCODE_F1 &&
                    e.key.repeat == 0) {
                    g_bStickFigure = !g_bStickFigure;
                    printf("[display] stick-figure %s%s\n",
                           g_bStickFigure ? "ON" : "OFF",
                           (g_bStickFigure && !g_bLineArt)
                               ? " (needs line-art mode - press F7)" : "");
                    break;
                }
                // F7 drives line-art mode live (port extension). The game only
                // ever maps F1/F6/F8/F9 (platform.cpp), so F7 and its modifier
                // forms are invisible to it - and F7 must stay unforwarded.
                // Plain F7 toggles, Shift+F7 raises the edge detail, and
                // Ctrl+Shift+F7 lowers it. Touching the level switches the mode
                // on so the change is visible. Seven plain presses in a row
                // (within two seconds of each other) instead swap the cast:
                // the stick figures come back as the real character models,
                // which the line-art pass then inks like the level itself,
                // and line art is switched on so the result is visible.
                //
                // Ctrl+F7 on its own cannot be used: kwin (KDE) binds it to
                // "Present Windows (Window class)" by default, so kwin eats the
                // key before the game sees it and the player just gets the
                // window expose effect - the screen collapsing into a grid of
                // small squares. Ctrl+Shift+F7 is not bound by any desktop, so
                // it always reaches the game.
                //
                // There is deliberately NO Alt form. Ctrl+Alt+F1..F12 are the
                // kernel's virtual-console switch keys: the VT layer and the
                // display manager claim them below the desktop, so a desktop
                // shortcut listing never shows them as taken. Pressing one while
                // a fullscreen game holds the GL context and the DRM master
                // moves the foreground console away from the compositor, the
                // game freezes with no surface, and the session follows it down.
                // A Ctrl+Alt+F7 reset key did exactly that on this machine, so
                // the whole Ctrl+Alt+Fn family is off limits. The reset lives on
                // Ctrl+R instead.
                if (e.key.keysym.scancode == SDL_SCANCODE_F7 &&
                    e.key.repeat == 0) {
                    // Trust the event's own modifier mask when it carries one,
                    // and fall back to the tracked state otherwise (some
                    // stacks hand F7 an empty mask on a chord).
                    BOOL shift = ((e.key.keysym.mod & KMOD_SHIFT) != 0) || modShift;
                    BOOL ctrl  = ((e.key.keysym.mod & KMOD_CTRL) != 0) || modCtrl;
                    if (shift && ctrl) {
                        if (g_iLineArtDetail > 1) --g_iLineArtDetail;
                        g_bLineArt = TRUE;
                    } else if (shift) {
                        if (g_iLineArtDetail < 5) ++g_iLineArtDetail;
                        g_bLineArt = TRUE;
                    } else if (ctrl) {
                        // Only reachable on desktops that leave Ctrl+F7 alone.
                        if (g_iLineArtDetail > 1) --g_iLineArtDetail;
                        g_bLineArt = TRUE;
                    } else {
                        // Plain press: toggle line art, and count the presses
                        // - seven in a row is the cast swap (see the counter's
                        // declaration above).
                        DWORD nowMs = plat_time_ms();
                        if (nowMs - f7CheatLastMs > kF7CheatWindowMs)
                            f7CheatCount = 0;
                        f7CheatLastMs = nowMs;
                        if (++f7CheatCount >= kF7CheatCount) {
                            f7CheatCount = 0;
                            g_bLineArtRealChars = !g_bLineArtRealChars;
                            g_bLineArt = TRUE;
                            printf("[display] line-art cast: %s\n",
                                   g_bLineArtRealChars
                                       ? "real characters (inked like the level)"
                                       : "stick figures");
                        } else {
                            g_bLineArt = !g_bLineArt;
                        }
                    }
                    printf("[display] line-art %s, detail %d/5\n",
                           g_bLineArt ? "ON" : "OFF", g_iLineArtDetail);
                    break;
                }
                // Ctrl+R puts the line-art detail back to its default. It is a
                // plain Ctrl chord, so unlike Ctrl+Alt+Fn it never reaches the
                // kernel VT switch. The game does not map R, and the key is not
                // forwarded, so nothing else sees it.
                if (e.key.repeat == 0 &&
                    e.key.keysym.scancode == SDL_SCANCODE_R &&
                    (((e.key.keysym.mod & KMOD_CTRL) != 0) || modCtrl)) {
                    g_iLineArtDetail = kLineArtDefaultDetail;
                    g_bLineArt = TRUE;
                    printf("[display] line-art ON, detail %d/5 (default)\n",
                           g_iLineArtDetail);
                    break;
                }
                // F10 (and Ctrl+Q) quits: with no window decorations in
                // fullscreen there was otherwise no obvious way out.
                if (e.key.repeat == 0 &&
                    (e.key.keysym.scancode == SDL_SCANCODE_F10 ||
                     (e.key.keysym.scancode == SDL_SCANCODE_Q &&
                      (((e.key.keysym.mod & KMOD_CTRL) != 0) || modCtrl)))) {
                    printf("[RE1] quitting\n");
                    running = 0;
                    break;
                }
                plat_key_event(e.key.keysym.scancode, TRUE);
                break;
            case SDL_KEYUP:
                // Keep the tracked modifier state current; the F7 handler
                // consults it when the event mask is empty.
                switch (e.key.keysym.scancode) {
                case SDL_SCANCODE_LSHIFT:
                case SDL_SCANCODE_RSHIFT: modShift = FALSE; break;
                case SDL_SCANCODE_LCTRL:
                case SDL_SCANCODE_RCTRL:  modCtrl  = FALSE; break;
                default: break;
                }
                plat_key_event(e.key.keysym.scancode, FALSE);
                break;
            default:
                break;
            }
        }

        if (!running || g_bQuitFlag) break;
        if (!g_bWindowFocused) { SDL_Delay(1); continue; }

        DWORD now = plat_time_ms();

        // 0x00441f0c: the frame limiter. Skip a tick that arrives early.
        if (kFrameLimiterEnabled) {
            BOOL bSkipFrame = FALSE;
            if (g_bUseFrameSkip == 1) {
                if (!g_bFrameSkipDetected) {
                    if ((int)(kFrameIntervalMs + g_dwGameTimer1) > (int)now &&
                        (int)(g_dwGameTimer1 - 0x10000) < (int)now) {
                        bSkipFrame = TRUE;
                    }
                }
            } else {
                if ((int)(g_dwGameTimer1 + 16) > (int)now &&
                    (int)(g_dwGameTimer1 - 0x10000) < (int)now) {
                    bSkipFrame = TRUE;
                }
            }
            if (bSkipFrame) {
                SDL_Delay(1);
                continue;
            }
        }

        g_dwGameTimer1 = now;

        // Capture before the FMV gate: a movie's frames are presented by the
        // video backend, and the frame counter has to advance for those too or
        // --capture can never land inside a movie.
        ++presented;
        if (capturePath != NULL && presented >= captureFrame) {
            DumpCapture(capturePath, presented);
            break;
        }
        if (shotPrefix != NULL && shotCount > 0) {
            for (int s = 0; s < shotCount; ++s) {
                if (presented == shotFrames[s]) {
                    char path[512];
                    snprintf(path, sizeof(path), "%s_%d.raw", shotPrefix, presented);
                    DumpCapture(path, presented);
                }
            }
            if (presented >= shotFrames[shotCount - 1]) break;
        }

        // 0x00441dfe: FMV playback. While a movie is up the message loop runs
        // the video state machine instead of the game - the same gate
        // main.cpp uses, and the reason a movie freezes the game
        // underneath it.
        if (g_bMCINotifyEnabled) {
            if (g_bMCINotifyFlag) {
                UpdateVideoPlayback();
                continue;
            }
            g_bMCINotifyFlag = TRUE;
        }

        if (main_loop() == 0) break;

        if (getenv("RE1_INPUT_DEBUG") != NULL && (presented % 30) == 0) {
            fprintf(stderr, "[IN] f=%d keyMap[11]=0x%02x kbdCurr=0x%08x kbdPrev=0x%08x "
                            "padPressed=0x%x sw=%d btnWord=0x%x\n",
                    presented, g_pMasterInputState.keyMap[11],
                    (unsigned)g_pMasterInputState.keyboardCurr,
                    (unsigned)g_pMasterInputState.keyboardPrev,
                    (unsigned)g_PlayerPadPressed, read_sidewinder_pad(),
                    (unsigned)g_button_pressed_id);
        }

        if ((int)(g_dwSystemTimer1 + 1000) < (int)now) {
            g_dwSystemTimer1 = now;
        }
        (void)lastTick;
    }

    // Write the session's settings back, exactly where the Windows build's
    // message loop calls it on exit.
    CleanupVideoConfigAndSaveAllSettings();

    MarniDX_DestroyGlobal();
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
