#include "gamepack.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "friend_games.h"   // hubui::fg::norm_title
#include "games.h"          // hub::data_dir
#include "json.h"           // community pack list (data/packs.json)

namespace fs = std::filesystem;

static fs::path P(const std::string& s) { return fs::u8path(s); }
static std::wstring W(const std::string& s) { return P(s).wstring(); }
static std::string S(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }

namespace hubgp {

const char* root_folder() { return "1jp71Fx_VB1-fa1pZke-7eABC1pJzpu2z"; }

const char* sys_folder(const std::string& s) {
    if (s == "n64") return "19_2xjH7Ps7epabxUrr9sfxz69kuJGp3W";
    if (s == "gc") return "17AvjquyVOjSs0ua1LnLZGRN3B2bUYQar";
    if (s == "gba") return "10zAwtXq1STlpfdIpCDppwXwpKhK3gyRP";
    if (s == "gbc") return "1idefowNapO3KpwE4nRv8zqrpCeEZFgpc";
    if (s == "nds") return "1FVsjeQMBAcxw-_gRU4MSSRLQpIOD3ygu";
    if (s == "ps1") return "19H5x-ZY6-8HHUl0g3XI0NEsW2e6ROWzr";
    return "";
}

std::string decode_entities(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o += s[i]; continue; }
        size_t sc = s.find(';', i + 1);
        if (sc == std::string::npos || sc - i > 12) { o += s[i]; continue; }
        std::string e = s.substr(i + 1, sc - i - 1);
        char c = 0;
        unsigned cp = 0;
        bool known = false;
        if (e == "amp") { c = '&'; known = true; }
        else if (e == "lt") { c = '<'; known = true; }
        else if (e == "gt") { c = '>'; known = true; }
        else if (e == "quot") { c = '"'; known = true; }
        else if (e == "apos" || e == "#39") { c = '\''; known = true; }
        else if (!e.empty() && e[0] == '#') {   // &#NNN; / &#xHH;
            bool hex = e.size() > 2 && (e[1] == 'x' || e[1] == 'X');
            size_t k = hex ? 2 : 1;
            if (k < e.size()) {
                known = true;
                for (; k < e.size(); ++k) {
                    char d = e[k];
                    int v = (d >= '0' && d <= '9') ? d - '0' : (d >= 'a' && d <= 'f') ? d - 'a' + 10 : (d >= 'A' && d <= 'F') ? d - 'A' + 10 : -1;
                    if (v < 0 || v >= (hex ? 16 : 10)) { known = false; break; }
                    cp = cp * (hex ? 16 : 10) + (unsigned)v;
                }
                if (!(known && cp >= 1 && cp <= 0x10FFFF)) known = false;
            }
        }
        if (!known) { o += s[i]; continue; }
        if (c) o += c;
        else {   // UTF-8 encode the code point
            unsigned v = cp;
            if (v < 0x80) o += (char)v;
            else if (v < 0x800) { o += (char)(0xC0 | (v >> 6)); o += (char)(0x80 | (v & 63)); }
            else if (v < 0x10000) { o += (char)(0xE0 | (v >> 12)); o += (char)(0x80 | ((v >> 6) & 63)); o += (char)(0x80 | (v & 63)); }
            else { o += (char)(0xF0 | (v >> 18)); o += (char)(0x80 | ((v >> 12) & 63)); o += (char)(0x80 | ((v >> 6) & 63)); o += (char)(0x80 | (v & 63)); }
        }
        i = sc;
    }
    return o;
}

/* Drive ids are [A-Za-z0-9_-]; anything else from the scraped HTML must not reach a URL path. */
static bool valid_id(const std::string& id) {
    if (id.empty() || id.size() > 128) return false;
    for (char c : id) if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) return false;
    return true;
}

