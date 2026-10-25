#include "friends.h"
#include "log.h"
#include "net_internal.h"
#include "session.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>

namespace hubnet {

using namespace detail;
namespace fs = std::filesystem;

/* ====================================================================== helpers */

static int64_t unix_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

static const char kB32[] = "abcdefghijklmnopqrstuvwxyz234567";

static std::string b32(const uint8_t* d, int n)
{
    std::string o;
    uint32_t buf = 0;
    int bits = 0;
    for (int i = 0; i < n; i++) {
        buf = (buf << 8) | d[i];
        bits += 8;
        while (bits >= 5) { o += kB32[(buf >> (bits - 5)) & 31]; bits -= 5; }
    }
    if (bits > 0) o += kB32[(buf << (5 - bits)) & 31];
    return o;
}

static bool is_b32(const std::string& s, size_t len)
{
    if (s.size() != len) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '2' && c <= '7'))) return false;
    return true;
}

static std::string with_sep(std::string d)
{
    if (!d.empty() && d.back() != '\\' && d.back() != '/') d += '\\';
    return d;
}

static fs::path upath(const std::string& s) { return fs::u8path(s); }

static bool read_file(const std::string& path, std::string* out)
{
    std::ifstream f(upath(path), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

static bool write_file(const std::string& path, const std::string& data)
{
    std::error_code ec;
    fs::create_directories(upath(path).parent_path(), ec);
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(upath(tmp), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), (std::streamsize)data.size());
        if (!f) return false;
    }
    fs::rename(upath(tmp), upath(path), ec);
    if (ec) {   // fall back to direct write
        std::ofstream f(upath(path), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), (std::streamsize)data.size());
        return (bool)f;
    }
    return true;
}

static std::string q(const std::string& s) { return "\"" + json::escape(s) + "\""; }

static int64_t jnum(const Json& j, const char* k, int64_t def = 0)
{
    const Json* v = j.get(k);
    return (v && v->type == Json::Number) ? (int64_t)v->n : def;
}
static bool jbool(const Json& j, const char* k, bool def = false)
{
    const Json* v = j.get(k);
    return (v && v->type == Json::Bool) ? v->b : def;
}

/* ====================================================================== identity */

Identity identity_generate()
{
    std::random_device rd;
    uint8_t uid[16], sec[32];
    for (int i = 0; i < 16; i += 4) { uint32_t r = rd(); for (int k = 0; k < 4; k++) uid[i + k] = (uint8_t)(r >> (8 * k)); }
    for (int i = 0; i < 32; i += 4) { uint32_t r = rd(); for (int k = 0; k < 4; k++) sec[i + k] = (uint8_t)(r >> (8 * k)); }
    Identity id;
    id.user_id = b32(uid, 16);
    id.secret = b32(sec, 32);
    id.created = unix_ms() / 1000;
    return id;
}

std::string friend_code(const std::string& user_id)
{
    if (user_id.size() < 8) return "";
    std::string p = user_id.substr(0, 8);
    for (char& c : p) c = (char)toupper((unsigned char)c);
    return "RH-" + p.substr(0, 4) + "-" + p.substr(4, 4);
}

static bool identity_read(const std::string& path, Identity* id)
{
    std::string text;
    Json j;
    if (!read_file(path, &text) || !json_parse(text, &j) || j.type != Json::Object) return false;
    Identity t;
    t.user_id = j.str("user_id");
    t.secret = j.str("secret");
    t.created = jnum(j, "created");
    if (!is_b32(t.user_id, 26) || !is_b32(t.secret, 52)) return false;
    *id = t;
    return true;
}

static std::string identity_json(const Identity& id)
{
    return "{\"user_id\":" + q(id.user_id) + ",\"secret\":" + q(id.secret) +
           ",\"created\":" + std::to_string(id.created) + "}\n";
}

static void identity_save(const Identity& id, const std::string& data_dir, const std::string& vault_dir)
{
    write_file(with_sep(data_dir) + "identity.json", identity_json(id));
    if (!vault_dir.empty()) write_file(with_sep(vault_dir) + "_identity\\identity.json", identity_json(id));
}

Identity identity_load_or_create(const std::string& data_dir, const std::string& vault_dir)
{
    std::string lp = with_sep(data_dir) + "identity.json";
    std::string vp = vault_dir.empty() ? "" : with_sep(vault_dir) + "_identity\\identity.json";
    Identity id, v;
    if (identity_read(lp, &id)) {
        if (!vp.empty() && !identity_read(vp, &v)) write_file(vp, identity_json(id));   // mirror (never clobber a valid one)
        return id;
    }
    if (!vp.empty() && identity_read(vp, &v)) {
        net_log("Friends: adopted identity from vault");
        write_file(lp, identity_json(v));
        return v;
    }
    id = identity_generate();
    identity_save(id, data_dir, vault_dir);
    return id;
}

static std::string backup_stamp()
{
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_s(&tmv, &t);
    char b[32];
    strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tmv);
    return b;
}

bool identity_adopt_vault_if_older(const std::string& data_dir, const std::string& vault_dir, Identity* adopted)
{
    if (vault_dir.empty()) return false;
    std::string lp = with_sep(data_dir) + "identity.json";
    std::string vp = with_sep(vault_dir) + "_identity\\identity.json";
    Identity cur, v;
    if (!identity_read(vp, &v)) return false;
    bool have_cur = identity_read(lp, &cur);
    if (have_cur) {
        if (cur.user_id == v.user_id) return false;
        if (!(v.created < cur.created)) return false;
        std::string text;
        if (read_file(lp, &text)) {
            std::string bp = with_sep(data_dir) + "identity-backup-" + backup_stamp() + ".json";
            for (int n = 2; fs::exists(upath(bp)) && n < 100; n++)
                bp = with_sep(data_dir) + "identity-backup-" + backup_stamp() + "-" + std::to_string(n) + ".json";
            write_file(bp, text);
        }
    }
    write_file(lp, identity_json(v));
    net_log("Friends: adopted older identity from vault");
    if (adopted) *adopted = v;
    return true;
}

