/* RetroHub auto-updater. See updater.h. Ported from Smash Classic's Updater.cpp. */
#include "winutil.h"
#include "json.h"
#include "updater.h"
#include "games.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include "hub_version.h"

namespace hubupdate {

namespace {

constexpr const char* kLatestReleaseApi = "https://api.github.com/repos/fatty8me2/smash-classic/releases/latest";
constexpr const char* kDownloadPrefix = "https://github.com/fatty8me2/smash-classic/releases/download/";
constexpr const char* kPlatformAssetName = "BattleShip-windows.zip";
constexpr const char* kSumsAssetName = "SHA256SUMS.txt";
constexpr const char* kManualUrl = "https://github.com/fatty8me2/retrohub/releases/latest";

std::atomic<bool> s_checked{false};
std::atomic<bool> s_available{false};
std::atomic<bool> s_checking{false};
std::atomic<bool> s_failed{false};
std::atomic<bool> s_downloading{false};
std::atomic<bool> s_started{false};
std::atomic<float> s_progress{-1.0f};

std::string s_latest, s_notes, s_url, s_digest, s_sums_url, s_status;
long long s_size = 0;
std::mutex s_mu;

/* Every path crosses the Win32 boundary as UTF-16 (the *W APIs and wide
 * streams). The ANSI (*A) APIs and narrow CRT calls would mangle paths that
 * contain non-ASCII characters, e.g. a profile folder with an accent. */
std::wstring W(const std::string& s) { return winutil::to_wide(s); }
std::string U8(const std::wstring& s) { return winutil::to_utf8(s); }
std::filesystem::path P(const std::string& s) { return std::filesystem::path(W(s)); }
void rm_file(const std::string& p) { DeleteFileW(W(p).c_str()); }

void set_status(const std::string& s) { std::lock_guard<std::mutex> l(s_mu); s_status = s; }

std::string trim_slash(std::string s) {
    while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    return s;
}

void ulog(const char* fmt, ...) {
    std::string dir = hub::data_dir() + "logs";
    CreateDirectoryW(W(dir).c_str(), nullptr);
    FILE* f = _wfopen(W(dir + "\\updater.log").c_str(), L"ab");
    if (!f) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SYSTEMTIME t; GetLocalTime(&t);
    std::fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, buf);
    std::fclose(f);
}

/* ---------------- JSON: shared reader (src/common/json.h); GitHub's payload is an object, extra bytes tolerated ---------------- */
using JV = json::Value;

bool json_parse(const std::string& text, JV* out) {
    json::ParseOptions o;
    o.allow_trailing = true;
    return json::parse(text, out, o) && out->type == JV::Object;
}

/* "v1.2.3" / "1.2" -> {1,2,3}; anything non-numeric ends the version. */
std::vector<int> parse_version(const std::string& v) {
    std::vector<int> parts;
    size_t at = (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? 1 : 0;
    while (at < v.size()) {
        size_t end = at;
        while (end < v.size() && v[end] >= '0' && v[end] <= '9') end++;
        if (end == at) break;
        parts.push_back(std::atoi(v.substr(at, end - at).c_str()));
        if (end >= v.size() || v[end] != '.') break;
        at = end + 1;
    }
    return parts;
}

int run_capture(std::string cmd, const std::function<void(const std::string&)>& onLine) {
    HANDLE rd = nullptr, wr = nullptr;
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr; si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring wcmd = W(cmd);
    BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return -1; }
    std::string line; char buf[512]; DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) {
        for (DWORD i = 0; i < got; ++i) {
            char c = buf[i];
            if (c == '\r' || c == '\n') { if (!line.empty()) { onLine(line); line.clear(); } }
            else line += c;
        }
    }
    if (!line.empty()) onLine(line);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}

