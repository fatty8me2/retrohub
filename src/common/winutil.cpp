#include "winutil.h"

#ifdef _WIN32
#include <windows.h>
#endif
#include "plat/plat.h"

#include <cstdio>

#if !RH_WIN
#include <mach-o/dyld.h>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#endif

namespace winutil {

#if RH_WIN

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::string to_utf8(const wchar_t* w) { return w ? to_utf8(std::wstring(w)) : std::string(); }

bool getenv_utf8(const char* name, std::string* out) {
    if (out) out->clear();
    if (!name || !*name) return false;
    const std::wstring wn = to_wide(name);
    DWORD n = GetEnvironmentVariableW(wn.c_str(), nullptr, 0);   // size incl. NUL; 0 = unset (an empty value is 1)
    if (n == 0) return false;
    std::wstring v((size_t)n, L'\0');
    DWORD got = GetEnvironmentVariableW(wn.c_str(), &v[0], n);
    if (got >= n) return false;   // changed under us
    v.resize(got);
    if (out) *out = to_utf8(v);
    return true;
}

std::string getenv_utf8(const char* name) {
    std::string s;
    getenv_utf8(name, &s);
    return s;
}

std::wstring exe_path_w() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, &buf[0], (DWORD)buf.size());
        if (n == 0) return std::wstring();
        if (n < buf.size()) { buf.resize(n); return buf; }
        if (buf.size() > 65536) return std::wstring();
        buf.resize(buf.size() * 2);
    }
}

std::string exe_path() { return to_utf8(exe_path_w()); }

std::wstring exe_dir_w() {
    std::wstring p = exe_path_w();
    size_t pos = p.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return std::wstring();
    p.resize(pos + 1);
    if (p.back() != L'\\') p.back() = L'\\';
    return p;
}

std::string exe_dir() {
    static const std::string cached = to_utf8(exe_dir_w());
    return cached;
}

std::wstring quote_arg(const std::wstring& arg) {
    std::wstring o = L"\"";
    size_t bs = 0;   // pending backslashes
    for (wchar_t c : arg) {
        if (c == L'\\') { ++bs; continue; }
        if (c == L'"') { o.append(bs * 2 + 1, L'\\'); o += L'"'; }
        else { o.append(bs, L'\\'); o += c; }
        bs = 0;
    }
    o.append(bs * 2, L'\\');   // backslashes before the closing quote must be doubled
    o += L'"';
    return o;
}

std::string quote_arg(const std::string& arg) { return to_utf8(quote_arg(to_wide(arg))); }

bool read_file_w(const std::wstring& path, std::string* out) {
    out->clear();
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    char buf[1 << 16];
    size_t n;
    try {
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) out->append(buf, n);
    } catch (...) { fclose(f); out->clear(); return false; }
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) out->clear();
    return ok;
}

bool read_file(const std::string& path, std::string* out) { return read_file_w(to_wide(path), out); }

bool read_file(const std::string& path, std::vector<uint8_t>* out) {
    std::string s;
    if (!read_file(path, &s)) { out->clear(); return false; }
    out->assign(s.begin(), s.end());
    return true;
}

bool write_file(const std::string& path, const void* data, size_t len) {
    FILE* f = _wfopen(to_wide(path).c_str(), L"wb");
    if (!f) return false;
    bool ok = len == 0 || fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    return ok;
}

bool write_file(const std::string& path, const std::string& data) { return write_file(path, data.data(), data.size()); }

bool write_file_atomic(const std::string& path, const std::string& data) {
    std::wstring wp = to_wide(path), tmp = wp + L".tmp";
    if (!write_file(to_utf8(tmp), data)) { DeleteFileW(tmp.c_str()); return false; }
    if (!MoveFileExW(tmp.c_str(), wp.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

#else  // macOS: wchar_t is UTF-32, paths are UTF-8 already

/* Decode UTF-8 to code points; each invalid byte becomes U+FFFD (like MultiByteToWideChar). */
std::wstring to_wide(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    const unsigned char* p = (const unsigned char*)s.data();
    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        const unsigned c = p[i];
        int len = 0;
        uint32_t cp = 0, minv = 0;
        if (c < 0x80) { w += (wchar_t)c; ++i; continue; }
        else if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; minv = 0x80; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; minv = 0x800; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; minv = 0x10000; }
        else { w += (wchar_t)0xFFFD; ++i; continue; }
        bool ok = i + (size_t)len <= n;
        for (int k = 1; ok && k < len; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if (ok && (cp < minv || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) ok = false;
        if (!ok) { w += (wchar_t)0xFFFD; ++i; continue; }
        w += (wchar_t)cp;
        i += (size_t)len;
    }
    return w;
}

std::string to_utf8(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t wc : w) {
        uint32_t cp = (uint32_t)wc;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
        else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    }
    return s;
}

std::string to_utf8(const wchar_t* w) { return w ? to_utf8(std::wstring(w)) : std::string(); }

bool getenv_utf8(const char* name, std::string* out) {
    if (out) out->clear();
    if (!name || !*name) return false;
    const char* v = std::getenv(name);
    if (!v) return false;
    if (out) *out = v;   // already UTF-8 on macOS
    return true;
}

std::string getenv_utf8(const char* name) {
    std::string s;
    getenv_utf8(name, &s);
    return s;
}

std::string exe_path() {
    uint32_t n = 0;
    _NSGetExecutablePath(nullptr, &n);   // reports the needed size
    if (n == 0) return std::string();
    std::string buf(n, '\0');
    if (_NSGetExecutablePath(&buf[0], &n) != 0) return std::string();
    buf.resize(std::strlen(buf.c_str()));
    char* rp = realpath(buf.c_str(), nullptr);
    if (!rp) return buf;
    std::string r = rp;
    std::free(rp);
    return r;
}

std::wstring exe_path_w() { return to_wide(exe_path()); }

std::wstring exe_dir_w() { return to_wide(exe_dir()); }

std::string exe_dir() {
    static const std::string cached = [] {
        std::string p = exe_path();
        size_t pos = p.find_last_of('/');
        if (pos == std::string::npos) return std::string();
        p.resize(pos + 1);
        return p;
    }();
    return cached;
}

/* POSIX shell single-quote quoting: 'it'\''s'. */
std::string quote_arg(const std::string& arg) {
    std::string o = "'";
    for (char c : arg) {
        if (c == '\'') o += "'\\''";
        else o += c;
    }
    o += '\'';
    return o;
}

std::wstring quote_arg(const std::wstring& arg) { return to_wide(quote_arg(to_utf8(arg))); }

bool read_file(const std::string& path, std::string* out) {
    out->clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[1 << 16];
    size_t n;
    try {
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) out->append(buf, n);
    } catch (...) { fclose(f); out->clear(); return false; }
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) out->clear();
    return ok;
}

bool read_file_w(const std::wstring& path, std::string* out) { return read_file(to_utf8(path), out); }

bool read_file(const std::string& path, std::vector<uint8_t>* out) {
    std::string s;
    if (!read_file(path, &s)) { out->clear(); return false; }
    out->assign(s.begin(), s.end());
    return true;
}

bool write_file(const std::string& path, const void* data, size_t len) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = len == 0 || fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    return ok;
}

bool write_file(const std::string& path, const std::string& data) { return write_file(path, data.data(), data.size()); }

bool write_file_atomic(const std::string& path, const std::string& data) {
    const std::string tmp = path + ".tmp";
    if (!write_file(tmp, data)) { unlink(tmp.c_str()); return false; }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

#endif

}  // namespace winutil
