/* Native update applier. See update_apply.h. */
#include "winutil.h"
#include "update_apply.h"
#include "popup_guard.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <set>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

namespace fs = std::filesystem;

namespace hubupdate {
namespace apply {

namespace {

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)std::towlower(c);
    return s;
}
std::wstring norm(std::wstring s) {
    for (auto& c : s) if (c == L'/') c = L'\\';
    while (!s.empty() && s.back() == L'\\') s.pop_back();
    return s;
}
std::vector<std::wstring> split(const std::wstring& rel) {
    std::vector<std::wstring> out; std::wstring cur;
    for (wchar_t c : rel) {
        if (c == L'\\' || c == L'/') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string u8(const std::wstring& s) { return winutil::to_utf8(s); }

std::wstring g_log_path;
void alog(const std::wstring& msg) {
    if (g_log_path.empty()) return;
    HANDLE h = CreateFileW(g_log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t head[64];
    std::swprintf(head, 64, L"[%04d-%02d-%02d %02d:%02d:%02d] apply: ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::string line = u8(std::wstring(head) + msg) + "\r\n";
    DWORD w = 0; WriteFile(h, line.data(), (DWORD)line.size(), &w, nullptr);
    CloseHandle(h);
}

bool file_exists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool make_parent(const std::wstring& file) {
    std::error_code ec;
    fs::path parent = fs::path(file).parent_path();
    if (parent.empty()) return true;
    fs::create_directories(parent, ec);
    return !ec || fs::exists(parent);
}

std::wstring last_error_text(const wchar_t* what, const std::wstring& rel) {
    wchar_t b[64]; std::swprintf(b, 64, L" (error %lu)", GetLastError());
    return std::wstring(what) + L" " + rel + b;
}

/* Copy with retries: antivirus scans can hold a just-released file for a moment. */
bool copy_retry(const std::wstring& from, const std::wstring& to, int retries, int retry_ms) {
    for (int t = 1;; t++) {
        SetFileAttributesW(to.c_str(), FILE_ATTRIBUTE_NORMAL);   // clear read-only on an existing target
        if (CopyFileW(from.c_str(), to.c_str(), FALSE)) return true;
        if (t >= retries) return false;
        if (retry_ms > 0) Sleep((DWORD)retry_ms);
    }
}

} // namespace

bool is_excluded(const std::wstring& relIn) {
    std::wstring rel = lower(norm(relIn));
    std::vector<std::wstring> parts = split(rel);
    if (parts.empty()) return true;
    static const wchar_t* kDirs[] = {L"userdata", L"logs", L"updates", L"saves"};
    if (parts.size() > 1)
        for (auto d : kDirs) if (parts[0] == d) return true;
    const std::wstring& name = parts.back();
    if (name == L"battleship.o2r") return true;
    size_t dot = name.rfind(L'.');
    if (dot != std::wstring::npos) {
        std::wstring ext = name.substr(dot);
        static const wchar_t* kExt[] = {L".log", L".sav", L".srm", L".eep", L".fla", L".mpk", L".sra"};
        for (auto e : kExt) if (ext == e) return true;
    }
    return false;
}

bool is_safe_rel(const std::wstring& rel) {
    if (rel.empty()) return false;
    if (rel.size() >= 2 && rel[1] == L':') return false;
    if (rel[0] == L'\\' || rel[0] == L'/') return false;
    if (rel.find(L':') != std::wstring::npos) return false;   // drive letters, NTFS alternate data streams
    for (auto& p : split(rel)) if (p == L".." || p == L".") return false;
    return true;
}

bool path_is_under(const std::wstring& dir, const std::wstring& path) {
    std::wstring d = lower(norm(dir)), p = lower(norm(path));
    if (d.empty() || p.size() <= d.size()) return false;
    return p.compare(0, d.size(), d) == 0 && p[d.size()] == L'\\';
}

std::vector<std::wstring> list_staged(const std::wstring& staged) {
    std::vector<std::wstring> out;
    std::error_code ec;
    std::wstring root = norm(staged);
    for (fs::recursive_directory_iterator it(fs::path(root), fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        std::wstring full = it->path().wstring();
        if (full.size() <= root.size() + 1) continue;
        std::wstring rel = full.substr(root.size() + 1);
        for (auto& c : rel) if (c == L'/') c = L'\\';
        if (!is_safe_rel(rel) || is_excluded(rel)) continue;
        out.push_back(rel);
    }
    std::sort(out.begin(), out.end());
    return out;
}

Result apply_files(const Options& o) {
    Result r;
    std::wstring staged = norm(o.staged), install = norm(o.install);
    if (staged.empty() || install.empty()) { r.error = L"missing directories"; return r; }
    if (!file_exists(staged + L"\\RetroHub.exe") || !file_exists(staged + L"\\BattleShip.exe")) {
        r.error = L"the update is missing RetroHub.exe or BattleShip.exe";
        return r;
    }
    std::vector<std::wstring> files = list_staged(staged);
    std::wstring backup = o.backup.empty() ? install + L"\\updates\\backup" : norm(o.backup);
    std::error_code ec;
    fs::create_directories(fs::path(backup), ec);

    std::vector<std::wstring> installed;
    std::set<std::wstring> backed;
    std::wstring fail;
    for (const std::wstring& rel : files) {
        std::wstring dest = install + L"\\" + rel, src = staged + L"\\" + rel, saved = backup + L"\\" + rel;
        if (file_exists(dest)) {
            if (!make_parent(saved) || !CopyFileW(dest.c_str(), saved.c_str(), FALSE)) {
                fail = last_error_text(L"could not back up", rel);
                break;
            }
            backed.insert(rel);
        }
        if (!make_parent(dest)) { fail = last_error_text(L"could not create folder for", rel); break; }
        /* Record before copying: a failed CopyFileW can leave dest truncated, and rollback must
         * still restore it from the backup (or delete it if it was new). */
        installed.push_back(rel);
        bool copied = o.copy_hook ? o.copy_hook(src, dest) : copy_retry(src, dest, o.retries, o.retry_ms);
        if (!copied) {
            fail = last_error_text(L"could not replace", rel);
            break;
        }
        r.copied++;
    }
    if (!fail.empty()) {
        alog(L"FAILED: " + fail + L"; rolling back " + std::to_wstring(installed.size()) + L" file(s)");
        for (const std::wstring& rel : installed) {
            std::wstring dest = install + L"\\" + rel;
            if (backed.count(rel)) {
                if (!copy_retry(backup + L"\\" + rel, dest, 5, 200)) alog(L"rollback could not restore " + rel);
            } else {
                SetFileAttributesW(dest.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (!DeleteFileW(dest.c_str())) alog(L"rollback could not remove " + rel);
            }
        }
        fs::remove_all(fs::path(backup), ec);
        r.error = fail;
        r.rolled_back = true;
        r.copied = 0;
        return r;
    }
    fs::remove_all(fs::path(backup), ec);
    r.ok = true;
    return r;
}

namespace {

void wait_for_old_processes(const Options& o) {
    if (o.pid) {
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)o.pid);
        if (h) { WaitForSingleObject(h, 20000); CloseHandle(h); }
    }
    std::wstring install = norm(o.install);
    DWORD self = GetCurrentProcessId();
    /* Anything still running from the install folder (RetroHub, n64run, retrorun, BattleShip,
     * emulators it launched) keeps files locked. Give it a few seconds, then close it: the
     * player chose to update. */
    for (int i = 0; i < 40; i++) {
        std::vector<DWORD> running;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
            for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
                if (pe.th32ProcessID == self || pe.th32ProcessID == 0) continue;
                HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                if (!p) continue;
                wchar_t buf[4096]; DWORD n = 4096;
                if (QueryFullProcessImageNameW(p, 0, buf, &n) && path_is_under(install, std::wstring(buf, n)))
                    running.push_back(pe.th32ProcessID);
                CloseHandle(p);
            }
            CloseHandle(snap);
        }
        if (running.empty()) return;
        if (i >= 10)
            for (DWORD pid : running) {
                HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                if (p) { TerminateProcess(p, 1); CloseHandle(p); }
            }
        Sleep(500);
    }
}

void update_display_version(const Options& o) {
    if (o.version.empty()) return;
    /* AppId must match installer\RetroHub.iss in the BattleShip repo. */
    const wchar_t* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{B7F3C1A2-5D4E-4F6A-9C8B-3E1D2A7F5C90}_is1";
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    wchar_t loc[2048]; DWORD sz = sizeof(loc) - sizeof(wchar_t), type = 0;
    if (RegQueryValueExW(k, L"InstallLocation", nullptr, &type, (LPBYTE)loc, &sz) == ERROR_SUCCESS && type == REG_SZ) {
        loc[sz / sizeof(wchar_t)] = 0;
        if (lower(norm(loc)) == lower(norm(o.install))) {
            std::wstring v = o.version;
            while (!v.empty() && (v[0] == L'v' || v[0] == L'V')) v.erase(0, 1);
            RegSetValueExW(k, L"DisplayVersion", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
        }
    }
    RegCloseKey(k);
}

void relaunch(const Options& o) {
    std::wstring exe = norm(o.install) + L"\\RetroHub.exe";
    std::wstring cmd = L"\"" + exe + L"\"", dir = norm(o.install);
    {   /* Only when a game is actually active: the hub told us (--game-active: any session it knew of when the update started) or the OS
         * reports a full-screen / presentation / busy app now. Then RetroHub reopens minimised without activation. A normal update with a
         * browser open reopens it normally. */
        QUERY_USER_NOTIFICATION_STATE qs = QUNS_ACCEPTS_NOTIFICATIONS;
        const bool os_busy = SUCCEEDED(SHQueryUserNotificationState(&qs)) &&
                             (qs == QUNS_BUSY || qs == QUNS_RUNNING_D3D_FULL_SCREEN || qs == QUNS_PRESENTATION_MODE);
        if (hubguard::start_quiet(false, hubguard::game_active(o.game_active, os_busy))) cmd += L" --background";
    }
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring c1 = cmd, c2 = cmd;
    if (CreateProcessW(nullptr, c1.data(), nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB, nullptr, dir.c_str(), &si, &pi) ||
        CreateProcessW(nullptr, c2.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        alog(L"relaunched RetroHub");
    } else alog(L"could not relaunch RetroHub");
}

} // namespace

int run(const Options& o) {
    std::wstring install = norm(o.install);
    std::error_code ec;
    fs::create_directories(fs::path(install + L"\\userdata\\logs"), ec);
    g_log_path = install + L"\\userdata\\logs\\updater.log";
    if (!o.started_marker.empty()) {
        HANDLE h = CreateFileW(o.started_marker.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    alog(L"start: staged=" + o.staged + L" install=" + o.install + L" version=" + o.version);
    wait_for_old_processes(o);

    Options opt = o;
    bool gen_backup = opt.backup.empty();
    if (gen_backup) {
        wchar_t tmp[MAX_PATH + 1]; DWORD n = GetTempPathW(MAX_PATH, tmp);
        std::wstring base = n ? std::wstring(tmp, n) : install + L"\\updates\\";
        opt.backup = norm(base) + L"\\RetroHub-update-" + std::to_wstring(GetCurrentProcessId()) + L"\\backup";
    }
    Result r = apply_files(opt);
    /* Keep the staged files when the apply failed so a retry does not have to download again. */
    if (r.ok) fs::remove_all(fs::path(norm(o.staged)), ec);
    if (gen_backup) fs::remove_all(fs::path(opt.backup).parent_path(), ec);
    if (r.ok) {
        alog(L"applied " + std::to_wstring(r.copied) + L" file(s)");
        update_display_version(o);
        if (gen_backup) fs::remove_all(fs::path(opt.backup).parent_path(), ec);
    } else {
        alog(L"update failed: " + r.error);
        /* No MessageBox here: this runs with the user possibly in a game, and a modal box would land on top of it. The error is in the
         * apply log; the previous version is reopened (quietly, see relaunch). */
    }
    if (o.relaunch) relaunch(o);
    return r.ok ? 0 : 1;
}

bool try_apply_mode(int* exit_code) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    bool mine = argc >= 2 && wcscmp(argv[1], L"--apply-update") == 0;
    if (!mine) { LocalFree(argv); return false; }
    int rc = 2;
    if (argc >= 5) {
        Options o;
        o.staged = argv[2]; o.install = argv[3]; o.pid = wcstoul(argv[4], nullptr, 10);
        for (int i = 5; i < argc; i++) {
            std::wstring a = argv[i];
            bool more = i + 1 < argc;
            if (a == L"--no-relaunch") o.relaunch = false;
            else if (a == L"--game-active") o.game_active = true;
            else if (a == L"--version" && more) o.version = argv[++i];
            else if (a == L"--retries" && more) o.retries = std::max(1, _wtoi(argv[++i]));
            else if (a == L"--retry-ms" && more) o.retry_ms = std::max(0, _wtoi(argv[++i]));
            else if (a == L"--started-marker" && more) o.started_marker = argv[++i];
            else if (a == L"--backup-dir" && more) o.backup = argv[++i];
        }
        rc = run(o);
    }
    LocalFree(argv);
    *exit_code = rc;
    return true;
}

} // namespace apply
} // namespace hubupdate
