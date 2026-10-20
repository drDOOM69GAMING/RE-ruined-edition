// ---------------------------------------------------------------------------
// crash_win.cpp - crash.log diagnostics for the MinGW Windows build.
//
// Counterpart of src/platform/linux/crash.cpp, which uses glibc's execinfo
// (backtrace / backtrace_symbols_fd) and therefore cannot build on Windows.
// Same contract and the same crash.log file, written through the Win32
// unhandled-exception filter instead of POSIX signals, so a standalone run
// still says where it died without a debugger attached.
//
// This is the crash path for the GL/SDL Windows build only. The MSVC build
// uses src/system/CrashLog.cpp.
// ---------------------------------------------------------------------------
#include "platform/platform.h"

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <string.h>

#define CRASH_LOG "crash.log"

namespace {

void AppendLine(const char* text)
{
    FILE* f = fopen(CRASH_LOG, "ab");
    if (f == NULL) return;
    fputs(text, f);
    fclose(f);
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
             "\n==== FATAL exception 0x%08lX at %p ====\n",
             (unsigned long)info->ExceptionRecord->ExceptionCode,
             info->ExceptionRecord->ExceptionAddress);
    AppendLine(buf);

    // Symbol names need SymInitialize; if that is unavailable the addresses
    // above are still enough to match against the build.
    HANDLE proc = GetCurrentProcess();
    if (SymInitialize(proc, NULL, TRUE)) {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);

        CONTEXT ctx = *info->ContextRecord;
        STACKFRAME64 frame = { 0 };
        frame.AddrPC.Offset = ctx.Eip;
        frame.AddrPC.Mode   = AddrModeFlat;
        frame.AddrFrame.Offset = ctx.Ebp;
        frame.AddrFrame.Mode   = AddrModeFlat;
        frame.AddrStack.Offset = ctx.Esp;
        frame.AddrStack.Mode   = AddrModeFlat;

        for (int i = 0; i < 32; i++) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_I386, proc, GetCurrentThread(),
                             &frame, &ctx, NULL, SymFunctionTableAccess64,
                             SymGetModuleBase64, NULL))
                break;
            if (frame.AddrPC.Offset == 0) break;

            char symBuf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO* sym = (SYMBOL_INFO*)symBuf;
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            DWORD64 disp = 0;
            if (SymFromAddr(proc, frame.AddrPC.Offset, &disp, sym))
                snprintf(buf, sizeof(buf), "  %-40s +0x%llX\n",
                         sym->Name, (unsigned long long)disp);
            else
                snprintf(buf, sizeof(buf), "  %p\n", (void*)frame.AddrPC.Offset);
            AppendLine(buf);
        }
        SymCleanup(proc);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

void crashlog_install(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "\n---- session start (pid %lu) ----\n",
             (unsigned long)GetCurrentProcessId());
    AppendLine(buf);
    SetUnhandledExceptionFilter(OnUnhandledException);
}

void crashlog_mark(const char* step)
{
    if (step == NULL) return;
    char buf[256];
    snprintf(buf, sizeof(buf), "[step] %s\n", step);
    AppendLine(buf);
}
