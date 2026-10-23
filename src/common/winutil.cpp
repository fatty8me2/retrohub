#include "winutil.h"

#include <windows.h>

#include <cstdio>

namespace winutil {

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

}  // namespace winutil
