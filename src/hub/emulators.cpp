#include "emulators.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <tlhelp32.h>
#include <cwctype>
#include <xinput.h>
#include <setupapi.h>
#include <initguid.h>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "emu_saves.h"
#include "miniz.h"
#include "games.h"
#include "logic.h"

namespace hubemu {

namespace fs = std::filesystem;

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static fs::path P(const std::string& s) { return fs::u8path(s); }
static std::string S(const fs::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}
static std::wstring W(const std::string& s) { return P(s).wstring(); }
static std::string U(const std::wstring& w) { return S(fs::path(w)); }

static bool file_exists(const std::string& p) {
    std::error_code ec;
    return fs::is_regular_file(P(p), ec);
}
static bool dir_exists(const std::string& p) {
    std::error_code ec;
    return fs::is_directory(P(p), ec);
}
static void make_dirs(const std::string& p) {
    std::error_code ec;
    fs::create_directories(P(p), ec);
}
static std::string read_text(const std::string& p) {
    std::ifstream f(P(p), std::ios::binary);
    if (!f) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static bool write_text(const std::string& p, const std::string& t) {
    make_dirs(S(P(p).parent_path()));
    std::ofstream f(P(p), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(t.data(), (std::streamsize)t.size());
    return (bool)f;
}
static void remove_tree(const std::string& p) {
    std::error_code ec;
    fs::remove_all(P(p), ec);
}
static std::string env(const wchar_t* n) {
    wchar_t b[2048];
    DWORD len = GetEnvironmentVariableW(n, b, 2048);
    return (len > 0 && len < 2048) ? U(std::wstring(b, len)) : std::string();
}
static std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char c = a.back();
    return (c == '\\' || c == '/') ? a + b : a + "\\" + b;
}

const char* name(Kind k) { return k == Kind::Azahar ? "Azahar" : k == Kind::Cemu ? "Cemu" : "Dolphin"; }
Kind kind_for(hub::System s) { return s == hub::System::WiiU ? Kind::Cemu : s == hub::System::GC ? Kind::Dolphin : Kind::Azahar; }

/* Pinned official releases. Azahar: github.com/azahar-emu/azahar (GPL-3.0), asset digest as published by GitHub.
 * Cemu: github.com/cemu-project/Cemu (MPL-2.0); GitHub lists no digest for the Windows zip, so the SHA-256 was
 * computed from the official v2.6 asset when this was written (trust on first use). */
const Release& release(Kind k) {
    static const Release az = {
        "2126.2", L"github.com", L"/azahar-emu/azahar/releases/download/2126.2/azahar-windows-msvc-2126.2.zip",
        43941048ull, "4412eac8d13123c8840f11890fc26e5b0e4c76d16aa28a608941377b948a9db0", L"azahar.exe", "GPL-3.0"};
    static const Release ce = {
        "2.6", L"github.com", L"/cemu-project/Cemu/releases/download/v2.6/cemu-2.6-windows-x64.zip",
        26374093ull, "a6bcc2bc42a362d10213819948f3152fae7d47f70067f25939b51d3ddcfb0896", L"Cemu.exe", "MPL-2.0"};
    /* Dolphin 2509 (GPL-2.0+), the official x64 7z from dl.dolphin-emu.org; the SHA-256 was computed from that file. */
    static const Release dol = {
        "2509", L"dl.dolphin-emu.org", L"/releases/2509/dolphin-2509-x64.7z",
        18402050ull, "3a910cd09887bdc17df0d344de18249ac9c060d006649cd8660b791a5e8ba355", L"Dolphin.exe", "GPL-2.0+"};
    return k == Kind::Azahar ? az : k == Kind::Cemu ? ce : dol;
}

std::string managed_root() { return join(hub::data_dir(), "emulators"); }
std::string managed_dir(Kind k, const std::string& root) { return join(root, k == Kind::Azahar ? "azahar" : k == Kind::Cemu ? "cemu" : "dolphin"); }

/* ------------------------------------------------------------------ */
/* locating                                                            */
/* ------------------------------------------------------------------ */

static std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c + 32);
    return s;
}

/* Folders named like "Cemu", "Cemu_2.6" below `base` (two levels) that hold Cemu.exe. */
static std::string cemu_in(const std::string& base, int depth) {
    std::error_code ec;
    if (base.empty() || !fs::is_directory(P(base), ec)) return "";
    if (file_exists(join(base, "Cemu.exe"))) return join(base, "Cemu.exe");
    if (depth <= 0) return "";
    for (fs::directory_iterator it(P(base), fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2)) continue;
        std::string n = lower(S(it->path().filename()));
        if (depth == 2 && n.find("cemu") == std::string::npos && n != "games" && n != "emulators" && n != "emulator" && n != "everything") continue;
        std::string r = cemu_in(S(it->path()), depth - 1);
        if (!r.empty()) return r;
    }
    return "";
}

