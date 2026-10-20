// config.cpp - single-instance guard and the settings-save hook (Phase 8).
//
// Settings themselves live in config.ini now and are handled by the shared
// src/system/ConfigFile.cpp, which both builds use (see ConfigFile.h). What is
// left here is the Linux half of the platform services:
//   - plat_single_instance_check: a lock file, replacing the Windows mutex
//   - CleanupVideoConfigAndSaveAllSettings: the exit hook that writes the file
#include "../platform.h"

#include "../../Globals.h"
#include "../../system/ConfigFile.h"

#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <unistd.h>

namespace {
int s_lockFd = -1;
}

// ---------------------------------------------------------------------------
// CleanupVideoConfigAndSaveAllSettings (0x00497ea0)
//
// The shared call sites (main loop exit, window close) land here on Linux.
// Windows also tears the Direct3D object down; the GL backend is released by
// main.cpp's own teardown, so this is just the settings write.
// ---------------------------------------------------------------------------
void CleanupVideoConfigAndSaveAllSettings(void)
{
    if (g_bHasFinalizedSettings) {
        return;
    }
    g_bHasFinalizedSettings = TRUE;
    ConfigFile_Save();
}

// ---------------------------------------------------------------------------
// plat_single_instance_check
//
// A lock file beside the game executable. Two details matter:
//
//   * The path is built from the executable's own directory, never a bare
//     relative name. A bare "re1.lock" is created in whatever directory the
//     player happened to launch from - a file manager, a home directory, a
//     terminal elsewhere - so the lock used to get stranded in random places
//     and a later launch from one of them would see it and refuse to start.
//     Anchoring it to the exe means every launch uses the same file.
//   * The lock is released by the kernel when the process exits, so a crash
//     still cannot leave it held. Only a running process holds it.
// ---------------------------------------------------------------------------
BOOL plat_single_instance_check(void)
{
    char dir[512];
    char path[600];
    if (plat_exe_dir(dir, sizeof(dir))) {
        snprintf(path, sizeof(path), "%s/re1.lock", dir);
    } else {
        snprintf(path, sizeof(path), "re1.lock");   // last resort
    }

    s_lockFd = open(path, O_RDWR | O_CREAT, 0666);
    if (s_lockFd < 0) {
        return TRUE;   // cannot create the lock: do not block the game
    }
    if (flock(s_lockFd, LOCK_EX | LOCK_NB) != 0) {
        close(s_lockFd);
        s_lockFd = -1;
        return FALSE;
    }
    return TRUE;
}
