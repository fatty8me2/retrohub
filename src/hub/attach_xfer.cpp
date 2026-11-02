#include "attach_xfer.h"

#include "cloud.h"
#include "cloud_http.h"
#include "json.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fs = std::filesystem;

namespace hub::attach {

namespace {

std::mutex g_mu;
std::function<Account()> g_provider;
std::map<int, std::shared_ptr<Result>> g_jobs;
std::atomic<int> g_next{1};

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
bool is_hex(const std::string& s, size_t n) {
    if (s.size() != n) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

Account account() {
    std::function<Account()> p;
    { std::lock_guard<std::mutex> lk(g_mu); p = g_provider; }
    return p ? p() : Account();
}

void update(const std::shared_ptr<Result>& r, const std::function<void(Result&)>& fn) {
    std::lock_guard<std::mutex> lk(g_mu);
    fn(*r);
}
void fail(const std::shared_ptr<Result>& r, int status, const std::string& code, const std::string& text = "") {
    update(r, [&](Result& x) {
        x.done = true; x.ok = false; x.http_status = status; x.code = code;
        x.error = text.empty() ? attach_error_text(status, code) : text;
        x.percent = 100;
    });
}

std::shared_ptr<Result> make_job(bool upload, const std::string& path) {
    auto r = std::make_shared<Result>();
    r->job = g_next++;
    r->upload = upload;
    r->path = path;
    std::lock_guard<std::mutex> lk(g_mu);
    g_jobs[r->job] = r;
    return r;
}

bool transient(int status, const std::string& code) {
    return status == 0 || status == 502 || status == 504 || (status == 503 && code == "cloud-saves-retry");
}

void upload_thread(std::shared_ptr<Result> r, std::string path, std::string kind, std::string name, std::vector<std::string> to) {
    update(r, [](Result& x) { x.percent = 10; });
    std::error_code ec;
    auto sz = fs::file_size(fs::u8path(path), ec);
    if (ec) { fail(r, 0, "", "Couldn't read that file."); return; }
    std::string problem = attach_size_problem((int64_t)sz, kind);
    if (!problem.empty()) { fail(r, 0, "attachment-too-large", problem); return; }
    std::string body;
    {
        std::ifstream f(fs::u8path(path), std::ios::binary);
        if (!f) { fail(r, 0, "", "Couldn't read that file."); return; }
        body.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    if (body.empty()) { fail(r, 0, "", "That file is empty, so there is nothing to send."); return; }
    Account a = account();
    if (!a.valid()) { fail(r, 0, "", "You're not signed in to friends yet, so files can't be sent. Try again in a moment."); return; }
    std::string sha = hub::saves::sha256_hex_of(body);
    std::string dev = a.device_id;
    std::string uids;
    for (const std::string& u : to) { if (!uids.empty()) uids += ","; uids += u; }
    std::string pq = "/saves/v2/attach?to=" + uids + "&kind=" + kind + "&name=" + url_encode(sanitize_name(name));
    hub::saves::CloudConfig cfg;
    cfg.base_url = a.base_url; cfg.user_id = a.user_id; cfg.secret = a.secret;
    hub::saves::CloudHttp http(cfg, 8000, 60000);
    if (!http.ok()) { fail(r, 0, "", "Couldn't reach the server (bad address)."); return; }
    update(r, [](Result& x) { x.percent = 40; });
    hub::saves::HttpReply rep;
    for (int attempt = 0; attempt < 2; attempt++) {
        std::vector<std::string> hdr = {"X-RH-Sha: " + sha};
        if (!dev.empty()) hdr.push_back("X-RH-Device: " + dev);
        rep = http.call("PUT", pq, hdr, body);
        json::Value j;
        std::string code;
        if (json::parse(rep.body, &j)) code = j.str("error");
        if (!transient(rep.status, code)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }
    json::Value j;
    bool parsed = json::parse(rep.body, &j) && j.is_object();
    if (rep.status != 200 || !parsed || !j.boolean("ok")) {
        std::string code = parsed ? j.str("error") : "";
        fail(r, rep.status, code);
        return;
    }
    update(r, [&](Result& x) {
        x.ref.id = j.str("id"); x.ref.name = j.str("name"); x.ref.sha = j.str("sha"); x.ref.kind = j.str("kind");
        x.ref.size = j.i64("size"); x.ref.expires = j.i64("expires");
        x.done = true; x.ok = is_hex(x.ref.id, 32); x.percent = 100; x.http_status = 200;
        if (!x.ok) x.error = attach_error_text(500, "");
    });
}

void download_thread(std::shared_ptr<Result> r, hubnet::Attach att, std::string dest) {
    update(r, [](Result& x) { x.percent = 10; });
    Account a = account();
    if (!a.valid()) { fail(r, 0, "", "You're not signed in to friends yet, so the file can't be fetched. Try again in a moment."); return; }
    hub::saves::CloudConfig cfg;
    cfg.base_url = a.base_url; cfg.user_id = a.user_id; cfg.secret = a.secret;
    hub::saves::CloudHttp http(cfg, 8000, 60000);
    if (!http.ok()) { fail(r, 0, "", "Couldn't reach the server (bad address)."); return; }
    update(r, [](Result& x) { x.percent = 40; });
    std::string data;
    hub::saves::HttpReply rep;
    for (int attempt = 0; attempt < 2; attempt++) {
        data.clear();
        std::vector<std::string> hdr;
        if (!a.device_id.empty()) hdr.push_back("X-RH-Device: " + a.device_id);
        rep = http.call("GET", "/saves/v2/attach?id=" + att.id, hdr, std::string(), [&](const char* p, size_t n) {
            if (data.size() + n > (size_t)kMaxBytes + 1) return false;
            data.append(p, n);
            return true;
        });
        json::Value j;
        std::string code;
        if (json::parse(rep.body, &j)) code = j.str("error");
        if (!transient(rep.status, code)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }
    if (rep.status != 200) {
        json::Value j;
        std::string code = (json::parse(rep.body, &j) && j.is_object()) ? j.str("error") : "";
        fail(r, rep.status, code);
        return;
    }
    update(r, [](Result& x) { x.percent = 90; });
    std::string sha = hub::saves::sha256_hex_of(data);
    std::string want = att.sha;
    auto it = rep.headers.find("x-rh-sha");
    if (want.empty() && it != rep.headers.end()) want = it->second;
    if (data.empty() || (!want.empty() && sha != want) || (int64_t)data.size() > kMaxBytes) {
        fail(r, 200, "bad-download", "The download was damaged on the way. Please try again.");
        return;
    }
    std::error_code ec;
    fs::path d = fs::u8path(dest);
    if (d.has_parent_path()) fs::create_directories(d.parent_path(), ec);
    fs::path part = d; part += ".part";
    {
        std::ofstream f(part, std::ios::binary | std::ios::trunc);
        f.write(data.data(), (std::streamsize)data.size());
        if (!f) { fail(r, 0, "", "Couldn't save the file to this PC (disk full or folder locked)."); return; }
    }
    if (!MoveFileExW(part.c_str(), d.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        std::error_code ec2; fs::remove(part, ec2);
        fail(r, 0, "", "Couldn't save the file to this PC (disk full or folder locked).");
        return;
    }
    update(r, [&](Result& x) { x.done = true; x.ok = true; x.percent = 100; x.http_status = 200; x.ref = att; x.path = dest; });
}

} // namespace

void set_account_provider(std::function<Account()> fn) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_provider = std::move(fn);
}

std::string http_base_from_server(const std::string& s) {
    std::string b = s;
    if (b.rfind("wss://", 0) == 0) b = "https://" + b.substr(6);
    else if (b.rfind("ws://", 0) == 0) b = "http://" + b.substr(5);
    while (!b.empty() && b.back() == '/') b.pop_back();
    return b;
}

std::string size_text(int64_t bytes) {
    char buf[32];
    if (bytes >= 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024) std::snprintf(buf, sizeof buf, "%lld KB", (long long)((bytes + 1023) / 1024));
    else std::snprintf(buf, sizeof buf, "%lld bytes", (long long)bytes);
    return buf;
}

bool valid_kind(const std::string& k) { return k == "shot" || k == "state" || k == "patch"; }

std::string attach_size_problem(int64_t bytes, const std::string& kind) {
    if (bytes <= 0) return "That file is empty, so there is nothing to send.";
    if (bytes <= kMaxBytes) return "";
    std::string what = kind == "state" ? "save state" : kind == "patch" ? "patch" : "picture";
    std::string s = "That " + what + " is " + size_text(bytes) + ". Attachments are limited to 8 MB.";
    if (kind == "state") s += " Games with big save states (DS, PS1 and some others) can't be sent as challenges.";
    else if (kind == "patch") s += " Share this patch another way, for example a link.";
    else s += " Try a smaller picture.";
    return s;
}

std::string attach_error_text(int status, const std::string& code) {
    if (code == "attachment-too-large") return "Attachments are limited to 8 MB.";
    if (code == "attachment-daily-limit") return "You've sent a lot of files today - try again tomorrow.";
    if (code == "account-daily-limit") return "You've used up today's cloud allowance - try again tomorrow.";
    if (code == "attachment-pool-full") return "You already have 24 MB of files waiting for friends. They disappear after 7 days, or wait until a friend has picked some up.";
    if (code == "attachment-pool-tight") return "The server is nearly full, so only small files (up to 2 MB) can be sent right now.";
    if (code == "quota-exceeded") return "Your cloud space is full (50 MB). Free some space in Saves > Cloud, then try again.";
    if (code == "cloud-saves-full") return "Cloud space is full on the server right now. Try again later.";
    if (code == "cloud-saves-paused" || code == "cloud-saves-busy-today" || code == "cloud-saves-unavailable")
        return "Sending files is paused for now to stay inside the free limits. Try again later today.";
    if (code == "rate-limited") return "Slow down a little - too many files at once.";
    if (code == "not-friends") return "You can only send files to friends.";
    if (code == "bad-recipient") return "Pick at least one friend (up to 8) to send to.";
    if (code == "bad-kind" || code == "bad-sha" || code == "sha-required" || code == "bad-body")
        return "The server didn't accept that file. Try again, and if it keeps failing update RetroHub.";
    if (code == "attachment-expired" || status == 410) return "That file has expired (files last 7 days).";
    if (code == "not-found" || status == 404) return "That file isn't available any more (it may have expired, or it wasn't sent to you).";
    if (code == "cloud-saves-disabled") return "File sending is switched off on the server.";
    if (code == "cloud-saves-error" || code == "cloud-saves-retry" || status == 503 || status >= 500)
        return "The server had a problem. Try again in a minute.";
    if (code == "unauthorized" || status == 401) return "The server doesn't recognise this PC's account right now. Restart RetroHub and try again.";
    if (code == "device-revoked") return "This PC was signed out of your account. Sign in again in Settings > Account.";
    if (status == 413) return "Attachments are limited to 8 MB.";
    if (status == 429) return "Too many requests - try again in a minute.";
    if (status == 0) return "Couldn't reach the server. Check your connection and try again.";
    return "Couldn't send the file (error " + std::to_string(status) + ").";
}

std::string sanitize_name(const std::string& raw) {
    std::string base = raw;
    size_t sl = base.find_last_of("/\\");
    if (sl != std::string::npos) base = base.substr(sl + 1);
    std::string out;
    bool sp = false;
    for (unsigned char c : base) {
        if (c < 32 || c == 127 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') continue;
        if (c == ' ' || c == '\t') { sp = !out.empty(); continue; }
        if (sp) { out += ' '; sp = false; }
        out += (char)c;
    }
    // limit to 64 characters without cutting a UTF-8 sequence
    size_t chars = 0, i = 0;
    while (i < out.size()) {
        if (chars == 64) break;
        unsigned char c = out[i];
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        i += n; chars++;
    }
    out.resize(std::min(i, out.size()));
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out.empty() ? "file" : out;
}

std::string url_encode(const std::string& s) {
    static const char* hx = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hx[c >> 4]; o += hx[c & 15]; }
    }
    return o;
}

int upload(const std::string& path, const std::string& kind, const std::string& display_name, const std::vector<std::string>& recipients) {
    auto r = make_job(true, path);
    if (!valid_kind(kind)) { fail(r, 400, "bad-kind"); return r->job; }
    if (recipients.empty() || recipients.size() > 8) { fail(r, 400, "bad-recipient"); return r->job; }
    std::thread(upload_thread, r, path, kind, display_name, recipients).detach();
    return r->job;
}

int download(const hubnet::Attach& att, const std::string& dest_path) {
    auto r = make_job(false, dest_path);
    r->ref = att;
    if (!is_hex(att.id, 32)) { fail(r, 404, "not-found"); return r->job; }
    if (att.expires > 0 && att.expires <= now_ms()) { fail(r, 410, "attachment-expired"); return r->job; }
    std::thread(download_thread, r, att, dest_path).detach();
    return r->job;
}

bool poll(int job, Result* out) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_jobs.find(job);
    if (it == g_jobs.end()) return false;
    if (out) *out = *it->second;
    return true;
}

void forget(int job) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_jobs.erase(job);
}

bool wait(int job, Result* out, int timeout_ms) {
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        Result r;
        if (!poll(job, &r)) return false;
        if (r.done) { if (out) *out = r; return true; }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > timeout_ms) {
            if (out) *out = r;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

} // namespace hub::attach