bool identity_read_file(const std::string& path, Identity* id)
{
    return identity_read(path, id);
}

bool identity_adopt(const std::string& data_dir, const std::string& vault_dir, const Identity& v)
{
    if (!is_b32(v.user_id, 26) || !is_b32(v.secret, 52)) return false;
    std::string lp = with_sep(data_dir) + "identity.json";
    Identity cur;
    if (identity_read(lp, &cur) && cur.user_id != v.user_id) {
        std::string text;
        if (read_file(lp, &text)) {
            std::string bp = with_sep(data_dir) + "identity-backup-" + backup_stamp() + ".json";
            for (int n = 2; fs::exists(upath(bp)) && n < 100; n++)
                bp = with_sep(data_dir) + "identity-backup-" + backup_stamp() + "-" + std::to_string(n) + ".json";
            write_file(bp, text);
        }
    }
    identity_save(v, data_dir, vault_dir);
    net_log("Friends: identity replaced by linked/imported identity");
    return true;
}

std::vector<Identity> older_identities(const std::string& data_dir, const std::string& vault_dir, const std::string& current_uid)
{
    std::vector<Identity> out;
    std::set<std::string> skip;
    std::string dd = with_sep(data_dir);
    skip = read_retired_ids(data_dir);   // uids the server already confirmed retired
    auto take = [&](const fs::path& p) {
        Identity t;
        if (!identity_read(p.u8string(), &t)) return;
        if (t.user_id == current_uid || skip.count(t.user_id)) return;
        for (const Identity& o : out) if (o.user_id == t.user_id) return;
        out.push_back(t);
    };
    std::error_code ec;
    take(upath(dd + "pending-retire.json"));
    for (fs::directory_iterator it(upath(dd), ec), end; !ec && it != end; it.increment(ec)) {
        std::string n = it->path().filename().u8string();
        if (n.rfind("identity-backup", 0) == 0 && n.size() > 5 && n.substr(n.size() - 5) == ".json") take(it->path());
    }
    if (!vault_dir.empty()) {
        std::string vd = with_sep(vault_dir) + "_identity";
        for (fs::directory_iterator it(upath(vd), ec), end; !ec && it != end; it.increment(ec)) {
            std::string n = it->path().filename().u8string();
            if (n != "identity.json" && n.size() > 5 && n.substr(n.size() - 5) == ".json") take(it->path());
        }
    }
    return out;
}

/* ====================================================================== impl */

namespace {
struct Conv {
    std::vector<Message> msgs;
    int unread = 0;
    bool loaded = false, dirty = false;
};
} // namespace

static int presence_rank(PresenceState st)
{
    return st == PresenceState::InGame ? 3 : st == PresenceState::InLobby ? 2 : st == PresenceState::Online ? 1 : 0;
}

void dedupe_friends(std::vector<Friend>& friends, const std::string& self_uid)
{
    std::vector<Friend> out;
    for (Friend& f : friends) {
        if (f.uid.empty() || f.uid == self_uid) continue;
        Friend* have = nullptr;
        for (Friend& o : out) if (o.uid == f.uid) { have = &o; break; }
        if (!have) { out.push_back(std::move(f)); continue; }
        int ra = presence_rank(f.status.state), rb = presence_rank(have->status.state);
        bool take = ra != rb ? ra > rb : f.status.updated > have->status.updated;
        int64_t since = have->since && f.since ? std::min(have->since, f.since) : (have->since ? have->since : f.since);
        std::string note = have->note.empty() ? f.note : have->note;
        if (take) {
            if (f.name.empty()) f.name = have->name;
            if (f.code.empty()) f.code = have->code;
            *have = std::move(f);
        } else {
            if (have->name.empty()) have->name = f.name;
            if (have->code.empty()) have->code = f.code;
        }
        have->since = since;
        have->note = note;
    }
    friends = std::move(out);
}

void dedupe_requests(const std::vector<Friend>& friends, std::vector<Request>& incoming, std::vector<Request>& outgoing,
                     const std::string& self_uid)
{
    auto is_friend = [&](const std::string& u) {
        if (u.empty() || u == self_uid) return true;
        for (const Friend& f : friends) if (f.uid == u) return true;
        return false;
    };
    auto uniq = [&](std::vector<Request>& v, const std::vector<Request>* other) {
        std::vector<Request> out;
        for (Request& r : v) {
            if (is_friend(r.uid)) continue;
            if (other) {
                bool dup = false;
                for (const Request& o : *other) if (o.uid == r.uid) dup = true;
                if (dup) continue;
            }
            bool seen = false;
            for (Request& o : out)
                if (o.uid == r.uid) { seen = true; if (o.name.empty()) o.name = r.name; if (r.at > o.at) o.at = r.at; }
            if (!seen) out.push_back(std::move(r));
        }
        v = std::move(out);
    };
    uniq(incoming, nullptr);
    uniq(outgoing, &incoming);
}

static std::string name_key(const std::string& n)
{
    std::string k;
    for (char c : n) k += (char)tolower((unsigned char)c);
    size_t a = k.find_first_not_of(' '), b = k.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : k.substr(a, b - a + 1);
}

std::set<std::string> read_retired_ids(const std::string& data_dir)
{
    std::set<std::string> out;
    std::string text;
    if (!read_file(with_sep(data_dir) + "retired-ids.txt", &text)) return out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) out.insert(line);
    }
    return out;
}

