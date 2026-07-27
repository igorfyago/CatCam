// ============================================================================
// Logger.h — Simple thread-safe logging utility
// ============================================================================
#pragma once
#include <windows.h>
#include <stdio.h>

inline void Log(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    FILE* f = nullptr;
    // Log to the root of C: to ensure accessibility from Session 0 (LOCAL SERVICE)
    if (fopen_s(&f, "C:\\CatCam.log", "a") == 0)
    {
        // Wall-clock + ms: without it the 2026-07-26 TDR forensics could not
        // align 1446 DLL log lines against 11 timestamped nvlddmkm events.
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "[%02u:%02u:%02u.%03u][%lu] %s\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            GetCurrentThreadId(), buf);
        fclose(f);
    }
}