std::vector<Entry> parse_folder_html(const std::string& html) {
    std::vector<Entry> out;
    size_t pos = 0;
    while ((pos = html.find("id=\"entry-", pos)) != std::string::npos) {
        size_t id_end = html.find('"', pos + 10);   // the closing quote of the entry id
        if (id_end == std::string::npos) break;
        Entry e;
        e.id = html.substr(pos + 10, id_end - pos - 10);
        size_t next = html.find("id=\"entry-", id_end + 1);
        std::string chunk = html.substr(id_end, next == std::string::npos ? std::string::npos : next - id_end);
        size_t t = chunk.find("flip-entry-title\">");
        if (t != std::string::npos) {
            t += std::strlen("flip-entry-title\">");
            size_t te = chunk.find("</div>", t);
            e.name = decode_entities(chunk.substr(t, te == std::string::npos ? std::string::npos : te - t));
        }
        size_t f = chunk.find("drive/folders/");
        size_t fl = chunk.find("file/d/");
        e.is_folder = f != std::string::npos && (fl == std::string::npos || f < fl);
        if (!e.id.empty() && !e.name.empty() && (f != std::string::npos || fl != std::string::npos)) out.push_back(std::move(e));
        pos = id_end + 1;
    }
    return out;
}

std::string safe_name(const std::string& t) {
    std::string o;
    for (char ch : t) {
        unsigned char c = (unsigned char)ch;
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*' || c < 0x20) o += ' ';
        else o += ch;
    }
    std::string r;
    for (char ch : o) { if (ch == ' ' && !r.empty() && r.back() == ' ') continue; r += ch; }
    while (!r.empty() && (r.front() == ' ' || r.front() == '.')) r.erase(r.begin());
    while (!r.empty() && (r.back() == ' ' || r.back() == '.')) r.pop_back();
    if (r.empty()) r = "game";
    static const char* dev[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
                                "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    std::string stem = r.substr(0, r.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    for (char& c : stem) c = (char)std::tolower((unsigned char)c);
    for (const char* d : dev) if (stem == d) { r = "_" + r; break; }   // Windows device names (NUL.gba) cannot be files
    return r;
}

static std::string lower_name(const std::string& s) {
    std::string o = s;
    for (char& c : o) c = (char)std::tolower((unsigned char)c);
    return o;
}

bool console_code(const std::string& name) {
    static const char* codes[] = {"n64", "gc", "wiiu", "3ds", "nds", "gba", "gbc", "gb", "ps1"};
    std::string l = lower_name(name);
    for (const char* c : codes) if (l == c) return true;
    return false;
}

std::vector<std::string> parse_links(const std::string& text) {
    std::vector<std::string> out;
    auto push = [&](const char* p, size_t len) {
        if (len < 10 || len > 80) return;
        for (size_t i = 0; i < len; ++i) { char c = p[i]; if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-')) return; }
        std::string id(p, len);
        for (const std::string& s : out) if (s == id) return;
        out.push_back(std::move(id));
    };
    auto take = [&](size_t start) {   // read one id token starting at `start`
        size_t i = start;
        while (i < text.size()) { char c = text[i]; if (std::isalnum((unsigned char)c) || c == '_' || c == '-') ++i; else break; }
        push(text.data() + start, i - start);
    };
    size_t pos = 0;
    while (pos < text.size()) {   // ids inside Drive URLs, earliest marker first
        size_t a = text.find("folders/", pos);
        size_t b = text.find("folder/", pos);
        size_t c = text.find("embeddedfolderview?id=", pos);
        if (a == std::string::npos && b == std::string::npos && c == std::string::npos) break;
        if (a != std::string::npos && (b == std::string::npos || a < b) && (c == std::string::npos || a < c)) { take(a + 8); pos = a + 9; }
        else if (b != std::string::npos && (c == std::string::npos || b < c)) { take(b + 7); pos = b + 8; }
        else { take(c + 22); pos = c + 23; }   // strlen("embeddedfolderview?id=") == 22
    }
    size_t i = 0;
    while (i < text.size()) {   // bare ids: long tokens of id characters, not part of a URL (preceded by / or :)
        char c = text[i];
        if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-')) { ++i; continue; }
        size_t j = i;
        while (j < text.size()) { char d = text[j]; if (std::isalnum((unsigned char)d) || d == '_' || d == '-') ++j; else break; }
        if (j - i >= 15 && (i == 0 || (text[i - 1] != '/' && text[i - 1] != ':'))) push(text.data() + i, j - i);
        i = j;
    }
    return out;
}

bool parse_packs_json(const std::string& text, std::vector<std::string>* out) {
    if (out) out->clear();
    json::Value v;
    if (!json::parse(text, &v)) return false;
    const json::Value* arr = nullptr;
    if (v.is_array()) arr = &v;
    else if (v.is_object()) { const json::Value* p = v.get("packs"); if (p && p->is_array()) arr = p; }
    if (!arr) return false;
    std::set<std::string> seen;
    for (const json::Value& it : arr->a) {
        std::string id = it.is_object() ? it.str("id") : (it.is_string() ? it.s : std::string());
        if (!valid_id(id) || !seen.insert(id).second) continue;
        if (out) out->push_back(id);
    }
    return true;
}

/* QA only: RETROHUB_QA=1 + RETROHUB_GP_BASE=127.0.0.1:<port> sends every GamePack request (community list, Drive listings,
 * downloads) to a local plain-HTTP fake instead of GitHub / Google Drive. Loopback only; ignored otherwise. */
static bool qa_base(INTERNET_PORT* port) {
    const char* q = std::getenv("RETROHUB_QA");
    const char* b = std::getenv("RETROHUB_GP_BASE");
    if (!q || std::strcmp(q, "1") != 0 || !b || std::strncmp(b, "127.0.0.1:", 10) != 0) return false;
    int p = std::atoi(b + 10);
    if (p <= 0 || p > 65535) return false;
    *port = (INTERNET_PORT)p;
    return true;
}

/* Plain GET that returns the whole body (folder listings are small). */
static bool http_get(const std::wstring& host, const std::wstring& path, std::string* body, std::string* err) {
    HINTERNET ses = WinHttpOpen(L"RetroHub", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!ses) { *err = "network unavailable"; return false; }
    bool ok = false;
    INTERNET_PORT qp = 0;
    bool qa = qa_base(&qp);
    HINTERNET con = WinHttpConnect(ses, qa ? L"127.0.0.1" : host.c_str(), qa ? qp : INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, qa ? 0 : WINHTTP_FLAG_SECURE) : nullptr;
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, sz = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
        if (status != 200) *err = "server answered " + std::to_string(status);
        else {
            ok = true;
            for (;;) {
                std::vector<char> buf(64 * 1024);
                DWORD n = 0;
                if (!WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &n)) { ok = false; *err = "download interrupted"; break; }
                if (n == 0) break;
                if (body->size() + n > (16u << 20)) { ok = false; *err = "listing too large"; break; }   // listings are small; never buffer a huge reply
                body->append(buf.data(), n);
            }
        }
    } else *err = "could not reach Google Drive";
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

/* The shared community pack list: data/packs.json on GitHub (main branch, live). Every client fetches it, so a
 * link added there is installable for everyone without per-user setup. The last good list stays in memory; a
 * failed fetch (offline) keeps using it. Worker threads only (may do one HTTP fetch). */
static const wchar_t* kCommunityHost = L"raw.githubusercontent.com";
static const wchar_t* kCommunityPath = L"/fatty8me2/retrohub/main/data/packs.json";
static struct { std::mutex mu; std::vector<std::string> ids; std::chrono::steady_clock::time_point tried{}; bool tried_once = false; } CM;

static void community_fetch() {
    std::string body, err;
    if (!http_get(kCommunityHost, kCommunityPath, &body, &err)) return;
    std::vector<std::string> ids;
    if (!parse_packs_json(body, &ids)) return;
    std::lock_guard<std::mutex> lk(CM.mu); CM.ids = ids;
}

/* Every extra root: the community list first, then `user` links (de-duplicated). */
static void all_roots(const std::vector<std::string>& user, std::vector<std::string>* out) {
    bool need = false;
    {   // refetch at most every 10 minutes, so packs added to the list show up without a restart
        std::lock_guard<std::mutex> lk(CM.mu);
        auto t = std::chrono::steady_clock::now();
        need = !CM.tried_once || t - CM.tried >= std::chrono::minutes(10);
        if (need) { CM.tried = t; CM.tried_once = true; }
    }
    if (need) community_fetch();   // best effort: a failure keeps the previous list
    std::vector<std::string> merged;
    { std::lock_guard<std::mutex> lk(CM.mu); merged = CM.ids; }
    for (const std::string& id : user) {
        if (!valid_id(id)) continue;
        bool dup = false;
        for (const std::string& m : merged) if (m == id) { dup = true; break; }
        if (!dup) merged.push_back(id);
    }
    *out = std::move(merged);
}

bool list_folder(const std::string& folder_id, std::vector<Entry>* out) {
    if (out) out->clear();
    if (folder_id.empty()) return false;
    if (!valid_id(folder_id)) return false;
    std::string body, err;
    if (!http_get(L"drive.google.com", (L"/embeddedfolderview?id=" + W(folder_id)).c_str(), &body, &err)) return false;
    if (out) *out = parse_folder_html(body);
    return true;
}

/* The id of `sys`'s subfolder inside pack root `root_id` ("" when absent or on error). */
static std::string sys_subfolder(const std::string& root_id, const std::string& sys) {
    if (root_id.empty()) return "";
    std::vector<Entry> entries;
    if (!list_folder(root_id, &entries)) return "";
    for (const Entry& e : entries)
        if (e.is_folder && console_code(e.name) && lower_name(e.name) == sys) return e.id;
    return "";
}

std::string match_game(const std::string& sys_short, const std::string& title, const std::vector<std::string>& roots) {
    std::string want = hubui::fg::norm_title(title);
    if (want.empty()) return "";
    std::vector<std::string> sources;
    const char* sf = sys_folder(sys_short);
    if (sf[0]) sources.push_back(sf);
    for (const std::string& r : roots) {
        std::string sub = sys_subfolder(r, lower_name(sys_short));
        if (!sub.empty()) sources.push_back(sub);
    }
    for (const std::string& s : sources) {
        std::vector<Entry> entries;
        if (!list_folder(s, &entries)) continue;
        for (const Entry& e : entries)
            if (e.is_folder && hubui::fg::norm_title(e.name) == want) return e.id;
    }
    return "";
}

bool list_files(const std::string& game_folder_id, std::vector<Entry>* out) {
    if (out) out->clear();
    std::vector<Entry> entries;
    if (!list_folder(game_folder_id, &entries)) return false;
    for (const Entry& e : entries)
        if (!e.is_folder) out->push_back(e);
    return true;
}

bool download_file(const std::string& file_id, const std::string& out_path,
                   std::atomic<uint64_t>* got, std::atomic<uint64_t>* total,
                   std::atomic<bool>* stop, std::string* err) {
    if (!valid_id(file_id)) { *err = "bad file id"; return false; }
    HINTERNET ses = WinHttpOpen(L"RetroHub", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!ses) { *err = "network unavailable"; return false; }
    bool ok = false;
    std::wstring part = W(out_path + ".part");   // written here, renamed over out_path only once complete
    INTERNET_PORT qp = 0;
    bool qa = qa_base(&qp);
    HINTERNET con = WinHttpConnect(ses, qa ? L"127.0.0.1" : L"drive.usercontent.google.com", qa ? qp : INTERNET_DEFAULT_HTTPS_PORT, 0);
    std::wstring path = L"/download?id=" + W(file_id) + L"&export=download&confirm=t";
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, qa ? 0 : WINHTTP_FLAG_SECURE) : nullptr;
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, sz = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
        wchar_t lenb[32] = {0};
        sz = sizeof lenb;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, lenb, &sz, nullptr) && total) *total = (uint64_t)_wtoi64(lenb);
        uint64_t want = total ? total->load() : 0;
        wchar_t ctype[128] = {0};
        sz = sizeof ctype;
        bool html = WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_TYPE, nullptr, ctype, &sz, nullptr) && wcsstr(ctype, L"text/html");
        if (status != 200) *err = "server answered " + std::to_string(status);
        else if (html) *err = "Google Drive refused the download (quota or permission page)";
        else {
            uint64_t mine = 0;
            HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f == INVALID_HANDLE_VALUE) *err = "cannot write " + out_path;
            else {
                std::vector<char> buf(64 * 1024);
                ok = true;
                for (;;) {
                    if (stop && stop->load()) { ok = false; *err = "cancelled"; break; }
                    DWORD n = 0;
                    if (!WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &n)) { ok = false; *err = "download interrupted"; break; }
                    if (n == 0) break;
                    DWORD w = 0;
                    if (!WriteFile(f, buf.data(), n, &w, nullptr) || w != n) { ok = false; *err = "disk write failed"; break; }
                    mine += n;
                    if (got) *got += n;
                }
                CloseHandle(f);
                if (ok && want && mine != want) { ok = false; *err = "download incomplete"; }
                if (ok && !MoveFileExW(part.c_str(), W(out_path).c_str(), MOVEFILE_REPLACE_EXISTING)) { ok = false; *err = "cannot write " + out_path; }
                if (!ok) DeleteFileW(part.c_str());
            }
        }
    } else *err = "could not reach Google Drive";
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

