#include "winutil.h"
#include <cctype>
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
#include <map>
#include <sstream>
#include <vector>

#include "controls.h"
#include "emu_saves.h"
#include "wiiu_meta.h"
#include "miniz.h"
#include "games.h"
#include "logic.h"
#include "qm_logic.h"
#include "dolphin_speed_logic.h"

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

const char* name(Kind k) { return k == Kind::Azahar ? "Azahar" : k == Kind::Cemu ? "Cemu" : k == Kind::Pcsx2 ? "PCSX2" : k == Kind::Lrps2 ? "PS2 online core" : "Dolphin"; }
Kind kind_for(hub::System s) { return s == hub::System::WiiU ? Kind::Cemu : s == hub::System::GC ? Kind::Dolphin : s == hub::System::PS2 ? Kind::Pcsx2 : Kind::Azahar; }

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
    /* PCSX2 v2.8.2 stable (GPL-3.0+), github.com/PCSX2/pcsx2; the SHA-256 is GitHub's published asset digest. Portable via portable.ini. */
    static const Release ps = {
        "2.8.2", L"github.com", L"/PCSX2/pcsx2/releases/download/v2.8.2/pcsx2-v2.8.2-windows-x64-Qt.7z",
        25670075ull, "7dfc829ca1994cc1045ac49f05e39b6cf968b72e6a374c40e05c2a2b4ac200b4", L"pcsx2-qt.exe", "GPL-3.0+"};
    /* LRPS2 / "ParaLLEl PS2" v2.0.0-38d3a83 (GPL-3.0), the libretro buildbot nightly of pcsx2_libretro.dll; used by retrorun for PS2 ONLINE rooms only
     * (docs/PS2_ONLINE.md). "latest" is a moving URL: if the nightly changes, the pinned size/SHA-256 reject it (the install fails with a clear message)
     * and this pin must be refreshed after re-running the determinism and lockstep QA. The DLL is never shipped in the package. */
    static const Release lr = {
        "2.0.0-38d3a83", L"buildbot.libretro.com", L"/nightly/windows/x86_64/latest/pcsx2_libretro.dll.zip",
        3760634ull, "730112622345f0ca12e7650c6d4052f443603aa630e9b6ab8a68874a4d66bae6", L"pcsx2_libretro.dll", "GPL-3.0"};
    return k == Kind::Azahar ? az : k == Kind::Cemu ? ce : k == Kind::Pcsx2 ? ps : k == Kind::Lrps2 ? lr : dol;
}

std::string managed_root() { return join(hub::data_dir(), "emulators"); }
std::string managed_dir(Kind k, const std::string& root) { return join(root, k == Kind::Azahar ? "azahar" : k == Kind::Cemu ? "cemu" : k == Kind::Pcsx2 ? "pcsx2" : k == Kind::Lrps2 ? "lrps2" : "dolphin"); }

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