std::string sha256_file(const std::string& path) {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr; std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return hex;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::ifstream in(P(path), std::ios::binary);
        std::vector<char> buf(1 << 16);
        bool ok = static_cast<bool>(in);
        while (ok && in) {
            in.read(buf.data(), buf.size());
            std::streamsize n = in.gcount();
            if (n > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), (ULONG)n, 0) != 0) ok = false;
        }
        unsigned char digest[32];
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            static const char* kHex = "0123456789abcdef";
            for (unsigned char b : digest) { hex += kHex[b >> 4]; hex += kHex[b & 15]; }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

/* Growing UTF-16 buffer: long or non-ASCII install paths are not truncated. */
std::string exe_path() { return winutil::exe_path(); }

std::string settings_path() { return hub::data_dir() + "updater.json"; }

std::string read_skipped() {
    std::ifstream in(P(settings_path()), std::ios::binary);
    if (!in) return "";
    std::stringstream ss; ss << in.rdbuf();
    JV v;
    if (!json_parse(ss.str(), &v)) return "";
    const JV* s = v.get("skipped_version");
    return (s && s->type == JV::String) ? s->s : "";
}

} // namespace

/* ---------------- detail (testable) ---------------- */
namespace detail {

int compare_versions(const std::string& a, const std::string& b) {
    std::vector<int> pa = parse_version(a), pb = parse_version(b);
    if (pa.empty() || pb.empty()) return pa.empty() == pb.empty() ? 0 : (pa.empty() ? -1 : 1);
    for (size_t i = 0; i < pa.size() || i < pb.size(); i++) {
        int va = i < pa.size() ? pa[i] : 0, vb = i < pb.size() ? pb[i] : 0;
        if (va != vb) return va < vb ? -1 : 1;
    }
    return 0;
}

/* Only this repo's release downloads are trusted, and the URL is spliced into
 * a command line, so it must not contain quoting characters. */
bool is_trusted_download_url(const std::string& url) {
    if (url.rfind(kDownloadPrefix, 0) != 0) return false;
    for (char c : url)
        if (c == '"' || c == '\'' || c == '\\' || c == '%' || c == '^' || c == '&' || c == '|' ||
            c == '<' || c == '>' || (unsigned char)c <= 0x20 || c == 0x7F) return false;
    return true;
}

std::string strip_markdown(const std::string& md) {
    std::string out;
    std::istringstream in(md);
    std::string line;
    bool fence = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t a = line.find_first_not_of(" \t");
        std::string t = a == std::string::npos ? "" : line.substr(a);
        if (t.rfind("```", 0) == 0) { fence = !fence; continue; }
        if (!fence) {
            size_t h = 0;
            while (h < t.size() && t[h] == '#') h++;
            if (h > 0 && h < t.size() && t[h] == ' ') t = t.substr(h + 1);
            else if ((t.rfind("- ", 0) == 0 || t.rfind("* ", 0) == 0)) t = "- " + t.substr(2);
            else if (t.rfind("> ", 0) == 0) t = t.substr(2);
            // [text](url) -> text ; strip emphasis/code markers and HTML tags
            std::string r;
            for (size_t i = 0; i < t.size(); i++) {
                char c = t[i];
                if (c == '!' && i + 1 < t.size() && t[i + 1] == '[') continue;
                if (c == '[') {
                    size_t e = t.find(']', i);
                    if (e != std::string::npos && e + 1 < t.size() && t[e + 1] == '(') {
                        size_t p = t.find(')', e);
                        if (p != std::string::npos) { r += t.substr(i + 1, e - i - 1); i = p; continue; }
                    }
                }
                if (c == '<') {
                    size_t e = t.find('>', i);
                    if (e != std::string::npos && e - i < 40) { i = e; continue; }
                }
                if (c == '*' || c == '`' || (c == '_' && (i == 0 || t[i - 1] == ' ' || (i + 1 < t.size() && t[i + 1] == '_')))) continue;
                r += c;
            }
            t = r;
        }
        out += t; out += '\n';
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

Release parse_release(const std::string& json) {
    Release r;
    JV v;
    if (!json_parse(json, &v)) return r;
    const JV* tag = v.get("tag_name");
    if (!tag || tag->type != JV::String || tag->s.empty()) return r;
    r.ok = true;
    r.tag = tag->s;
    if (const JV* b = v.get("body")) if (b->type == JV::String) r.body = b->s;
    const JV* assets = v.get("assets");
    if (assets && assets->type == JV::Array) {
        for (const JV& a : assets->a) {
            const JV* name = a.get("name");
            const JV* url = a.get("browser_download_url");
            if (name && name->type == JV::String && name->s == kSumsAssetName && url && url->type == JV::String &&
                is_trusted_download_url(url->s)) { r.sums_url = url->s; continue; }
            if (!name || name->type != JV::String || name->s != kPlatformAssetName || !url || url->type != JV::String) continue;
            const JV* size = a.get("size");
            long long sz = (size && size->type == JV::Number) ? (long long)size->n : 0;
            if (!is_trusted_download_url(url->s) || sz <= 0) continue;
            r.has_asset = true; r.url = url->s; r.size = sz;
            const JV* d = a.get("digest");
            if (d && d->type == JV::String) r.digest = d->s;
        }
    }
    return r;
}

const char* const kFirstChecksumRelease = "v1.17.1";

bool checksums_required(const std::string& tag) { return compare_versions(tag, kFirstChecksumRelease) >= 0; }

static std::string lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a'); return s; }
static bool is_hex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    return true;
}

std::map<std::string, std::string> parse_sha256sums(const std::string& text) {
    std::map<std::string, std::string> m;
    std::istringstream in(text);
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (first && line.rfind("\xEF\xBB\xBF", 0) == 0) line = line.substr(3);   // BOM from Windows PowerShell
        first = false;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() < 66) continue;
        std::string hex = line.substr(0, 64);
        if (!is_hex64(hex) || line[64] != ' ') continue;
        size_t at = 65;
        if (at < line.size() && (line[at] == ' ' || line[at] == '*')) at++;
        std::string name = line.substr(at);
        if (name.empty()) continue;
        hex = lower(hex);
        auto it = m.find(name);
        if (it == m.end()) m[name] = hex;
        else if (it->second != hex) it->second.clear();
    }
    return m;
}

