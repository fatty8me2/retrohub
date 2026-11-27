#pragma once
/* Temporary environment variables for one child launch. The hub sets them right before CreateProcess (the child inherits them) and must
 * put the world back afterwards: a variable the player set themselves before starting the hub gets its own value back, not a removal. */

#include "plat/plat.h"
#if RH_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdlib>
#include "winutil.h"
#endif

#include <string>
#include <utility>
#include <vector>

namespace hubenv {

struct Saved {
    std::wstring name;
    bool had = false;        // the variable existed before apply()
    std::wstring value;      // its value then
};

#if RH_WIN
inline Saved capture(const std::wstring& name) {
    Saved s;
    s.name = name;
    std::vector<wchar_t> buf(256);
    for (int tries = 0; tries < 4; ++tries) {
        SetLastError(0);
        DWORD n = GetEnvironmentVariableW(name.c_str(), buf.data(), (DWORD)buf.size());
        if (n == 0) { s.had = GetLastError() != ERROR_ENVVAR_NOT_FOUND; s.value.clear(); return s; }   // set to an empty string counts as set
        if (n < buf.size()) { s.had = true; s.value.assign(buf.data(), n); return s; }
        buf.resize((size_t)n + 1);   // too small: n is the size needed
    }
    s.had = true;
    return s;
}

#else
/* macOS: getenv / setenv / unsetenv on the UTF-8 form of each name and value (the std::wstring API is kept so launch.cpp reads the same on both). */
inline Saved capture(const std::wstring& name) {
    Saved s;
    s.name = name;
    const char* v = std::getenv(winutil::to_utf8(name).c_str());
    s.had = v != nullptr;
    if (v) s.value = winutil::to_wide(v);
    return s;
}
#endif

/* Remembers every variable's previous state, then sets the new values. */
inline std::vector<Saved> apply(const std::vector<std::pair<std::wstring, std::wstring>>& vars) {
    std::vector<Saved> saved;
    for (const auto& kv : vars) {
        saved.push_back(capture(kv.first));
#if RH_WIN
        SetEnvironmentVariableW(kv.first.c_str(), kv.second.c_str());
#else
        setenv(winutil::to_utf8(kv.first).c_str(), winutil::to_utf8(kv.second).c_str(), 1);
#endif
    }
    return saved;
}

/* Undo apply(): newest first, so a name listed twice ends with the value it had before the first apply. */
inline void restore(const std::vector<Saved>& saved) {
    for (auto it = saved.rbegin(); it != saved.rend(); ++it)
#if RH_WIN
        SetEnvironmentVariableW(it->name.c_str(), it->had ? it->value.c_str() : nullptr);
#else
    {
        const std::string n = winutil::to_utf8(it->name);
        if (it->had) setenv(n.c_str(), winutil::to_utf8(it->value).c_str(), 1);
        else unsetenv(n.c_str());
    }
#endif
}

} // namespace hubenv
