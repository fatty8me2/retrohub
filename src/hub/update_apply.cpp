/* Native update applier. See update_apply.h. */
#include "winutil.h"
#include "json.h"
#include "update_apply.h"
#include "popup_guard.h"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <ctime>
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
std::wstring w16(const std::string& s) { return winutil::to_wide(s); }

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

std::wstring error_text(const wchar_t* what, const std::wstring& rel, DWORD err) {
    wchar_t b[64]; std::swprintf(b, 64, L" (error %lu)", err);
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

/* Same size and same last-write time = the same file for our purposes (CopyFileW keeps the time, so a backup copy matches its original). */
bool same_stamp(const std::wstring& a, const std::wstring& b) {
    WIN32_FILE_ATTRIBUTE_DATA x{}, y{};
    if (!GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &x) || !GetFileAttributesExW(b.c_str(), GetFileExInfoStandard, &y)) return false;
    return x.nFileSizeHigh == y.nFileSizeHigh && x.nFileSizeLow == y.nFileSizeLow &&
           x.ftLastWriteTime.dwHighDateTime == y.ftLastWriteTime.dwHighDateTime && x.ftLastWriteTime.dwLowDateTime == y.ftLastWriteTime.dwLowDateTime;
}

/* Byte-for-byte equality (recovery only: it runs once in a blue moon and must never skip a file that really differs). */
bool identical_files(const std::wstring& a, const std::wstring& b) {
    WIN32_FILE_ATTRIBUTE_DATA x{}, y{};
    if (!GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &x) || !GetFileAttributesExW(b.c_str(), GetFileExInfoStandard, &y)) return false;
    if (x.nFileSizeHigh != y.nFileSizeHigh || x.nFileSizeLow != y.nFileSizeLow) return false;
    HANDLE ha = CreateFileW(a.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    HANDLE hb = CreateFileW(b.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    bool same = ha != INVALID_HANDLE_VALUE && hb != INVALID_HANDLE_VALUE;
    if (same) {
        std::vector<char> ba(1 << 16), bb(1 << 16);
        for (;;) {
            DWORD na = 0, nb = 0;
            if (!ReadFile(ha, ba.data(), (DWORD)ba.size(), &na, nullptr) || !ReadFile(hb, bb.data(), (DWORD)bb.size(), &nb, nullptr) || na != nb) { same = false; break; }
            if (na == 0) break;
            if (std::memcmp(ba.data(), bb.data(), na) != 0) { same = false; break; }
        }
    }
    if (ha != INVALID_HANDLE_VALUE) CloseHandle(ha);
    if (hb != INVALID_HANDLE_VALUE) CloseHandle(hb);
    return same;
}

std::wstring temp_dir() {
    wchar_t tmp[MAX_PATH + 1]; DWORD n = GetTempPathW(MAX_PATH, tmp);
    return n ? norm(std::wstring(tmp, n)) : std::wstring();
}

long long unix_now() { return (long long)std::time(nullptr); }

std::string strip_v(std::string v) {
    while (!v.empty() && (v[0] == 'v' || v[0] == 'V' || v[0] == ' ')) v.erase(0, 1);
    while (!v.empty() && v.back() == ' ') v.pop_back();
    return v;
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

bool is_smash_classic_file(const std::wstring& relIn) {
    std::vector<std::wstring> parts = split(lower(norm(relIn)));
    if (parts.empty()) return false;
    const std::wstring& name = parts.back();
    if (parts.size() > 1) {
        const std::wstring& d = parts[0];
        if (d == L"yamls" || d == L"assets" || d == L".tcc") return true;
        if (d == L"licenses")
            return name == L"libultraship-license.txt" || name == L"torch-license.txt" || name == L"readme.txt";
        return false;
    }
    static const wchar_t* kRoot[] = {L"battleship.exe", L"battleship-jp.exe", L"battleship.pdb", L"battleship-jp.pdb", L"torch.exe",
                                     L"f3d.o2r", L"config.yml", L"gamecontrollerdb.txt", L"battleship.ico", L"battleship-jp.ico",
                                     L"license.txt"};
    for (auto k : kRoot) if (name == k) return true;
    return name.size() > 4 && name.compare(name.size() - 4, 4, L".o2r") == 0;
}

std::vector<std::wstring> list_staged(const std::wstring& staged, bool hub_only) {
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
        if (hub_only && is_smash_classic_file(rel)) continue;
        out.push_back(rel);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string plain_reason(unsigned long e) {
    switch (e) {
        case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: return "a file was in use by another program";
        case ERROR_ACCESS_DENIED: return "Windows denied access to a file (antivirus or permissions)";
        case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return "the disk is full";
        case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_BAD_PATHNAME: case ERROR_FILENAME_EXCED_RANGE:
            return "a file or folder could not be found";
        case 0: return "the downloaded update was incomplete";
        default: return "a file could not be replaced";
    }
}

/* ---------------- processes from the install folder ---------------- */

std::vector<Running> processes_under(const std::wstring& dir, const std::vector<unsigned long>& except) {
    std::vector<Running> out;
    std::wstring d = norm(dir);
    wchar_t lbuf[4096];
    DWORD ln = GetLongPathNameW(d.c_str(), lbuf, 4096);
    std::wstring dl = (ln && ln < 4096) ? norm(std::wstring(lbuf, ln)) : d;   // the image path is the long form; the caller's may be 8.3
    const DWORD self = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == self || pe.th32ProcessID == 0) continue;
        if (std::find(except.begin(), except.end(), (unsigned long)pe.th32ProcessID) != except.end()) continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!p) continue;
        wchar_t buf[4096]; DWORD n = 4096;
        if (QueryFullProcessImageNameW(p, 0, buf, &n)) {
            std::wstring img(buf, n);
            if (path_is_under(d, img) || path_is_under(dl, img)) out.push_back({(unsigned long)pe.th32ProcessID, img});
        }
        CloseHandle(p);
    }
    CloseHandle(snap);
    return out;
}