Verdict verify_hashes(const std::string& tag, const std::string& actual, const std::string* sums, const std::string& api_digest) {
    std::string a = lower(actual);
    if (!is_hex64(a)) return Verdict::BadSums;
    if (api_digest.rfind("sha256:", 0) == 0 && lower(api_digest.substr(7)) != a) return Verdict::DigestMismatch;
    if (!sums) return checksums_required(tag) ? Verdict::NoSums : Verdict::OkLegacy;
    auto m = parse_sha256sums(*sums);
    auto it = m.find(kPlatformAssetName);
    if (it == m.end() || !is_hex64(it->second)) return Verdict::BadSums;
    return it->second == a ? Verdict::Ok : Verdict::ZipMismatch;
}

const char* verdict_message(Verdict v) {
    switch (v) {
        case Verdict::Ok: case Verdict::OkLegacy: return "ok";
        case Verdict::NoSums: return "this release has no SHA256SUMS.txt, so the download cannot be verified.";
        case Verdict::BadSums: return "the release checksum file is unreadable or does not list the update.";
        case Verdict::ZipMismatch: return "the download does not match the published checksum (SHA256SUMS.txt).";
        case Verdict::DigestMismatch: return "the download does not match GitHub's checksum for the file.";
    }
    return "verification failed.";
}

std::string sha256_file_hex(const std::string& path) { return sha256_file(path); }