/* Hashes a file with a BCrypt algorithm (SHA-256 or MD5). Returns lowercase hex, "" on error. */
static std::string bcrypt_file(const std::string& path, LPCWSTR alg_id, int digest_len) {
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return "";
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, alg_id, nullptr, 0) != 0) return "";
    std::string out;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<char> buf(1 << 20);
        while (f) {
            f.read(buf.data(), (std::streamsize)buf.size());
            std::streamsize n = f.gcount();
            if (n > 0) BCryptHashData(h, (PUCHAR)buf.data(), (ULONG)n, 0);
        }
        std::vector<UCHAR> dig(digest_len);
        if (BCryptFinishHash(h, dig.data(), (ULONG)digest_len, 0) == 0) {
            std::string hex;
            char tmp[3];
            for (int i = 0; i < digest_len; ++i) { std::snprintf(tmp, sizeof tmp, "%02x", dig[i]); hex += tmp; }
            out = hex;
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

std::string sha256_file(const std::string& path) { return bcrypt_file(path, BCRYPT_SHA256_ALGORITHM, 32); }
std::string md5_file(const std::string& path) { return bcrypt_file(path, BCRYPT_MD5_ALGORITHM, 16); }

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
    if (ok && k == Kind::Pcsx2) ok = write_text(join(dir, "portable.ini"), "");   // portable mode: all data beside the exe, a user's own PCSX2 is never touched
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

/* Profile-driven 3DS pad: control -> qt-config key. The default profile maps by position (3DS A = east = the Xbox B
 * button). maptype "all" with api "controller" means any connected game controller (so the first XInput pad). */
static const char* const kAzButtons[] = {"a", "b", "x", "y", "up", "down", "left", "right", "l", "r", "start", "select", "zl", "zr"};

static std::string fmt_g(double v) { char b[40]; std::snprintf(b, sizeof b, "%g", v); return b; }

/* Which pad Azahar's SDL input binds: a guid + the n-th pad with that guid ("port"). Without a choice every binding keeps
 * "maptype:all,port:0" (any pad). */
static std::string az_target(const AzPad& pad) {
    if (pad.guid.empty()) return "maptype:all,port:0";
    return "guid:" + pad.guid + ",port:" + std::to_string(pad.port);
}

static std::string az_binding(const hubctl::Profile& p, const std::string& control, const AzPad& pad) {
    auto it = p.buttons.find(control);
    std::vector<hubctl::Term> t;
    if (it == p.buttons.end() || !hubctl::parse_expr(it->second, p.axis_threshold, &t) || t.empty()) return "";
    const hubctl::Term& m = t[0];   // Azahar takes one source per button
    if (!m.axis) return "engine:sdl,api:controller,button:" + std::to_string(m.idx) + "," + az_target(pad);
    return "engine:sdl,api:controller,axis:" + std::to_string(m.idx) + ",direction:" + (m.sign < 0 ? "-" : "+") +
           ",threshold:" + fmt_g(m.threshold / 32768.0) + "," + az_target(pad);
}

static std::string az_analog(const hubctl::Profile& p, const std::string& control, const AzPad& pad) {
    auto it = p.analogs.find(control);
    if (it == p.analogs.end()) return "";
    int ax = hubctl::axis_index(it->second.x), ay = hubctl::axis_index(it->second.y);
    if (ax < 0 || ay < 0) return "";
    return "engine:sdl,api:controller,axis_x:" + std::to_string(ax) + ",axis_y:" + std::to_string(ay) + ",deadzone:" +
           fmt_g(it->second.deadzone) + "," + az_target(pad);
}

std::string azahar_config_patch(const std::string& in, const hubctl::Profile& prof, const AzPad& pad) {
    std::string t = in;
    auto set = [&](const char* sec, const std::string& key, const std::string& val) {
        t = hubui::ini_set(t, sec, key + "\\default", "false");
        t = hubui::ini_set(t, sec, key, val);
    };
    set("UI", "confirmClose", "false");
    set("UI", "firstStart", "false");
    set("UI", "enable_discord_presence", "false");
    set("Miscellaneous", "check_for_update_on_start", "false");
    for (const char* c : kAzButtons) { std::string v = az_binding(prof, c, pad); if (!v.empty()) set("Controls", std::string("button_") + c, "\"" + v + "\""); }
    for (const char* c : {"circle_pad", "c_stick"}) { std::string v = az_analog(prof, c, pad); if (!v.empty()) set("Controls", c, "\"" + v + "\""); }
    return t;
}
std::string azahar_config_patch(const std::string& in, const hubctl::Profile& prof) { return azahar_config_patch(in, prof, AzPad()); }
std::string azahar_config_patch(const std::string& in) { return azahar_config_patch(in, hubctl::default_profile("3ds"), AzPad()); }

/* Player 1's pad = first in port order. Its "port" is its rank among the pads with the same guid in SDL's own order. */
bool azahar_pad_select(const std::vector<PadDesc>& by_port, const std::vector<PadDesc>& by_sdl, AzPad* out) {
    *out = AzPad();
    if (by_port.empty()) return false;
    const PadDesc& p = by_port[0];
    int rank = 0;
    for (const PadDesc& d : by_sdl) { if (d.key == p.key) break; if (d.guid == p.guid) ++rank; }
    out->guid = p.guid;
    out->port = rank;
    return true;
}

/* Marker suffix for the chosen pad: nothing when none (default profiles keep their old marker hash). */
static std::string pad_suffix(const std::string& sel) {
    if (sel.empty()) return "";
    uint64_t h = 0xCBF29CE484222325ull;
    for (unsigned char c : sel) h = (h ^ c) * 0x100000001B3ull;
    return "-" + hubctl::hash_hex(h);
}

/* The generated pad configs are written when missing, and again only when the RetroHub profile they come from changed:
 * the marker file holds the hash of the profile that was written ("profile-hash: ..."). A marker without a hash is from
 * an older RetroHub that wrote the built-in default. Edits made in the emulator's own dialogs survive until then. */
static std::string marker_hash(const std::string& marker_text, const std::string& system) {
    size_t p = marker_text.find("profile-hash: ");
    if (p == std::string::npos) return hubctl::hash_hex(hubctl::mapping_hash(hubctl::default_profile(system)));
    size_t e = marker_text.find_first_of("\r\n", p);
    return marker_text.substr(p + 14, e == std::string::npos ? std::string::npos : e - p - 14);
}

static hubctl::Profile stored_profile(const std::string& system) {
    hubctl::Store st;
    hubctl::load_store(hub::data_dir(), "", &st);
    return hubctl::resolve(st, system);
}

void prepare_azahar(const std::string& install_dir, const hubctl::Profile& prof, const AzPad& pad) {
    std::string user = join(install_dir, "user");   // a "user" folder beside azahar.exe makes it portable
    std::string cfgdir = join(user, "config");
    make_dirs(cfgdir);
    make_dirs(join(user, "sdmc"));
    std::string marker = join(cfgdir, "retrohub-pad.txt");
    std::string want = hubctl::hash_hex(hubctl::mapping_hash(prof)) + pad_suffix(pad.guid.empty() ? "" : pad.guid + "/" + std::to_string(pad.port));
    if (file_exists(marker) && marker_hash(read_text(marker), "3ds") == want) return;   // already written for this profile; later edits are kept
    std::string ini = join(cfgdir, "qt-config.ini");
    write_text(ini, azahar_config_patch(read_text(ini), prof, pad));
    write_text(marker, "RetroHub wrote the controller mapping from its Controllers profile.\n"
                       "It is rewritten only when that profile changes. Delete this file to have it written again.\n"
                       "profile-hash: " + want + "\n");
}
void prepare_azahar(const std::string& install_dir, const hubctl::Profile& prof) { prepare_azahar(install_dir, prof, AzPad()); }
void prepare_azahar(const std::string& install_dir) { prepare_azahar(install_dir, stored_profile("3ds"), AzPad()); }

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

/* SDL source -> Cemu's XInput mapping ids (from Cemu's own set_default_mapping, VPADController.cpp). */
static int cemu_xinput_id(const hubctl::Term& t) {
    static const std::map<std::string, int> m = {
        {"a", 12}, {"b", 13}, {"x", 14}, {"y", 15}, {"leftshoulder", 8}, {"rightshoulder", 9}, {"start", 4}, {"back", 5},
        {"dpup", 0}, {"dpdown", 1}, {"dpleft", 2}, {"dpright", 3}, {"leftstick", 6}, {"rightstick", 7},
        {"lefttrigger", 42}, {"righttrigger", 43}, {"leftx+", 38}, {"lefty-", 39}, {"rightx+", 40}, {"righty-", 41},
        {"leftx-", 44}, {"lefty+", 45}, {"rightx-", 46}, {"righty+", 47}};
    auto it = m.find(hubctl::term_name(t));
    return it == m.end() ? -1 : it->second;
}

/* Wii U player N = the N-th XInput pad of the RetroHub port order (Cemu's XInput API numbers pads by XInput slot). Pads that are
 * not XInput are skipped here (managed download only): Cemu's SDL API uses its own ids. A user's own Cemu gets those pads
 * as SDL players instead (cemu_pads_for_own, page_controllers.cpp). */
CemuPads cemu_pads_with(const std::vector<PadDesc>& by_port) {
    CemuPads c;
    int xrank = 0;
    for (const PadDesc& p : by_port) {
        if (!p.xinput) continue;
        if (c.xinput.size() < 4) c.xinput.push_back(xrank);
        ++xrank;
    }
    return c;
}

static std::string cemu_sel(const CemuPads& c) {
    std::string s;
    for (size_t i = 0; i < c.xinput.size(); ++i)
        s += std::to_string(i) + "=" + (i < c.dsu.size() && c.dsu[i] >= 0 ? "dsu" + std::to_string(c.dsu[i]) : i < c.sdl.size() && !c.sdl[i].empty() ? c.sdl[i] : std::to_string(c.xinput[i])) + ";";
    return s == "0=0;" ? "" : s;   // the old single-pad setup keeps its marker hash
}

/* Cemu's SDL pad ids (SDLController.cpp: buttons are SDL's own gamepad button numbers; axes/triggers share the XInput ids). */
static int cemu_sdl_id(const hubctl::Term& t) {
    static const std::map<std::string, int> m = {
        {"a", 0}, {"b", 1}, {"x", 2}, {"y", 3}, {"back", 4}, {"start", 6}, {"leftstick", 7}, {"rightstick", 8},
        {"leftshoulder", 9}, {"rightshoulder", 10}, {"dpup", 11}, {"dpdown", 12}, {"dpleft", 13}, {"dpright", 14},
        {"lefttrigger", 42}, {"righttrigger", 43}, {"leftx+", 38}, {"lefty-", 39}, {"rightx+", 40}, {"righty-", 41},
        {"leftx-", 44}, {"lefty+", 45}, {"rightx-", 46}, {"righty+", 47}};
    auto it = m.find(hubctl::term_name(t));
    return it == m.end() ? -1 : it->second;
}

/* Cemu's SDL controller uuid: "<index>_<SDL GUID string>", the index counting pads with the same GUID (SDLController.cpp). */
std::string cemu_sdl_uuid(const std::string& guid, int same_guid_index) { return std::to_string(same_guid_index) + "_" + guid; }

CemuPads cemu_pads_with_sdl(const std::vector<PadDesc>& by_port, const std::vector<PadDesc>& all_pads) {
    CemuPads c;
    int xrank = 0;
    for (const PadDesc& p : by_port) {
        int idx = 0;   // position among the pads that share this GUID, in SDL order
        for (const PadDesc& q : all_pads) { if (q.key == p.key) break; if (q.guid == p.guid) ++idx; }
        if (c.xinput.size() < 4) {
            c.xinput.push_back(p.xinput ? xrank : -1);
            c.sdl.push_back(p.xinput ? std::string() : cemu_sdl_uuid(p.guid, idx));
        }
        if (p.xinput) ++xrank;
    }
    return c;
}

/* Cemu's DSUController ids (DSUController.cpp: bits of the DSU button bytes in order; sticks/triggers share the XInput ids).
 * Wii U A/B/X/Y sit on the DSU A/B/X/Y bits; the remote pad's XInput layout is what dsu_server.cpp maps from. */
static int cemu_dsu_id(const hubctl::Term& t) {
    static const std::map<std::string, int> m = {
        {"back", 0}, {"leftstick", 1}, {"rightstick", 2}, {"start", 3}, {"dpup", 4}, {"dpright", 5}, {"dpdown", 6}, {"dpleft", 7},
        {"leftshoulder", 10}, {"rightshoulder", 11}, {"x", 12}, {"a", 13}, {"b", 14}, {"y", 15},
        {"lefttrigger", 42}, {"righttrigger", 43}, {"leftx+", 38}, {"lefty-", 39}, {"rightx+", 40}, {"righty-", 41},
        {"leftx-", 44}, {"lefty+", 45}, {"rightx-", 46}, {"righty+", 47}};
    auto it = m.find(hubctl::term_name(t));
    return it == m.end() ? -1 : it->second;
}

static std::string cemu_pad_xml(const hubctl::Profile& prof, int xinput_idx, int player, bool pro, const std::string& sdl_uuid = "", int dsu_slot = -1) {
    const bool dsu = dsu_slot >= 0;
    const bool sdl = !dsu && !sdl_uuid.empty();
    /* VPAD ButtonId (1..24) -> profile control. The default profile reproduces the old fixed table exactly. */
    static const char* const vpad[] = {"a", "b", "x", "y", "l", "r", "zl", "zr", "plus", "minus", "dpup", "dpdown", "dpleft", "dpright",
                                       "l3", "r3", "ls_up", "ls_down", "ls_left", "ls_right", "rs_up", "rs_down", "rs_left", "rs_right"};
    auto dz = [&](const char* name) { auto it = prof.analogs.find(name); return fmt_g(it == prof.analogs.end() ? 0.25 : it->second.deadzone); };
    std::string x = std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<emulated_controller>\n\t<type>") + (pro ? "Wii U Pro Controller" : "Wii U GamePad") + "</type>\n"
                    "\t<controller>\n\t\t<api>" + std::string(dsu ? "DSUController" : sdl ? "SDLController" : "XInput") + "</api>\n\t\t<uuid>" + (dsu ? std::to_string(dsu_slot) : sdl ? sdl_uuid : std::to_string(xinput_idx)) + "</uuid>\n\t\t<display_name>Controller " + std::to_string(player + 1) + "</display_name>\n"
                    "\t\t<rumble>0</rumble>\n"
                    "\t\t<axis>\n\t\t\t<deadzone>" + dz("left_stick") + "</deadzone>\n\t\t\t<range>1</range>\n\t\t</axis>\n"
                    "\t\t<rotation>\n\t\t\t<deadzone>" + dz("right_stick") + "</deadzone>\n\t\t\t<range>1</range>\n\t\t</rotation>\n"
                    "\t\t<trigger>\n\t\t\t<deadzone>0.25</deadzone>\n\t\t\t<range>1</range>\n\t\t</trigger>\n"
                    "\t\t<mappings>\n";
    for (int i = 0; i < 24; i++) {
        auto it = prof.buttons.find(vpad[i]);
        std::vector<hubctl::Term> t;
        if (it == prof.buttons.end() || !hubctl::parse_expr(it->second, prof.axis_threshold, &t) || t.empty()) continue;
        int id = dsu ? cemu_dsu_id(t[0]) : sdl ? cemu_sdl_id(t[0]) : cemu_xinput_id(t[0]);   // one source per Wii U button
        if (id < 0) continue;
        x += "\t\t\t<entry>\n\t\t\t\t<mapping>" + std::to_string(i + 1) + "</mapping>\n\t\t\t\t<button>" + std::to_string(id) +
             "</button>\n\t\t\t</entry>\n";
    }
    x += "\t\t</mappings>\n\t</controller>\n</emulated_controller>\n";
    return x;
}
std::string cemu_gamepad_profile_xml(const hubctl::Profile& prof) { return cemu_pad_xml(prof, 0, 0, false); }
std::string cemu_gamepad_profile_xml(const hubctl::Profile& prof, int xinput_idx) { return cemu_pad_xml(prof, xinput_idx, 0, false); }
std::string cemu_pro_profile_xml(const hubctl::Profile& prof, int xinput_idx, int player) { return cemu_pad_xml(prof, xinput_idx, player, true); }
std::string cemu_gamepad_profile_xml() { return cemu_gamepad_profile_xml(hubctl::default_profile("wiiu")); }
std::string cemu_gamepad_profile_xml_sdl(const hubctl::Profile& prof, const std::string& sdl_uuid) { return cemu_pad_xml(prof, 0, 0, false, sdl_uuid); }
std::string cemu_pro_profile_xml_dsu(const hubctl::Profile& prof, int dsu_slot, int player) { return cemu_pad_xml(prof, 0, player, true, "", dsu_slot); }
CemuPads cemu_pads_with_dsu(const CemuPads& local, int remote_players) {
    CemuPads c;
    c.xinput.push_back(!local.xinput.empty() ? local.xinput[0] : 0);
    c.sdl.push_back(!local.sdl.empty() ? local.sdl[0] : std::string());
    c.dsu.push_back(-1);
    for (int i = 0; i < remote_players && i < 3; ++i) { c.xinput.push_back(-1); c.sdl.push_back(std::string()); c.dsu.push_back(i); }
    return c;
}
std::string cemu_pro_profile_xml_sdl(const hubctl::Profile& prof, const std::string& sdl_uuid, int player) { return cemu_pad_xml(prof, 0, player, true, sdl_uuid); }

/* Writes controller0-3.xml into `cdir` (the controllerProfiles folder) when missing or when the profile/pad order changed.
 * `need_marker`: a user's own folder is only trusted as ours when the marker exists. */
static void write_cemu_profiles(const std::string& cdir, const hubctl::Profile& prof, const CemuPads& pads, bool need_marker) {
    make_dirs(cdir);
    std::string pad = join(cdir, "controller0.xml");
    std::string marker = join(cdir, "retrohub-pad.txt");
    std::string want = hubctl::hash_hex(hubctl::mapping_hash(prof)) + pad_suffix(cemu_sel(pads));
    /* No marker = written by an older RetroHub (the built-in default) or never written. */
    if (file_exists(pad) && (!need_marker || file_exists(marker)) && marker_hash(file_exists(marker) ? read_text(marker) : "", "wiiu") == want) return;
    auto xml = [&](int i) {
        bool sdl = i < (int)pads.sdl.size() && !pads.sdl[i].empty();
        int xi = i < (int)pads.xinput.size() && pads.xinput[i] >= 0 ? pads.xinput[i] : 0;
        int ds = i < (int)pads.dsu.size() ? pads.dsu[i] : -1;
        return cemu_pad_xml(prof, xi, i, i > 0, sdl ? pads.sdl[i] : std::string(), ds);
    };
    write_text(pad, xml(0));   // player 1 = the Wii U GamePad
    for (int i = 1; i < 4; ++i) {   // players 2-4 = Pro Controllers on the next pads; stale ones from an earlier setup go
        std::string f = join(cdir, "controller" + std::to_string(i) + ".xml");
        if (i < (int)pads.xinput.size()) write_text(f, xml(i));
        else if (file_exists(f) && read_text(f).find("Wii U Pro Controller") != std::string::npos) { std::error_code ec; fs::remove(P(f), ec); }
    }
    write_text(marker, "RetroHub wrote controller0.xml (and controller1-3.xml for more pads) from its Controllers profile; it is rewritten only when that profile or the pad order changes.\n"
                       "profile-hash: " + want + "\n");
}

void prepare_cemu_managed(const std::string& install_dir, const hubctl::Profile& prof, const CemuPads& pads) {
    std::string portable = join(install_dir, "portable");   // Cemu's portable marker + data folder
    make_dirs(join(portable, "controllerProfiles"));
    std::string settings = join(portable, "settings.xml");
    if (!file_exists(settings)) write_text(settings, cemu_default_settings_xml());   // its absence triggers the first-run dialog
    write_cemu_profiles(join(portable, "controllerProfiles"), prof, pads, false);
}

std::string cemu_user_data_dir(const std::string& exe_dir, const std::string& appdata) {
    std::string ed = exe_dir;
    while (!ed.empty() && (ed.back() == '\\' || ed.back() == '/')) ed.pop_back();
    if (fs::is_directory(P(join(ed, "portable")))) return join(ed, "portable");
    if (file_exists(join(ed, "settings.xml"))) return ed;
    return join(appdata, "Cemu");
}

bool cemu_backup_controls(const std::string& user_data) {
    std::string cdir = join(user_data, "controllerProfiles"), bak = join(user_data, "controllerProfiles.retrohub-backup");
    std::error_code ec;
    if (fs::exists(P(bak), ec)) return true;   // once: the first backup is the user's original
    if (fs::is_directory(P(cdir), ec)) fs::copy(P(cdir), P(bak), fs::copy_options::recursive, ec);
    else fs::create_directories(P(bak), ec);   // nothing to keep: an empty backup restores to "no profiles"
    return !ec;
}

void prepare_cemu_own(const std::string& user_data, const hubctl::Profile& prof, const CemuPads& pads, bool manage) {
    if (!manage) return;   // the user's own controls stay untouched
    if (!cemu_backup_controls(user_data)) return;   // never write without a backup
    write_cemu_profiles(join(user_data, "controllerProfiles"), prof, pads, true);
}

bool cemu_restore_controls(const std::string& user_data) {
    std::string cdir = join(user_data, "controllerProfiles"), bak = join(user_data, "controllerProfiles.retrohub-backup");
    std::error_code ec;
    if (!fs::is_directory(P(bak), ec)) return false;
    fs::remove_all(P(cdir), ec);
    fs::create_directories(P(cdir), ec);
    fs::copy(P(bak), P(cdir), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    return !ec;
}
void prepare_cemu_managed(const std::string& install_dir, const hubctl::Profile& prof) { prepare_cemu_managed(install_dir, prof, CemuPads()); }
void prepare_cemu_managed(const std::string& install_dir) { prepare_cemu_managed(install_dir, stored_profile("wiiu")); }

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
    t = hubui::ini_set(t, "Input", "BackgroundInput", "False");              // Dolphin must ignore the pad while the hub menu has focus
    t = hubui::ini_set(t, "General", "HotkeysRequireFocus", "False");        // the speed hotkeys come from the hub's DSU pad (dolphin_speed.cpp): no focus needed
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
    t = hubui::ini_set(t, "Core", "EmulationSpeed", "1.0");   // the Game menu's Speed row starts every session at 1x
    t = hubui::ini_set(t, "General", "HotkeysRequireFocus", "False");   // every launch (upgraded installs too): the speed pad needs no focus; other hotkeys are unbound (dolphin_hotkeys_ini)
    t = hubui::ini_set(t, "Core", "SkipIPL", "True");   // every launch; the offline "real console startup" turns it off for that launch only (real_boot_ui.cpp)
    for (int i = 0; i < 4; ++i)
        t = hubui::ini_set(t, "Core", "SIDevice" + std::to_string(i), pads.adapter ? "12" : (i == 0 || pads.xinput[i] || !pads.dev[i].empty()) ? "6" : "0");
    return t;
}

/* Hotkeys.ini: the two emulation-speed hotkeys on Ctrl+Shift+F13 / F14 (keys nobody presses), which the Game menu's Speed row
 * sends after focusing Dolphin. Other lines (the user's own hotkeys) are kept. */
/* Dolphin.ini sets HotkeysRequireFocus = False (the speed pad must work without focus). In Dolphin 2509 that one switch opens the focus
 * gate for EVERY hotkey (DolphinQt/HotkeyScheduler.cpp: Core::UpdateInputGate(MAIN_FOCUSED_HOTKEYS) before all of them), and its
 * keyboard backend reads keys globally: a default binding such as Stop = Escape or Toggle Pause = F10 would fire while the user types in
 * another app. So every other hotkey in RetroHub's own Dolphin is unbound; the quick menu covers stop / pause / states. */
static std::string blank_other_hotkeys(const std::string& in) {
    std::string out, line;
    bool in_hk = false;
    size_t i = 0;
    while (i <= in.size()) {
        size_t nl = in.find('\n', i);
        line = in.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        std::string tl = line;
        while (!tl.empty() && (tl.back() == '\r' || tl.back() == ' ')) tl.pop_back();
        size_t a = tl.find_first_not_of(" \t");
        if (a != std::string::npos && tl[a] == '[') in_hk = tl.substr(a) == "[Hotkeys]";
        else if (in_hk && a != std::string::npos && tl[a] != ';' && tl[a] != '#') {
            size_t eq = tl.find('=');
            if (eq != std::string::npos) {
                std::string key = tl.substr(a, eq - a), val = tl.substr(eq + 1);
                while (!key.empty() && key.back() == ' ') key.pop_back();
                const bool ours = key == "Device" || key == hubqm::dolphin_hotkey_name(hubqm::DolphinKey::Increase) || key == hubqm::dolphin_hotkey_name(hubqm::DolphinKey::Decrease);
                if (!ours && val.find_first_not_of(" ") != std::string::npos) line = key + " = " + (!line.empty() && line.back() == '\r' ? "\r" : "");
            }
        }
        out += line;
        if (nl == std::string::npos) break;
        out += '\n';
        i = nl + 1;
    }
    return out;
}

std::string dolphin_hotkeys_ini(const std::string& in) {
    std::string t = blank_other_hotkeys(in);
    if (hubui::ini_get(t, "Hotkeys", "Device").empty()) t = hubui::ini_set(t, "Hotkeys", "Device", "DInput/0/Keyboard Mouse");
    for (auto k : {hubqm::DolphinKey::Increase, hubqm::DolphinKey::Decrease})
        t = hubui::ini_set(t, "Hotkeys", hubqm::dolphin_hotkey_name(k), hubds::hotkey_expr(k));   // DSU pad button | Ctrl+Shift+F13/F14
    return t;
}

/* DSUClient.ini (Dolphin's DSU client): one server, the hub's speed pad (dolphin_speed.cpp). Other entries are kept. */
std::string dolphin_dsu_ini(const std::string& in) {
    std::string t = hubui::ini_set(in, "Server", "Enabled", "True");
    std::string e = hubui::ini_get(t, "Server", "Entries");
    const std::string mine = hubds::server_entries();
    if (e.find(mine) == std::string::npos) t = hubui::ini_set(t, "Server", "Entries", e + (!e.empty() && e.back() != ';' ? ";" : "") + mine);
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

/* SDL source -> Dolphin's XInput device control names. */
static std::string dolphin_xinput_name(const hubctl::Term& t) {
    static const std::map<std::string, const char*> m = {
        {"a", "`Button A`"}, {"b", "`Button B`"}, {"x", "`Button X`"}, {"y", "`Button Y`"}, {"leftshoulder", "`Shoulder L`"},
        {"rightshoulder", "`Shoulder R`"}, {"start", "Start"}, {"back", "Back"}, {"leftstick", "`Thumb L`"}, {"rightstick", "`Thumb R`"},
        {"dpup", "`Pad N`"}, {"dpdown", "`Pad S`"}, {"dpleft", "`Pad W`"}, {"dpright", "`Pad E`"},
        {"lefttrigger", "`Trigger L`"}, {"righttrigger", "`Trigger R`"},
        {"leftx+", "`Left X+`"}, {"leftx-", "`Left X-`"}, {"lefty-", "`Left Y+`"}, {"lefty+", "`Left Y-`"},
        {"rightx+", "`Right X+`"}, {"rightx-", "`Right X-`"}, {"righty-", "`Right Y+`"}, {"righty+", "`Right Y-`"}};
    auto it = m.find(hubctl::term_name(t));
    return it == m.end() ? "" : it->second;
}

/* SDL source -> Dolphin's SDL device control names ("Button S" = SDL south = a; Y- is up on this backend). */
static std::string dolphin_sdl_name(const hubctl::Term& t) {
    static const std::map<std::string, const char*> m = {
        {"a", "`Button S`"}, {"b", "`Button E`"}, {"x", "`Button W`"}, {"y", "`Button N`"}, {"leftshoulder", "`Shoulder L`"},
        {"rightshoulder", "`Shoulder R`"}, {"start", "Start"}, {"back", "Back"}, {"guide", "Guide"}, {"leftstick", "`Thumb L`"},
        {"rightstick", "`Thumb R`"}, {"dpup", "`Pad N`"}, {"dpdown", "`Pad S`"}, {"dpleft", "`Pad W`"}, {"dpright", "`Pad E`"},
        {"lefttrigger", "`Trigger L`"}, {"righttrigger", "`Trigger R`"},
        {"leftx+", "`Left X+`"}, {"leftx-", "`Left X-`"}, {"lefty-", "`Left Y-`"}, {"lefty+", "`Left Y+`"},
        {"rightx+", "`Right X+`"}, {"rightx-", "`Right X-`"}, {"righty-", "`Right Y-`"}, {"righty+", "`Right Y+`"}};
    auto it = m.find(hubctl::term_name(t));
    return it == m.end() ? "" : it->second;
}

/* Key name -> Dolphin's DInput keyboard names (upper-case letters / digits; its defaults use UP, RETURN ...). */
std::string dolphin_key_name(const std::string& k) {
    if (k.size() == 1) return std::string(1, (char)std::toupper((unsigned char)k[0]));
    static const std::map<std::string, const char*> m = {
        {"up", "UP"}, {"down", "DOWN"}, {"left", "LEFT"}, {"right", "RIGHT"}, {"return", "RETURN"}, {"kp_enter", "NUMPADENTER"},
        {"space", "SPACE"}, {"tab", "TAB"}, {"backspace", "BACK"}, {"lshift", "LSHIFT"}, {"rshift", "RSHIFT"}, {"lctrl", "LCONTROL"},
        {"rctrl", "RCONTROL"}, {"lalt", "LMENU"}, {"ralt", "RMENU"}, {"comma", "COMMA"}, {"period", "PERIOD"}, {"slash", "SLASH"},
        {"semicolon", "SEMICOLON"}, {"quote", "APOSTROPHE"}, {"lbracket", "LBRACKET"}, {"rbracket", "RBRACKET"}, {"minus", "MINUS"},
        {"equals", "EQUALS"}, {"backslash", "BACKSLASH"}, {"grave", "GRAVE"}, {"insert", "INSERT"}, {"delete", "DELETE"},
        {"home", "HOME"}, {"end", "END"}, {"pageup", "PRIOR"}, {"pagedown", "NEXT"}};
    auto it = m.find(k);
    if (it != m.end()) return it->second;
    if (k.size() >= 2 && k[0] == 'f' && std::isdigit((unsigned char)k[1])) return "F" + k.substr(1);
    if (k.size() == 3 && k[0] == 'k' && k[1] == 'p' && std::isdigit((unsigned char)k[2])) return std::string("NUMPAD") + k[2];
    return "";
}

DolphinPads dolphin_pads_with(DolphinPads base, const std::vector<PadDesc>& by_port, const std::vector<PadDesc>& by_sdl) {
    int xrank = 0;
    for (size_t i = 0; i < by_port.size() && i < 4; ++i) {
        const PadDesc& p = by_port[i];
        if (p.xinput) {
            if ((int)i != xrank) base.dev[i] = "XInput/" + std::to_string(xrank) + "/Gamepad";
            ++xrank;
        } else {
            int n = 0;   // Dolphin numbers same-named SDL devices in SDL's order
            for (const PadDesc& d : by_sdl) { if (d.key == p.key) break; if (d.name == p.name) ++n; }
            base.dev[i] = "SDL/" + std::to_string(n) + "/" + p.name;
        }
    }
    return base;
}

std::string dolphin_pad_ini(const std::string& in, const DolphinPads& pads, const hubctl::Profile& prof) {
    std::string t = in;
    const std::string kb = "DInput/0/Keyboard Mouse";
    struct M { const char* control; const char* key; const char* key_kb; };
    static const M map[] = {   // profile control -> GC control; keyboard fallback on port 1 (Dolphin's own default keys)
        {"a", "Buttons/A", "X"},        {"b", "Buttons/B", "Z"},      {"x", "Buttons/X", "C"},
        {"y", "Buttons/Y", "S"},        {"z", "Buttons/Z", "D"},      {"start", "Buttons/Start", "RETURN"},
        {"main_up", "Main Stick/Up", "UP"},    {"main_down", "Main Stick/Down", "DOWN"},
        {"main_left", "Main Stick/Left", "LEFT"}, {"main_right", "Main Stick/Right", "RIGHT"},
        {"c_up", "C-Stick/Up", "I"},       {"c_down", "C-Stick/Down", "K"},
        {"c_left", "C-Stick/Left", "J"},     {"c_right", "C-Stick/Right", "L"},
        {"l", "Triggers/L", "Q"},      {"r", "Triggers/R", "W"},
        {"l_analog", "Triggers/L-Analog", ""}, {"r_analog", "Triggers/R-Analog", ""},
        {"dpad_up", "D-Pad/Up", "T"},            {"dpad_down", "D-Pad/Down", "G"},
        {"dpad_left", "D-Pad/Left", "F"},          {"dpad_right", "D-Pad/Right", "H"},
    };
    for (int i = 0; i < 4; ++i) {
        std::string sec = "GCPad" + std::to_string(i + 1);
        const std::string dev = pads.dev[i].empty() ? "XInput/" + std::to_string(i) + "/Gamepad" : pads.dev[i];
        const bool sdl = dev.compare(0, 4, "SDL/") == 0;
        t = hubui::ini_set(t, sec, "Device", dev);
        for (const auto& m : map) {
            std::string v;
            auto it = prof.buttons.find(m.control);
            std::vector<hubctl::Term> terms;
            if (it != prof.buttons.end() && hubctl::parse_expr(it->second, prof.axis_threshold, &terms))
                for (const auto& term : terms) { std::string n = sdl ? dolphin_sdl_name(term) : dolphin_xinput_name(term); if (!n.empty()) v += (v.empty() ? "" : " | ") + n; }
            if (i == 0) {   // the keyboard pad (profile keys; the defaults are Dolphin's own)
                auto kit = prof.keys.find(m.control);
                std::vector<int> codes;
                if (kit != prof.keys.end() && hubctl::parse_keys(kit->second, &codes))
                    for (int c : codes) { std::string kn = dolphin_key_name(hubctl::key_name(c)); if (!kn.empty()) v += (v.empty() ? "" : " | ") + std::string("`") + kb + ":" + kn + "`"; }
            }
            t = hubui::ini_set(t, sec, m.key, v);
        }
        /* A deadzone is only written when the player set one (0 = Dolphin's own default). */
        for (const auto& st : {std::make_pair("main_stick", "Main Stick"), std::make_pair("c_stick", "C-Stick")}) {
            auto it = prof.analogs.find(st.first);
            if (it != prof.analogs.end() && it->second.deadzone > 0) t = hubui::ini_set(t, sec, std::string(st.second) + "/Dead Zone", fmt_g(it->second.deadzone * 100));
        }
        t = hubui::ini_set(t, sec, "Rumble/Motor", "`Motor L`|`Motor R`");
    }
    return t;
}
std::string dolphin_pad_ini(const std::string& in, const DolphinPads& p) { return dolphin_pad_ini(in, p, hubctl::default_profile("gc")); }
static std::string dolphin_sel(const DolphinPads& p) {
    std::string s;
    for (int i = 0; i < 4; ++i) if (!p.dev[i].empty()) s += std::to_string(i) + "=" + p.dev[i] + ";";
    return s;
}

void prepare_dolphin(const std::string& install_dir, const DolphinPads& pads, const hubctl::Profile& prof) {
    std::string cfg = join(install_dir, "User\\Config");
    make_dirs(cfg);
    if (!file_exists(join(install_dir, "portable.txt"))) write_text(join(install_dir, "portable.txt"), "");
    /* Written once (marker), so settings changed later in Dolphin's own dialogs or by hand are kept: Dolphin.ini defaults,
     * GFX.ini defaults and the pad mappings. The pad mappings are rewritten when the RetroHub profile changed. */
    std::string ini = join(cfg, "Dolphin.ini"), gfx = join(cfg, "GFX.ini"), pad = join(cfg, "GCPadNew.ini");
    std::string marker = join(cfg, "retrohub-defaults.txt");
    std::string want = hubctl::hash_hex(hubctl::mapping_hash(prof)) + pad_suffix(dolphin_sel(pads));
    const char* kText = "RetroHub wrote its default Dolphin settings and pad mappings once. Delete this file to have them written again.\n";
    if (!file_exists(marker)) {
        write_text(ini, dolphin_ini_defaults(read_text(ini)));
        write_text(gfx, dolphin_gfx_ini_patch(read_text(gfx)));
        write_text(pad, dolphin_pad_ini(read_text(pad), pads, prof));
        write_text(marker, std::string(kText) + "profile-hash: " + want + "\n");
    } else if (marker_hash(read_text(marker), "gc") != want) {
        write_text(pad, dolphin_pad_ini(read_text(pad), pads, prof));
        write_text(marker, std::string(kText) + "profile-hash: " + want + "\n");
    }
    /* Every launch: only the memory card slots and which ports have a pad plugged in. */
    write_text(ini, dolphin_ini_ports(read_text(ini), pads));
    write_text(join(cfg, "Hotkeys.ini"), dolphin_hotkeys_ini(read_text(join(cfg, "Hotkeys.ini"))));
    write_text(join(cfg, "DSUClient.ini"), dolphin_dsu_ini(read_text(join(cfg, "DSUClient.ini"))));
}
void prepare_dolphin(const std::string& install_dir, const DolphinPads& pads) { prepare_dolphin(install_dir, pads, stored_profile("gc")); }

/* True when a process whose image is exactly this file is running (is_running matches the bare file name, so an unrelated
 * Dolphin.exe elsewhere would wrongly keep an old download alive and make the cleanup depend on what else runs). */
static bool running_from(const std::string& exe) {
    std::wstring want = P(exe).wstring();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, P(exe).filename().wstring().c_str()) != 0) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) { found = true; continue; }   // cannot tell: be careful and treat it as running
        wchar_t buf[MAX_PATH * 2];
        DWORD n = (DWORD)(sizeof buf / sizeof buf[0]);
        if (QueryFullProcessImageNameW(h, 0, buf, &n) && _wcsicmp(buf, want.c_str()) == 0) found = true;
        CloseHandle(h);
    }
    CloseHandle(snap);
    return found;
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
        !file_exists(join(old_inst, "retrohub-install.txt")) && !running_from(join(old_inst, "Dolphin.exe")))
        remove_tree(old_inst);
}

