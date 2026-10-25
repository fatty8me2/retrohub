/* Persistent retry queue, per-account base state and the three-way rule of the cloud saves v2 engine. */
#include "cloud_internal.h"
#include "json.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
typedef json::Value JV;
using str = std::string;

namespace hub::saves {

namespace {
bool read_text(const str& path, str* out) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) return false;
    std::stringstream s;
    s << f.rdbuf();
    *out = s.str();
    return true;
}
bool write_atomic(const str& path, const str& data) {
    std::error_code ec;
    fs::path p = fs::u8path(path);
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".hubtmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << data;
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(p, ec); fs::rename(tmp, p, ec); }
    return !ec;
}
}  // namespace

/* ------------------------------------------------------------------ three-way */
Act three_way(const Side& local, const Side& base, const Side& cloud, bool cloud_evicted, FileClass cls) {
    if (cloud_evicted) return local.present ? Act::LocalOnlyCap : Act::Nothing;
    if (!local.present && !cloud.present) return Act::Nothing;
    if (!local.present) return Act::Pull;                       // new game on this PC, or an accidental local loss: heal
    if (!cloud.present) return Act::Push;
    if (local.sha == cloud.sha) return base.present && base.sha == local.sha ? Act::Nothing : Act::Adopt;
    bool lchg = !base.present || local.sha != base.sha;
    bool cchg = !base.present || cloud.sha != base.sha;
    if (lchg && !cchg) return Act::Push;
    if (!lchg && cchg) return Act::Pull;
    if (!lchg && !cchg) return Act::Nothing;                    // unreachable (shas differ), kept for safety
    if (cls == FileClass::State || cls == FileClass::Shot || cls == FileClass::Settings)   // newest wins, never prompts
        return local.mtime >= cloud.mtime ? Act::Push : Act::Pull;
    return Act::Conflict;
}

/* ------------------------------------------------------------------ queue */
int backoff_secs(int tries) {
    static const int kSteps[] = {30, 120, 600, 3600};
    if (tries < 1) return 30;
    return tries <= 4 ? kSteps[tries - 1] : -1;
}

bool CloudQueue::load(const std::string& path) {
    path_ = path;
    items_.clear();
    str text;
    if (!read_text(path, &text)) return false;
    JV root;
    if (!json::parse(text, &root, json::ParseOptions{16, true}) || root.type != JV::Object) return false;
    const JV* a = root.get("items");
    if (!a || a->type != JV::Array) return false;
    for (auto& j : a->a) {
        if (j.type != JV::Object) continue;
        QItem q;
        q.gkey = j.str("gkey");
        q.reason = j.str("reason");
        q.tries = (int)j.i64("tries", 0);
        q.next_at = j.i64("next_at", 0);
        if (!q.gkey.empty()) items_.push_back(q);
    }
    return true;
}

bool CloudQueue::save() const {
    str o = "{\"items\":[";
    for (size_t i = 0; i < items_.size(); i++) {
        if (i) o += ',';
        o += "{\"gkey\":" + json::quote(items_[i].gkey) + ",\"reason\":" + json::quote(items_[i].reason) + ",\"tries\":" +
             std::to_string(items_[i].tries) + ",\"next_at\":" + std::to_string(items_[i].next_at) + "}";
    }
    o += "]}";
    return path_.empty() ? false : write_atomic(path_, o);
}

void CloudQueue::put(const std::string& gkey, const std::string& reason, int64_t next_at, bool reset_tries) {
    for (auto& q : items_)
        if (q.gkey == gkey && q.reason == reason) {
            q.next_at = next_at;
            if (reset_tries) q.tries = 0;
            return;
        }
    items_.push_back(QItem{gkey, reason, 0, next_at});
}

void CloudQueue::remove(const std::string& gkey, const std::string& reason) {
    items_.erase(std::remove_if(items_.begin(), items_.end(), [&](const QItem& q) { return q.gkey == gkey && q.reason == reason; }), items_.end());
}

void CloudQueue::fail(const std::string& gkey, const std::string& reason, int64_t now, int64_t retry_after) {
    for (auto& q : items_)
        if (q.gkey == gkey && q.reason == reason) {
            q.tries++;
            int b = backoff_secs(q.tries);
            if (retry_after > 0) q.next_at = now + retry_after;          // the server said when
            else q.next_at = b < 0 ? -1 : now + b;                       // -1 = parked until a trigger
            return;
        }
}

