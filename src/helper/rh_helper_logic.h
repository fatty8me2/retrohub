#pragma once
/* rh-helper: pure rules shared by the helper exe, the hub and the tests.
 * The hub launches `rh-helper.exe run <script name> <args...>`; the helper only ever runs a script from a fixed allowlist,
 * looked up by plain file name in <its own dir>\data\helpers\. Nothing here touches the OS. */
#include <cstring>
#include <string>

namespace rhhelper {

/* The scripts that ship in data\helpers\ and may be run. Add a name here and nowhere else. */
constexpr const char* kScriptDolphinDriver = "dolphin_netplay_driver.ps1";   // GameCube NetPlay UI Automation driver
constexpr const char* kScriptN3dsJoin = "n3ds_join.ps1";                     // 3DS (Azahar) room join helper
constexpr const char* kAllowedScripts[] = {kScriptDolphinDriver, kScriptN3dsJoin};

/* A bare file name: no path separators, no drive/stream colon, no "." / ".." or control characters. */
inline bool is_plain_file_name(const std::string& n) {
    if (n.empty() || n == "." || n == "..") return false;
    for (unsigned char c : n) if (c < 0x20 || c == '\\' || c == '/' || c == ':' || c == '"' || c == '*' || c == '?' || c == '<' || c == '>' || c == '|') return false;
    return n.back() != ' ' && n.back() != '.';
}

/* Exact (case-sensitive) match against the allowlist. */
inline bool script_allowed(const std::string& n) {
    if (!is_plain_file_name(n)) return false;
    for (const char* a : kAllowedScripts) if (n == a) return true;
    return false;
}

/* "<exe dir>\data\helpers\<name>" (exe_dir ends with a separator). Empty when the name is rejected. */
inline std::wstring script_path(const std::wstring& exe_dir, const std::string& name) {
    if (!script_allowed(name)) return L"";
    std::wstring d = exe_dir;
    if (!d.empty() && d.back() != L'\\' && d.back() != L'/') d += L'\\';
    d += L"data\\helpers\\";
    for (char c : name) d += (wchar_t)(unsigned char)c;   // allowlisted names are ASCII
    return d;
}

/* Split a raw command line `<exe> run <script> <tail...>` into the script name and the untouched tail text (what the hub wrote
 * after the script name, passed to the script as is). The first three tokens follow the usual rules (a "quoted" token may hold
 * spaces; no escapes). Returns false when the line is not `run <ascii name>`; the name is NOT validated here (see script_allowed). */
inline bool parse_run(const std::wstring& line, std::string* name, std::wstring* tail) {
    size_t i = 0;
    auto skip_ws = [&] { while (i < line.size() && (line[i] == L' ' || line[i] == L'\t')) ++i; };
    auto token = [&](std::wstring* out) {
        skip_ws();
        if (i >= line.size()) return false;
        out->clear();
        if (line[i] == L'"') {
            ++i;
            while (i < line.size() && line[i] != L'"') *out += line[i++];
            if (i < line.size()) ++i;
        } else {
            while (i < line.size() && line[i] != L' ' && line[i] != L'\t') *out += line[i++];
        }
        return true;
    };
    std::wstring exe, verb, script;
    if (!token(&exe) || !token(&verb) || verb != L"run" || !token(&script) || script.empty()) return false;
    std::string n;
    for (wchar_t c : script) { if (c > 0x7e) return false; n += (char)c; }
    skip_ws();
    *name = n;
    *tail = line.substr(i);
    return true;
}

}  // namespace rhhelper