bool download_verified(const FetchSpec& f, const std::string& zipPath, std::string* err,
                       const std::function<void(const std::string&)>& onLine) {
    auto bad = [&](const std::string& m) { rm_file(zipPath); if (err) *err = m; return false; };
    std::string sums; bool haveSums = false;
    if (f.sums_url.empty() && checksums_required(f.tag)) return bad(verdict_message(Verdict::NoSums));
    if (!f.sums_url.empty()) {
        std::string sp = zipPath + ".sums";
        rm_file(sp);
        int c = run_capture("curl -fLsS -m 30 --max-filesize 262144 -o \"" + sp + "\" \"" + f.sums_url + "\"", [](const std::string&) {});
        if (c == 0) {
            std::ifstream in(P(sp), std::ios::binary);
            std::stringstream ss; ss << in.rdbuf();
            sums = ss.str(); haveSums = !sums.empty();
        }
        rm_file(sp);
        /* A listed-but-unfetchable checksum file is never treated as "legacy". */
        if (!haveSums) return bad("could not download SHA256SUMS.txt to verify the update.");
    }
    rm_file(zipPath);
    if (run_capture("curl -fL -# -m 600 -o \"" + zipPath + "\" \"" + f.zip_url + "\"", onLine) != 0) return bad("download failed.");
    std::ifstream in(P(zipPath), std::ios::binary | std::ios::ate);
    long long actual = in ? (long long)in.tellg() : -1;
    in.close();
    if (actual != f.size) return bad("download was incomplete.");
    Verdict v = verify_hashes(f.tag, sha256_file(zipPath), haveSums ? &sums : nullptr, f.digest);
    if (v != Verdict::Ok && v != Verdict::OkLegacy) return bad(verdict_message(v));
    if (v == Verdict::OkLegacy) ulog("release %s predates SHA256SUMS.txt; accepted on TLS + size (+ API digest if present)", f.tag.c_str());
    return true;
}

} // namespace detail

/* ---------------- public API ---------------- */
std::string current_version() { return HUB_VERSION_STR; }

bool launch_check_disabled() {
    for (const char* name : {"RETROHUB_NO_UPDATE_CHECK", "SSB64_NO_UPDATE_CHECK"}) {
        const char* env = std::getenv(name);
        if (env && env[0] != '\0' && env[0] != '0') return true;
    }
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    for (int i = 1; argv && i < argc; i++)
        if (wcscmp(argv[i], L"--no-update-check") == 0) found = true;
    if (argv) LocalFree(argv);
    return found;
}

void check_async(bool force) {
    if (s_checking.load() || s_downloading.load()) return;
    if (!force && s_checked.load()) return;
    if (!force && launch_check_disabled()) {
        s_checked.store(true);
        set_status("Update check disabled.");
        return;
    }
    s_checked.store(true);
    s_checking.store(true);
    s_failed.store(false);
    s_available.store(false);
    set_status("");

    std::thread([]() {
        std::string response;
        std::string cmd = std::string("curl -fLsS -m 10 -H \"User-Agent: RetroHub-Updater\" "
                                      "-H \"Accept: application/vnd.github+json\" \"") + kLatestReleaseApi + "\"";
        int code = run_capture(cmd, [&](const std::string& l) { response += l; });
        if (code != 0 || response.empty()) {
            ulog("check failed (curl exit %d)", code);
            s_failed.store(true);
            set_status("Unable to check for updates.");
            s_checking.store(false);
            return;
        }
        detail::Release rel = detail::parse_release(response);
        if (!rel.ok) {
            ulog("check failed: unreadable release JSON");
            s_failed.store(true);
            set_status("Unable to read update information.");
            s_checking.store(false);
            return;
        }
        {
            std::lock_guard<std::mutex> l(s_mu);
            s_latest = rel.tag;
            s_notes = detail::strip_markdown(rel.body);
            s_url.clear(); s_digest.clear(); s_sums_url.clear(); s_size = 0;
        }
        std::string cur = current_version();
        if (detail::compare_versions(rel.tag, cur) <= 0) {
            set_status("Up to date (" + cur + ").");
            ulog("up to date: have %s, latest %s", cur.c_str(), rel.tag.c_str());
        } else if (rel.has_asset) {
            {
                std::lock_guard<std::mutex> l(s_mu);
                s_url = rel.url; s_size = rel.size; s_digest = rel.digest; s_sums_url = rel.sums_url;
            }
            s_available.store(true);
            set_status("");
            ulog("update available: %s (have %s)", rel.tag.c_str(), cur.c_str());
        } else {
            s_failed.store(true);
            set_status("Latest release has no update for this platform.");
            ulog("latest %s has no usable %s asset", rel.tag.c_str(), kPlatformAssetName);
        }
        s_checking.store(false);
    }).detach();
}