WaitOutcome wait_for_old_processes(const Options& o, std::wstring* blocker) {
    /* 1. The old RetroHub asked for this update and is shutting down. It gets a generous grace period (its shutdown joins network
     *    threads); only if it is still alive after that is IT ended, because it is the one process here that is not a game. */
    if (o.pid) {
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, (DWORD)o.pid);
        if (h) {
            if (WaitForSingleObject(h, (DWORD)(o.hub_wait_secs * 1000u)) == WAIT_TIMEOUT) {
                /* Never ended: the hub may own a game through a kill-on-close job object. Give up instead; nothing is changed. */
                alog(L"the old RetroHub is still running after " + std::to_wstring(o.hub_wait_secs) + L" s; not touching it");
                CloseHandle(h);
                if (blocker) *blocker = L"RetroHub.exe";
                return WaitOutcome::TimedOut;
            }
            CloseHandle(h);
        }
    }
    /* 2. Anything else still running from the install folder (n64run, retrorun, BattleShip, an emulator it launched) keeps files locked.
     *    It is somebody's game: wait for it, never terminate it. After a long wait give up and change nothing. */
    const std::wstring install = norm(o.install);
    const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)o.wait_games_secs * 1000ull;
    bool logged = false;
    for (;;) {
        std::vector<Running> running = processes_under(install, {o.pid});
        if (running.empty()) return WaitOutcome::Clear;
        if (!logged) {
            logged = true;
            alog(L"waiting for " + running[0].image + L" (pid " + std::to_wstring(running[0].pid) + L") to close; it is not touched");
        }
        if (GetTickCount64() >= deadline) {
            if (blocker) *blocker = running[0].image;
            return WaitOutcome::TimedOut;
        }
        Sleep(500);
    }
}

/* ---------------- status note ---------------- */

/* The curated short form of a plain_reason() for the bell (the long one stays in the status file). Unknown text is cut at 24 characters. */
static std::string short_reason(const std::string& r) {
    if (r == plain_reason(ERROR_SHARING_VIOLATION)) return "a file was in use";
    if (r == plain_reason(ERROR_ACCESS_DENIED)) return "access was denied";
    if (r == plain_reason(ERROR_DISK_FULL)) return "the disk is full";
    if (r == plain_reason(ERROR_FILE_NOT_FOUND)) return "a file went missing";
    if (r == plain_reason(0)) return "download incomplete";
    if (r == plain_reason(0xFFFFFFFFul)) return "a file was not replaced";
    return r.size() <= 24 ? r : r.substr(0, 21) + "...";
}