std::string pack_root() { return hub::data_dir() + "gamepack\\"; }

bool installed(const std::string& sys_short, const std::string& title) {
    if (sys_short.empty()) return false;
    fs::path d = P(pack_root()) / sys_short / safe_name(title);
    std::error_code ec;   // the marker is written only when every file finished: a cancelled install can be retried
    return fs::is_regular_file(d / "gamepack.done", ec);
}

/* ------------------------------------------------------------------ */
/* pack index: which games are installable (built-in pack + extra roots) */
/* ------------------------------------------------------------------ */

static long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static struct {
    std::mutex mu;
    std::map<std::string, int> state;   // sys -> 0 pending, 1 ok (membership known), 2 failed
    std::map<std::string, std::set<std::string>> titles;   // sys -> normalized game folder names
    std::map<std::string, bool> extra;   // sys -> an extra root has this console
    long long last_ms = 0;
    bool finished = true;               // the job in `th` has ended (mu)
    std::atomic<bool> stop{false};      // index_shutdown(): the job stops between listings
    std::thread th;
} IX;

void index_refresh(const std::vector<std::string>& roots) {
    bool run = false;
    {
        std::lock_guard<std::mutex> lk(IX.mu);
        if (IX.th.joinable() && !IX.finished) return;   // one job at a time
        if (IX.th.joinable()) IX.th.join();             // the previous job ended: reclaim its thread
        IX.finished = false;
        IX.last_ms = now_ms();
        run = true;
    }
    if (!run) return;
    IX.th = std::thread([roots] {
        std::vector<std::string> all;   // community packs (fetched if not known yet) + the user's extra roots
        all_roots(roots, &all);
        auto list_into = [&](const char* folder_id, const std::string& sys) {
            if (!folder_id || !folder_id[0] || IX.stop.load()) return false;
            std::vector<Entry> entries;
            if (!list_folder(folder_id, &entries)) return false;
            std::lock_guard<std::mutex> lk(IX.mu);
            for (const Entry& e : entries) if (e.is_folder) IX.titles[sys].insert(hubui::fg::norm_title(e.name));
            return true;
        };
        static const char* builtin[] = {"n64", "gc", "gba", "gbc", "nds", "ps1"};
        for (const char* s : builtin) {   // the built-in pack first: its consoles are known even before any listing
            { std::lock_guard<std::mutex> lk(IX.mu); IX.state[s] = 0; }
            bool ok = list_into(sys_folder(s), s);
            { std::lock_guard<std::mutex> lk(IX.mu); IX.state[s] = ok ? 1 : 2; }
        }
        for (const std::string& root : all) {   // extra roots: same layout as the built-in pack (a subfolder per console)
            if (IX.stop.load()) break;
            std::vector<Entry> entries;
            if (!list_folder(root, &entries)) continue;
            for (const Entry& e : entries) {
                if (!e.is_folder || !console_code(e.name)) continue;
                std::string sys = lower_name(e.name);
                { std::lock_guard<std::mutex> lk(IX.mu); IX.extra[sys] = true; }
                if (list_into(e.id.c_str(), sys)) {
                    std::lock_guard<std::mutex> lk(IX.mu); IX.state[sys] = 1;   // a listed extra root makes the console known even if the built-in listing failed
                } else {
                    std::lock_guard<std::mutex> lk(IX.mu); IX.state[sys] = 2;
                }
            }
        }
        std::lock_guard<std::mutex> lk(IX.mu); IX.finished = true;
    });
}