std::vector<StaleEntry> stale_same_name(const std::vector<Friend>& friends, const std::set<std::string>& retired, int64_t gap_ms)
{
    std::map<std::string, std::vector<const Friend*>> groups;
    for (const Friend& f : friends) { std::string k = name_key(f.name); if (!k.empty()) groups[k].push_back(&f); }
    std::vector<StaleEntry> out;
    auto seen = [](const Friend* x) { return std::max(x->status.updated, x->since); };
    for (auto& g : groups) {
        if (g.second.size() < 2) continue;
        const Friend* keep = nullptr;   // freshest entry that is not a confirmed-retired identity
        for (const Friend* f : g.second) {
            if (retired.count(f->uid)) continue;
            if (!keep) { keep = f; continue; }
            int rf = presence_rank(f->status.state), rk = presence_rank(keep->status.state);
            if (rf != rk ? rf > rk : f->status.updated > keep->status.updated) keep = f;
        }
        if (!keep) continue;
        for (const Friend* f : g.second) {
            if (f == keep || f->status.state != PresenceState::Offline) continue;
            if (retired.count(f->uid)) out.push_back({f->uid, true});                   // the server confirmed this identity was replaced
            else if (seen(keep) - seen(f) >= gap_ms) out.push_back({f->uid, false});   // heuristic only: needs a per-entry confirm
        }
    }
    return out;
}

struct FriendsClient::Impl {
    std::string data_dir = ".\\userdata\\", vault_dir;
    std::string server, name = "Player";
    Identity id;
    WsClient ws;
    bool started = false, hello_sent = false, welcomed = false, gave_up = false;
    std::string status_text = "Offline";
    uint32_t last_keepalive = 0, reconnect_at = 0, backoff_ms = 1000, last_flush = 0;
    bool have_status = false, auto_friend = true;
    Status my_status;

    std::vector<Friend> friends;
    std::vector<Request> incoming, outgoing;
    std::vector<std::string> blocked;
    std::map<std::string, Conv> convs;
    std::deque<Event> events;
    uint32_t local_seq = 0;
    std::string username; bool needs_new_name = false, username_known = false; int ref_seq = 0;

    /* ---- sending */
    bool send(const std::string& s) { return welcomed && ws.send_text(s); }

    static std::string status_json(const Status& s)
    {
        const char* st = s.state == PresenceState::Online ? "online" : s.state == PresenceState::InLobby ? "in_lobby"
                       : s.state == PresenceState::InGame ? "in_game" : "offline";
        std::string o = std::string("{\"state\":\"") + st + "\"";
        if (!s.game_id.empty()) o += ",\"game_id\":" + q(s.game_id);
        if (!s.game_title.empty()) o += ",\"game_title\":" + q(s.game_title);
        if (!s.system.empty()) o += ",\"system\":" + q(s.system);
        if (!s.lobby_code.empty()) o += ",\"lobby_code\":" + q(s.lobby_code);
        o += std::string(",\"joinable\":") + (s.joinable ? "true" : "false");
        o += ",\"players\":" + std::to_string(s.players) + ",\"max_players\":" + std::to_string(s.max_players);
        return o + "}";
    }
    static std::string invite_json(const Invite& v)
    {
        return "{\"lobby_code\":" + q(v.lobby_code) + ",\"game_id\":" + q(v.game_id) + ",\"game_title\":" + q(v.game_title) +
               ",\"system\":" + q(v.system) + ",\"expires_at\":" + std::to_string(v.expires_at) + "}";
    }
    void send_uid(const char* type, const std::string& uid)
    {
        send(std::string("{\"type\":\"") + type + "\",\"uid\":" + q(uid) + "}");
    }

    /* ---- parsing of data shapes */
    static Status parse_status(const Json* j)
    {
        Status s;
        if (!j || j->type != Json::Object) return s;
        std::string st = j->str("state");
        s.state = st == "online" ? PresenceState::Online : st == "in_lobby" ? PresenceState::InLobby
                : st == "in_game" ? PresenceState::InGame : PresenceState::Offline;
        s.game_id = j->str("game_id");
        s.game_title = j->str("game_title");
        s.system = j->str("system");
        s.lobby_code = j->str("lobby_code");
        s.joinable = jbool(*j, "joinable");
        s.players = (int)jnum(*j, "players");
        s.max_players = (int)jnum(*j, "max_players");
        s.updated = jnum(*j, "updated");
        return s;
    }
    static Friend parse_friend(const Json& j)
    {
        Friend f;
        f.uid = j.str("uid"); f.code = j.str("code"); f.name = j.str("name"); f.note = j.str("note");
        f.since = jnum(j, "since");
        f.status = parse_status(j.get("status"));
        return f;
    }
    static Request parse_req(const Json& j)
    {
        Request r;
        r.uid = j.str("uid"); r.code = j.str("code"); r.name = j.str("name"); r.at = jnum(j, "at");
        return r;
    }
    static Invite parse_invite(const Json* j)
    {
        Invite v;
        if (!j || j->type != Json::Object) return v;
        v.lobby_code = j->str("lobby_code"); v.game_id = j->str("game_id"); v.game_title = j->str("game_title");
        v.system = j->str("system"); v.expires_at = jnum(*j, "expires_at");
        return v;
    }
    Message parse_msg(const Json& j) const
    {
        Message m;
        m.id = j.str("id"); m.from = j.str("from"); m.from_name = j.str("from_name"); m.to = j.str("to");
        m.invite = j.str("kind") == "invite";
        m.text = j.str("text");
        m.inv = parse_invite(j.get("invite"));
        m.at = jnum(j, "at");
        m.mine = (m.from == id.user_id);
        return m;
    }