std::wstring default_status_path(const std::wstring& install) { return norm(install) + L"\\userdata\\update_status.json"; }

bool write_status(const std::wstring& path, const Status& s) {
    if (path.empty()) return false;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    json::Value o = json::make_object();
    o.o.push_back({"v", json::make_int(1)});
    o.o.push_back({"result", json::make_string(s.result)});
    o.o.push_back({"version", json::make_string(s.version)});
    o.o.push_back({"reason", json::make_string(s.reason)});
    o.o.push_back({"detail", json::make_string(s.detail)});
    o.o.push_back({"backup", json::make_string(s.backup)});
    o.o.push_back({"at", json::make_int(s.at ? s.at : unix_now())});
    return winutil::write_file_atomic(u8(path), json::stringify(o) + "\n");
}

bool read_status(const std::wstring& path, Status* s) {
    std::string text;
    if (!winutil::read_file_w(path, &text)) return false;
    json::Value v;
    if (!json::parse(text, &v) || !v.is_object()) return false;
    Status r;
    r.result = v.str("result"); r.version = v.str("version"); r.reason = v.str("reason");
    r.detail = v.str("detail"); r.backup = v.str("backup"); r.at = v.i64("at");
    if (r.result.empty()) return false;
    if (s) *s = r;
    return true;
}

/* The bell cuts a headline off at about 42 characters of bold type, so every line here stays under 40 (the version and the technical
 * detail are in the status file and updater.log). */
std::string status_line(const Status& s) {
    if (s.result == "postponed") return "Update postponed: a game was running";
    if (s.result == "rollback_failed") return "Update failed; some files not restored";
    if (s.result == "interrupted") return "Earlier update interrupted; run Setup";
    if (s.result == "failed") return "Update failed: " + (s.reason.empty() ? std::string("a file was not replaced") : short_reason(s.reason));
    return "The last update didn't finish";
}

bool take_status(const std::wstring& path, Status* s) {
    if (!read_status(path, s)) return false;
    DeleteFileW(path.c_str());
    return true;
}

/* ---------------- manifest of one apply (lets startup recovery tell whose backup a leftover dir is) ---------------- */

namespace {

struct Manifest {
    std::string install, from, to;
    unsigned long pid = 0;
    long long started = 0;
    bool reported = false;
};

bool write_manifest(const fs::path& file, const Manifest& m) {
    json::Value o = json::make_object();
    o.o.push_back({"v", json::make_int(1)});
    o.o.push_back({"install", json::make_string(m.install)});
    o.o.push_back({"from", json::make_string(m.from)});
    o.o.push_back({"to", json::make_string(m.to)});
    o.o.push_back({"pid", json::make_int((int64_t)m.pid)});
    o.o.push_back({"started", json::make_int(m.started)});
    o.o.push_back({"reported", json::make_bool(m.reported)});
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    return winutil::write_file_atomic(u8(file.wstring()), json::stringify(o) + "\n");
}

bool read_manifest(const fs::path& file, Manifest* m) {
    std::string text;
    if (!winutil::read_file_w(file.wstring(), &text)) return false;
    json::Value v;
    if (!json::parse(text, &v) || !v.is_object() || v.str("install").empty()) return false;
    m->install = v.str("install"); m->from = v.str("from"); m->to = v.str("to");
    m->pid = (unsigned long)v.i64("pid"); m->started = v.i64("started"); m->reported = v.boolean("reported");
    return true;
}

/* Is the applier that wrote this manifest still running? (the dir name carries its pid; its image is "...\RetroHub-apply-<hub pid>\RetroHub.exe") */
bool applier_alive(unsigned long pid) {
    if (!pid) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!h) return false;
    bool alive = false;
    if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) {
        wchar_t buf[4096]; DWORD n = 4096;
        alive = QueryFullProcessImageNameW(h, 0, buf, &n) && lower(std::wstring(buf, n)).find(L"retrohub-apply-") != std::wstring::npos;
    }
    CloseHandle(h);
    return alive;
}

} // namespace