void index_shutdown() {   // app exit: stop the job and join it (it ends between listings)
    IX.stop = true;
    std::thread th;
    { std::lock_guard<std::mutex> lk(IX.mu); if (IX.th.joinable()) th = std::move(IX.th); }
    if (th.joinable()) th.join();
}

long long index_last_attempt_ms() {
    std::lock_guard<std::mutex> lk(IX.mu);
    return IX.last_ms;
}

IndexState index_state(const std::string& sys_short) {
    std::lock_guard<std::mutex> lk(IX.mu);
    auto it = IX.state.find(sys_short);
    if (it == IX.state.end()) return IndexState::Unknown;   // no source known for this console
    if (it->second == 1) return IndexState::Ready;
    if (IX.th.joinable() && !IX.finished) return IndexState::Checking;
    return IndexState::Unknown;   // failed (or pending with no job running): membership unknown
}

bool index_has(const std::string& sys_short, const std::string& title) {
    std::string want = hubui::fg::norm_title(title);
    if (want.empty()) return false;
    std::lock_guard<std::mutex> lk(IX.mu);
    auto it = IX.state.find(sys_short);
    if (it == IX.state.end() || it->second != 1) return false;
    auto t = IX.titles.find(sys_short);
    return t != IX.titles.end() && t->second.count(want) > 0;
}