    /* ---- list helpers */
    Friend* find(const std::string& uid)
    {
        for (auto& f : friends) if (f.uid == uid) return &f;
        return nullptr;
    }
    static void drop(std::vector<Request>& v, const std::string& uid)
    {
        v.erase(std::remove_if(v.begin(), v.end(), [&](const Request& r) { return r.uid == uid; }), v.end());
    }
    static bool has_req(const std::vector<Request>& v, const std::string& uid)
    {
        for (const Request& r : v) if (r.uid == uid) return true;
        return false;
    }
    static void put(std::vector<Request>& v, const Request& r)
    {
        drop(v, r.uid);
        v.push_back(r);
    }
    void push_event(Event e)
    {
        if (events.size() > 512) events.pop_front();
        events.push_back(std::move(e));
    }

    /* ---- conversations */
    std::string conv_path(const std::string& uid) const { return with_sep(data_dir) + "friends\\" + uid + ".json"; }
    Conv& conv(const std::string& uid)
    {
        Conv& c = convs[uid];
        if (!c.loaded) {
            c.loaded = true;
            if (is_b32(uid, 26)) {
                std::string text;
                Json j;
                if (read_file(conv_path(uid), &text) && json_parse(text, &j) && j.type == Json::Object) {
                    c.unread = (int)jnum(j, "unread");
                    if (const Json* a = j.get("msgs"))
                        for (const Json& m : a->a) c.msgs.push_back(parse_msg(m));
                }
            }
        }
        return c;
    }
    static void trim(Conv& c)
    {
        std::stable_sort(c.msgs.begin(), c.msgs.end(), [](const Message& a, const Message& b) { return a.at < b.at; });
        if (c.msgs.size() > 200) c.msgs.erase(c.msgs.begin(), c.msgs.end() - 200);
    }
    /* returns true if it was new */
    bool add_msg(const std::string& peer, const Message& m, bool count_unread)
    {
        Conv& c = conv(peer);
        if (!m.id.empty())
            for (const Message& x : c.msgs) if (x.id == m.id) return false;
        c.msgs.push_back(m);
        if (count_unread && !m.mine) c.unread++;
        trim(c);
        c.dirty = true;
        return true;
    }
    void flush(bool force)
    {
        for (auto& kv : convs) {
            Conv& c = kv.second;
            if (!c.dirty || !is_b32(kv.first, 26)) continue;
            std::string o = "{\"unread\":" + std::to_string(c.unread) + ",\"msgs\":[";
            for (size_t i = 0; i < c.msgs.size(); i++) {
                const Message& m = c.msgs[i];
                if (i) o += ",";
                o += "{\"id\":" + q(m.id) + ",\"from\":" + q(m.from) + ",\"from_name\":" + q(m.from_name) + ",\"to\":" + q(m.to) +
                     ",\"kind\":" + (m.invite ? "\"invite\"" : "\"text\"") + ",\"text\":" + q(m.text) +
                     ",\"invite\":" + invite_json(m.inv) + ",\"at\":" + std::to_string(m.at) + "}";
            }
            o += "]}\n";
            if (write_file(conv_path(kv.first), o)) c.dirty = false;
        }
        (void)force;
    }

    /* ---- connection */
    std::string url() const
    {
        std::string base = server.empty() ? default_server() : server;
        while (!base.empty() && base.back() == '/') base.pop_back();
        return base + "/friends?uid=" + url_encode(id.user_id);
    }
    void connect()
    {
        hello_sent = welcomed = false;
        status_text = "Connecting...";
        net_log("Friends: connecting %s", url().c_str());
        ws.connect(url());
    }
    void schedule_reconnect(const std::string& why)
    {
        ws.close();
        welcomed = hello_sent = false;
        // flag as offline in friend list: presence is unknown while disconnected
        reconnect_at = now_ms() + backoff_ms;
        net_log("Friends: %s; retry in %u ms", why.c_str(), backoff_ms);
        status_text = "Disconnected (" + why + ")";
        backoff_ms = std::min<uint32_t>(backoff_ms * 2, 30000);
    }

    /* QA/test clients are never auto-friended to the owner by the server. */
    bool is_test_client() const { return is_qa_run() || name.rfind("qa-", 0) == 0; }
    void send_hello()
    {
        hello_sent = true;
        ws.send_text("{\"type\":\"hello\",\"secret\":" + q(id.secret) + ",\"name\":" + q(name) + ",\"version\":\"retrohub-1\",\"auto_friend\":" + (auto_friend ? "true" : "false") + (is_test_client() ? ",\"test\":true" : "") + "}");
    }

    void normalize_lists() { dedupe_friends(friends, id.user_id); dedupe_requests(friends, incoming, outgoing, id.user_id); }

    /* Identity retirement: after this PC switches to an older/linked identity, the identity it replaced is
     * handed to the server (over the NEW authed socket) so its friendships move to the new uid. Persisted
     * until the server confirms, so a crash or an offline start does not lose it. */
    std::vector<Identity> retire;   // every older identity of this person still to be retired (pending file, backups, vault)
    std::string retire_path() const { return data_dir + "pending-retire.json"; }
    std::string retired_ids_path() const { return data_dir + "retired-ids.txt"; }
    void load_retire()
    {
        retire = older_identities(data_dir, vault_dir, id.user_id);
        std::error_code ec;
        Identity r;
        if (fs::exists(upath(retire_path()), ec) && identity_read(retire_path(), &r) && r.user_id == id.user_id) fs::remove(upath(retire_path()), ec);
    }
    void send_retire()
    {
        int n = 0;
        for (const Identity& r : retire) {
            if (++n > 8) break;   // server allows 10 per hour
            send("{\"type\":\"retire_old\",\"uid\":" + q(r.user_id) + ",\"secret\":" + q(r.secret) + "}");
        }
    }
    /* uid empty: forget all (legacy single reply). */
    void retire_done(const std::string& uid = std::string())
    {
        std::error_code ec;
        for (size_t i = 0; i < retire.size();) {
            if (uid.empty() || retire[i].user_id == uid) {
                std::ofstream f(upath(retired_ids_path()), std::ios::app);
                f << retire[i].user_id << '\n';
                Identity pr;
                if (identity_read(retire_path(), &pr) && pr.user_id == retire[i].user_id) fs::remove(upath(retire_path()), ec);
                retire.erase(retire.begin() + i);
            } else ++i;
        }
    }