Install find(Kind k, const std::string& configured, const std::string& game_path, const std::string& root) {
    Install in;
    if (k == Kind::Cemu) {
        if (!configured.empty() && file_exists(configured)) {
            in.exe = configured;
            in.how = "chosen in Settings";
        }
        bool only_managed = !env(L"RETROHUB_NO_EXISTING_EMU").empty();   // test/diagnostic: ignore any Cemu outside RetroHub's data dir
        if (only_managed) in.exe.clear();
        if (in.exe.empty() && !game_path.empty() && !only_managed) {   // an install next to / above the game file (Cemu's own "game" folder)
            fs::path d = P(game_path).parent_path();
            for (int i = 0; i < 4 && !d.empty(); ++i, d = d.parent_path()) {
                if (file_exists(S(d / L"Cemu.exe"))) { in.exe = S(d / L"Cemu.exe"); in.how = "existing install near the game"; break; }
                if (d == d.root_path()) break;
            }
        }
        if (in.exe.empty() && !only_managed) {
            std::string up = env(L"USERPROFILE");
            const std::string roots[] = {join(up, "Desktop"), join(up, "Documents"), join(up, "Downloads"), "C:\\Cemu", "C:\\Games",
                                         "C:\\Emulators", env(L"ProgramFiles")};
            for (const auto& b : roots) {
                std::string r = cemu_in(b, 2);
                if (!r.empty()) { in.exe = r; in.how = "existing install (found automatically)"; break; }
            }
        }
        if (!in.exe.empty()) {
            in.dir = S(P(in.exe).parent_path());
            in.managed = false;
            return in;
        }
    }
    std::string md = managed_dir(k, root);
    std::string exe = join(md, U(release(k).exe));
    if (file_exists(exe) && file_exists(join(md, "retrohub-install.txt"))) {   // complete installs only
        in.exe = exe;
        in.dir = md;
        in.managed = true;
        in.how = "RetroHub download";
    }
    return in;
}

/* ------------------------------------------------------------------ */
/* download + install                                                  */
/* ------------------------------------------------------------------ */

std::string sha256_file(const std::string& path) {
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return "";
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return "";
    std::string out;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<char> buf(1 << 20);
        while (f) {
            f.read(buf.data(), (std::streamsize)buf.size());
            std::streamsize n = f.gcount();
            if (n > 0) BCryptHashData(h, (PUCHAR)buf.data(), (ULONG)n, 0);
        }
        UCHAR dig[32];
        if (BCryptFinishHash(h, dig, 32, 0) == 0) {
            char hex[65];
            for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", dig[i]);
            out = hex;
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

struct Progress {
    std::atomic<uint64_t>* got;
    std::atomic<uint64_t>* total;
};

static bool http_download(const std::wstring& host, const std::wstring& path, const std::string& out_file, Progress pr, std::string* err) {
    HINTERNET ses = WinHttpOpen(L"RetroHub", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!ses) { *err = "network unavailable"; return false; }
    bool ok = false;
    HINTERNET con = WinHttpConnect(ses, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, sz = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
        wchar_t lenb[32] = {0};
        sz = sizeof lenb;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, lenb, &sz, nullptr) && pr.total) *pr.total = (uint64_t)_wtoi64(lenb);
        if (status != 200) *err = "server answered " + std::to_string(status);
        else {
            HANDLE f = CreateFileW(W(out_file).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) *err = "cannot write " + out_file;
            else {
                std::vector<char> buf(64 * 1024);
                ok = true;
                for (;;) {
                    DWORD n = 0;
                    if (!WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &n)) { ok = false; *err = "download interrupted"; break; }
                    if (n == 0) break;
                    DWORD w = 0;
                    if (!WriteFile(f, buf.data(), n, &w, nullptr) || w != n) { ok = false; *err = "disk write failed"; break; }
                    if (pr.got) *pr.got += n;
                }
                CloseHandle(f);
            }
        }
    } else *err = "could not reach " + U(host);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

/* Directory (searched to `depth` levels) that contains a file called `name`. */
static bool find_file_below(const std::string& dir, const std::wstring& name, int depth, std::string* found_dir) {
    std::error_code ec;
    std::vector<fs::path> subs;
    for (fs::directory_iterator it(P(dir), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2)) subs.push_back(it->path());
        else if (_wcsicmp(it->path().filename().c_str(), name.c_str()) == 0) { *found_dir = dir; return true; }
    }
    if (depth <= 0) return false;
    for (auto& s : subs)
        if (find_file_below(S(s), name, depth - 1, found_dir)) return true;
    return false;
}

/* Entry names from the zip: UTF-8, relative, no "..", no drive or root. */
static bool safe_zip_name(const std::string& n) {
    if (n.empty() || n[0] == '/' || n[0] == '\\' || n.find(':') != std::string::npos) return false;
    size_t p = 0;
    while (p <= n.size()) {
        size_t q = n.find_first_of("/\\", p);
        std::string part = n.substr(p, q == std::string::npos ? std::string::npos : q - p);
        if (part == "..") return false;
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return true;
}

/* Own unzip (miniz) instead of bsdtar: every entry name is checked, and any entry that cannot be written fails the install
 * (bsdtar skipped Cemu's resources/ar/<RTL mark>cemu.mo and exited 1; the wide-char API writes that name fine). */
static bool unzip_to(const std::string& archive, const std::string& dest, std::string* err) {
    mz_zip_archive za;
    std::memset(&za, 0, sizeof za);
    FILE* zf = _wfopen(W(archive).c_str(), L"rb");
    if (!zf) { *err = "cannot open the download"; return false; }
    if (!mz_zip_reader_init_cfile(&za, zf, 0, 0)) { std::fclose(zf); *err = "the download is not a valid zip"; return false; }
    bool ok = true;
    mz_uint n = mz_zip_reader_get_num_files(&za);
    for (mz_uint i = 0; i < n && ok; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&za, i, &st)) { ok = false; *err = "damaged zip entry"; break; }
        std::string name = st.m_filename;
        if (!safe_zip_name(name)) { ok = false; *err = "unsafe path in the zip: " + name; break; }
        fs::path target = P(dest) / fs::u8path(name);
        std::error_code ec;
        if (st.m_is_directory || name.back() == '/' || name.back() == '\\') { fs::create_directories(target, ec); continue; }
        fs::create_directories(target.parent_path(), ec);
        FILE* out = _wfopen(target.wstring().c_str(), L"wb");
        if (!out) { ok = false; *err = "cannot write " + name; break; }
        bool r = mz_zip_reader_extract_to_cfile(&za, i, out, 0) != 0;
        bool flushed = std::fflush(out) == 0;
        std::fclose(out);
        if (!r || !flushed) { ok = false; *err = "cannot extract " + name; }
    }
    mz_zip_reader_end(&za);
    std::fclose(zf);
    return ok;
}