Result apply_files(const Options& o) {
    Result r;
    std::wstring staged = norm(o.staged), install = norm(o.install);
    if (staged.empty() || install.empty()) { r.error = L"missing directories"; return r; }
    if (!file_exists(staged + L"\\RetroHub.exe") || (!o.hub_only && !file_exists(staged + L"\\BattleShip.exe"))) {
        r.error = o.hub_only ? L"the update is missing RetroHub.exe" : L"the update is missing RetroHub.exe or BattleShip.exe";
        return r;
    }
    std::vector<std::wstring> files = list_staged(staged, o.hub_only);
    std::wstring backup = o.backup.empty() ? install + L"\\updates\\backup" : norm(o.backup);
    std::error_code ec;
    fs::create_directories(fs::path(backup), ec);

    std::vector<std::wstring> installed;
    std::set<std::wstring> backed;
    std::wstring fail, failed_rel;   // failed_rel: the file whose copy failed (its restore is skipped when the copy never wrote a byte)
    for (const std::wstring& rel : files) {
        std::wstring dest = install + L"\\" + rel, src = staged + L"\\" + rel, saved = backup + L"\\" + rel;
        if (file_exists(dest)) {
            if (!make_parent(saved) || !CopyFileW(dest.c_str(), saved.c_str(), FALSE)) {
                r.err = GetLastError();
                fail = error_text(L"could not back up", rel, r.err);
                break;
            }
            backed.insert(rel);
        }
        if (!make_parent(dest)) { r.err = GetLastError(); fail = error_text(L"could not create folder for", rel, r.err); break; }
        /* Record before copying: a failed CopyFileW can leave dest truncated, and rollback must
         * still restore it from the backup (or delete it if it was new). */
        installed.push_back(rel);
        bool copied = o.copy_hook ? o.copy_hook(src, dest) : copy_retry(src, dest, o.retries, o.retry_ms);
        if (!copied) {
            r.err = GetLastError();
            fail = error_text(L"could not replace", rel, r.err);
            failed_rel = rel;
            break;
        }
        r.copied++;
    }
    if (!fail.empty()) {
        alog(L"FAILED: " + fail + L"; rolling back " + std::to_wstring(installed.size()) + L" file(s)");
        for (const std::wstring& rel : installed) {
            std::wstring dest = install + L"\\" + rel;
            if (backed.count(rel)) {
                /* The copy that failed usually failed because the file is locked: it never wrote a byte, so the original is still there
                 * (same size, same time as its backup) and there is nothing to put back. A truncated or half-written one differs. */
                if (rel == failed_rel && same_stamp(backup + L"\\" + rel, dest)) continue;
                if (!copy_retry(backup + L"\\" + rel, dest, o.restore_retries, o.restore_retry_ms)) {
                    alog(L"rollback could not restore " + rel);
                    r.unrestored.push_back(rel);
                }
            } else {
                SetFileAttributesW(dest.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (!DeleteFileW(dest.c_str()) && file_exists(dest)) alog(L"rollback could not remove " + rel + L" (a new file, nothing to restore)");
            }
        }
        r.error = fail;
        r.copied = 0;
        if (r.unrestored.empty()) {
            fs::remove_all(fs::path(backup), ec);
            r.rolled_back = true;
        } else {
            /* A restore failed: the backup holds the ONLY good copy of those files. Keep it; startup recovery repairs from it. */
            r.rolled_back = false;
            r.backup_kept = true;
            r.backup_dir = backup;
            alog(L"BACKUP KEPT in " + backup + L": " + std::to_wstring(r.unrestored.size()) + L" file(s) could not be put back");
        }
        return r;
    }
    fs::remove_all(fs::path(backup), ec);
    r.ok = true;
    return r;
}

/* ---------------- startup recovery ---------------- */

RecoverReport recover_interrupted(const std::wstring& installIn, const std::string& current_version, const std::wstring& tmp_root_in) {
    RecoverReport rep;
    const std::wstring install = norm(installIn);
    if (install.empty()) return rep;
    if (g_log_path.empty()) g_log_path = install + L"\\userdata\\logs\\updater.log";
    const std::wstring tmp = tmp_root_in.empty() ? temp_dir() : norm(tmp_root_in);
    if (tmp.empty()) return rep;
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (fs::directory_iterator it(fs::path(tmp), fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2) && it->path().filename().wstring().rfind(L"RetroHub-update-", 0) == 0) dirs.push_back(it->path());
    }
    for (const fs::path& dir : dirs) {
        ++rep.found;
        Manifest m;
        if (!read_manifest(dir / L"manifest.json", &m)) { ++rep.other; continue; }   // no manifest: cannot tell whose it is; never touched
        if (lower(norm(w16(m.install))) != lower(install)) { ++rep.other; continue; }
        if (applier_alive(m.pid)) { ++rep.active; continue; }
        const fs::path backup = dir / L"backup";
        if (!fs::exists(backup, ec)) { fs::remove_all(dir, ec); ++rep.cleaned; continue; }
        const std::string cur = strip_v(current_version), from = strip_v(m.from), to = strip_v(m.to);
        if (from.empty() || cur.empty() || from != cur) {
            /* The install is no longer on the version this backup came from (an update or reinstall landed since, or the new RetroHub.exe
             * was already in place when the apply stopped): putting old files back would downgrade or mix versions. Never restore. */
            ++rep.stale;
            alog(L"recovery: leftover " + dir.wstring() + L" is for another version (backup from " + w16(m.from) + L", running " + w16(current_version) + L"); not restoring");
            if (!to.empty() && to == cur && !m.reported) {
                Status st; st.result = "interrupted"; st.version = m.to;
                rep.notes.push_back(status_line(st));
            }
            fs::remove_all(dir, ec);
            continue;
        }
        int restored = 0, failed = 0;
        const std::wstring broot = norm(backup.wstring());
        for (fs::recursive_directory_iterator bi(backup, fs::directory_options::skip_permission_denied, ec), bend; !ec && bi != bend; bi.increment(ec)) {
            std::error_code e2;
            if (!bi->is_regular_file(e2)) continue;
            std::wstring full = bi->path().wstring();
            if (full.size() <= broot.size() + 1) continue;
            std::wstring rel = full.substr(broot.size() + 1);
            if (!is_safe_rel(rel) || is_excluded(rel)) continue;
            const std::wstring dest = install + L"\\" + rel;
            if (file_exists(dest) && identical_files(full, dest)) continue;   // already the original
            make_parent(dest);
            if (copy_retry(full, dest, 3, 300)) { ++restored; alog(L"recovery: restored " + rel); }
            else { ++failed; alog(L"recovery: could not restore " + rel); }
        }
        if (failed == 0) {
            fs::remove_all(dir, ec);
            if (restored > 0) {
                ++rep.repaired; rep.restored_files += restored;
                rep.notes.push_back("Repaired an interrupted update");   // the file count is in updater.log
            } else ++rep.cleaned;
        } else {
            ++rep.partial;
            if (!m.reported) {
                m.reported = true;
                write_manifest(dir / L"manifest.json", m);
                rep.notes.push_back("Update repair incomplete (backup kept)");   // which files: updater.log
            }
        }
    }
    return rep;
}