bool CloudQueue::next_due(int64_t now, QItem* out) const {
    const QItem* best = nullptr;
    for (auto& q : items_)
        if (q.next_at >= 0 && q.next_at <= now && (!best || q.next_at < best->next_at)) best = &q;
    if (!best) return false;
    if (out) *out = *best;
    return true;
}

int64_t CloudQueue::earliest(int64_t now) const {
    int64_t e = 0;
    for (auto& q : items_)
        if (q.next_at > now && (e == 0 || q.next_at < e)) e = q.next_at;
    return e;
}

void CloudQueue::wake_parked(int64_t now) {
    for (auto& q : items_)
        if (q.next_at < 0 || q.next_at > now) { q.next_at = now; q.tries = 0; }
}

bool CloudQueue::has(const std::string& gkey) const {
    for (auto& q : items_) if (q.gkey == gkey) return true;
    return false;
}

/* ------------------------------------------------------------------ base state */
bool BaseStore::load(const std::string& path) {
    games.clear();
    rev = 0; restore = false; migrated = false; cap_toast_day = 0;
    str text;
    if (!read_text(path, &text)) return false;
    JV root;
    if (!json::parse(text, &root, json::ParseOptions{16, true}) || root.type != JV::Object) return false;
    rev = root.i64("rev", 0);
    device = root.str("device");
    restore = root.boolean("restore", false);
    migrated = root.boolean("migrated", false);
    cap_toast_day = root.i64("cap_toast_day", 0);
    const JV* g = root.get("games");
    if (g && g->type == JV::Object)
        for (auto& kv : g->o) {
            if (kv.second.type != JV::Object) continue;
            BGame bg;
            bg.lo_user = kv.second.boolean("lo_user", false);
            bg.decide_later = kv.second.boolean("dl", false);
            bg.pinned = kv.second.boolean("pin", false);
            bg.played = kv.second.i64("played", 0);
            bg.last_synced = kv.second.i64("synced", 0);
            const JV* fs = kv.second.get("files");
            if (fs && fs->type == JV::Object)
                for (auto& fk : fs->o) {
                    if (fk.second.type != JV::Object) continue;
                    BFile bf;
                    bf.ver = (int)fk.second.i64("ver", 0);
                    bf.sha = fk.second.str("sha");
                    bf.size = fk.second.i64("size", 0);
                    bf.mtime = fk.second.i64("mtime", 0);
                    bf.lo = fk.second.str("lo");
                    bg.files[fk.first] = bf;
                }
            games[kv.first] = std::move(bg);
        }
    return true;
}

bool BaseStore::save(const std::string& path) const {
    str o = "{\"rev\":" + std::to_string(rev) + ",\"device\":" + json::quote(device) + ",\"restore\":" + (restore ? "true" : "false") +
            ",\"migrated\":" + (migrated ? "true" : "false") + ",\"cap_toast_day\":" + std::to_string(cap_toast_day) + ",\"games\":{";
    bool fg = true;
    for (auto& kv : games) {
        if (!fg) o += ',';
        fg = false;
        const BGame& g = kv.second;
        o += json::quote(kv.first) + ":{\"lo_user\":" + (g.lo_user ? "true" : "false") + ",\"dl\":" + (g.decide_later ? "true" : "false") +
             ",\"pin\":" + (g.pinned ? "true" : "false") + ",\"played\":" + std::to_string(g.played) + ",\"synced\":" +
             std::to_string(g.last_synced) + ",\"files\":{";
        bool ff = true;
        for (auto& f : g.files) {
            if (!ff) o += ',';
            ff = false;
            o += json::quote(f.first) + ":{\"ver\":" + std::to_string(f.second.ver) + ",\"sha\":" + json::quote(f.second.sha) + ",\"size\":" +
                 std::to_string(f.second.size) + ",\"mtime\":" + std::to_string(f.second.mtime);
            if (!f.second.lo.empty()) o += ",\"lo\":" + json::quote(f.second.lo);
            o += "}";
        }
        o += "}}";
    }
    o += "}}";
    return write_atomic(path, o);
}

}  // namespace hub::saves
