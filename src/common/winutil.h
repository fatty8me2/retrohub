#pragma once
/* Windows helpers shared by the hub, the runners and the libs: UTF-8 <-> UTF-16, the exe's location,
 * command-line quoting and whole-file read/write. All strings that cross APIs are UTF-8; no exceptions escape. */

#include <cstdint>
#include <string>
#include <vector>

namespace winutil {

/* ---- UTF-8 <-> UTF-16 (invalid UTF-8 bytes become U+FFFD, as MultiByteToWideChar does) ---- */
std::wstring to_wide(const std::string& utf8);
std::string to_utf8(const std::wstring& w);
std::string to_utf8(const wchar_t* w);   // nullptr -> ""

/* ---- this executable (GetModuleFileNameW with a growing buffer: long and non-ASCII paths are never truncated) ---- */
std::wstring exe_path_w();               // full path, "" on failure
std::string exe_path();                  // UTF-8
std::wstring exe_dir_w();                // directory containing the exe, with a trailing '\'
std::string exe_dir();                   // UTF-8, with a trailing '\' (cached)

/* ---- command lines ---- */
/* Quote one argument so CommandLineToArgvW / the MSVC CRT parse it back to exactly `arg`
 * (always quoted; embedded quotes and backslash runs before quotes/the end are escaped). */
std::wstring quote_arg(const std::wstring& arg);
std::string quote_arg(const std::string& arg);    // same, for UTF-8 command lines

/* ---- files (paths are UTF-8; binary mode) ---- */
bool read_file(const std::string& path_utf8, std::string* out);
bool read_file(const std::string& path_utf8, std::vector<uint8_t>* out);
bool read_file_w(const std::wstring& path, std::string* out);
/* Plain write (truncates). */
bool write_file(const std::string& path_utf8, const std::string& data);
bool write_file(const std::string& path_utf8, const void* data, size_t len);
/* Write to "<path>.tmp" then replace `path` (a crash never leaves a half-written file). */
bool write_file_atomic(const std::string& path_utf8, const std::string& data);

}  // namespace winutil
