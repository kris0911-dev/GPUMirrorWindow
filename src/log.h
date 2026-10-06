#pragma once

#include <cstdarg>
#include <cstdio>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

inline void logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fputs(buf, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
#ifdef _WIN32
    ::OutputDebugStringA(buf);
    ::OutputDebugStringA("\n");
#endif
}