/* Dolphin ships as a .7z only. Windows' own tar.exe (bsdtar) reads it; the entry list is checked first (same path rules as
 * the zip reader) so nothing can land outside the staging folder, and any tar failure fails the install. */
static bool run_tar(const std::wstring& args, const std::string& stdout_file, std::string* err) {
    wchar_t sysdir[MAX_PATH];
    GetSystemDirectoryW(sysdir, MAX_PATH);
    std::wstring tar = std::wstring(sysdir) + L"\\tar.exe";
    std::wstring cmd = L"\"" + tar + L"\" " + args;
    std::vector<wchar_t> cb(cmd.begin(), cmd.end());
    cb.push_back(0);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out = INVALID_HANDLE_VALUE;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    if (!stdout_file.empty()) {
        out = CreateFileW(W(stdout_file).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = out;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(tar.c_str(), cb.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!ok) { *err = "tar.exe not found (needs Windows 10 or later)"; return false; }
    DWORD w = WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
    DWORD code = 1;
    if (w == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code); else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code != 0) { *err = "unpacking failed (tar exit " + std::to_string(code) + ")"; return false; }
    return true;
}

static bool untar_to(const std::string& archive, const std::string& dest, const std::string& list_tmp, std::string* err) {
    struct Cleanup { std::string f; ~Cleanup() { std::error_code ec; fs::remove(P(f), ec); } } cleanup{list_tmp};
    /* names (-tf): every path checked. types (-tvf): the first column is the entry type; only plain files and folders pass,
     * so no symlink or hardlink can point out of the staging folder. */
    if (!run_tar(L"-tf \"" + W(archive) + L"\"", list_tmp, err)) return false;
    std::istringstream ls(read_text(list_tmp));
    std::string line;
    int n = 0;
    while (std::getline(ls, line)) {
        while (!line.empty() && (line.back() == 13 || line.back() == 10)) line.pop_back();
        if (line.empty()) continue;
        if (!safe_zip_name(line)) { *err = "unsafe path in the download: " + line; return false; }
        ++n;
    }
    if (n == 0) { *err = "the download is empty"; return false; }
    if (!run_tar(L"-tvf \"" + W(archive) + L"\"", list_tmp, err)) return false;
    std::istringstream vs(read_text(list_tmp));
    int m = 0;
    while (std::getline(vs, line)) {
        if (line.empty() || line[0] == 13) continue;
        if ((line[0] != '-' && line[0] != 'd') || line.find(" link to ") != std::string::npos) { *err = "the download contains a link entry: " + line.substr(0, 80); return false; }
        ++m;
    }
    if (m != n) { *err = "the download listing is inconsistent"; return false; }
    return run_tar(L"-xf \"" + W(archive) + L"\" -C \"" + W(dest) + L"\"", "", err);
}

static bool install_core(Kind k, const std::string& root, std::string* err, std::atomic<int>* phase, Progress pr) {
    const Release& rel = release(k);
    std::string dir = managed_dir(k, root);
    std::string exe = join(dir, U(rel.exe));
    if (file_exists(exe) && file_exists(join(dir, "retrohub-install.txt"))) return true;   // complete install (marker is written last)
    remove_tree(dir);   // a leftover from a failed attempt
    std::string stage = join(dir, "_extract");
    std::string archive = join(join(root, "_downloads"), S(fs::path(rel.path).filename()));
    make_dirs(dir);
    make_dirs(S(P(archive).parent_path()));
    remove_tree(stage);
    make_dirs(stage);
    bool ok = true;
    auto setp = [&](int p) { if (phase) *phase = p; };

    setp(0);
    ok = http_download(rel.host, rel.path, archive + ".part", pr, err);
    if (ok) {
        std::error_code ec;
        uint64_t sz = fs::file_size(P(archive + ".part"), ec);
        if (ec || sz != rel.size) { ok = false; *err = "download has the wrong size (" + std::to_string(sz) + " bytes)"; }
    }
    if (ok) {
        setp(1);
        std::string h = sha256_file(archive + ".part");
        if (h != rel.sha256) { ok = false; *err = "checksum mismatch: the download was rejected"; }
    }
    if (ok) {
        std::error_code ec;
        fs::rename(P(archive + ".part"), P(archive), ec);
        if (ec) { ok = false; *err = "cannot move the download into place"; }
    }
    if (ok) {
        setp(2);
        ok = (archive.size() > 3 && archive.compare(archive.size() - 3, 3, ".7z") == 0) ? untar_to(archive, stage, join(root, "_downloads\\list.txt"), err)
                                                                                         : unzip_to(archive, stage, err);
    }
    if (ok) {
        setp(3);
        std::string inner;
        if (!find_file_below(stage, rel.exe, 3, &inner)) { ok = false; *err = U(rel.exe) + " not found in the download"; }
        else {
            std::error_code ec;
            for (fs::directory_iterator it(P(inner), ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code e2;
                fs::path to = P(dir) / it->path().filename();
                fs::remove_all(to, e2);
                fs::rename(it->path(), to, e2);
                if (e2) { ok = false; *err = "could not install " + S(it->path().filename()); break; }
            }
            if (ok && !file_exists(exe)) { ok = false; *err = U(rel.exe) + " missing after install"; }
        }
    }
    remove_tree(stage);
    if (ok && k == Kind::Dolphin) ok = write_text(join(dir, "portable.txt"), "");   // Dolphin keeps its User folder beside the exe
    if (ok) ok = write_text(join(dir, "retrohub-install.txt"), std::string(name(k)) + " " + rel.version + "\nsha256 " + rel.sha256 + "\n");
    if (!ok) remove_tree(dir);   // never leave a half install behind
    std::error_code ec;
    fs::remove(P(archive), ec);
    fs::remove(P(archive + ".part"), ec);
    return ok;
}

bool install_blocking(Kind k, const std::string& root, std::string* err,
                      const std::function<void(int, uint64_t, uint64_t)>& progress) {
    std::atomic<int> phase{0};
    std::atomic<uint64_t> got{0}, total{0};
    std::atomic<bool> stop{false};
    bool ok = false;
    std::string e;
    std::thread t([&] { ok = install_core(k, root, &e, &phase, {&got, &total}); stop = true; });
    while (!stop) {
        if (progress) progress(phase, got, total);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    t.join();
    if (progress) progress(phase, got, total);
    if (err) *err = e;
    return ok;
}

Installer::~Installer() { if (th.joinable()) th.join(); }

void Installer::start(Kind k, const std::string& managed_root_dir) {
    if (th.joinable()) th.join();
    kind = k;
    root = managed_root_dir;
    done = false;
    ok = false;
    got = 0;
    total = 0;
    phase = 0;
    error.clear();
    state = Working;
    th = std::thread([this] {
        std::string e;
        bool r = install_core(kind, root, &e, &phase, {&got, &total});
        if (!r) { std::lock_guard<std::mutex> lk(mu); error = e; }
        ok = r;
        done = true;
    });
}

bool Installer::poll(bool* success) {
    if (state != Working || !done) return false;
    if (th.joinable()) th.join();
    if (success) *success = ok;
    state = ok ? Idle : Failed;
    return true;
}

/* ------------------------------------------------------------------ */
/* config pre-writing                                                  */
/* ------------------------------------------------------------------ */

/* 3DS button -> SDL game-controller button, by position (3DS A = east = the Xbox B button). maptype "all" with
 * api "controller" means any connected game controller (so the first XInput pad), no GUID needed. */
static const char* kAzPad[][2] = {
    {"button_a", "engine:sdl,api:controller,button:1,maptype:all,port:0"},
    {"button_b", "engine:sdl,api:controller,button:0,maptype:all,port:0"},
    {"button_x", "engine:sdl,api:controller,button:3,maptype:all,port:0"},
    {"button_y", "engine:sdl,api:controller,button:2,maptype:all,port:0"},
    {"button_up", "engine:sdl,api:controller,button:11,maptype:all,port:0"},
    {"button_down", "engine:sdl,api:controller,button:12,maptype:all,port:0"},
    {"button_left", "engine:sdl,api:controller,button:13,maptype:all,port:0"},
    {"button_right", "engine:sdl,api:controller,button:14,maptype:all,port:0"},
    {"button_l", "engine:sdl,api:controller,button:9,maptype:all,port:0"},
    {"button_r", "engine:sdl,api:controller,button:10,maptype:all,port:0"},
    {"button_start", "engine:sdl,api:controller,button:6,maptype:all,port:0"},
    {"button_select", "engine:sdl,api:controller,button:4,maptype:all,port:0"},
    {"button_zl", "engine:sdl,api:controller,axis:4,direction:+,threshold:0.5,maptype:all,port:0"},
    {"button_zr", "engine:sdl,api:controller,axis:5,direction:+,threshold:0.5,maptype:all,port:0"},
    {"circle_pad", "engine:sdl,api:controller,axis_x:0,axis_y:1,deadzone:0.15,maptype:all,port:0"},
    {"c_stick", "engine:sdl,api:controller,axis_x:2,axis_y:3,deadzone:0.15,maptype:all,port:0"},
};

std::string azahar_config_patch(const std::string& in) {
    std::string t = in;
    auto set = [&](const char* sec, const std::string& key, const std::string& val) {
        t = hubui::ini_set(t, sec, key + "\\default", "false");
        t = hubui::ini_set(t, sec, key, val);
    };
    set("UI", "confirmClose", "false");
    set("UI", "firstStart", "false");
    set("UI", "enable_discord_presence", "false");
    set("Miscellaneous", "check_for_update_on_start", "false");
    for (auto& kv : kAzPad) set("Controls", kv[0], std::string("\"") + kv[1] + "\"");
    return t;
}

void prepare_azahar(const std::string& install_dir) {
    std::string user = join(install_dir, "user");   // a "user" folder beside azahar.exe makes it portable
    std::string cfgdir = join(user, "config");
    make_dirs(cfgdir);
    make_dirs(join(user, "sdmc"));
    std::string marker = join(cfgdir, "retrohub-pad.txt");
    if (file_exists(marker)) return;   // defaults were written once; later edits (in Azahar's own Configure) are kept
    std::string ini = join(cfgdir, "qt-config.ini");
    write_text(ini, azahar_config_patch(read_text(ini)));
    write_text(marker, "RetroHub wrote the default controller mapping (any game controller, by position) once.\n"
                       "Delete this file to have it written again.\n");
}

std::string cemu_default_settings_xml() {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<content>\n"
           "\t<logflag>0</logflag>\n"
           "\t<mlc_path/>\n"
           "\t<disable_screensaver>true</disable_screensaver>\n"
           "\t<check_update>false</check_update>\n"
           "\t<receive_untested_updates>false</receive_untested_updates>\n"
           "\t<use_discord_presence>false</use_discord_presence>\n"
           "\t<vk_warning>true</vk_warning>\n"
           "\t<gp_download>true</gp_download>\n"
           "\t<fullscreen>false</fullscreen>\n"
           "</content>\n";
}

std::string cemu_gamepad_profile_xml() {
    /* VPAD ButtonId -> XInput-based mapping ids, copied from Cemu's own set_default_mapping (VPADController.cpp) */
    static const int m[][2] = {
        {1, 13}, {2, 12}, {3, 15}, {4, 14},          // A B X Y (Nintendo layout: A is the Xbox B button)
        {5, 8}, {6, 9}, {7, 42}, {8, 43},            // L R ZL ZR (triggers = kTriggerXP/YP)
        {9, 4}, {10, 5},                             // + -
        {11, 0}, {12, 1}, {13, 2}, {14, 3},          // d-pad
        {15, 6}, {16, 7},                            // stick clicks
        {17, 39}, {18, 45}, {19, 44}, {20, 38},      // left stick up/down/left/right
        {21, 41}, {22, 47}, {23, 46}, {24, 40},      // right stick
    };
    std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<emulated_controller>\n\t<type>Wii U GamePad</type>\n"
                    "\t<controller>\n\t\t<api>XInput</api>\n\t\t<uuid>0</uuid>\n\t\t<display_name>Controller 1</display_name>\n"
                    "\t\t<rumble>0</rumble>\n"
                    "\t\t<axis>\n\t\t\t<deadzone>0.25</deadzone>\n\t\t\t<range>1</range>\n\t\t</axis>\n"
                    "\t\t<rotation>\n\t\t\t<deadzone>0.25</deadzone>\n\t\t\t<range>1</range>\n\t\t</rotation>\n"
                    "\t\t<trigger>\n\t\t\t<deadzone>0.25</deadzone>\n\t\t\t<range>1</range>\n\t\t</trigger>\n"
                    "\t\t<mappings>\n";
    for (auto& e : m)
        x += "\t\t\t<entry>\n\t\t\t\t<mapping>" + std::to_string(e[0]) + "</mapping>\n\t\t\t\t<button>" + std::to_string(e[1]) +
             "</button>\n\t\t\t</entry>\n";
    x += "\t\t</mappings>\n\t</controller>\n</emulated_controller>\n";
    return x;
}

void prepare_cemu_managed(const std::string& install_dir) {
    std::string portable = join(install_dir, "portable");   // Cemu's portable marker + data folder
    make_dirs(join(portable, "controllerProfiles"));
    std::string settings = join(portable, "settings.xml");
    if (!file_exists(settings)) write_text(settings, cemu_default_settings_xml());   // its absence triggers the first-run dialog
    std::string pad = join(portable, "controllerProfiles\\controller0.xml");
    if (!file_exists(pad)) write_text(pad, cemu_gamepad_profile_xml());
}

/* ------------------------------------------------------------------ */
/* Dolphin (GameCube)                                                  */
/* ------------------------------------------------------------------ */

DEFINE_GUID(kGuidUsbDevice, 0xA5DCBF10, 0x6530, 0x11D2, 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED);   // GUID_DEVINTERFACE_USB_DEVICE

/* The Nintendo GameCube adapter (WUP-028, USB 057E:0337) counts only when its driver is WinUSB/libusbK: Dolphin cannot
 * open it through Nintendo's own driver, and a port set to "adapter" without that would have no controller at all. */
static bool gc_adapter_usable() {
    HDEVINFO set = SetupDiGetClassDevsW(&kGuidUsbDevice, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    SP_DEVINFO_DATA d{};
    d.cbSize = sizeof d;
    for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(set, i, &d); ++i) {
        wchar_t id[512] = {0}, svc[128] = {0};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_HARDWAREID, nullptr, (PBYTE)id, sizeof id - 2, nullptr)) continue;
        std::wstring hw = id;
        for (auto& ch : hw) ch = (wchar_t)towupper(ch);
        if (hw.find(L"VID_057E&PID_0337") == std::wstring::npos) continue;
        if (SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_SERVICE, nullptr, (PBYTE)svc, sizeof svc - 2, nullptr)) {
            std::wstring s = svc;
            for (auto& ch : s) ch = (wchar_t)towupper(ch);
            found = s == L"WINUSB" || s == L"LIBUSBK" || s == L"LIBUSB0";
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

DolphinPads detect_dolphin_pads() {
    DolphinPads p;
    for (DWORD i = 0; i < 4; ++i) {
        XINPUT_STATE st{};
        p.xinput[i] = XInputGetState(i, &st) == ERROR_SUCCESS;
    }
    p.adapter = gc_adapter_usable();
    return p;
}

std::string dolphin_ini_defaults(const std::string& in) {
    std::string t = in;
    t = hubui::ini_set(t, "Analytics", "Enabled", "False");
    t = hubui::ini_set(t, "Analytics", "PermissionAsked", "True");
    t = hubui::ini_set(t, "AutoUpdate", "UpdateTrack", "");
    t = hubui::ini_set(t, "Interface", "ConfirmStop", "False");
    t = hubui::ini_set(t, "Interface", "UsePanicHandlers", "False");
    t = hubui::ini_set(t, "Interface", "ShowActiveTitle", "False");
    t = hubui::ini_set(t, "Display", "RenderToMain", "False");
    t = hubui::ini_set(t, "Display", "KeepWindowOnTop", "False");
    t = hubui::ini_set(t, "Core", "SkipIPL", "True");
    t = hubui::ini_set(t, "Interface", "SkipNKitWarning", "True");          // NKit images (.nkit.iso) otherwise stop at a dialog
    if (hubui::ini_get(t, "Core", "GFXBackend").empty()) t = hubui::ini_set(t, "Core", "GFXBackend", "D3D");   // Direct3D 11
    if (hubui::ini_get(t, "DSP", "Volume").empty()) t = hubui::ini_set(t, "DSP", "Volume", "100");
    return t;
}

/* Per launch: what must follow the current situation. SlotA = GCI folder (the vault copy), card B empty. A pad is plugged into
 * a port where an XInput pad is connected (port 1 always, keyboard fallback); with a usable GameCube adapter all four ports
 * are the adapter's. SIDevice: 0 = nothing, 6 = standard pad, 12 = adapter. */
std::string dolphin_ini_ports(const std::string& in, const DolphinPads& pads) {
    std::string t = in;
    t = hubui::ini_set(t, "Core", "SlotA", "8");
    t = hubui::ini_set(t, "Core", "SlotB", "255");
    for (int i = 0; i < 4; ++i)
        t = hubui::ini_set(t, "Core", "SIDevice" + std::to_string(i), pads.adapter ? "12" : (i == 0 || pads.xinput[i]) ? "6" : "0");
    return t;
}

std::string dolphin_dolphin_ini_patch(const std::string& in, const DolphinPads& pads) {
    return dolphin_ini_ports(dolphin_ini_defaults(in), pads);
}

std::string dolphin_gfx_ini_patch(const std::string& in) {
    std::string t = in;
    t = hubui::ini_set(t, "Settings", "InternalResolution", "2");           // 2x native (1280x1056)
    t = hubui::ini_set(t, "Settings", "wideScreenHack", "False");
    t = hubui::ini_set(t, "Settings", "ShaderCompilationMode", "2");        // asynchronous ubershaders: no compile stutter
    t = hubui::ini_set(t, "Settings", "WaitForShadersBeforeStarting", "False");
    t = hubui::ini_set(t, "Settings", "BorderlessFullscreen", "True");      // fullscreen without a mode switch
    t = hubui::ini_set(t, "Settings", "ShowFPS", "False");
    return t;
}

std::string dolphin_pad_ini(const std::string& in, const DolphinPads&) {
    std::string t = in;
    const std::string kb = "DInput/0/Keyboard Mouse";
    struct M { const char* key; const char* xi; const char* key_kb; };
    static const M map[] = {   // GC control <- Xbox control (by position), keyboard fallback on port 1 (Dolphin's own default keys)
        {"Buttons/A", "`Button A`", "X"},        {"Buttons/B", "`Button X`", "Z"},      {"Buttons/X", "`Button B`", "C"},
        {"Buttons/Y", "`Button Y`", "S"},        {"Buttons/Z", "`Shoulder R`", "D"},    {"Buttons/Start", "Start", "RETURN"},
        {"Main Stick/Up", "`Left Y+`", "UP"},    {"Main Stick/Down", "`Left Y-`", "DOWN"},
        {"Main Stick/Left", "`Left X-`", "LEFT"}, {"Main Stick/Right", "`Left X+`", "RIGHT"},
        {"C-Stick/Up", "`Right Y+`", "I"},       {"C-Stick/Down", "`Right Y-`", "K"},
        {"C-Stick/Left", "`Right X-`", "J"},     {"C-Stick/Right", "`Right X+`", "L"},
        {"Triggers/L", "`Trigger L`", "Q"},      {"Triggers/R", "`Trigger R`", "W"},
        {"Triggers/L-Analog", "`Trigger L`", ""}, {"Triggers/R-Analog", "`Trigger R`", ""},
        {"D-Pad/Up", "`Pad N`", "T"},            {"D-Pad/Down", "`Pad S`", "G"},
        {"D-Pad/Left", "`Pad W`", "F"},          {"D-Pad/Right", "`Pad E`", "H"},
    };
    for (int i = 0; i < 4; ++i) {
        std::string sec = "GCPad" + std::to_string(i + 1);
        t = hubui::ini_set(t, sec, "Device", "XInput/" + std::to_string(i) + "/Gamepad");
        for (const auto& m : map) {
            std::string v = m.xi;
            if (i == 0 && m.key_kb[0]) v += " | `" + kb + ":" + m.key_kb + "`";
            t = hubui::ini_set(t, sec, m.key, v);
        }
        t = hubui::ini_set(t, sec, "Rumble/Motor", "`Motor L`|`Motor R`");
    }
    return t;
}

void prepare_dolphin(const std::string& install_dir, const DolphinPads& pads) {
    std::string cfg = join(install_dir, "User\\Config");
    make_dirs(cfg);
    if (!file_exists(join(install_dir, "portable.txt"))) write_text(join(install_dir, "portable.txt"), "");
    /* Written once (marker), so settings changed later in Dolphin's own dialogs or by hand are kept: Dolphin.ini defaults,
     * GFX.ini defaults and the pad mappings. */
    std::string ini = join(cfg, "Dolphin.ini"), gfx = join(cfg, "GFX.ini"), pad = join(cfg, "GCPadNew.ini");
    std::string marker = join(cfg, "retrohub-defaults.txt");
    if (!file_exists(marker)) {
        write_text(ini, dolphin_ini_defaults(read_text(ini)));
        write_text(gfx, dolphin_gfx_ini_patch(read_text(gfx)));
        write_text(pad, dolphin_pad_ini(read_text(pad), pads));
        write_text(marker, "RetroHub wrote its default Dolphin settings and pad mappings once. Delete this file to have them written again.\n");
    }
    /* Every launch: only the memory card slots and which ports have a pad plugged in. */
    write_text(ini, dolphin_ini_ports(read_text(ini), pads));
}

/* Older RetroHub builds ran a Dolphin downloaded to <exe>\dolphin with its user folder at <data>\dolphin-user, and kept the
 * memory card straight in the vault (<vault>\gc\<game>\battery\GCI): that location is the one the new sync still reads, so
 * saves need no move. This only tidies up: save states move to the new User folder, the old user folder is renamed (never
 * deleted), and an untouched old download (exe + portable.txt, no User folder of its own) is removed. */
void dolphin_migrate_old(const std::string& data_dir, const std::string& exe_dir, const std::string& install_dir) {
    std::error_code ec;
    std::string old_user = join(data_dir, "dolphin-user");
    if (dir_exists(old_user) && !dir_exists(old_user + ".migrated")) {
        std::string st = join(old_user, "StateSaves"), nst = join(install_dir, "User\\StateSaves");
        if (dir_exists(st) && !dir_exists(nst)) {
            make_dirs(S(P(nst).parent_path()));
            fs::copy(P(st), P(nst), fs::copy_options::recursive, ec);
        }
        fs::rename(P(old_user), P(old_user + ".migrated"), ec);
    }
    std::string old_inst = join(exe_dir, "dolphin");
    if (file_exists(join(old_inst, "Dolphin.exe")) && file_exists(join(old_inst, "portable.txt")) && !dir_exists(join(old_inst, "User")) &&
        !file_exists(join(old_inst, "retrohub-install.txt")) && !is_running(join(old_inst, "Dolphin.exe")))
        remove_tree(old_inst);
}

std::string dolphin_card_root(const std::string& install_dir, const std::string& key) {
    std::string k = key;
    for (char& ch : k) if (!isalnum((unsigned char)ch) && ch != '-' && ch != '_' && ch != '.') ch = '_';
    return join(install_dir, "User\\GC\\RetroHub\\" + k);
}

/* ------------------------------------------------------------------ */
/* launch arguments                                                    */
/* ------------------------------------------------------------------ */

static std::string quote(const std::string& s) { return "\"" + s + "\""; }

std::string dolphin_args(const std::string& game, bool fullscreen, const std::string& gci) {
    /* -b: no main window, Esc / closing the render window ends the process. The GCI override pins the memory card to the
     * folder RetroHub syncs with the vault. The game path must be the last argument. */
    return std::string("-b -C Dolphin.Display.Fullscreen=") + (fullscreen ? "True" : "False") + " -C Dolphin.Core.SlotA=8 -C " +
           quote("Dolphin.Core.GCIFolderAPathOverride=" + gci) + " -e " + quote(game);
}

std::string azahar_args(const std::string& game, bool fullscreen) {
    return std::string(fullscreen ? "-f " : "-w ") + quote(game);   // the game path must be the last argument
}
std::string cemu_args(const std::string& game, bool fullscreen) {
    return "-g " + quote(game) + (fullscreen ? " -f" : "");
}

/* ------------------------------------------------------------------ */
/* saves                                                               */
/* ------------------------------------------------------------------ */

bool is_running(const std::string& exe) {
    std::wstring want = P(exe).filename().wstring();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, want.c_str()) == 0) found = true;
    CloseHandle(snap);
    return found;
}

uint64_t title_id_from_key(const std::string& key) {
    size_t c = key.find(':');
    if (c == std::string::npos || key.size() < c + 17) return 0;
    uint64_t v = 0;
    for (size_t i = c + 1; i < c + 17; ++i) {
        char h = key[i];
        int d = (h >= '0' && h <= '9') ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10 : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
        if (d < 0) return 0;
        v = (v << 4) | uint64_t(d);
    }
    return (key.size() == c + 17) ? v : 0;
}

static std::string stamp_tag() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[48];
    std::strftime(b, sizeof b, "local.%Y%m%d-%H%M%S", &tm);
    return b;
}