std::string pcsx2_safe_key(const std::string& key) {
    std::string k = key;
    for (char& ch : k) if (!isalnum((unsigned char)ch) && ch != '-' && ch != '_' && ch != '.') ch = '_';
    return k;
}
std::string pcsx2_card_dir_rel(const std::string& key) { return "memcards/RetroHub/" + pcsx2_safe_key(key); }

std::string dolphin_card_root(const std::string& install_dir, const std::string& key) {
    std::string k = key;
    for (char& ch : k) if (!isalnum((unsigned char)ch) && ch != '-' && ch != '_' && ch != '.') ch = '_';
    return join(install_dir, "User\\GC\\RetroHub\\" + k);
}

/* ---- per-game settings (src/common/game_settings.h): applied just before each launch ---- */
std::string dolphin_gfx_apply(const std::string& in, const hub::gs::Values& v) {
    std::string t = in;
    auto g = [&](const char* k, const char* d) { auto i = v.find(k); return i == v.end() ? std::string(d) : i->second; };
    t = hubui::ini_set(t, "Settings", "InternalResolution", g("gc.res", "2"));
    t = hubui::ini_set(t, "Settings", "wideScreenHack", g("gc.widescreen", "false") == "true" ? "True" : "False");
    return t;
}

std::string azahar_config_apply(const std::string& in, const hub::gs::Values& v) {
    std::string t = in;
    auto g = [&](const char* k, const char* d) { auto i = v.find(k); return i == v.end() ? std::string(d) : i->second; };
    auto set = [&](const char* sec, const std::string& key, const std::string& val) {
        t = hubui::ini_set(t, sec, key + "\\default", "false");
        t = hubui::ini_set(t, sec, key, val);
    };
    set("Renderer", "resolution_factor", g("3ds.res", "1"));
    std::string l = g("3ds.layout", "default");
    set("Layout", "layout_option", l == "single" ? "1" : l == "large" ? "2" : l == "side" ? "3" : l == "hybrid" ? "5" : "0");
    return t;
}