    void on_welcome(const Json& j)
    {
        welcomed = true;
        backoff_ms = 1000;
        last_keepalive = now_ms();
        status_text = "Online";
        friends.clear(); incoming.clear(); outgoing.clear(); blocked.clear();
        if (const Json* a = j.get("friends")) for (const Json& f : a->a) friends.push_back(parse_friend(f));
        if (const Json* r = j.get("requests")) {
            if (const Json* a = r->get("incoming")) for (const Json& x : a->a) incoming.push_back(parse_req(x));
            if (const Json* a = r->get("outgoing")) for (const Json& x : a->a) outgoing.push_back(parse_req(x));
        }
        if (const Json* a = j.get("blocked"))
            for (const Json& x : a->a) if (x.type == Json::String) blocked.push_back(x.s);
        {   // usernames: the server's name for this account wins over whatever we sent in the hello
            username = j.str("username");
            needs_new_name = j.boolean("needs_new_name") || (username.empty() && j.get("username") != nullptr);
            username_known = true;
            if (!username.empty()) name = username;
            Event e; e.kind = Event::UsernameChanged; e.name = username; e.ok = !username.empty() && !needs_new_name;
            events.push_back(std::move(e));
        }
        normalize_lists();
        send_retire();
        if (have_status) send("{\"type\":\"status\",\"status\":" + status_json(my_status) + "}");

        std::vector<std::string> ids;
        if (const Json* a = j.get("inbox")) {
            std::vector<Message> in;
            for (const Json& x : a->a) in.push_back(parse_msg(x));
            std::stable_sort(in.begin(), in.end(), [](const Message& a2, const Message& b) { return a2.at < b.at; });
            for (const Message& m : in) {
                if (!m.id.empty()) ids.push_back(m.id);
                emit_message(m);
            }
        }
        if (!ids.empty()) ack_ids(ids);
    }

    void emit_message(const Message& m)
    {
        const std::string& peer = m.mine ? m.to : m.from;
        if (!add_msg(peer, m, true)) return;
        if (m.mine) return;
        Event e;
        e.kind = m.invite ? Event::InviteReceived : Event::MessageReceived;
        e.uid = m.from; e.name = m.from_name; e.text = m.text; e.msg = m; e.invite = m.inv;
        push_event(std::move(e));
    }

    void ack_ids(const std::vector<std::string>& ids)
    {
        std::string o = "{\"type\":\"ack\",\"ids\":[";
        for (size_t i = 0; i < ids.size(); i++) o += (i ? "," : "") + q(ids[i]);
        send(o + "]}");
    }

    void on_presence(const Json& j)
    {
        Friend* f = find(j.str("uid"));
        if (!f) return;
        bool was = f->online();
        std::string nm = j.str("name");
        if (!nm.empty()) f->name = nm;
        f->status = parse_status(j.get("status"));
        Event e;
        e.uid = f->uid; e.name = f->name;
        bool now = f->online();
        e.kind = (!was && now) ? Event::FriendOnline : (was && !now) ? Event::FriendOffline : Event::PresenceChanged;
        push_event(std::move(e));
    }