static hub::FsProbe probe() {
    hub::FsProbe p;
    p.dir_exists = dir_exists;
    p.file_exists = file_exists;
    p.read_text = read_text;
    return p;
}

SaveSession begin_session(const hub::GameEntry& e, const Install& inst, const std::string& battery_in, const std::string&) {
    SaveSession s;
    s.started_unix = (int64_t)std::time(nullptr);
    std::string battery = battery_in;
    while (!battery.empty() && (battery.back() == '\\' || battery.back() == '/')) battery.pop_back();
    s.battery = battery;
    /* <vault>\<system>\<game>\battery -> <vault>\_conflicts\<system>\<game>\battery */
    fs::path game = P(battery).parent_path(), sysdir = game.parent_path(), vault = sysdir.parent_path();
    s.backup_dir = S(vault / L"_conflicts" / sysdir.filename() / game.filename() / L"battery");
    s.manifest_path = S(vault / L".sync" / (L"emu-" + sysdir.filename().wstring() + L"-" + game.filename().wstring() + L".json"));

    uint64_t tid = title_id_from_key(e.key);
    if (e.system == hub::System::N3DS) {
        if (!tid || inst.dir.empty()) return s;   // homebrew (.3dsx) and unknown titles: nothing to map
        s.emu_root = hub::n3ds_sd_root(join(inst.dir, "user"));
        s.rels = {hub::n3ds_save_rel(tid), hub::n3ds_extdata_rel(tid)};
    } else if (e.system == hub::System::WiiU) {
        if (inst.dir.empty()) return s;
        hub::CemuDirs d;
        if (inst.managed) {
            d.user_data = join(inst.dir, "portable");
            d.mlc = join(d.user_data, "mlc01");
        } else {
            d = hub::cemu_dirs_for(inst.dir, env(L"APPDATA"), probe());
        }
        s.emu_root = d.mlc;
        s.restore_only_if_missing = !inst.managed;   // the user's own install: never overwrite its saves
        s.wiiu_save_parent = join(d.mlc, "usr\\save\\00050000");
        if (!tid) {   // .wud/.wux: the title id is not readable without decrypting; reuse what an earlier session stored
            auto known = hub::subdirs_modified_since(join(battery, "usr\\save\\00050000"), 0);
            for (const auto& k : known) s.rels.push_back("usr/save/00050000/" + k);   // every folder an earlier session stored
            s.wiiu_unknown_title = true;
        } else {
            s.rels = {hub::wiiu_save_rel(tid)};
        }
    } else if (e.system == hub::System::GC) {
        if (inst.dir.empty()) return s;
        s.emu_root = dolphin_card_root(inst.dir, e.key);   // <install>\User\GC\RetroHub\<game>\GCI  <->  <vault>\gc\<game>\battery\GCI
        s.rels = {"GCI"};
        make_dirs(join(s.emu_root, "GCI"));
    } else {
        return s;
    }
    s.active = true;
    std::string tag = stamp_tag();
    hub::SyncManifest man;
    hub::manifest_load(s.manifest_path, &man);
    int in = 0, kept = 0;
    for (const auto& rel : s.rels) {
        std::string vsrc = join(battery, rel), edst = join(s.emu_root, rel);
        if (s.restore_only_if_missing && hub::tree_has_files(edst)) continue;
        hub::TreeSyncReport r = hub::sync_tree_newest(vsrc, edst, join(join(s.backup_dir, "emulator"), rel), tag, &man, rel + "/");
        in += r.copied;
        kept += r.kept;
        if (!r.ok && s.error.empty()) s.error = r.error;
    }
    hub::manifest_save(s.manifest_path, man);
    s.summary = std::to_string(in) + " save file(s) restored from the vault, " + std::to_string(kept) + " kept (emulator copy newer)";
    return s;
}