std::string cemu_settings_apply(const std::string& in, const hub::gs::Values& v) {
    /* Cemu has no switch for graphic packs; the <GraphicPack> list (the user's enabled packs) is renamed away and back,
     * so turning them off never loses the selection. */
    auto i = v.find("wiiu.graphic_packs");
    bool on = i == v.end() || i->second != "false";
    std::string t = in;
    auto rename = [&](const std::string& from, const std::string& to) {
        for (const char* pre : {"<", "</"}) {
            std::string a = std::string(pre) + from + ">", b = std::string(pre) + to + ">";
            for (size_t p = 0; (p = t.find(a, p)) != std::string::npos; p += b.size()) t.replace(p, a.size(), b);
        }
    };
    if (on) rename("GraphicPack_off_by_retrohub", "GraphicPack");
    else rename("GraphicPack", "GraphicPack_off_by_retrohub");
    return t;
}

void apply_game_settings(Kind k, const std::string& install_dir, const hub::gs::Values& v) {
    if (k == Kind::Dolphin) {
        std::string f = join(install_dir, "User\\Config\\GFX.ini");
        make_dirs(join(install_dir, "User\\Config"));
        write_text(f, dolphin_gfx_apply(read_text(f), v));
    } else if (k == Kind::Azahar) {
        std::string d = join(install_dir, "user\\config");
        make_dirs(d);
        std::string f = join(d, "qt-config.ini");
        write_text(f, azahar_config_apply(read_text(f), v));
    } else if (k == Kind::Pcsx2) {
        return;   // written by prepare_pcsx2 (pcsx2.h)
    } else {   // RetroHub's own portable Cemu (a user's own install goes through prepare_cemu_own_settings)
        std::string f = join(install_dir, "portable\\settings.xml");
        if (file_exists(f)) write_text(f, cemu_fullscreen_apply(cemu_settings_apply(read_text(f), v), hub::gs::is_fullscreen(v)));
    }
}