    /* returns true if the identity must be regenerated */
    bool on_server_message(const std::string& text)
    {
        Json j;
        if (!json_parse(text, &j) || j.type != Json::Object) return false;
        std::string type = j.str("type");
        if (type == "welcome") on_welcome(j);
        else if (type == "presence") on_presence(j);
        else if (type == "retired") retire_done(j.str("uid"));
        else if (type == "request") {
            const Json* r = j.get("req");
            if (!r) return false;
            Request rq = parse_req(*r);
            if (rq.uid == id.user_id || find(rq.uid)) return false;   // yourself / already a friend: not a request
            if (j.str("dir") == "out") { if (!has_req(incoming, rq.uid)) put(outgoing, rq); return false; }
            drop(outgoing, rq.uid);
            put(incoming, rq);
            Event e; e.kind = Event::RequestReceived; e.uid = rq.uid; e.name = rq.name; e.text = rq.code;
            push_event(std::move(e));
        } else if (type == "request_out") {   // optional: server confirms an outgoing request
            if (const Json* r = j.get("req")) {
                Request rq = parse_req(*r);
                if (rq.uid != id.user_id && !find(rq.uid) && !has_req(incoming, rq.uid)) put(outgoing, rq);
            }
        } else if (type == "request_update") {
            std::string uid = j.str("uid");
            std::string name;
            for (auto& r : incoming) if (r.uid == uid) name = r.name;
            for (auto& r : outgoing) if (r.uid == uid) name = r.name;
            size_t before = incoming.size() + outgoing.size();
            drop(incoming, uid); drop(outgoing, uid);
            if (before == incoming.size() + outgoing.size()) return false;   // already resolved locally
            Event e; e.kind = Event::RequestResolved; e.uid = uid; e.name = name; e.text = j.str("result");
            push_event(std::move(e));
        } else if (type == "block_update") {
            std::string uid = j.str("uid");
            blocked.erase(std::remove(blocked.begin(), blocked.end(), uid), blocked.end());
            if (jbool(j, "blocked")) blocked.push_back(uid);
        } else if (type == "friend_added") {
            const Json* fj = j.get("friend");
            if (!fj) return false;
            Friend f = parse_friend(*fj);
            drop(incoming, f.uid); drop(outgoing, f.uid);
            if (f.uid == id.user_id) return false;
            if (Friend* old = find(f.uid)) *old = f; else friends.push_back(f);
            Event e; e.kind = Event::FriendAdded; e.uid = f.uid; e.name = f.name; e.text = f.code;
            e.automatic = jbool(j, "auto");
            push_event(std::move(e));
        } else if (type == "friend_removed") {
            std::string uid = j.str("uid");
            std::string nm;
            if (Friend* f = find(uid)) nm = f->name;
            friends.erase(std::remove_if(friends.begin(), friends.end(), [&](const Friend& f) { return f.uid == uid; }), friends.end());
            Event e; e.kind = Event::FriendRemoved; e.uid = uid; e.name = nm;
            push_event(std::move(e));
        } else if (type == "msg") {
            if (const Json* m = j.get("msg")) {
                Message msg = parse_msg(*m);
                emit_message(msg);
                if (!msg.id.empty()) ack_ids({msg.id});
            }
        } else if (type == "history") {
            std::string with = j.str("with");
            const Json* a = j.get("msgs");
            if (!a) a = j.get("messages");
            if (a && is_b32(with, 26))
                for (const Json& m : a->a) add_msg(with, parse_msg(m), false);
        } else if (type == "error") {
            std::string reason = j.str("reason", "error");
            if (retire_error_is_terminal(j.str("ref"), j.str("detail"))) retire_done(j.str("uid"));   // can never succeed: stop resending
            if (!welcomed && (reason == "code-taken" || reason == "code-collision" || reason == "collision")) return true;
            Event e; e.kind = Event::Error; e.text = reason; e.name = j.str("ref"); e.detail = j.str("detail");
            push_event(std::move(e));
        } else if (type == "username_check" || type == "username_result") {
            Event e; e.kind = type == "username_check" ? Event::UsernameChecked : Event::UsernameResult;
            e.ref = (int)j.num("ref", 0); e.name = j.str("name"); e.ok = j.boolean("ok") || j.boolean("available");
            e.text = e.ok ? std::string() : j.str("reason", "unavailable"); e.hold_days = (int)j.num("hold_days", 0);
            if (const Json* a = j.get("suggestions")) for (const Json& x : a->a) if (x.type == Json::String) e.suggestions.push_back(x.s);
            if (e.kind == Event::UsernameResult && e.ok) { username = e.name; needs_new_name = false; if (!e.name.empty()) name = e.name; }
            push_event(std::move(e));
        } else if (type == "username") {   // pushed to every device of the account when the name changes elsewhere
            username = j.str("name"); needs_new_name = j.boolean("needs_new_name"); if (!username.empty()) name = username;
            Event e; e.kind = Event::UsernameChanged; e.name = username; e.ok = !username.empty() && !needs_new_name;
            push_event(std::move(e));
        } else if (type == "invite_result") {
            Event e; e.kind = Event::InviteResult;
            if (const Json* a = j.get("results"))
                for (const Json& r : a->a) {
                    std::string u = r.str("uid");
                    if (!is_b32(u, 26)) continue;
                    if (r.boolean("ok")) e.ok_uids.push_back(u);
                    else { e.failed_uids.push_back(u); e.failed_reasons.push_back(r.str("reason", "undeliverable")); }
                }
            push_event(std::move(e));
        }
        return false;
    }
};

/* ====================================================================== public */

bool retire_error_is_terminal(const std::string& ref, const std::string& detail) { return ref == "retire_old" && detail == "bad-secret"; }

static std::string sanitize_name(const std::string& in)
{
    std::string o;
    int chars = 0;
    for (size_t i = 0; i < in.size() && chars < 24; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x20 || c == 0x7f) continue;
        if (c == ' ' && o.empty()) continue;
        o += (char)c;
        if ((c & 0xC0) != 0x80) {
            chars++;
            if (chars == 24) {   // finish this code point's continuation bytes
                while (i + 1 < in.size() && ((unsigned char)in[i + 1] & 0xC0) == 0x80) o += in[++i];
            }
        }
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    // never cut inside a code point when a 25th char started (loop ended before it)
    return o.empty() ? "Player" : o;
}

FriendsClient::FriendsClient() : impl_(new Impl) {}
FriendsClient::~FriendsClient() { stop(); }

void FriendsClient::set_paths(const std::string& data_dir, const std::string& vault_dir)
{
    impl_->data_dir = with_sep(data_dir);
    impl_->vault_dir = vault_dir;
}

bool FriendsClient::start(const std::string& server, const std::string& name)
{
    stop();
    Impl& d = *impl_;
    d.server = server;
    d.name = sanitize_name(name);
    d.id = identity_load_or_create(d.data_dir, d.vault_dir);
    d.load_retire();
    d.started = true;
    d.gave_up = false;
    d.backoff_ms = 1000;
    d.convs.clear();
    d.username.clear(); d.needs_new_name = false; d.username_known = false;
    d.friends.clear(); d.incoming.clear(); d.outgoing.clear(); d.blocked.clear();
    d.connect();
    return true;
}

void FriendsClient::stop()
{
    Impl& d = *impl_;
    d.flush(true);
    d.ws.close();
    d.started = d.welcomed = d.hello_sent = false;
    d.status_text = "Offline";
    for (auto& f : d.friends) f.status.state = PresenceState::Offline;
}

