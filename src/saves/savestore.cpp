/* Save vault implementation. See savestore.h and docs/SAVES.md. */
#include "winutil.h"
#include "durable_write.h"
#include "json.h"
#include "savestore.h"
#include "games.h"
#include "library.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <set>

#include "miniz.h"

#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x00400000
#endif
#ifndef FILE_ATTRIBUTE_RECALL_ON_OPEN
#define FILE_ATTRIBUTE_RECALL_ON_OPEN 0x00040000
#endif
#ifndef IO_REPARSE_TAG_MOUNT_POINT
#define IO_REPARSE_TAG_MOUNT_POINT (0xA0000003L)
#endif
#ifndef IO_REPARSE_TAG_SYMLINK
#define IO_REPARSE_TAG_SYMLINK (0xA000000CL)
#endif

namespace hub::saves {
namespace {

typedef std::wstring ws;
typedef std::string str;

/* ---------------------------------------------------------------- strings */

ws W(const std::string& s) { return winutil::to_wide(s); }
str U(const std::wstring& s) { return winutil::to_utf8(s); }
ws lower(ws w) {
    if (!w.empty()) CharLowerBuffW(&w[0], (DWORD)w.size());
    return w;
}

/* "dir" without trailing separators, backslash style. */
ws norm_dir(const str& utf8) {
    ws w = W(utf8);
    for (auto& c : w) if (c == L'/') c = L'\\';
    while (w.size() > 3 && w.back() == L'\\') w.pop_back();
    return w;
}

/* Extended-length absolute path (\\?\C:\... or \\?\UNC\srv\share\...). */
ws lp(const ws& p0) {
    ws p = p0;
    for (auto& c : p) if (c == L'/') c = L'\\';
    if (p.rfind(L"\\\\?\\", 0) == 0 || p.rfind(L"\\\\.\\", 0) == 0) return p;
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (!n) return p;
    ws f((size_t)n, L'\0');
    n = GetFullPathNameW(p.c_str(), n, &f[0], nullptr);
    f.resize(n);
    if (f.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + f.substr(2);
    return L"\\\\?\\" + f;
}

bool path_exists(const ws& p) { return GetFileAttributesW(lp(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
bool is_dir(const ws& p) {
    DWORD a = GetFileAttributesW(lp(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool mkdirs(const ws& dir) {
    ws p = lp(dir);
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    if (is_dir(p)) return true;
    size_t pos = p.find_last_of(L'\\');
    if (pos != ws::npos && pos > 6) mkdirs(p.substr(0, pos));
    if (!CreateDirectoryW(p.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return is_dir(p);
    return true;
}
bool mkdirs_for_file(const ws& file) {
    ws p = file;
    for (auto& c : p) if (c == L'/') c = L'\\';
    size_t pos = p.find_last_of(L'\\');
    if (pos == ws::npos) return true;
    return mkdirs(p.substr(0, pos));
}

/* ------------------------------------------------------------ misc helpers */

ws sanitize_component(const str& in) {
    ws w = W(in);
    for (auto& c : w) {
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
    }
    while (!w.empty() && (w.back() == L'.' || w.back() == L' ')) w.pop_back();
    if (w.empty()) w = L"_";
    if (w == L"." || w == L"..") w = L"_";
    return w;
}

str device_name_utf8() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 2];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(buf, &n)) return U(ws(buf, n));
    return "unknown";
}
ws device_file_tag() {
    ws d = sanitize_component(device_name_utf8());
    return d;
}

ws timestamp_tag() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t b[40];
    swprintf(b, 40, L"%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

const uint64_t kEpochDiff = 116444736000000000ULL;
int64_t ft_to_unix(uint64_t ft) { return ft >= kEpochDiff ? (int64_t)((ft - kEpochDiff) / 10000000ULL) : 0; }
uint64_t unix_to_ft(int64_t t) { return (uint64_t)t * 10000000ULL + kEpochDiff; }
int64_t now_unix() { return (int64_t)time(nullptr); }

/* ------------------------------------------------------------------- SHA-256 */

BCRYPT_ALG_HANDLE sha_alg() {
    static BCRYPT_ALG_HANDLE h = [] {
        BCRYPT_ALG_HANDLE a = nullptr;
        if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) a = nullptr;
        return a;
    }();
    return h;
}

class Sha256 {
public:
    Sha256() {
        BCRYPT_ALG_HANDLE a = sha_alg();
        if (!a || BCryptCreateHash(a, &h_, nullptr, 0, nullptr, 0, 0) < 0) h_ = nullptr;
    }
    ~Sha256() { if (h_) BCryptDestroyHash(h_); }
    bool update(const void* d, size_t n) {
        return h_ && BCryptHashData(h_, (PUCHAR)d, (ULONG)n, 0) >= 0;
    }
    bool finish(str* hex) {
        unsigned char out[32];
        if (!h_ || BCryptFinishHash(h_, out, 32, 0) < 0) return false;
        static const char* d = "0123456789abcdef";
        hex->clear();
        for (int i = 0; i < 32; i++) { *hex += d[out[i] >> 4]; *hex += d[out[i] & 15]; }
        return true;
    }
private:
    BCRYPT_HASH_HANDLE h_ = nullptr;
};

str sha_bytes(const void* d, size_t n) {
    Sha256 s; str hex;
    s.update(d, n);
    s.finish(&hex);
    return hex;
}

HANDLE open_read(const ws& p) {
    return CreateFileW(lp(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}

bool hash_file(const ws& p, str* hex) {
    HANDLE h = open_read(p);
    if (h == INVALID_HANDLE_VALUE) return false;
    Sha256 s;
    std::vector<uint8_t> buf(256 * 1024);
    bool ok = true;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr)) { ok = false; break; }
        if (!got) break;
        s.update(buf.data(), got);
    }
    CloseHandle(h);
    return ok && s.finish(hex);
}

bool read_file(const ws& p, std::vector<uint8_t>* out) {
    HANDLE h = open_read(p);
    if (h == INVALID_HANDLE_VALUE) return false;
    out->clear();
    std::vector<uint8_t> buf(256 * 1024);
    bool ok = true;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr)) { ok = false; break; }
        if (!got) break;
        out->insert(out->end(), buf.begin(), buf.begin() + got);
    }
    CloseHandle(h);
    return ok;
}

bool set_mtime(const ws& p, uint64_t ft) {
    HANDLE h = CreateFileW(lp(p).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    FILETIME f; f.dwLowDateTime = (DWORD)(ft & 0xFFFFFFFFu); f.dwHighDateTime = (DWORD)(ft >> 32);
    bool ok = SetFileTime(h, nullptr, nullptr, &f) != 0;
    CloseHandle(h);
    return ok;
}

bool commit_tmp(const ws& tmp, const ws& dst) {
    SetFileAttributesW(lp(dst).c_str(), FILE_ATTRIBUTE_NORMAL);  // allow replacing read-only files
    if (MoveFileExW(lp(tmp).c_str(), lp(dst).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileW(lp(tmp).c_str());
    return false;
}

bool write_atomic(const ws& dst, const void* data, size_t n, uint64_t ft /*0 = now*/) {
    if (!mkdirs_for_file(dst)) return false;
    ws tmp = dst + L".hubtmp";
    HANDLE h = CreateFileW(lp(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const uint8_t* p = (const uint8_t*)data;
    bool ok = true;
    while (n && ok) {
        DWORD chunk = (DWORD)std::min<size_t>(n, 1u << 24), w = 0;
        ok = WriteFile(h, p, chunk, &w, nullptr) && w == chunk;
        p += chunk; n -= chunk;
    }
    if (ok) ok = FlushFileBuffers(h) != 0;
    CloseHandle(h);
    if (!ok) { DeleteFileW(lp(tmp).c_str()); return false; }
    if (ft) set_mtime(tmp, ft);
    return commit_tmp(tmp, dst);
}

bool copy_atomic(const ws& src, const ws& dst, uint64_t ft) {
    if (!mkdirs_for_file(dst)) return false;
    ws tmp = dst + L".hubtmp";
    SetFileAttributesW(lp(tmp).c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!CopyFileW(lp(src).c_str(), lp(tmp).c_str(), FALSE)) return false;
    SetFileAttributesW(lp(tmp).c_str(), FILE_ATTRIBUTE_NORMAL);
    set_mtime(tmp, ft);
    SetFileAttributesW(lp(dst).c_str(), FILE_ATTRIBUTE_NORMAL);  // allow replacing read-only files (as commit_tmp)
    if (durable::commit_replace(tmp, dst, durable::kRetryLongMs)) return true;           // flushes the copied data first, then one atomic replace
    DeleteFileW(lp(tmp).c_str());
    return false;
}

/* ---------------------------------------------------------------- scanning */

struct FE { ws rel; uint64_t size = 0, mtime = 0; DWORD attrs = 0; };
typedef std::map<ws, FE> FMap;  // key = lowercase rel

int comp_count(const ws& rel) { return (int)std::count(rel.begin(), rel.end(), L'/') + 1; }
ws comp_at(const ws& rel, int idx) {
    size_t s = 0; int i = 0;
    for (;;) {
        size_t e = rel.find(L'/', s);
        if (i == idx) return rel.substr(s, e == ws::npos ? ws::npos : e - s);
        if (e == ws::npos) return ws();
        s = e + 1; i++;
    }
}
bool is_states_path(const ws& rel) { return comp_count(rel) >= 3 && lower(comp_at(rel, 2)) == L"states"; }

/* The account secret (<vault>/_identity/, and _conflicts/_identity/ where older builds kept the losing identity of a sync conflict) stays on the PC that holds it. It
 * is never written into an export zip or a sync folder and never read back from either, so importing someone's zip (or syncing a folder they
 * share) cannot switch the player to another account, and a zip handed to a friend does not hand over the account (F6). Older builds also
 * kept the losing identity of a sync conflict as _conflicts/_identity/identity.json.<PC>.<time>: covered by the same two rules.
 * The cloud engine and the Worker already refuse it. */
bool is_identity_rel(const ws& rel) {   // rel uses '/' separators: the cloud's rule (top-level _identity) plus the loser copies of old builds
    if (lower(comp_at(rel, 0)) == L"_identity") return true;
    return comp_count(rel) >= 2 && lower(comp_at(rel, 0)) == L"_conflicts" && lower(comp_at(rel, 1)) == L"_identity";
}

EnumErrorHook g_enum_hook = nullptr;   // test seam, null (inert) unless a test sets it

str hook_dir(const ws& dir) { str s = U(dir); for (auto& c : s) if (c == '/') c = '\\'; return s; }

/* Cloud-client / OS litter that must never travel into a vault battery folder (emulators read those folders): matched on the file
 * name only, case-insensitively, on both sides of a sync. Deliberately NOT here: *.tmp, *.partial and names ending in "~". Games
 * running under Cemu, Azahar and Eden choose their own save file names inside the vault battery folder, so those could be real saves. */
bool is_junk_name(const ws& name0) {
    ws n = lower(name0);
    auto starts = [&](const wchar_t* p) { return n.rfind(p, 0) == 0; };
    auto ends = [&](const wchar_t* p) { size_t k = wcslen(p); return n.size() >= k && n.compare(n.size() - k, k, p) == 0; };
    return n == L"desktop.ini" || n == L"thumbs.db" || n == L".ds_store" || starts(L"~$") || starts(L".~lock.") ||
           n == L".dropbox" || starts(L".dropbox.") || n == L".sync" || starts(L".syncthing.") ||
           ends(L".crdownload") || ends(L".!sync");
}

/* Lists root/rel into `out`. Returns false (with *err set) when the listing is incomplete for any reason other than the folder being
 * gone or empty: an incomplete scan would make files that exist look absent, so callers must not act on it. */
/* Parts of a tree that could not be listed and are left out of this sync on BOTH sides (lowercase "<system>/<game>" or lowercase
 * top-level folder name -> text for the player). Only used by sync_folder. `vault` = the vault root. */
struct Excluded { std::map<ws, str> games, folders; ws vault; };

/* A top-level folder is a save tree (system folder) when its name is in the canonical system table (hub::system_short / console_id,
 * so a system added there is protected automatically) OR the vault itself already has a folder of that name. */
bool known_system_dir(const ws& lower_name, const Excluded* ex) {
    if (is_system_folder_name(U(lower_name))) return true;
    return ex && !ex->vault.empty() && is_dir(ex->vault + L"/" + lower_name);
}

/* With `ex` set, a listing error is only fatal where a wrong "absent" verdict could lead to an overwrite and the damage cannot be
 * confined: the root, `_conflicts`, `_account` and a system folder itself. Inside <system>/<game>/ the game is excluded; in any other
 * (foreign) top-level folder that folder is excluded with a log line. */
bool walk(const ws& root, const ws& rel, bool states, FMap& out, std::vector<str>& log, str* err = nullptr, bool junk = false, Excluded* ex = nullptr, const char* side = "") {
    ws dir = rel.empty() ? root : root + L"/" + rel;
    auto bad = [&](DWORD code) {
        str code_s = std::to_string((unsigned long)code);
        if (ex && !rel.empty()) {
            ws top = lower(comp_at(rel, 0));
            if (top != L"_conflicts" && top != L"_account") {
                if (!known_system_dir(top, ex)) {
                    ex->folders[top] = U(comp_at(rel, 0));
                    log.push_back("skip folder " + U(comp_at(rel, 0)) + ": could not be read in the " + side + " (Windows error " + code_s + "); left out of this sync");
                    return true;
                }
                if (comp_count(rel) >= 2) {
                    ws prefix = top + L"/" + lower(comp_at(rel, 1));
                    str name = U(comp_at(rel, 0) + L"/" + comp_at(rel, 1));
                    ex->games[prefix] = name;
                    log.push_back("skip game " + name + ": its folder could not be read in the " + side + " (Windows error " + code_s + "); not synced this time");
                    return true;
                }
            }
        }
        if (err) *err = "Windows could not read the folder " + U(dir) + " (error " + code_s + ")";
        return false;
    };
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((lp(dir) + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    DWORD first_err = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    if (g_enum_hook) if (DWORD inj = (DWORD)g_enum_hook(hook_dir(dir), 0)) { if (h != INVALID_HANDLE_VALUE) FindClose(h); return bad(inj); }
    if (h == INVALID_HANDLE_VALUE) {
        if (first_err == ERROR_FILE_NOT_FOUND || first_err == ERROR_PATH_NOT_FOUND || first_err == ERROR_NO_MORE_FILES) return true;   // gone or empty: normal
        return bad(first_err);
    }
    int seen = 0;
    do {
        if (seen && g_enum_hook) if (DWORD inj = (DWORD)g_enum_hook(hook_dir(dir), seen)) { FindClose(h); return bad(inj); }
        seen++;
        ws name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        ws crel = rel.empty() ? name : rel + L"/" + name;
        bool isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            (fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT || fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK)) {
            log.push_back("skip link " + U(crel));
            continue;
        }
        if (isdir) {
            if (rel.empty() && lower(name) == L".sync") continue;
            if (rel.empty() && (lower(name) == L"system volume information" || lower(name) == L"$recycle.bin")) continue;   // a USB stick's root: Windows keeps these closed, they never hold saves
            if ((rel.empty() && lower(name) == L"_identity") || (lower(rel) == L"_conflicts" && lower(name) == L"_identity")) continue;   // the account secret never travels (F6)
            if (!states && is_states_path(crel)) continue;
            if (!walk(root, crel, states, out, log, err, junk, ex, side)) { FindClose(h); return false; }
        } else {
            if (name.size() > 7 && name.compare(name.size() - 7, 7, L".hubtmp") == 0) continue;
            if (junk && is_junk_name(name)) continue;
            if (!states && is_states_path(crel)) continue;
            FE e;
            e.rel = crel;
            e.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            e.mtime = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
            e.attrs = fd.dwFileAttributes;
            out[lower(crel)] = e;
        }
    } while (FindNextFileW(h, &fd));
    DWORD last = GetLastError();
    FindClose(h);
    if (last != ERROR_NO_MORE_FILES && last != ERROR_FILE_NOT_FOUND && last != ERROR_PATH_NOT_FOUND) return bad(last);
    if (g_enum_hook) g_enum_hook(hook_dir(dir), -1);   // directory finished (the return value is ignored)
    return true;
}

/* --------------------------------------------------------- JSON (shared reader, src/common/json.h) */

typedef json::Value JV;
bool jparse(const str& text, JV* out) { return json::parse(text, out, json::ParseOptions{64, true}); }
/* A number's exact text (sizes/FILETIMEs exceed a double's 53 bits), or a string's contents. */
str jtok(const JV* v) { return v->type == JV::Number ? v->raw : v->s; }

/* ---------------------------------------------------------------- manifest */

struct ME { ws rel; uint64_t size = 0, mtime = 0; str sha; };
typedef std::map<ws, ME> Manifest;  // key = lowercase rel

ws manifest_path(const ws& vault, const ws& sync_root) {
    str r = U(lower(sync_root));
    str name;
    for (char c : r) name += (isalnum((unsigned char)c) || c == '.' || c == '-') ? c : '_';
    if (name.size() > 60) name = name.substr(name.size() - 60);
    str h = sha_bytes(r.data(), r.size()).substr(0, 8);
    return vault + L"\\.sync\\" + W(name) + L"-" + W(h) + L".json";
}

void load_manifest(const ws& path, Manifest& m) {
    std::vector<uint8_t> data;
    if (!read_file(path, &data)) return;
    str text((const char*)data.data(), data.size());
    JV root;
    if (!jparse(text, &root) || root.type != JV::Object) return;
    const JV* files = root.get("files");
    if (!files || files->type != JV::Object) return;
    for (auto& kv : files->o) {
        const JV& e = kv.second;
        const JV *s = e.get("s"), *mt = e.get("m"), *h = e.get("h");
        if (!s || !mt || !h || h->s.size() != 64) continue;
        ME me;
        me.rel = W(kv.first);
        me.size = strtoull(jtok(s).c_str(), nullptr, 10);
        me.mtime = strtoull(jtok(mt).c_str(), nullptr, 10);
        me.sha = h->s;
        m[lower(me.rel)] = me;
    }
}

bool save_manifest(const ws& path, const ws& sync_root, const Manifest& m) {
    str o = "{\n \"version\": 1,\n \"root\": \"" + json::escape(U(sync_root)) + "\",\n \"files\": {";
    bool first = true;
    for (auto& kv : m) {
        const ME& e = kv.second;
        o += first ? "\n" : ",\n";
        first = false;
        o += "  \"" + json::escape(U(e.rel)) + "\": {\"s\": " + std::to_string(e.size) + ", \"m\": " +
             std::to_string(e.mtime) + ", \"h\": \"" + e.sha + "\"}";
    }
    o += "\n }\n}\n";
    return write_atomic(path, o.data(), o.size(), 0);
}

/* -------------------------------------------------------------------- vault */

std::mutex g_mu;
str g_vault;  // utf8, trailing backslash; empty = default

ws vault_w() {
    return norm_dir(vault_dir());
}

bool is_meta(const ws& rel) { return comp_count(rel) == 3 && lower(comp_at(rel, 2)) == L"meta.json"; }

/* Pick a _conflicts name that is free in all given roots. */
ws conflict_rel(const ws& rel, const std::vector<ws>& roots) {
    ws base = L"_conflicts/" + rel + L"." + device_file_tag() + L"." + timestamp_tag();
    for (int n = 0;; n++) {
        ws cand = n ? base + L"-" + std::to_wstring(n + 1) : base;
        bool taken = false;
        for (auto& r : roots) if (path_exists(r + L"/" + cand)) taken = true;
        if (!taken) return cand;
    }
}

bool same_or_nested(const ws& a, const ws& b) {
    auto full = [](const ws& p) {
        ws f = lower(lp(p));
        while (!f.empty() && f.back() == L'\\') f.pop_back();
        return f + L"\\";
    };
    ws fa = full(a), fb = full(b);
    return fa.rfind(fb, 0) == 0 || fb.rfind(fa, 0) == 0;
}

} // namespace

/* ------------------------------------------------------------- public API */

const char* account_notice() {
    return "Your account isn't included. Use Settings > Account > Add this account to another PC to sign in elsewhere.";
}

std::string vault_dir() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_vault.empty()) return g_vault;
    }
    ws d = norm_dir(hub::data_dir());
    str s = U(d) + "\\saves\\";
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_vault.empty()) g_vault = s;
    return g_vault;
}

void set_vault_dir(const std::string& dir) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (dir.empty()) { g_vault.clear(); return; }
    g_vault = U(norm_dir(dir)) + "\\";
}

std::string game_dir(const std::string& system_short, const std::string& game_key) {
    ws d = vault_w() + L"\\" + sanitize_component(system_short) + L"\\" + sanitize_component(game_key);
    mkdirs(d);
    return U(d) + "\\";
}
std::string game_dir_path(const std::string& system_short, const std::string& game_key) {
    return U(vault_w() + L"\\" + sanitize_component(system_short) + L"\\" + sanitize_component(game_key)) + "\\";
}
std::string battery_dir_path(const std::string& system_short, const std::string& game_key) { return game_dir_path(system_short, game_key) + "battery\\"; }
std::string states_dir_path(const std::string& system_short, const std::string& game_key) { return game_dir_path(system_short, game_key) + "states\\"; }
std::string battery_dir(const std::string& system_short, const std::string& game_key) {
    ws d = W(game_dir(system_short, game_key)) + L"battery";
    mkdirs(d);
    return U(d) + "\\";
}
std::string states_dir(const std::string& system_short, const std::string& game_key) {
    ws d = W(game_dir(system_short, game_key)) + L"states";
    mkdirs(d);
    return U(d) + "\\";
}

void touch_game(const std::string& system_short, const std::string& game_key, const std::string& title) {
    ws dir = W(game_dir(system_short, game_key));
    str j = "{\n \"key\": \"" + json::escape(game_key) + "\",\n \"title\": \"" + json::escape(title) +
            "\",\n \"system\": \"" + json::escape(system_short) + "\",\n \"updated\": " + std::to_string(now_unix()) +
            ",\n \"device\": \"" + json::escape(device_name_utf8()) + "\"\n}\n";
    write_atomic(dir + L"meta.json", j.data(), j.size(), 0);
}

void set_enum_error_hook(EnumErrorHook h) { g_enum_hook = h; }

bool is_system_folder_name(const std::string& name) {
    std::string l = name;
    for (auto& c : l) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return hub::console_system(l) != hub::System::Unknown;
}

SyncReport sync_folder(const std::string& sync_root_utf8, bool include_states) {
    SyncReport rep;
    auto fail = [&](const str& e) { rep.ok = false; rep.error = e; return rep; };
    rep.log.push_back(std::string("note: ") + account_notice());
    if (sync_root_utf8.empty()) return fail("no sync folder given");

    ws vault = vault_w(), root = norm_dir(sync_root_utf8);
    if (!mkdirs(vault)) return fail("cannot create vault folder");
    if (!is_dir(root) && !mkdirs(root)) return fail("cannot create sync folder: " + sync_root_utf8);
    if (same_or_nested(vault, root)) return fail("sync folder and save vault must be separate folders");

    FMap L, R;
    /* Both listings must be complete before anything is copied: a folder that could not be listed would make its files look absent
     * and the "only one side has it" branches would then replace them. On any listing error nothing is changed. */
    {
        str werr;
        Excluded ex;
        ex.vault = vault;
        if (!walk(vault, L"", include_states, L, rep.log, &werr, true, &ex, "vault")) return fail("Sync stopped and nothing was changed. " + werr + " in the save vault.");
        if (!walk(root, L"", include_states, R, rep.log, &werr, true, &ex, "sync folder")) return fail("Sync stopped and nothing was changed. " + werr + " in the sync folder.");
        auto drop = [&](FMap& m, const ws& prefix) {
            for (auto it = m.begin(); it != m.end();) it = it->first.rfind(prefix + L"/", 0) == 0 ? m.erase(it) : std::next(it);
        };
        for (auto& g : ex.games) { drop(L, g.first); drop(R, g.first); }
        for (auto& f : ex.folders) { drop(L, f.first); drop(R, f.first); }
        for (auto& g : ex.games) rep.warning += (rep.warning.empty() ? "" : " ") + str("Could not read the save folder of ") + g.second + ", so that game was not synced this time.";
        for (auto& f : ex.folders) rep.warning += (rep.warning.empty() ? "" : " ") + str("Could not read the folder ") + f.second + " in your sync folder or vault, so it was left out.";
    }

    ws mpath = manifest_path(vault, root);
    Manifest old;
    load_manifest(mpath, old);
    Manifest next = old;

    std::set<ws> keys;
    for (auto& kv : L) keys.insert(kv.first);
    for (auto& kv : R) keys.insert(kv.first);

    auto lrel = [](const FE& e) { return U(e.rel); };

    for (const ws& k : keys) {
        auto il = L.find(k), ir = R.find(k);
        auto mi = old.find(k);
        const ME* m = mi == old.end() ? nullptr : &mi->second;
        auto hash_of = [&](const ws& base, const FE& e, str* h) {
            if (m && m->size == e.size && m->mtime == e.mtime) { *h = m->sha; return true; }
            return hash_file(base + L"/" + e.rel, h);
        };
        auto record = [&](const FE& src, const str& sha) {
            ME me; me.rel = src.rel; me.size = src.size; me.mtime = src.mtime; me.sha = sha;
            next[k] = me;
        };
        auto skip = [&](const FE& e, const char* why) {
            rep.log.push_back("skip " + lrel(e) + ": " + why +
                              ((e.attrs & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN))
                                   ? " (cloud placeholder not downloaded)" : ""));
        };

        /* Never replace an existing destination in the "only one side has it" branches: the scan said the file is absent there, but if
         * it exists after all (it appeared since the scan, or the scan missed it) it is compared like any two-sided file below. */
        FE extra;   // the destination file when the scan missed it
        auto probe = [&](const ws& base, const FE& want, FE* out) -> int {   // 0 absent, 1 present (filled), 2 cannot tell / not a file
            WIN32_FILE_ATTRIBUTE_DATA ad;
            if (!GetFileAttributesExW(lp(base + L"/" + want.rel).c_str(), GetFileExInfoStandard, &ad)) {
                DWORD e = GetLastError();
                return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? 0 : 2;
            }
            if (ad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 2;
            out->rel = want.rel;
            out->size = ((uint64_t)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
            out->mtime = ((uint64_t)ad.ftLastWriteTime.dwHighDateTime << 32) | ad.ftLastWriteTime.dwLowDateTime;
            out->attrs = ad.dwFileAttributes;
            return 1;
        };
        const FE* pl = il != L.end() ? &il->second : nullptr;
        const FE* pr = ir != R.end() ? &ir->second : nullptr;
        if (pl && !pr) {
            const FE& e = *pl; str h;
            int pr_state = probe(root, e, &extra);
            if (pr_state == 2) { skip(e, "cannot check the sync folder copy; left untouched"); continue; }
            if (pr_state == 0) {
                if (!hash_of(vault, e, &h)) { skip(e, "cannot read"); continue; }
                if (copy_atomic(vault + L"/" + e.rel, root + L"/" + e.rel, e.mtime)) {
                    rep.pushed++; rep.log.push_back("push " + lrel(e)); record(e, h);
                } else skip(e, "copy to sync folder failed");
                continue;
            }
            rep.log.push_back("note: " + lrel(e) + " exists in the sync folder although the scan missed it; compared instead of overwritten");
            pr = &extra;
        } else if (!pl && pr) {
            const FE& e = *pr; str h;
            int pl_state = probe(vault, e, &extra);
            if (pl_state == 2) { skip(e, "cannot check the vault copy; left untouched"); continue; }
            if (pl_state == 0) {
                if (!hash_of(root, e, &h)) { skip(e, "cannot read"); continue; }
                if (copy_atomic(root + L"/" + e.rel, vault + L"/" + e.rel, e.mtime)) {
                    rep.pulled++; rep.log.push_back("pull " + lrel(e)); record(e, h);
                } else skip(e, "copy from sync folder failed");
                continue;
            }
            rep.log.push_back("note: " + lrel(e) + " exists in the vault although the scan missed it; compared instead of overwritten");
            pl = &extra;
        }

        const FE& el = *pl; const FE& er = *pr;
        str hl, hr;
        if (!hash_of(vault, el, &hl)) { skip(el, "cannot read (vault)"); continue; }
        if (!hash_of(root, er, &hr)) { skip(er, "cannot read (sync folder)"); continue; }
        if (hl == hr) { record(el, hl); continue; }

        bool lch = !m || hl != m->sha, rch = !m || hr != m->sha;
        bool conflict = lch && rch;
        bool local_wins;
        if (lch && !rch) local_wins = true;
        else if (rch && !lch) local_wins = false;
        else if (el.mtime != er.mtime) local_wins = el.mtime > er.mtime;
        else local_wins = hl > hr;  // deterministic on both PCs

        const FE& win = local_wins ? el : er;
        const FE& lose = local_wins ? er : el;
        const ws& wbase = local_wins ? vault : root;
        const ws& lbase = local_wins ? root : vault;

        /* One side changed: it wins on its hash alone, as always. But when the file about to be overwritten is clearly NEWER than the
         * incoming one (2 s = FAT granularity; an old version restored by a cloud client, a stale USB stick) or the incoming one is
         * empty while the existing one is not (a half-written file), the overwritten copy is kept in _conflicts first. */
        const uint64_t kTwoSec = 20000000ULL;
        bool suspicious = !conflict && ((lose.mtime > win.mtime && lose.mtime - win.mtime > kTwoSec) || (win.size == 0 && lose.size != 0));
        if ((conflict || suspicious) && !is_meta(el.rel)) {
            ws cr = conflict_rel(el.rel, {vault, root});
            uint64_t lm = lose.mtime;
            bool vcopy = copy_atomic(lbase + L"/" + lose.rel, vault + L"/" + cr, lm);
            if (!vcopy || !copy_atomic(lbase + L"/" + lose.rel, root + L"/" + cr, lm)) {
                /* conflict_rel() picked a name free on both sides, so a vault copy that exists now is exactly the one THIS attempt made:
                 * remove it, or every retry would pile up another timestamped copy. Nothing older is ever touched. */
                if (vcopy) DeleteFileW(lp(vault + L"/" + cr).c_str());
                skip(el, "could not preserve the overwritten copy; left untouched");
                continue;
            }
            rep.conflicts++;
            if (conflict)
                rep.log.push_back("conflict " + lrel(el) + ": " + (local_wins ? "vault" : "sync folder") +
                                  " is newer and wins; other version kept as " + U(cr));
            else
                rep.log.push_back("backup " + lrel(el) + ": the " + (local_wins ? "vault" : "sync folder") +
                                  " copy changed but is older or empty compared to the one it replaces; the replaced version kept as " + U(cr));
        }
        if (copy_atomic(wbase + L"/" + win.rel, lbase + L"/" + win.rel, win.mtime)) {
            if (local_wins) { rep.pushed++; rep.log.push_back("push " + lrel(el)); }
            else { rep.pulled++; rep.log.push_back("pull " + lrel(el)); }
            record(win, local_wins ? hl : hr);
        } else skip(win, "copy failed");
    }

    for (auto it = next.begin(); it != next.end();)   // manifests of older builds list the identity file: forget it
        it = is_identity_rel(it->first) ? next.erase(it) : std::next(it);
    if (!save_manifest(mpath, root, next)) rep.log.push_back("warning: could not write sync manifest");
    return rep;
}

/* ------------------------------------------------------------------- zip */

bool export_zip(const std::string& zip_path, bool include_states, std::string* error) {
    auto fail = [&](const str& e) { if (error) *error = e; return false; };
    if (error) error->clear();
    ws vault = vault_w();
    if (!is_dir(vault)) mkdirs(vault);
    FMap files; std::vector<str> log;
    walk(vault, L"", include_states, files, log);

    ws dst = W(zip_path);
    if (!mkdirs_for_file(dst)) return fail("cannot create folder for " + zip_path);
    ws tmp = dst + L".hubtmp";
    FILE* f = _wfopen(lp(tmp).c_str(), L"wb");
    if (!f) return fail("cannot create " + zip_path);

    mz_zip_archive z; memset(&z, 0, sizeof z);
    if (!mz_zip_writer_init_cfile(&z, f, 0)) { fclose(f); DeleteFileW(lp(tmp).c_str()); return fail("zip init failed"); }
    bool ok = true;
    std::vector<uint8_t> buf;
    int skipped = 0;
    for (auto& kv : files) {
        const FE& e = kv.second;
        if (!read_file(vault + L"/" + e.rel, &buf)) { skipped++; continue; }
        str name = U(e.rel);
        MZ_TIME_T mt = (MZ_TIME_T)ft_to_unix(e.mtime);
        if (!mz_zip_writer_add_mem_ex_v2(&z, name.c_str(), buf.data(), buf.size(), nullptr, 0, MZ_DEFAULT_LEVEL, 0, 0, &mt,
                                         nullptr, 0, nullptr, 0)) {
            ok = false; break;
        }
    }
    if (ok) ok = mz_zip_writer_finalize_archive(&z) != 0;
    mz_zip_writer_end(&z);
    if (ok) ok = fflush(f) == 0;
    fclose(f);
    if (!ok) { DeleteFileW(lp(tmp).c_str()); return fail("writing zip failed"); }
    if (!commit_tmp(tmp, dst)) return fail("cannot write " + zip_path);
    if (skipped && error) *error = std::to_string(skipped) + " file(s) could not be read and were left out";
    return true;
}

namespace {
bool safe_zip_rel(const str& name, ws* out) {
    str n = name;
    for (auto& c : n) if (c == '\\') c = '/';
    if (n.empty() || n[0] == '/') return false;
    ws w = W(n);
    ws rel; size_t s = 0;
    while (s <= w.size()) {
        size_t e = w.find(L'/', s);
        ws c = w.substr(s, e == ws::npos ? ws::npos : e - s);
        if (c.empty() || c == L"." || c == L".." || c.find_first_of(L"<>:\"|?*") != ws::npos) return false;
        for (wchar_t ch : c) if (ch < 32) return false;
        if (c.back() == L' ' || c.back() == L'.') return false;
        rel += (rel.empty() ? L"" : L"/") + c;
        if (e == ws::npos) break;
        s = e + 1;
    }
    *out = rel;
    return true;
}
} // namespace

SyncReport import_zip(const std::string& zip_path) {
    SyncReport rep;
    auto fail = [&](const str& e) { rep.ok = false; rep.error = e; return rep; };
    ws vault = vault_w();
    if (!mkdirs(vault)) return fail("cannot create vault folder");

    FILE* f = _wfopen(lp(W(zip_path)).c_str(), L"rb");
    if (!f) return fail("cannot open " + zip_path);
    mz_zip_archive z; memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_cfile(&z, f, 0, 0)) { fclose(f); return fail("not a valid zip file: " + zip_path); }

    mz_uint n = mz_zip_reader_get_num_files(&z);
    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st)) continue;
        if (mz_zip_reader_is_file_a_directory(&z, i)) continue;
        ws rel;
        if (!safe_zip_rel(st.m_filename, &rel)) { rep.log.push_back(str("skip unsafe entry ") + st.m_filename); continue; }
        if (lower(comp_at(rel, 0)) == L".sync") continue;
        if (is_identity_rel(rel)) { rep.log.push_back("skip " + U(rel) + ": account files are never imported"); continue; }   // F6
        if (st.m_uncomp_size > (1ull << 30)) { rep.log.push_back("skip too large " + U(rel)); continue; }

        size_t sz = 0;
        void* data = mz_zip_reader_extract_to_heap(&z, i, &sz, 0);
        if (!data && st.m_uncomp_size != 0) { rep.log.push_back("skip unreadable entry " + U(rel)); continue; }
        uint64_t zft = unix_to_ft((int64_t)st.m_time);
        const void* bytes = data ? data : "";
        str zh = sha_bytes(bytes, sz);
        ws target = vault + L"/" + rel;

        if (!path_exists(target)) {
            if (write_atomic(target, bytes, sz, zft)) { rep.pulled++; rep.log.push_back("restore " + U(rel)); }
            else rep.log.push_back("skip " + U(rel) + ": write failed");
        } else {
            str lh;
            WIN32_FILE_ATTRIBUTE_DATA ad;
            bool have = GetFileAttributesExW(lp(target).c_str(), GetFileExInfoStandard, &ad) && hash_file(target, &lh);
            if (!have) {
                rep.log.push_back("skip " + U(rel) + ": cannot read existing file");
            } else if (lh != zh) {
                uint64_t lft = ((uint64_t)ad.ftLastWriteTime.dwHighDateTime << 32) | ad.ftLastWriteTime.dwLowDateTime;
                bool zip_wins = zft > lft + 20000000ULL;  // zip dates have 2 s resolution
                if (is_meta(rel)) {
                    if (zip_wins && write_atomic(target, bytes, sz, zft)) rep.pulled++;
                } else {
                    ws cr = conflict_rel(rel, {vault});
                    bool saved = zip_wins ? copy_atomic(target, vault + L"/" + cr, lft)
                                          : write_atomic(vault + L"/" + cr, bytes, sz, zft);
                    if (!saved) {
                        rep.log.push_back("skip " + U(rel) + ": could not preserve conflicting copy");
                    } else {
                        rep.conflicts++;
                        if (zip_wins) {
                            if (write_atomic(target, bytes, sz, zft)) rep.pulled++;
                            rep.log.push_back("conflict " + U(rel) + ": zip is newer and wins; vault version kept as " + U(cr));
                        } else {
                            rep.log.push_back("conflict " + U(rel) + ": vault is newer and kept; zip version saved as " + U(cr));
                        }
                    }
                }
            }
        }
        if (data) mz_free(data);
    }
    mz_zip_reader_end(&z);
    fclose(f);
    return rep;
}

} // namespace hub::saves