/* ---- Wii U: the user's own Cemu ---- */

static std::string cemu_tag_value(const std::string& xml, const std::string& tag) {
    std::string a = "<" + tag + ">", b = "</" + tag + ">";
    size_t p = xml.find(a);
    if (p == std::string::npos) return "";
    size_t e = xml.find(b, p);
    if (e == std::string::npos) return "";
    return xml.substr(p + a.size(), e - p - a.size());
}

std::string cemu_fullscreen_value(const std::string& xml) {
    std::string v = cemu_tag_value(xml, "fullscreen");
    return v == "true" || v == "false" ? v : std::string();
}

std::string cemu_fullscreen_apply(const std::string& in, bool fs) {
    const std::string val = fs ? "true" : "false";
    std::string t = in;
    size_t p = t.find("<fullscreen>");
    if (p != std::string::npos) {
        size_t e = t.find("</fullscreen>", p);
        if (e != std::string::npos) { t.replace(p + 12, e - p - 12, val); return t; }
    }
    size_t sc = t.find("<fullscreen/>");
    if (sc != std::string::npos) { t.replace(sc, 13, "<fullscreen>" + val + "</fullscreen>"); return t; }
    size_t end = t.rfind("</content>");
    if (end == std::string::npos) return t;   // not a Cemu settings file: leave it alone
    t.insert(end, "\t<fullscreen>" + val + "</fullscreen>\n");
    return t;
}