void FriendsClient::update()
{
    Impl& d = *impl_;
    if (!d.started) return;
    uint32_t now = now_ms();

    if (now - d.last_flush > 2000) { d.last_flush = now; d.flush(false); }
    if (d.gave_up) return;

    if (d.reconnect_at) {
        if ((int32_t)(now - d.reconnect_at) >= 0) { d.reconnect_at = 0; d.connect(); }
        else {
            d.status_text = "Reconnecting in " + std::to_string((d.reconnect_at - now + 999) / 1000) + "s";
            return;
        }
    }

    WsState st = d.ws.state();
    if (st == WsState::Open && !d.hello_sent) d.send_hello();

    std::string text;
    bool regen = false;
    while (d.ws.poll_text(&text))
        if (d.on_server_message(text)) regen = true;

    if (regen) {
        net_log("Friends: friend-code collision, regenerating identity");
        d.ws.close();
        d.id = identity_generate();
        identity_save(d.id, d.data_dir, d.vault_dir);
        d.connect();
        return;
    }

    st = d.ws.state();
    if (st == WsState::Failed || (st == WsState::Closed && d.started)) {
        int code = d.ws.close_code();
        if (code == 4004) {   // friend-code prefix collision at registration
            net_log("Friends: friend-code collision, regenerating identity");
            d.ws.close();
            d.id = identity_generate();
            identity_save(d.id, d.data_dir, d.vault_dir);
            d.connect();
            return;
        }
        if (code == 4005) {   // this uid was retired: its friendships moved to the identity that replaced it
            d.ws.close();
            d.welcomed = false;
            d.gave_up = true;
            d.status_text = "This identity was replaced by a newer one";
            return;
        }
        if (code == 4003) {
            d.ws.close();
            d.welcomed = false;
            d.gave_up = true;
            d.status_text = "Identity rejected by server (bad secret)";
            return;
        }
        std::string why = d.welcomed || d.hello_sent ? "connection lost" : d.ws.error();
        if (why.empty()) why = "connection failed";
        bool had = d.welcomed;
        d.schedule_reconnect(why);
        if (had)
            for (auto& f : d.friends)
                if (f.online()) { f.status.state = PresenceState::Offline; }   // unknown while offline
        return;
    }

    if (d.welcomed && now - d.last_keepalive >= 20000) {
        d.last_keepalive = now;
        d.ws.send_text("{\"type\":\"keepalive\"}");
    }
}

void FriendsClient::apply_adopted(const Identity& v)
{
    Impl& d = *impl_;
    {   // remember the identity being replaced so the server can retire it and move its friendships
        Identity old = d.id;
        if (!old.valid()) identity_read(d.data_dir + "identity.json", &old);
        if (old.valid() && old.user_id != v.user_id) {
            Identity prev;   // an earlier, still-unretired replaced identity: its backup file keeps it (older_identities scans them)
            if (identity_read(d.retire_path(), &prev) && prev.user_id != old.user_id) {
                std::string bp = d.data_dir + "identity-backup-pending-" + prev.user_id.substr(0, 8) + ".json";
                if (!fs::exists(upath(bp))) write_file(bp, identity_json(prev));
            }
            write_file(d.retire_path(), identity_json(old));
        }
    }
    if (d.started) {
        std::string server = d.server, name = d.name;
        start(server, name);   // reloads identity.json, drops per-account state, reconnects
    } else {
        d.id = v;
    }
    Event e;
    e.kind = Event::IdentityAdopted;
    e.uid = v.user_id;
    e.text = friend_code(v.user_id);
    d.events.push_back(e);
}

bool FriendsClient::adopt_vault_identity_if_older()
{
    Impl& d = *impl_;
    Identity v;
    if (!identity_adopt_vault_if_older(d.data_dir, d.vault_dir, &v)) return false;
    apply_adopted(v);
    return true;
}

bool FriendsClient::adopt_identity(const Identity& v)
{
    Impl& d = *impl_;
    if (!identity_adopt(d.data_dir, d.vault_dir, v)) return false;
    apply_adopted(v);
    return true;
}

bool FriendsClient::connected() const { return impl_->welcomed; }
std::string FriendsClient::status_text() const { return impl_->status_text; }
std::string FriendsClient::my_code() const { return friend_code(impl_->id.user_id); }
std::string FriendsClient::my_uid() const { return impl_->id.user_id; }
std::string FriendsClient::my_secret() const { return impl_->id.secret; }
void FriendsClient::set_auto_friend(bool on)
{
    impl_->auto_friend = on;
    impl_->send(std::string("{\"type\":\"prefs\",\"auto_friend\":") + (on ? "true" : "false") + "}");
}
std::set<std::string> FriendsClient::retired_ids() const { return read_retired_ids(impl_->data_dir); }

void FriendsClient::set_status(const Status& s)
{
    Impl& d = *impl_;
    d.my_status = s;
    d.have_status = true;
    d.send("{\"type\":\"status\",\"status\":" + Impl::status_json(s) + "}");
}

void FriendsClient::add_friend(const std::string& code)
{
    std::string c = code;
    for (char& ch : c) ch = (char)toupper((unsigned char)ch);
    // allow "7KQ2M9XD" and "7KQ2-M9XD"
    if (c.compare(0, 3, "RH-") != 0) {
        c.erase(std::remove(c.begin(), c.end(), '-'), c.end());
        if (c.size() == 8) c = "RH-" + c.substr(0, 4) + "-" + c.substr(4);
    }
    impl_->send("{\"type\":\"add_friend\",\"code\":" + q(c) + "}");
}

