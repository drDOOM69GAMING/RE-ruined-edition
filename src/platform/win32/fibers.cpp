// fibers.cpp - Win32 fiber wrappers for the task scheduler (platform.h).
//
// TaskScheduler.cpp's stack switching comes in three shapes: the original
// MSVC x86 naked-asm ESP switching, ucontext on POSIX, and -- for the MinGW
// cross build (tools/build_windows_mingw.sh), which cannot compile MSVC's
// __asm { } syntax -- the Win32 fiber API. Fibers are the operating system's
// own implementation of exactly that contract: a fiber runs until it switches
// away, and resuming re-enters it where it left off.
//
// The OS calls live behind the Phase 0 boundary (docs/LINUX_PORT.md), so this
// is one of the few files allowed to include <windows.h> directly. Behaviour
// mirrors the inline code the scheduler used before the move: fiber creation
// takes the default stack size (0) and every return value the scheduler
// ignored is still ignored here.
#include "../platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// Convert the calling thread into a fiber so tasks have a scheduler context
// to switch back to. Runs once, on first use.
void plat_fiber_convert_thread(void)
{
    ConvertThreadToFiber(NULL);
}

// The current thread's fiber (valid after plat_fiber_convert_thread).
void* plat_fiber_current(void)
{
    return GetCurrentFiber();
}

// Create a fiber that starts at `entry(param)` on its own stack. Returns NULL
// on failure, which the scheduler handles the same way it always has.
void* plat_fiber_create(void (CALLBACK* entry)(void*), void* param)
{
    return CreateFiber(0, entry, param);
}

// Transfer control to `fiber`; returns when some other fiber switches back.
void plat_fiber_switch(void* fiber)
{
    SwitchToFiber(fiber);
}