bool index_source(const std::string& sys_short) {
    if (sys_folder(sys_short)[0]) return true;
    std::lock_guard<std::mutex> lk(IX.mu);
    auto it = IX.extra.find(sys_short);
    return it != IX.extra.end() && it->second;
}

/* ------------------------------------------------------------------ */
/* background install job                                              */
/* ------------------------------------------------------------------ */

Installer::~Installer() { if (th.joinable()) th.join(); }

void Installer::request_cancel() { cancel = true; }

void Installer::start(const std::string& sys, const std::string& title, const std::vector<std::string>& roots) {
    if (state == Working || th.joinable()) return;   // one job at a time
    this->sys = sys;
    this->title = title;
    done = false;
    ok = false;
    cancel = false;
    got = 0;
    total = 0;
    file_idx = 0;
    file_count = 0;
    { std::lock_guard<std::mutex> lk(mu); current.clear(); error.clear(); }
    state = Working;
    th = std::thread([this, roots] {
        bool r = false;
        std::string e;
        const char* sf = sys_folder(this->sys);
        {
            std::string want = hubui::fg::norm_title(this->title);
            std::vector<std::string> sources;   // the built-in pack first, then every extra root that has this console
            if (sf[0]) sources.push_back(sf);
            std::vector<std::string> all;   // community packs (fetched if not known yet) + the user's extra roots
            all_roots(roots, &all);
            for (const std::string& root : all) {
                std::string sub = sys_subfolder(root, lower_name(this->sys));
                if (!sub.empty()) sources.push_back(sub);
            }
            if (sources.empty()) e = "this console is not in the shared pack";
            else {
                std::string game_id; bool any_listed = false;
                for (const std::string& s : sources) {
                    if (!game_id.empty()) break;
                    std::vector<Entry> games;
                    if (!list_folder(s, &games)) continue;
                    any_listed = true;
                    for (const Entry& g : games)
                        if (g.is_folder && hubui::fg::norm_title(g.name) == want) { game_id = g.id; break; }
                }
                if (game_id.empty()) e = any_listed ? "not in the shared pack" : "could not list the shared pack";
                else {
                    std::vector<Entry> files;
                    if (!list_files(game_id, &files)) e = "could not list the game's files";
                    else if (files.empty()) e = "the shared copy has no files";
                    else {
                        std::error_code ec;
                        fs::path dir = P(pack_root()) / this->sys / safe_name(this->title);
                        bool fresh = !fs::is_directory(dir, ec);
                        fs::create_directories(dir, ec);
                        file_count = (int)files.size();
                        r = true;
                        for (size_t i = 0; i < files.size() && r; ++i) {
                            if (cancel.load()) { r = false; e = "cancelled"; break; }
                            if (safe_name(files[i].name) == "gamepack.done") continue;   // never let a remote file fake the marker
                            file_idx = (int)i + 1;
                            got = 0;
                            total = 0;
                            { std::lock_guard<std::mutex> lk(mu); current = files[i].name; }
                            std::string fe;
                            r = download_file(files[i].id, S(dir / safe_name(files[i].name)), &got, &total, &cancel, &fe);
                            if (!r) e = fe;
                        }
                        if (r) { std::ofstream m(S(dir / "gamepack.done"), std::ios::binary); m << "ok"; }
                        if (fresh && !r) { std::error_code ec2; fs::remove_all(dir, ec2); }
                    }
                }
            }
        }
        if (!r) { std::lock_guard<std::mutex> lk(mu); error = e; }
        ok = r;
        done = true;
    });
}

bool Installer::poll(bool* success) {
    if (state != Working || !done.load()) return false;
    if (th.joinable()) th.join();
    if (success) *success = ok.load();
    state = ok.load() ? Idle : Failed;
    return true;
}

} // namespace hubgp
