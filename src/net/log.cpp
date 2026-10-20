#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

#include <windows.h>

namespace {
std::mutex g_mutex;
std::string g_path;
}

void net_set_log_file(const char* path)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_path = path ? path : "";
}

void net_log(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 3) n = (int)sizeof(buf) - 3;
    if (n == 0 || buf[n - 1] != '\n') buf[n++] = '\n';
    buf[n] = 0;

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1100];
    snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [net] %s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);

    OutputDebugStringA(line);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_path.empty()) {
        if (FILE* f = fopen(g_path.c_str(), "ab")) {
            fputs(line, f);
            fclose(f);
        }
    }
}
