#include "netplay_3ds_join.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "helper_cmd.h"
#include "logic.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace hubnp3ds {

std::string join_driver_args(unsigned long pid, const std::string& status, const std::string& nick, const std::string& pw,
                             const std::string& host, int port, int timeout_s) {
    return "-AzPid " + std::to_string(pid) + " -Status \"" + status + "\" -Nick \"" + nick + "\" -Pw \"" + pw + "\" -Addr \"" + host +
           "\" -Port " + std::to_string(port) + " -TimeoutS " + std::to_string(timeout_s);
}

std::string seed_join_config(const std::string& ini, const std::string& /*nick*/, const std::string& host, int port) {
    /* Azahar 2126.2 config.cpp: group [Multiplayer], keys ip / port (+ "<key>\\default" flag). The nickname box is read-only
     * (it shows the 3DS username), so it is not seeded. */
    std::string t = ini;
    auto set = [&](const char* key, const std::string& v) {
        t = hubui::ini_set(t, "Multiplayer", std::string(key) + "\\default", "false");
        t = hubui::ini_set(t, "Multiplayer", key, v);
    };
    set("ip", host);
    set("port", std::to_string(port));
    return t;
}

#ifdef _WIN32
/* UTF-8 -> UTF-16 (the data dir can hold an accented Windows user name; the ANSI APIs and byte-widening mangle it). */
static std::wstring w8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
#else
static std::string w8(const std::string& s) { return s; }
#endif

void seed_join_config_file(const std::string& install_dir, const std::string& nick, const std::string& host, int port) {
    std::string ini = install_dir + "\\user\\config\\qt-config.ini", text;
    { std::ifstream f(std::filesystem::path(w8(ini)), std::ios::binary); std::ostringstream o; o << f.rdbuf(); text = o.str(); }
#ifdef _WIN32
    CreateDirectoryW(w8(install_dir + "\\user").c_str(), nullptr);
    CreateDirectoryW(w8(install_dir + "\\user\\config").c_str(), nullptr);
#endif
    std::ofstream f(std::filesystem::path(w8(ini)), std::ios::binary);
    f << seed_join_config(text, nick, host, port);
}

std::string join_state(const std::string& text) {
    std::istringstream is(text);
    std::string l, st;
    while (std::getline(is, l)) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (l == "joined" || l.rfind("error:", 0) == 0) st = l;
    }
    return st;
}

#ifdef _WIN32
unsigned long start_join_driver(unsigned long azahar_pid, const std::string& work_dir, const std::string& nick, const std::string& pw,
                                const std::string& host, int port, const std::wstring& desktop, std::string* status_file) {
    CreateDirectoryW(w8(work_dir).c_str(), nullptr);
    std::string status = work_dir + "\\azahar_join_" + std::to_string(azahar_pid) + ".txt";
    DeleteFileW(w8(status).c_str());
    if (status_file) *status_file = status;
    std::string args = hubhelper::run_command(rhhelper::kScriptN3dsJoin, join_driver_args(azahar_pid, status, nick, pw, host, port, 120));
    std::wstring cmd = w8(args), desk = desktop;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    if (!desk.empty()) si.lpDesktop = &desk[0];
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return pi.dwProcessId;
}
#endif

}  // namespace hubnp3ds