void FriendsClient::accept(const std::string& uid) { Impl::drop(impl_->incoming, uid); impl_->send_uid("accept", uid); }
void FriendsClient::decline(const std::string& uid) { Impl::drop(impl_->incoming, uid); impl_->send_uid("decline", uid); }
void FriendsClient::cancel(const std::string& uid) { Impl::drop(impl_->outgoing, uid); impl_->send_uid("cancel", uid); }

void FriendsClient::remove_friend(const std::string& uid)
{
    Impl& d = *impl_;
    d.send_uid("remove_friend", uid);   // local list updates when friend_removed arrives
}

void FriendsClient::block(const std::string& uid)
{
    Impl& d = *impl_;
    if (std::find(d.blocked.begin(), d.blocked.end(), uid) == d.blocked.end()) d.blocked.push_back(uid);
    d.friends.erase(std::remove_if(d.friends.begin(), d.friends.end(), [&](const Friend& f) { return f.uid == uid; }), d.friends.end());
    Impl::drop(d.incoming, uid); Impl::drop(d.outgoing, uid);
    d.send_uid("block", uid);
}

void FriendsClient::unblock(const std::string& uid)
{
    Impl& d = *impl_;
    d.blocked.erase(std::remove(d.blocked.begin(), d.blocked.end(), uid), d.blocked.end());
    d.send_uid("unblock", uid);
}

void FriendsClient::send_message(const std::string& uid, const std::string& text)
{
    Impl& d = *impl_;
    size_t n = std::min<size_t>(text.size(), 500);
    while (n > 0 && n < text.size() && ((unsigned char)text[n] & 0xC0) == 0x80) n--;
    std::string t = text.substr(0, n);
    if (t.empty() || !d.welcomed) return;
    Message m;
    m.id = "local-" + std::to_string(unix_ms()) + "-" + std::to_string(d.local_seq++);
    m.from = d.id.user_id; m.from_name = d.name; m.to = uid; m.text = t; m.at = unix_ms(); m.mine = true;
    d.add_msg(uid, m, false);
    d.send("{\"type\":\"message\",\"to\":" + q(uid) + ",\"text\":" + q(t) + "}");
}

void FriendsClient::send_invite(const std::vector<std::string>& uids, const Invite& inv)
{
    if (uids.empty()) return;
    std::string o = "{\"type\":\"invite\",\"to\":[";
    for (size_t i = 0; i < uids.size(); i++) o += (i ? "," : "") + q(uids[i]);
    Invite v = inv;
    if (v.expires_at == 0) v.expires_at = unix_ms() + 30 * 60 * 1000;
    impl_->send(o + "],\"invite\":" + Impl::invite_json(v) + "}");
}

void FriendsClient::ack(const std::vector<std::string>& ids) { if (!ids.empty()) impl_->ack_ids(ids); }
void FriendsClient::rename(const std::string& name)
{
    impl_->name = sanitize_name(name);
    impl_->send("{\"type\":\"rename\",\"name\":" + q(impl_->name) + "}");
}
int FriendsClient::check_username(const std::string& name)
{
    Impl& d = *impl_;
    if (!d.welcomed) return 0;
    int ref = ++d.ref_seq;
    d.send("{\"type\":\"username_check\",\"name\":" + q(name) + ",\"ref\":" + std::to_string(ref) + "}");
    return ref;
}
int FriendsClient::claim_username(const std::string& name)
{
    Impl& d = *impl_;
    if (!d.welcomed) return 0;
    int ref = ++d.ref_seq;
    d.send("{\"type\":\"username_claim\",\"name\":" + q(name) + ",\"ref\":" + std::to_string(ref) + "}");
    return ref;
}
void FriendsClient::add_friend_username(const std::string& username)
{
    impl_->send("{\"type\":\"add_friend\",\"username\":" + q(username) + "}");
}
std::string FriendsClient::username() const { return impl_->username; }
bool FriendsClient::needs_new_name() const { return impl_->needs_new_name; }
bool FriendsClient::username_known() const { return impl_->username_known; }
void FriendsClient::request_history(const std::string& uid)
{
    impl_->send("{\"type\":\"history\",\"with\":" + q(uid) + "}");
}

const std::vector<Friend>& FriendsClient::friends() const { return impl_->friends; }
const std::vector<Request>& FriendsClient::incoming_requests() const { return impl_->incoming; }
const std::vector<Request>& FriendsClient::outgoing_requests() const { return impl_->outgoing; }
const std::vector<std::string>& FriendsClient::blocked() const { return impl_->blocked; }
const Friend* FriendsClient::find_friend(const std::string& uid) const
{
    for (const auto& f : impl_->friends) if (f.uid == uid) return &f;
    return nullptr;
}

const std::vector<Message>& FriendsClient::conversation(const std::string& uid) { return impl_->conv(uid).msgs; }
int FriendsClient::unread(const std::string& uid) const
{
    auto it = impl_->convs.find(uid);
    if (it != impl_->convs.end()) return it->second.unread;
    return impl_->conv(uid).unread;   // lazy load (Impl members are not const-qualified through unique_ptr)
}
int FriendsClient::total_unread() const
{
    int n = 0;
    for (const auto& f : impl_->friends) n += unread(f.uid);
    return n;
}
void FriendsClient::mark_read(const std::string& uid)
{
    Conv& c = impl_->conv(uid);
    if (c.unread) { c.unread = 0; c.dirty = true; }
}

bool FriendsClient::poll_event(Event* out)
{
    Impl& d = *impl_;
    if (d.events.empty()) return false;
    *out = std::move(d.events.front());
    d.events.pop_front();
    return true;
}

} // namespace hubnet
