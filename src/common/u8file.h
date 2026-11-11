#pragma once
/* UTF-8-correct file helpers for hub code that keeps paths as UTF-8 std::strings. The exe has no UTF-8 manifest, so fopen / std::ifstream
 * on such a string would read the bytes in the ANSI code page. Everything here goes through the wide APIs (durable::utf8_to_wide).
 * Header-only: link hubbase (durable_write.cpp). */

#include <cstdio>
#include <fstream>
#include <ios>
#include <string>

#include "durable_write.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace u8file {

/* fopen by UTF-8 path (mode is ASCII, e.g. "rb"). nullptr on failure. */
inline FILE* fopen_utf8(const std::string& path, const char* mode) {
    std::wstring wm;
    for (const char* m = mode; *m; ++m) wm += (wchar_t)(unsigned char)*m;
    return _wfopen(durable::utf8_to_wide(path).c_str(), wm.c_str());
}

/* Whole file, binary. false when it cannot be opened. */
inline bool slurp(const std::string& path, std::string* out) {
    FILE* f = fopen_utf8(path, "rb");
    if (!f) return false;
    char buf[65536];
    size_t n;
    out->clear();
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out->append(buf, n);
    std::fclose(f);
    return true;
}

/* Stream opens by UTF-8 path. */
inline std::ifstream open_in(const std::string& path, std::ios::openmode m = std::ios::in) {
    return std::ifstream(durable::utf8_to_wide(path), m);
}
inline std::ofstream open_out(const std::string& path, std::ios::openmode m = std::ios::out) {
    return std::ofstream(durable::utf8_to_wide(path), m);
}

/* ---- account_code.txt: the saved account code (how a player gets back into their cloud saves). ----
 * First line of the file, trailing CR / spaces trimmed. Returns false when there is no file or it is unreadable (never an error that
 * clears anything: the caller keeps whatever it had). A file found only at the legacy (ANSI-misread) location is re-saved at the
 * correct path; the legacy file is left alone. */
inline bool read_account_code(const std::string& path, std::string* code) {
    std::string text;
    const durable::ReadResult r = durable::read_file_utf8_migrating(path, &text);
    if (r != durable::ReadResult::Primary && r != durable::ReadResult::Legacy) return false;
    std::string c = text.substr(0, text.find('\n'));
    while (!c.empty() && (c.back() == '\r' || c.back() == ' ')) c.pop_back();
    if (c.empty()) return false;
    if (r == durable::ReadResult::Legacy) durable::write_file_atomic_utf8(path, c + "\n");
    *code = c;
    return true;
}
inline bool write_account_code(const std::string& path, const std::string& code) {
    return durable::write_file_atomic_utf8(path, code + "\n");
}
/* The saved code belonged to the previous account. A plain delete when the path reads the same in the ANSI code page (identical to the
 * old std::remove); otherwise an empty file at the correct path, so the read does not fall back to a stale legacy file. */
inline void clear_account_code(const std::string& path) {
    if (durable::legacy_ansi_to_wide(path) == durable::utf8_to_wide(path)) DeleteFileW(durable::utf8_to_wide(path).c_str());
    else durable::write_file_atomic_utf8(path, std::string());
}

}  // namespace u8file