/* The <GraphicPack>...</GraphicPack> element (empty string when absent or self-closing = nothing enabled). */
static bool cemu_gp_span(const std::string& xml, size_t* from, size_t* to) {
    size_t p = xml.find("<GraphicPack>");
    if (p == std::string::npos) return false;
    size_t e = xml.find("</GraphicPack>", p);
    if (e == std::string::npos) return false;
    *from = p;
    *to = e + 14;
    return true;
}

std::string cemu_own_settings_apply(const std::string& in, const hub::gs::Values& v, CemuOwnState* st) {
    std::string t = in;
    const bool fs = hub::gs::is_fullscreen(v);
    st->fs_orig = cemu_fullscreen_value(t);
    t = cemu_fullscreen_apply(t, fs);
    st->fs_written = fs ? "true" : "false";
    st->fs_touched = true;
    auto i = v.find("wiiu.graphic_packs");
    if (i != v.end() && i->second == "false") {
        size_t a, b;
        if (cemu_gp_span(t, &a, &b)) {   // the user's selection is parked in the state file, Cemu sees no packs
            st->gp_block = t.substr(a, b - a);
            st->gp_stripped = true;
            t.replace(a, b - a, "<GraphicPack/>");
        }
    }
    return t;
}

std::string cemu_own_settings_revert(const std::string& in, const CemuOwnState& st) {
    std::string t = in;
    if (st.fs_touched && cemu_fullscreen_value(t) == st.fs_written)   // unchanged since: the user did not toggle it in the session
        t = cemu_fullscreen_apply(t, st.fs_orig.empty() ? false : st.fs_orig == "true");
    if (st.gp_stripped && !st.gp_block.empty()) {
        size_t sc = t.find("<GraphicPack/>");
        size_t a, b;
        if (sc != std::string::npos) t.replace(sc, 14, st.gp_block);
        else if (cemu_gp_span(t, &a, &b) && t.substr(a, b - a).find("<Entry") == std::string::npos) t.replace(a, b - a, st.gp_block);   // Cemu wrote an empty list
        /* a non-empty list: the user enabled packs during the session; theirs wins */
    }
    return t;
}

