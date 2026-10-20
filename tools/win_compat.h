// ---------------------------------------------------------------------------
// win_compat.h - POSIX shims for the MinGW Windows build.
//
// The GL/SDL platform layer under src/platform/linux is shared with the Linux
// build and stays clean of Windows code. A few of its calls are POSIX-only and
// have direct Win32 equivalents. This header is force-included (-include) on
// the MinGW build only, so the shared sources compile unchanged on both.
//
//   mmap/mprotect/munmap  -> VirtualAlloc/VirtualProtect/VirtualFree
//       used by plat_alloc_guarded_stacks: the guard-page task-stack allocator.
//       VirtualAlloc's MEM_RESERVE with PAGE_NOACCESS is the same idea, and
//       VirtualProtect turns each stack slot readable/writable.
//   flock(LOCK_EX|LOCK_NB) -> CreateMutex
//       used by plat_single_instance_check. A named mutex is Windows' own
//       single-instance primitive.
//
// Only the shapes the shared sources actually use are provided.
// ---------------------------------------------------------------------------
#ifndef RE1_WIN_COMPAT_H
#define RE1_WIN_COMPAT_H

#ifdef _WIN32

#include <windows.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>

// --- mman -----------------------------------------------------------------
#ifndef PROT_NONE
#define PROT_NONE  0x0
#endif
#ifndef PROT_READ
#define PROT_READ  0x1
#endif
#ifndef PROT_WRITE
#define PROT_WRITE 0x2
#endif
#ifndef MAP_PRIVATE
#define MAP_PRIVATE   0x02
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif
#ifndef MAP_FAILED
#define MAP_FAILED ((void*)-1)
#endif

static inline void* re1_win_mmap(void* addr, size_t length, int prot,
                                 int flags, int fd, long offset)
{
    (void)addr; (void)flags; (void)fd; (void)offset;
    // Reserve the whole range inaccessible, matching PROT_NONE.
    (void)prot;
    void* mem = VirtualAlloc(NULL, length, MEM_RESERVE, PAGE_NOACCESS);
    return mem ? mem : MAP_FAILED;
}

static inline int re1_win_mprotect(void* addr, size_t len, int prot)
{
    DWORD winprot = PAGE_NOACCESS;
    if ((prot & PROT_WRITE) && (prot & PROT_READ)) winprot = PAGE_READWRITE;
    else if (prot & PROT_READ)                     winprot = PAGE_READONLY;
    else if (prot & PROT_WRITE)                    winprot = PAGE_READWRITE;
    DWORD old = 0;
    // Committing each slot as it is made usable mirrors mmap-then-mprotect.
    void* p = VirtualAlloc(addr, len, MEM_COMMIT, winprot);
    if (!p && !VirtualProtect(addr, len, winprot, &old)) return -1;
    return 0;
}

static inline int re1_win_munmap(void* addr, size_t len)
{
    (void)len;
    return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}

#define mmap(a, l, p, f, fd, off) re1_win_mmap((a), (l), (p), (f), (fd), (off))
#define mprotect(a, l, p)         re1_win_mprotect((a), (l), (p))
#define munmap(a, l)              re1_win_munmap((a), (l))

// --- flock -----------------------------------------------------------------
#ifndef LOCK_EX
#define LOCK_EX 2
#endif
#ifndef LOCK_NB
#define LOCK_NB 4
#endif

// One named mutex, held for the life of the process. The handle is a file
// descriptor elsewhere, so return a harmless positive int and keep the handle
// in a static.
static inline int re1_win_flock(int fd, int op)
{
    (void)fd; (void)op;
    static HANDLE s_mutex = NULL;
    if (s_mutex == NULL) {
        s_mutex = CreateMutexA(NULL, TRUE, "Global\\ResidentEvilRuinedEdition");
        if (s_mutex == NULL) return -1;
        // Already running: another process owns it.
        if (GetLastError() == ERROR_ALREADY_EXISTS) return -1;
    }
    return 0;
}
#define flock(fd, op) re1_win_flock((fd), (op))

#include <direct.h>

// --- readlink / mkdir ------------------------------------------------------
// plat_* helpers resolve the executable's own directory. Linux uses readlink
// on /proc/self/exe; Windows uses GetModuleFileName, which the shim below
// provides under the same name and signature.
static inline long re1_win_readlink(const char* path, char* buf, size_t bufsz)
{
    (void)path;
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)bufsz);
    if (n == 0 || n >= bufsz) return -1;
    return (long)n;
}
#define readlink(p, b, s) re1_win_readlink((p), (b), (s))

// Linux mkdir(path, mode): the mode is ignored on Windows, and one argument is
// enough there. Route the two-argument form to a one-argument helper.
static inline int re1_win_mkdir2(const char* path, int mode)
{
    (void)mode;
    return _mkdir(path);
}
#define mkdir(p, m) re1_win_mkdir2((p), (m))

#endif  // _WIN32
#endif  // RE1_WIN_COMPAT_H
