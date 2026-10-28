#include "gamepack.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "friend_games.h"   // hubui::fg::norm_title
#include "games.h"          // hub::data_dir

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
    return r;
}

/* Plain GET that returns the whole body (folder listings are small). */
static bool http_get(const std::wstring& host, const std::wstring& path, std::string* body, std::string* err) {
    HINTERNET ses = WinHttpOpen(L"RetroHub", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!ses) { *err = "network unavailable"; return false; }
    bool ok = false;
    HINTERNET con = WinHttpConnect(ses, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
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
                body->append(buf.data(), n);
            }
        }
    } else *err = "could not reach Google Drive";
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

bool list_folder(const std::string& folder_id, std::vector<Entry>* out) {
    if (out) out->clear();
    if (folder_id.empty()) return false;
    std::string body, err;
    if (!http_get(L"drive.google.com", (L"/embeddedfolderview?id=" + W(folder_id)).c_str(), &body, &err)) return false;
    if (out) *out = parse_folder_html(body);
    return true;
}

std::string match_game(const std::string& sys_short, const std::string& title) {
    const char* sf = sys_folder(sys_short);
    if (!sf[0]) return "";
    std::string want = hubui::fg::norm_title(title);
    if (want.empty()) return "";
    std::vector<Entry> entries;
    if (!list_folder(sf, &entries)) return "";
    for (const Entry& e : entries)
        if (e.is_folder && hubui::fg::norm_title(e.name) == want) return e.id;
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
    HINTERNET ses = WinHttpOpen(L"RetroHub", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!ses) { *err = "network unavailable"; return false; }
    bool ok = false;
    HINTERNET con = WinHttpConnect(ses, L"drive.usercontent.google.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    std::wstring path = L"/download?id=" + W(file_id) + L"&export=download&confirm=t";
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, sz = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
        wchar_t lenb[32] = {0};
        sz = sizeof lenb;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, lenb, &sz, nullptr) && total) *total = (uint64_t)_wtoi64(lenb);
        if (status != 200) *err = "server answered " + std::to_string(status);
        else {
            HANDLE f = CreateFileW(W(out_path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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
                    if (got) *got += n;
                }
                CloseHandle(f);
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
/* background install job                                              */
/* ------------------------------------------------------------------ */

Installer::~Installer() { if (th.joinable()) th.join(); }

void Installer::request_cancel() { cancel = true; }

void Installer::start(const std::string& sys, const std::string& title) {
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
    th = std::thread([this] {
        bool r = false;
        std::string e;
        const char* sf = sys_folder(this->sys);
        if (!sf[0]) e = "this console is not in the shared pack";
        else {
            std::string want = hubui::fg::norm_title(this->title);
            std::vector<Entry> games;
            if (!list_folder(sf, &games)) e = "could not list the shared pack";
            else {
                std::string game_id;
                for (const Entry& g : games)
                    if (g.is_folder && hubui::fg::norm_title(g.name) == want) { game_id = g.id; break; }
                if (game_id.empty()) e = "not in the shared pack";
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
                            if (cancel.load()) { e = "cancelled"; break; }
                            file_idx = (int)i + 1;
                            got = 0;
                            total = 0;
                            { std::lock_guard<std::mutex> lk(mu); current = files[i].name; }
                            std::string fe;
                            r = download_file(files[i].id, S(dir / safe_name(files[i].name)), &got, &total, &cancel, &fe);
                            if (!r) e = fe;
                        }
                        if (r) { std::ofstream m(S(dir / "gamepack.done"), std::ios::binary); m << "ok"; }
                        if (fresh && !r && e != "cancelled") { std::error_code ec2; fs::remove_all(dir, ec2); }
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