void start_update() {
    bool expected = false;
    if (!s_downloading.compare_exchange_strong(expected, true)) return;
    s_progress.store(-1.0f);

    std::string url, digest, sumsUrl, version; long long size;
    {
        std::lock_guard<std::mutex> l(s_mu);
        url = s_url; digest = s_digest; sumsUrl = s_sums_url; version = s_latest; size = s_size;
    }
    if (url.empty()) {
        set_status("Error: No update download available.");
        s_downloading.store(false);
        return;
    }
    set_status("Starting download...");

    std::thread([url, digest, sumsUrl, version, size]() {
        auto fail = [](const std::string& m) {
            ulog("update failed: %s", m.c_str());
            set_status("Error: " + m);
            s_downloading.store(false);
        };
        std::string exePath = exe_path();
        std::string exeDir = trim_slash(hub::exe_dir());
        if (exePath.empty() || exeDir.empty()) { fail("could not locate the install folder."); return; }
        std::string updatesDir = exeDir + "\\updates";
        CreateDirectoryW(W(updatesDir).c_str(), nullptr);
        std::string zipPath = updatesDir + "\\update.zip";
        rm_file(zipPath);

        auto onLine = [](const std::string& line) {
            size_t p = line.rfind('%');
            if (p == std::string::npos) return;
            size_t b = p;
            while (b > 0 && ((line[b - 1] >= '0' && line[b - 1] <= '9') || line[b - 1] == '.')) b--;
            if (b == p) return;
            float pct = (float)std::atof(line.substr(b, p - b).c_str());
            if (pct >= 0.0f && pct <= 100.0f) {
                s_progress.store(pct / 100.0f);
                char t[64]; std::snprintf(t, sizeof(t), "Downloading... %.0f%%", pct);
                set_status(t);
            }
        };
        ulog("downloading %s", url.c_str());
        detail::FetchSpec spec; spec.zip_url = url; spec.sums_url = sumsUrl; spec.tag = version; spec.digest = digest; spec.size = size;
        std::string verr;
        if (!detail::download_verified(spec, zipPath, &verr, onLine)) {
            fail(verr + " Download manually: " + kManualUrl);
            return;
        }
        set_status("Verified.");
        /* Unpack the zip (Windows' own tar.exe, hidden), then hand over to a TEMP copy of
         * RetroHub.exe running `--apply-update` (see update_apply.h). */
        set_status("Preparing...");
        std::wstring systemRoot = L"C:\\Windows";
        if (DWORD need = GetEnvironmentVariableW(L"SystemRoot", nullptr, 0)) {
            std::wstring b(need, L'\0');
            DWORD got = GetEnvironmentVariableW(L"SystemRoot", &b[0], need);
            if (got > 0 && got < need) { b.resize(got); systemRoot = b; }
        }
        std::string staged = updatesDir + "\\staged";
        std::error_code fec;
        std::filesystem::remove_all(P(staged), fec);
        std::filesystem::create_directories(P(staged), fec);
        std::string tarCmd = "\"" + U8(systemRoot) + "\\System32\\tar.exe\" -xf \"" + zipPath + "\" -C \"" + staged + "\"";
        int tarCode = run_capture(tarCmd, [](const std::string&) {});
        rm_file(zipPath);
        if (tarCode != 0) { std::filesystem::remove_all(P(staged), fec); fail("could not unpack the update."); return; }
        for (const char* req : {"RetroHub.exe", "BattleShip.exe"})
            if (GetFileAttributesW(W(staged + "\\" + req).c_str()) == INVALID_FILE_ATTRIBUTES) {
                std::filesystem::remove_all(P(staged), fec);
                fail(std::string("the update is missing ") + req);
                return;
            }

        /* TEMP copy of this exe (+ its DLLs) so the install's RetroHub.exe is not locked. */
        wchar_t tmpBuf[MAX_PATH + 1]; DWORD tn = GetTempPathW(MAX_PATH, tmpBuf);
        std::filesystem::path tmpRoot = tn ? std::filesystem::path(std::wstring(tmpBuf, tn)) : P(updatesDir);
        for (auto& e : std::filesystem::directory_iterator(tmpRoot, fec))   // best-effort: stale copies of earlier updates
            if (e.path().filename().wstring().rfind(L"RetroHub-apply-", 0) == 0) {
                std::error_code e2; std::filesystem::remove_all(e.path(), e2);
            }
        std::filesystem::path applyDir = tmpRoot / (L"RetroHub-apply-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(applyDir, fec);
        std::filesystem::path applyExe = applyDir / L"RetroHub.exe";
        std::filesystem::copy_file(P(exePath), applyExe, std::filesystem::copy_options::overwrite_existing, fec);
        if (fec) { fail("could not prepare the installer."); return; }
        for (auto& e : std::filesystem::directory_iterator(P(exeDir), fec)) {
            std::wstring ext = e.path().extension().wstring();
            for (auto& c : ext) c = (wchar_t)towlower(c);
            std::error_code e2;
            if (ext == L".dll") std::filesystem::copy_file(e.path(), applyDir / e.path().filename(), std::filesystem::copy_options::overwrite_existing, e2);
        }

        std::wstring marker = W(updatesDir + "\\apply.started");
        DeleteFileW(marker.c_str());
        std::wstring wcmd = L"\"" + applyExe.wstring() + L"\" --apply-update \"" + W(staged) + L"\" \"" + W(exeDir) + L"\" " +
                            std::to_wstring(GetCurrentProcessId()) + L" --version \"" + W(version) + L"\" --started-marker \"" + marker + L"\"";
        STARTUPINFOW si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::wstring wdir = applyDir.wstring();
        std::wstring cmdBreakaway = wcmd;
        if (!CreateProcessW(nullptr, cmdBreakaway.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, nullptr, wdir.c_str(), &si, &pi) &&
            !CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                            wdir.c_str(), &si, &pi)) {
            fail("could not start the installer.");
            return;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        /* Quit only once the installer is confirmed running; it waits for this
         * process to exit before touching any files. */
        bool started = false;
        for (int i = 0; i < 300 && !started; i++) {   // the applier can take a moment to cold-start
            Sleep(100);
            started = GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES;
        }
        if (!started) { fail("the installer did not start."); return; }
        set_status("Installing " + version + " - RetroHub will restart...");
        ulog("installer started for %s, quitting", version.c_str());
        s_progress.store(1.0f);
        s_started.store(true);
        s_downloading.store(false);
    }).detach();
}

void skip_latest() {
    std::string v = latest_version();
    if (v.empty()) return;
    std::string esc;
    for (char c : v) { if (c == '"' || c == '\\') esc += '\\'; esc += c; }
    std::ofstream out(P(settings_path()), std::ios::binary | std::ios::trunc);
    if (out) out << "{\"skipped_version\":\"" << esc << "\"}\n";
}

bool latest_skipped() {
    std::string v = latest_version();
    return !v.empty() && v == read_skipped();
}

bool checking() { return s_checking.load(); }
bool update_available() { return s_available.load(); }
bool check_failed() { return s_failed.load(); }
bool downloading() { return s_downloading.load(); }
float download_progress() { return s_progress.load(); }
bool installer_started() { return s_started.load(); }
std::string manual_download_url() { return kManualUrl; }
std::string status() { std::lock_guard<std::mutex> l(s_mu); return s_status; }
std::string latest_version() { std::lock_guard<std::mutex> l(s_mu); return s_latest; }
std::string release_notes() { std::lock_guard<std::mutex> l(s_mu); return s_notes; }

} // namespace hubupdate