void end_session(SaveSession& s) {
    if (!s.active) return;
    if (s.wiiu_unknown_title) {   // .wud/.wux: every save folder that changed during the session belongs to this game
        for (const auto& c : hub::subdirs_modified_since(s.wiiu_save_parent, s.started_unix - 2)) {
            std::string rel = "usr/save/00050000/" + c;
            if (std::find(s.rels.begin(), s.rels.end(), rel) == s.rels.end()) s.rels.push_back(rel);
        }
    }
    std::string tag = stamp_tag();
    hub::SyncManifest man;
    hub::manifest_load(s.manifest_path, &man);
    int out = 0, bk = 0;
    for (const auto& rel : s.rels) {
        hub::TreeSyncReport r = hub::sync_tree_newest(join(s.emu_root, rel), join(s.battery, rel), join(s.backup_dir, rel), tag, &man, rel + "/");
        out += r.copied;
        bk += r.backed_up;
        if (!r.ok && s.error.empty()) s.error = r.error;
    }
    hub::manifest_save(s.manifest_path, man);
    s.summary = std::to_string(out) + " save file(s) copied into the vault, " + std::to_string(bk) + " older vault file(s) kept in _conflicts" +
                (s.wiiu_unknown_title && s.rels.empty() ? "; no Wii U save folder changed during the session" : "");
    s.active = false;
}

} // namespace hubemu