static std::string state_esc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '\\') o += "\\\\"; else if (c == '\n') o += "\\n"; else if (c == '\r') o += "\\r"; else o += c; }
    return o;
}
static std::string state_unesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) { char n = s[++i]; o += n == 'n' ? '\n' : n == 'r' ? '\r' : n; }
        else o += s[i];
    }
    return o;
}

std::string cemu_own_state_text(const CemuOwnState& st) {
    return std::string("RetroHub changed these Cemu settings for a game session and puts them back afterwards. Do not edit.\n") +
           "fs_touched=" + (st.fs_touched ? "1" : "0") + "\nfs_orig=" + st.fs_orig + "\nfs_written=" + st.fs_written +
           "\ngp_stripped=" + (st.gp_stripped ? "1" : "0") + "\ngp_block=" + state_esc(st.gp_block) + "\n";
}

bool cemu_own_state_parse(const std::string& text, CemuOwnState* st) {
    CemuOwnState o;
    bool any = false;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "fs_touched") { o.fs_touched = v == "1"; any = true; }
        else if (k == "fs_orig") o.fs_orig = v;
        else if (k == "fs_written") o.fs_written = v;
        else if (k == "gp_stripped") o.gp_stripped = v == "1";
        else if (k == "gp_block") o.gp_block = state_unesc(v);
    }
    if (any) *st = o;
    return any;
}

bool revert_cemu_own_settings(const std::string& user_data) {
    std::string sf = join(user_data, "retrohub-cemu-state.txt"), settings = join(user_data, "settings.xml");
    if (!file_exists(sf)) return false;
    CemuOwnState st;
    bool ok = cemu_own_state_parse(read_text(sf), &st);
    if (ok && file_exists(settings)) write_text(settings, cemu_own_settings_revert(read_text(settings), st));
    std::error_code ec;
    fs::remove(P(sf), ec);
    return ok;
}

bool prepare_cemu_own_settings(const std::string& user_data, const hub::gs::Values& v) {
    std::string settings = join(user_data, "settings.xml");
    if (!file_exists(settings)) return false;   // Cemu has not run yet: nothing of the user's to protect, and the first-run dialog stays theirs
    revert_cemu_own_settings(user_data);        // leftovers of a session that never finished
    std::string bak = settings + ".retrohub-backup";
    std::string orig = read_text(settings);
    if (!file_exists(bak) && !write_text(bak, orig)) return false;   // never change the file without a backup
    CemuOwnState st;
    std::string out = cemu_own_settings_apply(orig, v, &st);
    if (!write_text(join(user_data, "retrohub-cemu-state.txt"), cemu_own_state_text(st))) return false;
    return write_text(settings, out);
}

std::string cemu_profile_apply(const std::string& in, const hub::gs::Values& v) {
    std::string t = in;
    auto i = v.find("wiiu.cpu_mode");
    const char* mode = i == v.end() ? "" : hub::wiiu_cpu_mode_ini(i->second);
    if (*mode) t = hub::ini_put(t, "CPU", "cpuMode", mode);
    return t;
}