namespace {

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

void relaunch(const Options& o, bool force_quiet) {
    std::wstring exe = norm(o.install) + L"\\RetroHub.exe";
    std::wstring cmd = L"\"" + exe + L"\"", dir = norm(o.install);
    {   /* Only when a game is actually active: the hub told us (--game-active: any session it knew of when the update started), we just
         * waited for a game ourselves (force_quiet), or the OS reports a full-screen / presentation / busy app now. Then RetroHub
         * reopens minimised without activation. A normal update with a browser open reopens it normally. */
        QUERY_USER_NOTIFICATION_STATE qs = QUNS_ACCEPTS_NOTIFICATIONS;
        const bool os_busy = SUCCEEDED(SHQueryUserNotificationState(&qs)) &&
                             (qs == QUNS_BUSY || qs == QUNS_RUNNING_D3D_FULL_SCREEN || qs == QUNS_PRESENTATION_MODE);
        if (hubguard::start_quiet(force_quiet, hubguard::game_active(o.game_active, os_busy))) cmd += L" --background";
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
    alog(L"start: staged=" + o.staged + L" install=" + o.install + L" version=" + o.version + (o.hub_only ? L" (RetroHub files only)" : L""));
    const std::wstring status_path = o.status_path.empty() ? default_status_path(install) : o.status_path;

    /* A game or emulator still running from the install folder is never closed to make room for an update. */
    std::wstring blocker;
    if (wait_for_old_processes(o, &blocker) == WaitOutcome::TimedOut) {
        alog(L"POSTPONED: " + blocker + L" is still running after " + std::to_wstring(o.wait_games_secs) + L" s; nothing was changed and it was not touched");
        Status st; st.result = "postponed"; st.version = u8(o.version); st.reason = "a game was still running"; st.detail = u8(blocker); st.at = unix_now();
        write_status(status_path, st);
        if (o.relaunch) relaunch(o, /*force_quiet=*/true);
        return 3;
    }

    Options opt = o;
    const bool gen_backup = opt.backup.empty();
    fs::path bk_parent;
    if (gen_backup) {
        const std::wstring base = o.tmp_root.empty() ? (temp_dir().empty() ? install + L"\\updates" : temp_dir()) : norm(o.tmp_root);
        bk_parent = fs::path(base) / (L"RetroHub-update-" + std::to_wstring(GetCurrentProcessId()));
        opt.backup = (bk_parent / L"backup").wstring();
        Manifest m;
        m.install = u8(install); m.from = u8(o.from_version); m.to = u8(o.version); m.pid = GetCurrentProcessId(); m.started = unix_now();
        write_manifest(bk_parent / L"manifest.json", m);
    }
    Result r = apply_files(opt);
    /* The staged files stay unless the apply worked: a retry reuses them (updater.cpp checks updates\staged.json) and needs no download. */
    if (r.ok) fs::remove_all(fs::path(norm(o.staged)), ec);
    if (r.ok) {
        alog(L"applied " + std::to_wstring(r.copied) + L" file(s)");
        update_display_version(o);
        DeleteFileW(status_path.c_str());   // an older failure note no longer applies
        DeleteFileW((install + L"\\updates\\staged.json").c_str());
        if (gen_backup) fs::remove_all(bk_parent, ec);
    } else {
        alog(L"update failed: " + r.error);
        Status st; st.version = u8(o.version); st.detail = u8(r.error); st.at = unix_now();
        if (r.backup_kept) {
            st.result = "rollback_failed"; st.reason = "some files could not be put back"; st.backup = u8(r.backup_dir);
            alog(L"the backup in " + r.backup_dir + L" is kept; the next start repairs from it");
        } else {
            st.result = "failed"; st.reason = plain_reason(r.err);
            if (gen_backup) fs::remove_all(bk_parent, ec);   // complete rollback: nothing left to protect
        }
        write_status(status_path, st);
        /* No MessageBox here: this runs with the user possibly in a game, and a modal box would land on top of it. The note above is shown
         * once in the bell on the next start; the previous version is reopened (quietly, see relaunch). */
    }
    if (o.relaunch) relaunch(o, /*force_quiet=*/false);
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
            else if (a == L"--hub-only") o.hub_only = true;
            else if (a == L"--version" && more) o.version = argv[++i];
            else if (a == L"--from" && more) o.from_version = argv[++i];
            else if (a == L"--retries" && more) o.retries = std::max(1, _wtoi(argv[++i]));
            else if (a == L"--retry-ms" && more) o.retry_ms = std::max(0, _wtoi(argv[++i]));
            else if (a == L"--wait-secs" && more) o.wait_games_secs = (unsigned)std::max(0, _wtoi(argv[++i]));
            else if (a == L"--hub-wait-secs" && more) o.hub_wait_secs = (unsigned)std::max(0, _wtoi(argv[++i]));
            else if (a == L"--started-marker" && more) o.started_marker = argv[++i];
            else if (a == L"--backup-dir" && more) o.backup = argv[++i];
            else if (a == L"--tmp-root" && more) o.tmp_root = argv[++i];
        }
        rc = run(o);
    }
    LocalFree(argv);
    *exit_code = rc;
    return true;
}

} // namespace apply
} // namespace hubupdate
