#include "app.h"

/* ============================ helpers ============================ */


std::wstring W(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
std::string U(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
bool file_exists(const std::string& p) {
    if (p.empty()) return false;
    DWORD a = GetFileAttributesW(W(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
std::string dir_of(const std::string& p) {
    size_t i = p.find_last_of("/\\");
    return i == std::string::npos ? std::string(".") : p.substr(0, i + 1);
}
std::string file_name(const std::string& p) {
    size_t i = p.find_last_of("/\\");
    return i == std::string::npos ? p : p.substr(i + 1);
}

/* IFileOpenDialog: folder or file picker. Returns "" on cancel. */
std::string pick_path(HWND owner, bool folder, const wchar_t* filter_name, const wchar_t* filter_spec) {
    std::string result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0);
    dlg->SetOptions(opts);
    if (!folder && filter_name) {
        COMDLG_FILTERSPEC f[1] = {{filter_name, filter_spec}};
        dlg->SetFileTypes(1, f);
    }
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                result = U(p);
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    dlg->Release();
    return result;
}

/* IFileSaveDialog. Returns "" on cancel. */
std::string pick_save_path(HWND owner, const wchar_t* default_name, const wchar_t* filter_name,
                                  const wchar_t* filter_spec, const wchar_t* default_ext) {
    std::string result;
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
    COMDLG_FILTERSPEC f[1] = {{filter_name, filter_spec}};
    dlg->SetFileTypes(1, f);
    dlg->SetDefaultExtension(default_ext);
    dlg->SetFileName(default_name);
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                result = U(p);
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    dlg->Release();
    return result;
}

bool dir_exists(const std::string& p) {
    if (p.empty()) return false;
    DWORD a = GetFileAttributesW(W(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
std::string read_text_file(const std::string& p) {
    std::ifstream f(W(p), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
void make_dirs(const std::string& p) {
    std::wstring w = W(p);
    for (size_t i = 3; i < w.size(); ++i)
        if (w[i] == L'\\' || w[i] == L'/') CreateDirectoryW(w.substr(0, i).c_str(), nullptr);
    CreateDirectoryW(w.c_str(), nullptr);
}
std::string with_slash(std::string p) {
    if (!p.empty() && p.back() != '\\' && p.back() != '/') p += '\\';
    return p;
}
/* Look for <rel> under exe_dir, then under up to 6 parent directories (dev builds: Hub\deps\...). */
std::string find_upward(const std::string& rel) {
    std::string base = hub::exe_dir();
    for (int i = 0; i < 7; ++i) {
        std::string c = base + rel;
        if (GetFileAttributesW(W(c).c_str()) != INVALID_FILE_ATTRIBUTES) return c;
        std::string t = base;
        while (!t.empty() && (t.back() == '\\' || t.back() == '/')) t.pop_back();
        size_t k = t.find_last_of("/\\");
        if (k == std::string::npos) break;
        base = t.substr(0, k + 1);
    }
    return "";
}
std::string env_var(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return (n > 0 && n < MAX_PATH * 2) ? U(buf) : "";
}
/* Number of files below dir (recursive) and their total size. */
void count_files(const std::string& dir, int* files, uint64_t* bytes) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(W(with_slash(dir) + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string n = U(fd.cFileName);
        if (n == "." || n == "..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) count_files(with_slash(dir) + n, files, bytes);
        else { ++*files; *bytes += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
void remove_tree(const std::string& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(W(with_slash(dir) + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string n = U(fd.cFileName);
        if (n == "." || n == "..") continue;
        std::string p = with_slash(dir) + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { remove_tree(p); RemoveDirectoryW(W(p).c_str()); }
        else DeleteFileW(W(p).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
std::string fmt_time(uint64_t unix_s) {
    if (!unix_s) return "never";
    time_t t = (time_t)unix_s;
    tm lt{};
    localtime_s(&lt, &t);
    time_t now = time(nullptr);
    tm nt{};
    localtime_s(&nt, &now);
    char b[48];
    if (lt.tm_year == nt.tm_year && lt.tm_yday == nt.tm_yday) std::strftime(b, sizeof b, "Today %H:%M", &lt);
    else std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &lt);
    return b;
}
uint64_t unix_now() { return (uint64_t)time(nullptr); }
/* Short relative form for tiles: "Today 08:29", "Yesterday", "3 days ago", else the date. */
std::string fmt_ago(uint64_t unix_s) {
    if (!unix_s) return "never";
    time_t t = (time_t)unix_s, now = time(nullptr);
    tm lt{}, nt{};
    localtime_s(&lt, &t);
    localtime_s(&nt, &now);
    int days = (int)((now - t) / 86400);
    if (lt.tm_year == nt.tm_year && lt.tm_yday == nt.tm_yday) return fmt_time(unix_s);
    if (days <= 1) return "Yesterday";
    if (days < 14) return std::to_string(days) + " days ago";
    char b[32];
    std::strftime(b, sizeof b, "%b %d, %Y", &lt);
    return b;
}

/* ============================ drawing helpers (palette, widgets, icons live in ui_kit) ============================ */



/* Per-game colour pair (text/gradients only, no art). */
void game_colors(const std::string& id, ImU32* a, ImU32* b) {
    if (id == "smash64") { *a = IM_COL32(214, 48, 49, 255); *b = IM_COL32(250, 150, 40, 255); }
    else if (id == "mp1") { *a = IM_COL32(30, 70, 200, 255); *b = IM_COL32(70, 170, 245, 255); }
    else if (id == "mp2") { *a = IM_COL32(110, 45, 190, 255); *b = IM_COL32(205, 95, 225, 255); }
    else if (id == "mp3") { *a = IM_COL32(20, 130, 70, 255); *b = IM_COL32(125, 205, 80, 255); }
    else if (id == "remix") { *a = IM_COL32(0, 130, 150, 255); *b = IM_COL32(60, 220, 230, 255); }
    else { *a = IM_COL32(70, 78, 100, 255); *b = IM_COL32(120, 130, 160, 255); }
}
const char* short_name(const std::string& id) {
    if (id == "smash64") return "Smash";
    if (id == "mp1") return "MP1";
    if (id == "mp2") return "MP2";
    if (id == "mp3") return "MP3";
    if (id == "remix") return "Remix";
    return "?";
}
/* Per-system default scheme. */
void system_colors(hub::System s, ImU32* a, ImU32* b) {
    switch (s) {
        case hub::System::N64: *a = IM_COL32(200, 50, 45, 255); *b = IM_COL32(245, 140, 50, 255); break;
        case hub::System::GC: *a = IM_COL32(70, 50, 170, 255); *b = IM_COL32(160, 95, 220, 255); break;
        case hub::System::GBA: *a = IM_COL32(40, 70, 190, 255); *b = IM_COL32(130, 100, 235, 255); break;
        case hub::System::GBC: *a = IM_COL32(0, 120, 125, 255); *b = IM_COL32(60, 200, 180, 255); break;
        case hub::System::GB: *a = IM_COL32(85, 100, 40, 255); *b = IM_COL32(150, 175, 70, 255); break;
        case hub::System::N3DS: *a = IM_COL32(170, 25, 55, 255); *b = IM_COL32(235, 90, 105, 255); break;
        case hub::System::WiiU: *a = IM_COL32(0, 100, 170, 255); *b = IM_COL32(60, 190, 230, 255); break;
        case hub::System::NDS: *a = IM_COL32(184, 108, 16, 255); *b = IM_COL32(242, 190, 76, 255); break;
        default: *a = IM_COL32(70, 78, 100, 255); *b = IM_COL32(120, 130, 160, 255); break;
    }
}
const char* sys_badge(hub::System s) {
    switch (s) {
        case hub::System::N64: return "N64";
        case hub::System::GC: return "GAMECUBE";
        case hub::System::GBA: return "GBA";
        case hub::System::GBC: return "GBC";
        case hub::System::GB: return "GAME BOY";
        case hub::System::N3DS: return "3DS";
        case hub::System::WiiU: return "WII U";
        case hub::System::NDS: return "NINTENDO DS";
        default: return "?";
    }
}
void entry_colors(const hub::GameEntry& e, ImU32* a, ImU32* b) {
    if (e.system == hub::System::N64 && !e.catalog_id.empty() && hub::find_game(e.catalog_id)) game_colors(e.catalog_id, a, b);
    else system_colors(e.system, a, b);
}