bool apply_cemu_game_profile(const std::string& user_data, uint64_t tid, const hub::gs::Values& v) {
    if (!tid) return false;
    std::string dir = join(user_data, "gameProfiles"), f = join(dir, hub::wiiu_profile_file_name(tid));
    revert_cemu_game_profile(user_data, tid);
    std::string cur = file_exists(f) ? read_text(f) : std::string();
    std::string out = cemu_profile_apply(cur, v);
    if (out == cur) return false;   // nothing to set: leave the user's profile alone
    make_dirs(dir);
    if (file_exists(f)) { if (!write_text(f + ".retrohub-orig", cur)) return false; }
    else if (!write_text(f + ".retrohub-new", "RetroHub created " + hub::wiiu_profile_file_name(tid) + " for a game session and removes it afterwards.\n")) return false;
    return write_text(f, out);
}

bool revert_cemu_game_profile(const std::string& user_data, uint64_t tid) {
    if (!tid) return false;
    std::string f = join(join(user_data, "gameProfiles"), hub::wiiu_profile_file_name(tid));
    std::error_code ec;
    if (file_exists(f + ".retrohub-orig")) {
        bool ok = write_text(f, read_text(f + ".retrohub-orig"));
        if (ok) fs::remove(P(f + ".retrohub-orig"), ec);
        return ok;
    }
    if (file_exists(f + ".retrohub-new")) { fs::remove(P(f), ec); fs::remove(P(f + ".retrohub-new"), ec); return true; }
    return false;
}

std::string cemu_profile_summary(const std::string& exe_dir, const std::string& user_data, uint64_t tid) {
    if (!tid) return "";
    std::string name = hub::wiiu_profile_file_name(tid);
    auto find_default = [&]() {   // Cemu ships names in mixed case; match case-insensitively
        std::error_code ec;
        std::string want = name;
        for (char& c : want) c = (char)std::tolower((unsigned char)c);
        for (fs::directory_iterator it(P(join(join(exe_dir, "gameProfiles"), "default")), ec), end; !ec && it != end; it.increment(ec)) {
            std::string n = S(it->path().filename());
            for (char& c : n) c = (char)std::tolower((unsigned char)c);
            if (n == want) return S(it->path());
        }
        return std::string();
    };
    std::string dp = find_default(), up = join(join(user_data, "gameProfiles"), name);
    std::string d = dp.empty() ? "" : read_text(dp), u = file_exists(up) ? read_text(up) : std::string();
    if (d.empty() && u.empty()) return "";
    std::string cpu = hub::wiiu_profile_value(d, u, "CPU", "cpuMode"), mul = hub::wiiu_profile_value(d, u, "Graphics", "accurateShaderMul"),
                so = hub::wiiu_profile_value(d, u, "Graphics", "streamoutBufferCacheSize");
    std::string o;
    if (!cpu.empty()) o += "CPU mode " + cpu;
    if (!mul.empty()) o += std::string(o.empty() ? "" : ", ") + "accurate shader multiplication " + mul;
    if (!so.empty()) o += std::string(o.empty() ? "" : ", ") + "stream-out cache " + so;
    return o.empty() ? "" : o + (u.empty() ? " (Cemu's default game profile)" : " (your game profile)");
}

std::vector<int> cemu_toggle_gamepad_keys() { return {VK_CONTROL, VK_TAB}; }
bool cemu_toggle_combo(unsigned b) { return (b & XINPUT_GAMEPAD_BACK) && (b & XINPUT_GAMEPAD_RIGHT_SHOULDER); }

/* ------------------------------------------------------------------ */
/* launch arguments                                                    */
/* ------------------------------------------------------------------ */

static std::string quote(const std::string& s) { return winutil::quote_arg(s); }

std::string dolphin_args(const std::string& game, bool fullscreen, const std::string& gci, int volume) {
    /* -b: no main window, Esc / closing the render window ends the process. The GCI override pins the memory card to the
     * folder RetroHub syncs with the vault. The game path must be the last argument. */
    return std::string("-b -C Dolphin.Display.Fullscreen=") + (fullscreen ? "True" : "False") + " -C Dolphin.Core.SlotA=8" +
           (volume >= 0 ? " -C Dolphin.DSP.Volume=" + std::to_string(volume > 100 ? 100 : volume) : std::string()) + " -C " +
           quote("Dolphin.Core.GCIFolderAPathOverride=" + gci) + " -e " + quote(game);
}

std::string azahar_args(const std::string& game, bool fullscreen, const std::string& extra) {
    return std::string(fullscreen ? "-f " : "-w ") + (extra.empty() ? "" : extra + " ") + quote(game);   // the game path must be the last argument
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

uint64_t wiiu_title_id_of_file(const std::string& path) {
    std::string low = path;
    for (char& c : low) c = (char)std::tolower((unsigned char)c);
    if (low.size() < 4 || low.compare(low.size() - 4, 4, ".wua") != 0) return 0;
    std::error_code ec;
    uint64_t size = fs::file_size(P(path), ec);
    if (ec) return 0;
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return 0;
    auto rd = [&](uint64_t off, void* buf, size_t n) -> bool {
        f.clear();
        f.seekg((std::streamoff)off, std::ios::beg);
        f.read(static_cast<char*>(buf), (std::streamsize)n);
        return f.gcount() == (std::streamsize)n;
    };
    hub::WiiuInfo w;
    if (hub::parse_wua(rd, size, &w) && w.title_id) return w.title_id;
    hub::WiiuMeta m;
    return hub::wua_read_meta(rd, size, &m) ? m.title_id : 0;   // repacks with a 0000000000000000_v0 folder: the id is in meta.xml
}

hub::FsProbe wiiu_probe() { return probe(); }

SaveSession begin_session(const hub::GameEntry& e, const Install& inst, const std::string& battery_in, const std::string&, uint64_t tid_override) {
    SaveSession s;
    s.started_unix = (int64_t)std::time(nullptr);
    std::string battery = battery_in;
    while (!battery.empty() && (battery.back() == '\\' || battery.back() == '/')) battery.pop_back();
    s.battery = battery;
    /* <vault>\<system>\<game>\battery -> <vault>\_conflicts\<system>\<game>\battery */
    fs::path game = P(battery).parent_path(), sysdir = game.parent_path(), vault = sysdir.parent_path();
    s.backup_dir = S(vault / L"_conflicts" / sysdir.filename() / game.filename() / L"battery");
    s.manifest_path = S(vault / L".sync" / (L"emu-" + sysdir.filename().wstring() + L"-" + game.filename().wstring() + L".json"));

    uint64_t tid = tid_override ? tid_override : title_id_from_key(e.key);
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
            for (const char* hi : {"00050000", "0005000e"}) {
                auto known = hub::subdirs_modified_since(join(battery, std::string("usr\\save\\") + hi), 0);
                for (const auto& k : known) s.rels.push_back(std::string("usr/save/") + hi + "/" + k);   // every folder an earlier session stored
            }
            s.wiiu_unknown_title = true;
        } else {
            s.rels = hub::wiiu_save_rels(tid);   // the game's save and its update title's; every account folder sits inside
        }
    } else if (e.system == hub::System::PS2) {
        if (inst.dir.empty()) return s;
        s.emu_root = inst.dir;   // <install>/memcards/RetroHub/<game> <-> <vault>/ps2/<game>/battery/memcards/RetroHub/<game>
        s.rels = {pcsx2_card_dir_rel(e.key)};
        make_dirs(join(s.emu_root, s.rels[0]));
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
        for (const char* hi : {"00050000", "0005000e"}) {
            std::string parent = join(S(P(s.wiiu_save_parent).parent_path()), hi);
            for (const auto& c : hub::subdirs_modified_since(parent, s.started_unix - 2)) {
                std::string rel = std::string("usr/save/") + hi + "/" + c;
                if (std::find(s.rels.begin(), s.rels.end(), rel) == s.rels.end()) s.rels.push_back(rel);
            }
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
