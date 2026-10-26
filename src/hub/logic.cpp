#include "logic.h"

#include "remix_data.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace hubui {

bool merge_installer_games_dir(Settings* s, const std::string& text) {
    std::string t = text;
    if (t.compare(0, 3, "\xEF\xBB\xBF") == 0) t.erase(0, 3);
    size_t nl = t.find_first_of("\r\n");
    if (nl != std::string::npos) t.resize(nl);
    size_t a = t.find_first_not_of(" \t\"");
    size_t b = t.find_last_not_of(" \t\"");
    t = a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
    if (t.empty()) return false;
    bool abs = (t.size() >= 3 && std::isalpha((unsigned char)t[0]) && t[1] == ':' && (t[2] == '\\' || t[2] == '/')) ||
               t.compare(0, 2, "\\\\") == 0;
    if (!abs) return false;
    auto norm = [](std::string x) {
        while (x.size() > 3 && (x.back() == '\\' || x.back() == '/')) x.pop_back();
        for (auto& ch : x) { if (ch == '/') ch = '\\'; ch = (char)std::tolower((unsigned char)ch); }
        return x;
    };
    std::string n = norm(t);
    for (auto& f : s->rom_folders) if (norm(f) == n) return false;
    s->rom_folders.push_back(t);
    return true;
}
/* ---------------- settings ---------------- */

std::string settings_to_json(const Settings& s) {
    std::ostringstream o;
    o << "{\n"
      << "  \"name\": \"" << json::escape(s.name) << "\",\n"
      << "  \"rom_folders\": [";
    for (size_t i = 0; i < s.rom_folders.size(); ++i)
        o << (i ? ", " : "") << "\"" << json::escape(s.rom_folders[i]) << "\"";
    o << "],\n"
      << "  \"server\": \"" << json::escape(s.server) << "\",\n"
      << "  \"input_delay\": " << s.input_delay << ",\n"
      << "  \"delay_auto\": " << (s.delay_auto ? "true" : "false") << ",\n"
      << "  \"last_game\": \"" << json::escape(s.last_game) << "\",\n"
      << "  \"smash_exe\": \"" << json::escape(s.smash_exe) << "\",\n"
      << "  \"gb_core\": \"" << json::escape(s.gb_core) << "\",\n"
      << "  \"fullscreen\": " << (s.fullscreen ? "true" : "false") << ",\n"
      << "  \"sync_folder\": \"" << json::escape(s.sync_folder) << "\",\n"
      << "  \"sync_auto\": " << (s.sync_auto ? "true" : "false") << ",\n"
      << "  \"backup_shots\": " << (s.backup_shots ? "true" : "false") << ",\n"
      << "  \"cloud_states\": " << (s.cloud_states ? "true" : "false") << ",\n"
      << "  \"library_sort\": \"" << json::escape(s.library_sort) << "\",\n"
      << "  \"cloud_enabled\": " << (s.cloud_enabled ? "true" : "false") << ",\n"
      << "  \"sync_card_dismissed\": " << (s.sync_card_dismissed ? "true" : "false") << ",\n"
      << "  \"smash_save_notice_shown\": " << (s.smash_save_notice_shown ? "true" : "false") << ",\n"
      << "  \"changelog_seen\": \"" << json::escape(s.changelog_seen) << "\",\n"
      << "  \"kt_intro_seen\": " << (s.kt_intro_seen ? "true" : "false") << ",\n"
      << "  \"vault_dir\": \"" << json::escape(s.vault_dir) << "\",\n"
      << "  \"download_art\": " << (s.download_art ? "true" : "false") << ",\n"
      << "  \"sidebar_mode\": " << s.sidebar_mode << ",\n"
      << "  \"show_friends\": " << (s.show_friends ? "true" : "false") << ",\n"
      << "  \"notify_in_game\": " << (s.notify_in_game ? "true" : "false") << ",\n"
      << "  \"notif_sound\": " << (s.notif_sound ? "true" : "false") << ",\n"
      << "  \"share_playtime\": " << (s.share_playtime ? "true" : "false") << ",\n"
      << "  \"auto_friend\": " << (s.auto_friend ? "true" : "false") << ",\n"
      << "  \"username_claimed\": " << (s.username_claimed ? "true" : "false") << ",\n"
      << "  \"find_games_done\": " << (s.find_games_done ? "true" : "false") << ",\n"
      << "  \"setup_done\": " << (s.setup_done ? "true" : "false") << ",\n"
      << "  \"games_dest\": \"" << json::escape(s.games_dest) << "\",\n"
      << "  \"ps1_bios\": \"" << json::escape(s.ps1_bios) << "\",\n"
      << "  \"ps1_openbios\": " << (s.ps1_openbios ? "true" : "false") << ",\n"
      << "  \"ps2_bios_dir\": \"" << json::escape(s.ps2_bios_dir) << "\",\n"
      << "  \"cemu_manage_controls\": " << (s.cemu_manage_controls ? "true" : "false") << ",\n"
      << "  \"cemu_exe\": \"" << json::escape(s.cemu_exe) << "\"\n"
      << "}\n";
    return o.str();
}

static bool json_bool(const Json& j, const char* key, bool def) {
    const Json* v = j.get(key);
    return (v && v->type == Json::Bool) ? v->b : def;
}

bool settings_from_json(const std::string& text, Settings* out) {
    Json j;
    if (!json::parse(text, &j) || j.type != Json::Object) return false;
    out->name = j.str("name", out->name);
    const Json* rf = j.get("rom_folders");
    if (rf && rf->type == Json::Array) {
        out->rom_folders.clear();
        for (auto& v : rf->a)
            if (v.type == Json::String && !v.s.empty() &&
                std::find(out->rom_folders.begin(), out->rom_folders.end(), v.s) == out->rom_folders.end())
                out->rom_folders.push_back(v.s);
    } else {
        /* migrate the old single-folder setting */
        std::string old = j.str("rom_folder");
        if (!old.empty() && std::find(out->rom_folders.begin(), out->rom_folders.end(), old) == out->rom_folders.end())
            out->rom_folders.push_back(old);
    }
    out->server = j.str("server", out->server);
    out->input_delay = (int)j.num("input_delay", out->input_delay);
    out->input_delay = std::max(1, std::min(8, out->input_delay));
    if (const Json* da = j.get("delay_auto")) if (da->type == Json::Bool) out->delay_auto = da->b;
    out->last_game = j.str("last_game", out->last_game);
    out->smash_exe = j.str("smash_exe", out->smash_exe);
    std::string core = j.str("gb_core", out->gb_core);
    out->gb_core = (core == "gambatte" || core == "sameboy") ? core : "sameboy";
    out->fullscreen = json_bool(j, "fullscreen", out->fullscreen);
    out->sync_folder = j.str("sync_folder", out->sync_folder);
    out->sync_auto = json_bool(j, "sync_auto", out->sync_auto);
    out->backup_shots = json_bool(j, "backup_shots", out->backup_shots);
    out->cloud_states = json_bool(j, "cloud_states", out->cloud_states);
    if (const json::Value* ls = j.get("library_sort")) if (ls->is_string()) out->library_sort = ls->s;
    out->cloud_enabled = json_bool(j, "cloud_enabled", out->cloud_enabled);   // legacy sync_states / cloud_saves keys are ignored
    out->sync_card_dismissed = json_bool(j, "sync_card_dismissed", out->sync_card_dismissed);
    out->smash_save_notice_shown = json_bool(j, "smash_save_notice_shown", out->smash_save_notice_shown);
    out->kt_intro_seen = json_bool(j, "kt_intro_seen", out->kt_intro_seen);
    out->vault_dir = j.str("vault_dir", out->vault_dir);
    out->download_art = json_bool(j, "download_art", out->download_art);
    out->sidebar_mode = std::max(0, std::min(2, (int)j.num("sidebar_mode", out->sidebar_mode)));
    out->show_friends = json_bool(j, "show_friends", out->show_friends);
    out->notify_in_game = json_bool(j, "notify_in_game", out->notify_in_game);
    out->notif_sound = json_bool(j, "notif_sound", out->notif_sound);
    out->auto_friend = json_bool(j, "auto_friend", out->auto_friend);
    out->share_playtime = json_bool(j, "share_playtime", out->share_playtime);
    out->username_claimed = json_bool(j, "username_claimed", out->username_claimed);
    out->cemu_manage_controls = json_bool(j, "cemu_manage_controls", out->cemu_manage_controls);
    out->find_games_done = json_bool(j, "find_games_done", out->find_games_done);
    out->setup_done = json_bool(j, "setup_done", out->setup_done);
    out->changelog_seen = j.str("changelog_seen", out->changelog_seen);
    out->cemu_exe = j.str("cemu_exe", out->cemu_exe);
    out->ps1_bios = j.str("ps1_bios", out->ps1_bios);
    out->ps1_openbios = json_bool(j, "ps1_openbios", out->ps1_openbios);
    out->ps2_bios_dir = j.str("ps2_bios_dir", out->ps2_bios_dir);
    out->games_dest = j.str("games_dest", out->games_dest);
    return true;
}

bool load_settings(const std::string& path, Settings* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    return settings_from_json(ss.str(), out);
}

bool save_settings(const std::string& path, const Settings& s) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << settings_to_json(s);
        if (!f.good()) return false;
    }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

/* ---------------- lobby rules ---------------- */

Selectability game_selectable(const std::string& game_id, int max_players,
                              const std::vector<PeerInfo>& peers, int host_slot) {
    Selectability r;
    if ((int)peers.size() > max_players) { r.reason = "too many players (max " + std::to_string(max_players) + ")"; return r; }
    /* ownership token: the catalog md5 if advertised, else the key itself */
    auto token = [&](const PeerInfo& p, std::string* t) {
        auto it = p.games.find(game_id);
        if (it != p.games.end()) { *t = it->second; return true; }
        if (std::find(p.keys.begin(), p.keys.end(), game_id) != p.keys.end()) { *t = game_id; return true; }
        return false;
    };
    const PeerInfo* host = nullptr;
    for (auto& p : peers) if (p.slot == host_slot) host = &p;
    std::string t;
    for (auto& p : peers) {
        if (!p.hello_complete()) { r.reason = "waiting for " + p.name; return r; }   // every hello part must have arrived
        if (!token(p, &t)) { r.reason = p.name + " doesn't have this game"; return r; }
    }
    if (host) {
        std::string ref;
        token(*host, &ref);
        for (auto& p : peers) {
            token(p, &t);
            if (t != ref) { r.reason = "different ROM version (" + p.name + ")"; return r; }
        }
    }
    r.ok = true;
    return r;
}

std::string hello_json(const std::string& version, const std::map<std::string, std::string>& games,
                       const std::vector<std::string>& keys, const std::string& app) {
    std::string o = "{\"kind\":\"hello\",\"version\":\"" + json::escape(version) + "\"";
    if (!app.empty()) o += ",\"app\":\"" + json::escape(app) + "\"";
    o += ",\"games\":{";
    bool first = true;
    for (auto& g : games) {
        if (!first) o += ",";
        first = false;
        o += "\"" + json::escape(g.first) + "\":\"" + json::escape(g.second) + "\"";
    }
    o += "},\"keys\":[";
    for (size_t i = 0; i < keys.size(); ++i) o += (i ? ",\"" : "\"") + json::escape(keys[i]) + "\"";
    return o + "]}";
}

std::vector<std::string> hello_messages(const std::string& version, const std::map<std::string, std::string>& games,
                                        const std::vector<std::string>& keys, size_t max_bytes,
                                        const std::string& app) {
    std::vector<std::string> out;
    size_t i = 0;
    int part = 0;
    do {
        std::string o = "{\"kind\":\"hello\",\"version\":\"" + json::escape(version) + "\",\"part\":" + std::to_string(part) + ",\"parts\":@@";
        if (part == 0) {
            if (!app.empty()) o += ",\"app\":\"" + json::escape(app) + "\"";
            o += ",\"games\":{";
            bool f = true;
            for (auto& g : games) {
                if (!f) o += ",";
                f = false;
                o += "\"" + json::escape(g.first) + "\":\"" + json::escape(g.second) + "\"";
            }
            o += "}";
        }
        o += ",\"keys\":[";
        bool first = true;
        while (i < keys.size()) {
            std::string k = "\"" + json::escape(keys[i]) + "\"";
            if (!first && o.size() + k.size() + 3 > max_bytes) break;
            o += (first ? "" : ",") + k;
            first = false;
            ++i;
        }
        out.push_back(o + "]}");
        ++part;
    } while (i < keys.size());
    for (auto& m : out) m.replace(m.find("@@"), 2, std::to_string(out.size()));   // total part count, so receivers know when the hello is complete
    return out;
}

bool parse_hello(const Json& body, PeerInfo* out) {
    if (body.str("kind") != "hello") return false;
    bool cont = body.num("part", 0) > 0;   // continuation of a long key list
    if (cont) {
        const Json* k = body.get("keys");
        if (k && k->type == Json::Array)
            for (auto& v : k->a)
                if (v.type == Json::String) out->keys.push_back(v.s);
        if (out->hello) ++out->parts_got;
        return true;
    }
    const Json* g = body.get("games");
    out->games.clear();
    out->keys.clear();
    if (g && g->type == Json::Object)
        for (auto& kv : g->o)
            if (kv.second.type == Json::String) out->games[kv.first] = kv.second.s;
    const Json* k = body.get("keys");
    if (k && k->type == Json::Array)
        for (auto& v : k->a)
            if (v.type == Json::String) out->keys.push_back(v.s);
    out->app = body.str("app");
    out->parts_expected = std::max(1, (int)body.num("parts", 1));
    out->parts_got = 1;
    out->hello = true;
    return true;
}

/* ---- app version gate ---- */
static bool parse_dotted(const std::string& v, std::vector<long>* out) {
    size_t i = 0;
    while (i < v.size() && (v[i] == 'v' || v[i] == 'V' || v[i] == ' ')) ++i;
    bool any = false;
    while (i < v.size()) {
        if (!std::isdigit((unsigned char)v[i])) break;
        long n = 0;
        while (i < v.size() && std::isdigit((unsigned char)v[i])) { n = n * 10 + (v[i] - '0'); if (n > 1000000) n = 1000000; ++i; }
        out->push_back(n);
        any = true;
        if (i < v.size() && v[i] == '.') ++i; else break;
    }
    return any;
}

int compare_app_versions(const std::string& a, const std::string& b) {
    std::vector<long> x, y;
    if (!parse_dotted(a, &x) || !parse_dotted(b, &y)) return 2;
    size_t n = std::max(x.size(), y.size());
    for (size_t i = 0; i < n; ++i) {
        long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

std::string app_version_label(const std::string& v) {
    if (v.empty()) return "";
    return (v[0] == 'v' || v[0] == 'V') ? v : "v" + v;
}

static bool same_app_version(const std::string& a, const std::string& b) {
    int c = compare_app_versions(a, b);
    return c == 2 ? a == b : c == 0;
}

VersionGate version_gate(const std::string& host_app, const std::vector<PeerInfo>& peers, bool allow_mismatch) {
    VersionGate g;
    if (allow_mismatch) return g;
    std::string need = app_version_label(host_app);
    for (auto& p : peers) {
        if (!p.hello) continue;   // game_selectable reports "waiting for ..."
        if (p.app.empty()) {
            g.ok = false;
            g.reason = p.name + " is on an old RetroHub (unknown version) - everyone needs " + need + ". Ask them to update.";
            return g;
        }
        if (!same_app_version(p.app, host_app)) {
            g.ok = false;
            g.reason = p.name + " is on " + app_version_label(p.app) + " - everyone needs " + need + ". Ask them to update.";
            return g;
        }
    }
    return g;
}

std::string ready_block_reason(const std::string& my_app, const std::string& host_app, bool host_hello, bool allow_mismatch) {
    if (allow_mismatch) return "";
    switch (guest_version_action(my_app, host_app, host_hello)) {
        case GuestVersionAction::None: return "";
        case GuestVersionAction::Waiting: return "Checking versions...";
        case GuestVersionAction::UpdateNow: return "Update RetroHub to " + app_version_label(host_app) + " to match the host";
        case GuestVersionAction::HostNeedsUpdate: return "The host needs to update RetroHub first";
    }
    return "";
}

std::string join_names(const std::vector<std::string>& n) {
    if (n.empty()) return "";
    if (n.size() == 1) return n[0];
    std::string s;
    for (size_t i = 0; i + 1 < n.size(); ++i) s += (i ? ", " : "") + n[i];
    return s + " and " + n.back();
}

RoomActionState room_action_state(bool host, bool have_game, const std::string& game_title, const std::string& block,
                                  bool my_ready, const std::vector<RoomMember>& members) {
    RoomActionState r;
    r.total = (int)members.size();
    std::vector<std::string> waiting;
    for (auto& m : members) {
        if (m.host || m.ready) ++r.ready_count;
        else waiting.push_back(m.me ? "you" : m.name);
    }
    char cnt[48];
    std::snprintf(cnt, sizeof cnt, "%d of %d ready", r.ready_count, r.total);
    if (host) {
        r.is_launch = true;
        r.button_label = "LAUNCH";
        if (!have_game) { r.headline = "Pick a game to start"; r.detail = "Choose a game below, then everyone presses Ready."; r.tone = RoomTone::Warn; return r; }
        if (!block.empty()) { r.headline = "Can't launch yet"; r.detail = block; r.tone = RoomTone::Bad; return r; }
        if (!waiting.empty()) {
            r.headline = "Waiting for " + join_names(waiting) + "...";
            r.detail = std::string(cnt) + ". Launch unlocks when everyone is ready.";
            r.tone = RoomTone::Warn;
            return r;
        }
        r.headline = r.total > 1 ? "Everyone's ready - Launch!" : "Ready - Launch!";
        r.detail = "Starts " + game_title + " on every PC at the same moment.";
        r.tone = RoomTone::Good;
        r.button_enabled = true;
        return r;
    }
    r.ready_on = my_ready;
    r.button_label = my_ready ? "NOT READY" : "READY";
    r.button_enabled = my_ready || (have_game && block.empty());   // you can always back out
    if (!have_game) { r.headline = "Waiting for the host to pick a game"; r.detail = "You can press Ready once a game is chosen."; r.tone = RoomTone::Warn; return r; }
    if (!block.empty()) { r.headline = "You can't ready up yet"; r.detail = block; r.tone = RoomTone::Bad; return r; }
    if (my_ready) {
        r.headline = "You're ready";
        r.tone = RoomTone::Good;
        std::vector<std::string> others;
        for (auto& m : members) if (!m.host && !m.ready && !m.me) others.push_back(m.name);
        r.detail = others.empty() ? "Waiting for the host to launch " + game_title + "." : "Waiting for " + join_names(others) + " (" + cnt + ").";
        return r;
    }
    r.headline = "Not ready";
    r.detail = "Press Ready when you're set to play " + game_title + ".";
    r.tone = RoomTone::Warn;
    return r;
}

GuestVersionAction guest_version_action(const std::string& my_app, const std::string& host_app, bool host_hello) {
    if (!host_hello) return GuestVersionAction::Waiting;   // no hello yet: host_app is empty for a reason other than an old build
    if (host_app.empty()) return GuestVersionAction::HostNeedsUpdate;
    if (same_app_version(my_app, host_app)) return GuestVersionAction::None;
    return compare_app_versions(my_app, host_app) == -1 ? GuestVersionAction::UpdateNow : GuestVersionAction::HostNeedsUpdate;
}

std::string launch_json(const std::string& game, int launch, int delay, unsigned seed, const std::string& core, bool share_host_save,
                        const PortPlan* plan, const MatchSpec* match, const std::string& extra_json) {
    std::string o = "{\"kind\":\"launch\",\"game\":\"" + json::escape(game) + "\",\"launch\":" + std::to_string(launch) +
                    ",\"delay\":" + std::to_string(delay) + ",\"seed\":" + std::to_string(seed);
    if (!core.empty()) o += ",\"core\":\"" + json::escape(core) + "\"";
    o += std::string(",\"share_save\":") + (share_host_save ? "true" : "false");
    if (plan) o += ",\"ports\":[" + plan->ports + "],\"players\":" + std::to_string(plan->players);
    if (match) o += ",\"match\":" + match_json(*match);
    o += extra_json;
    return o + "}";
}

std::string np_ds_json() { return "{\"v\":1,\"mode\":\"wireless\"}"; }

bool parse_np_ds(const Json& m, std::string* mode) {
    const Json* n = m.get("np_ds");
    if (!n || n->type != Json::Object || (int)n->num("v", 0) != 1) return false;
    if (mode) *mode = n->str("mode");
    return n->str("mode") == "wireless";
}

bool parse_launch_plan(const Json& m, std::string* ports, int* players) {
    const Json* pa = m.get("ports");
    const Json* pl = m.get("players");
    if (!pa || pa->type != Json::Array || pa->a.size() != 4 || !pl || pl->type != Json::Number) return false;
    int owner[4];
    for (int i = 0; i < 4; ++i) {
        const Json& v = pa->a[(size_t)i];
        if (v.type != Json::Number || v.n != (double)(int)v.n || v.n < -1 || v.n > 3) return false;
        owner[i] = (int)v.n;
    }
    bool seen[4] = {false, false, false, false};
    int distinct = 0;
    bool gap = false;
    for (int i = 0; i < 4; ++i) {
        if (owner[i] < 0) { gap = true; continue; }
        if (gap) return false;   // owners are packed first
        if (!seen[owner[i]]) { seen[owner[i]] = true; ++distinct; }
    }
    if (distinct < 1 || pl->n != (double)distinct) return false;
    // each owner's ports must be consecutive (build_ports never interleaves)
    for (int i = 1; i < 4; ++i)
        if (owner[i] >= 0 && owner[i] != owner[i - 1])
            for (int k = 0; k < i - 1; ++k) if (owner[k] == owner[i]) return false;
    if (ports) {
        ports->clear();
        for (int i = 0; i < 4; ++i) *ports += (i ? "," : "") + std::to_string(owner[i]);
    }
    if (players) *players = distinct;
    return true;
}

std::string delay_arg(int delay) { return delay <= 0 ? " --delay auto" : " --delay " + std::to_string(std::min(delay, 8)); }

PortPlan build_ports(std::vector<MemberPads> m) {
    std::sort(m.begin(), m.end(), [](const MemberPads& a, const MemberPads& b) { return a.slot < b.slot; });
    PortPlan p;
    if (m.size() > 4) m.resize(4);
    p.players = (int)m.size();
    std::vector<int> eff(m.size(), 1);
    int used = (int)m.size();
    for (size_t i = 0; i < m.size() && used < 4; ++i)
        if (m[i].locals >= 2) { eff[i] = 2; ++used; }
    std::vector<int> ports;
    for (size_t i = 0; i < m.size(); ++i)
        for (int k = 0; k < eff[i]; ++k) ports.push_back(m[i].slot);
    for (int i = 0; i < 4; ++i) {
        if (i) p.ports += ",";
        p.ports += i < (int)ports.size() ? std::to_string(ports[i]) : "-1";
    }
    p.locals = eff;
    return p;
}

int invite_room_left(int members, int game_max) {
    int cap = std::max(1, std::min(4, game_max > 0 ? game_max : 4));
    return std::max(0, cap - members);
}
std::string invite_room_block(int members, int game_max, int wanted) {
    int cap = std::max(1, std::min(4, game_max > 0 ? game_max : 4));
    int left = invite_room_left(members, game_max);
    if (left <= 0) return "The room is full (" + std::to_string(members) + "/" + std::to_string(cap) + "). Free a spot first.";
    if (wanted > left) return "Only " + std::to_string(left) + " more can join this room (" + std::to_string(members) + "/" + std::to_string(cap) + ").";
    return "";
}

/* ---- Smash match picker ---- */
namespace {
const remix::Fighter* find_fighter(int id) {
    for (auto& f : remix::kFighters) if (f.id == id) return &f;
    return nullptr;
}
const remix::Stage* find_stage(int id) {
    for (auto& s : remix::kStages) if (s.id == id) return &s;
    return nullptr;
}
bool fighter_listed(const remix::Fighter& f, bool remix_roster) { return f.on_css && (remix_roster || f.id <= 0x0B); }
bool stage_listed(const remix::Stage& s, bool remix_roster) {
    if (s.cls == remix::StageClass::vanilla_vs) return true;
    return remix_roster && s.cls == remix::StageClass::remix_vs && s.netplay_legal == remix::Flag::yes;
}
unsigned mix32(unsigned& st) {   // splitmix32-ish; deterministic everywhere
    st += 0x9E3779B9u;
    unsigned z = st;
    z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
    z = (z ^ (z >> 13)) * 0xC2B2AE35u;
    return z ^ (z >> 16);
}
std::vector<int> port_owners(const std::string& ports) {   // owner slot per used port ("-1" entries dropped)
    std::vector<int> v;
    std::string tok;
    for (size_t i = 0; i <= ports.size(); ++i) {
        if (i == ports.size() || ports[i] == ',') {
            if (!tok.empty() && std::atoi(tok.c_str()) >= 0) v.push_back(std::atoi(tok.c_str()));
            tok.clear();
        } else tok += ports[i];
    }
    return v;
}
bool pick_ok(const MatchPick& p, bool remix_roster) {
    if (!fighter_valid(p.fighter, remix_roster)) return false;
    if (p.fighter < 0) return p.costume == 0;
    return p.costume >= 0 && p.costume < fighter_costumes(p.fighter);
}
}  // namespace

bool match_game_supported(const std::string& id) { return id == "remix"; }   // add "ssb64rom" (remix=false) once n64run injects vanilla matches

std::vector<FighterChoice> fighter_choices(bool remix_roster) {
    std::vector<FighterChoice> v;
    for (auto& f : remix::kFighters)
        if (fighter_listed(f, remix_roster)) v.push_back({f.id, f.name, f.id > 0x0B, f.max_costume + 1});
    return v;   // table is id-ordered: vanilla 0x00-0x0B first, then Remix
}
std::vector<StageChoice> stage_choices(bool remix_roster) {
    std::vector<StageChoice> v;
    for (auto& s : remix::kStages)
        if (stage_listed(s, remix_roster)) v.push_back({s.id, s.name, s.cls != remix::StageClass::vanilla_vs});
    std::stable_sort(v.begin(), v.end(), [](const StageChoice& a, const StageChoice& b) { return !a.remix && b.remix; });
    return v;
}
std::string fighter_name(int id) {
    if (id < 0) return "Random";
    const remix::Fighter* f = find_fighter(id);
    return f ? f->name : "Fighter " + std::to_string(id);
}
std::string stage_name(int id) {
    if (id < 0) return "Random";
    const remix::Stage* s = find_stage(id);
    return s ? s->name : "Stage " + std::to_string(id);
}
int fighter_costumes(int id) {
    const remix::Fighter* f = find_fighter(id);
    return f ? f->max_costume + 1 : 1;
}
bool fighter_valid(int id, bool remix_roster) {
    if (id == -1) return true;
    const remix::Fighter* f = find_fighter(id);
    return f && fighter_listed(*f, remix_roster);
}
bool stage_valid(int id, bool remix_roster) {
    if (id == -1) return true;
    const remix::Stage* s = find_stage(id);
    return s && stage_listed(*s, remix_roster);
}

std::string pick_json(const MemberPick& p) {
    return "{\"kind\":\"pick\",\"fighter\":" + std::to_string(p.p[0].fighter) + ",\"costume\":" + std::to_string(p.p[0].costume) +
           ",\"fighter2\":" + std::to_string(p.p[1].fighter) + ",\"costume2\":" + std::to_string(p.p[1].costume) + "}";
}
bool parse_pick(const Json& b, MemberPick* out, bool remix_roster) {
    MemberPick m;
    const char* fk[2] = {"fighter", "fighter2"};
    const char* ck[2] = {"costume", "costume2"};
    for (int i = 0; i < 2; ++i) {
        const Json* f = b.get(fk[i]);
        if (!f) { if (i == 0) return false; continue; }   // fighter2 is optional
        const Json* c = b.get(ck[i]);
        if (f->type != Json::Number || f->n != (double)(int)f->n) return false;
        if (c && (c->type != Json::Number || c->n != (double)(int)c->n)) return false;
        m.p[i].fighter = (int)f->n;
        m.p[i].costume = c ? (int)c->n : 0;
        if (!pick_ok(m.p[i], remix_roster)) return false;
    }
    if (out) *out = m;
    return true;
}
std::string rules_json(const MatchRules& r) {
    return "{\"kind\":\"rules\",\"stage\":" + std::to_string(r.stage) + ",\"stocks\":" + std::to_string(r.stocks) + "}";
}
bool parse_rules(const Json& b, MatchRules* out, bool remix_roster) {
    const Json* s = b.get("stage");
    const Json* k = b.get("stocks");
    if (!s || !k || s->type != Json::Number || k->type != Json::Number || s->n != (double)(int)s->n || k->n != (double)(int)k->n) return false;
    if (!stage_valid((int)s->n, remix_roster) || k->n < 1 || k->n > 5) return false;
    if (out) { out->stage = (int)s->n; out->stocks = (int)k->n; }
    return true;
}
std::string pick_label(const MemberPick& p, int locals) {
    std::string o = fighter_name(p.p[0].fighter);
    if (locals >= 2) o += " + " + fighter_name(p.p[1].fighter);
    return o;
}

MatchSpec resolve_match(const std::string& ports, const std::map<int, MemberPick>& picks, const MatchRules& rules, unsigned seed,
                        bool remix_roster) {
    MatchSpec m;
    unsigned st = seed;
    m.stocks = std::max(1, std::min(5, rules.stocks));
    auto stages = stage_choices(remix_roster);
    auto fighters = fighter_choices(remix_roster);
    m.stage = rules.stage;
    if (!stage_valid(m.stage, remix_roster) || m.stage < 0) m.stage = stages.empty() ? 0 : stages[mix32(st) % stages.size()].id;
    std::map<int, int> used;   // pads already consumed per owner
    for (int owner : port_owners(ports)) {
        int k = std::min(used[owner]++, 1);
        MatchPick p;
        auto it = picks.find(owner);
        if (it != picks.end()) p = it->second.p[k];
        if (!pick_ok(p, remix_roster)) p = MatchPick{};
        if (p.fighter < 0) {   // Random: any CSS fighter, random base costume
            const FighterChoice& f = fighters[mix32(st) % fighters.size()];
            p.fighter = f.id;
            p.costume = (int)(mix32(st) % (unsigned)f.costumes);
        }
        m.players.push_back(p);
    }
    return m;
}

std::string match_arg(const MatchSpec& m) {
    std::string o = "stage=" + std::to_string(m.stage) + ";stocks=" + std::to_string(m.stocks);
    for (size_t i = 0; i < m.players.size(); ++i)
        o += ";p" + std::to_string(i) + "=" + std::to_string(m.players[i].fighter) + ":" + std::to_string(m.players[i].costume);
    return o;
}
std::string match_json(const MatchSpec& m) {
    std::string o = "{\"stage\":" + std::to_string(m.stage) + ",\"stocks\":" + std::to_string(m.stocks) + ",\"players\":[";
    for (size_t i = 0; i < m.players.size(); ++i)
        o += (i ? "," : "") + std::string("[") + std::to_string(m.players[i].fighter) + "," + std::to_string(m.players[i].costume) + "]";
    return o + "]}";
}
bool parse_match(const Json& msg, const std::string& ports, bool remix_roster, MatchSpec* out) {
    const Json* mj = msg.get("match");
    if (!mj || mj->type != Json::Object) return false;
    const Json* pl = mj->get("players");
    if (!pl || pl->type != Json::Array) return false;
    MatchSpec m;
    m.stage = (int)mj->num("stage", -2);
    m.stocks = (int)mj->num("stocks", 0);
    if (m.stage < 0 || !stage_valid(m.stage, remix_roster) || m.stocks < 1 || m.stocks > 5) return false;
    if (pl->a.size() != port_owners(ports).size() || pl->a.empty()) return false;
    for (auto& e : pl->a) {
        if (e.type != Json::Array || e.a.size() != 2 || e.a[0].type != Json::Number || e.a[1].type != Json::Number) return false;
        MatchPick p{(int)e.a[0].n, (int)e.a[1].n};
        if (p.fighter < 0 || !pick_ok(p, remix_roster)) return false;   // a launch spec has no Randoms left
        m.players.push_back(p);
    }
    if (out) *out = m;
    return true;
}

SyncAction sync_request(SyncGate& g, bool manual, bool game_running, bool job_running) {
    if (game_running || job_running) {
        g.pending = true;
        g.manual = g.manual || manual;
        return SyncAction::Queue;
    }
    return SyncAction::Start;
}
bool sync_take_pending(SyncGate& g, bool game_running, bool job_running, bool* manual) {
    if (!g.pending || game_running || job_running) return false;
    if (manual) *manual = g.manual;
    g.pending = false;
    g.manual = false;
    return true;
}

std::string ports_arg(std::vector<int> slots) {
    std::sort(slots.begin(), slots.end());
    std::string o;
    for (int i = 0; i < 4; ++i) {
        if (i) o += ",";
        o += (i < (int)slots.size()) ? std::to_string(slots[i]) : "-1";
    }
    return o;
}

std::string player_left_text(const std::string& left_name) {
    return (left_name.empty() ? std::string("A player") : left_name) + " left - game ended. You are back in the room.";
}

std::string exit_text_for(unsigned long code, const std::string& left_name) {
    if (!left_name.empty() && (code == 0 || code == 4 || code == 8)) return player_left_text(left_name);
    return exit_code_text(code);
}

std::string exit_code_text(unsigned long code) {
    switch (code) {
        case 0: return "Game closed normally.";
        case 2: return "The game couldn't start: the room's player/port setup or the ROM was rejected. Leave the room, rejoin and try again.";
        case 3: return "Emulator files are missing (emu/ folder). Reinstall or run scripts/fetch-emu.ps1.";
        case 4: return "Connection lost during the match.";
        case 5: return "The game went out of sync (desync) and was stopped.";
        case 6: return "Couldn't share the host's save - check your connection and try again.";
        case 8: return player_left_text("");
        case 7: return "The PlayStation setups differ: everyone in the room needs the same BIOS file (or all use the built-in OpenBIOS). Check Settings > Playing.";
        default: return "Game exited with code " + std::to_string(code) + ".";
    }
}

std::string smash_save_file(const std::string& battery_dir) {
    std::string d = battery_dir;
    while (!d.empty() && (d.back() == '\\' || d.back() == '/')) d.pop_back();
    return d + "\\ssb64_save.bin";
}

std::string smash_save_conflict_text(const std::string& legacy_path) {
    return "Using your synced Smash save. Your old local save is still at " + legacy_path + ".";
}

std::string find_smash_rom(const std::vector<RomRef>& lib) {
    for (auto& r : lib)
        if (!r.path.empty() && (r.catalog_id == "ssb64rom" || r.serial == "NALE")) return r.path;
    return "";
}

std::vector<std::pair<std::string, std::string>> smash_launch_env(const std::string& rom_path, const std::string& save_file) {
    std::vector<std::pair<std::string, std::string>> env;
    if (!rom_path.empty()) env.push_back({"SSB64_BASEROM", rom_path});
    if (!save_file.empty()) env.push_back({"SSB64_SAVE_PATH", save_file});
    return env;
}

bool smash_md5_supported(const std::string& md5_hex) {
    if (md5_hex.size() != 32) return false;
    for (size_t i = 0; i < 32; ++i)
        if (std::tolower((unsigned char)md5_hex[i]) != kSmashUsaMd5[i]) return false;
    return true;
}

SmashPlay smash_play_decision(bool o2r_exists, bool rom_found, const std::string& rom_md5_hex) {
    if (o2r_exists) return SmashPlay::Launch;          // already extracted: the ROM is not needed again
    if (!rom_found) return SmashPlay::NeedRom;
    return smash_md5_supported(rom_md5_hex) ? SmashPlay::Launch : SmashPlay::WarnMd5;
}

std::string smash_md5_warn_text(const std::string& md5_hex) {
    std::string m = md5_hex.empty() ? std::string("unreadable") : md5_hex;
    return "This ROM is not the supported Super Smash Bros. (USA) 1.0 dump (MD5 " + m + "). Smash Classic may not start. Launching anyway.";
}

std::string normalize_code(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (std::isspace(c)) continue;
        o += (char)std::toupper(c);
        if (o.size() == 6) break;
    }
    return o;
}

/* ---------------- library list helpers ---------------- */

static std::string lower(const std::string& s) {
    std::string o = s;
    for (auto& c : o) c = (char)std::tolower((unsigned char)c);
    return o;
}

bool list_item_less(const ListItem& x, const ListItem& y, SortMode mode) {
    if (x.pinned != y.pinned) return x.pinned;
    unsigned long long a = 0, b = 0;
    if (mode == SortMode::LastPlayed) { a = x.last_played; b = y.last_played; }
    else if (mode == SortMode::MostPlayed) { a = x.play_seconds; b = y.play_seconds; }
    else if (mode == SortMode::RecentlyAdded) { a = x.added; b = y.added; }
    if (a != b) return a > b;
    std::string lx = lower(x.title), ly = lower(y.title);
    if (lx != ly) return lx < ly;
    return x.key < y.key;
}

std::vector<int> filter_sort(const std::vector<ListItem>& items, const std::string& system_filter,
                             const std::string& query, SortMode mode) {
    std::string q = lower(query);
    size_t b = q.find_first_not_of(' ');
    size_t e = q.find_last_not_of(' ');
    q = (b == std::string::npos) ? "" : q.substr(b, e - b + 1);
    std::vector<int> v;
    for (size_t i = 0; i < items.size(); ++i) {
        if (!system_filter.empty() && items[i].system != system_filter) continue;
        if (!q.empty() && lower(items[i].title).find(q) == std::string::npos) continue;
        v.push_back((int)i);
    }
    std::stable_sort(v.begin(), v.end(), [&](int a, int b2) { return list_item_less(items[a], items[b2], mode); });
    return v;
}

std::vector<int> pick_recent(const std::vector<unsigned long long>& last_played, int n) {
    std::vector<int> v;
    for (size_t i = 0; i < last_played.size(); ++i)
        if (last_played[i] > 0) v.push_back((int)i);
    std::stable_sort(v.begin(), v.end(), [&](int a, int b) { return last_played[a] > last_played[b]; });
    if ((int)v.size() > n) v.resize(n);
    return v;
}

/* ---------------- home composition / layout ---------------- */

std::string system_shelf_title(const std::string& s) {
    if (s == "n64") return "Nintendo 64";
    if (s == "gc") return "GameCube";
    if (s == "gba") return "Game Boy Advance";
    if (s == "gbc") return "Game Boy Color";
    if (s == "gb") return "Game Boy";
    if (s == "3ds") return "Nintendo 3DS";
    if (s == "wiiu") return "Wii U";
    if (s == "nds") return "Nintendo DS";
    if (s == "ps1") return "PlayStation";
    if (s == "ps2") return "PlayStation 2";
    return s;
}

const std::vector<std::string>& console_order() {
    static const std::vector<std::string> v = {"n64", "gc", "ps1", "wiiu", "3ds", "nds", "ps2", "gba", "gbc", "gb"};
    return v;
}

std::vector<ConsoleSection> group_by_console(const std::vector<ListItem>& items, const std::vector<int>* indices, SortMode mode) {
    std::vector<int> idx;
    if (indices) idx = *indices;
    else for (size_t i = 0; i < items.size(); ++i) idx.push_back((int)i);
    std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return list_item_less(items[a], items[b], mode); });
    std::vector<ConsoleSection> out;
    for (const std::string& c : console_order()) {
        ConsoleSection s{c, system_shelf_title(c), {}};
        for (int i : idx) if (items[i].system == c) s.items.push_back(i);
        if (!s.items.empty()) out.push_back(s);
    }
    ConsoleSection rest{"other", "Other", {}};
    for (int i : idx) {
        const std::string& sy = items[i].system;
        if (std::find(console_order().begin(), console_order().end(), sy) == console_order().end()) rest.items.push_back(i);
    }
    if (!rest.items.empty()) out.push_back(rest);
    return out;
}

std::vector<Shelf> compose_shelves(const std::vector<ShelfGame>& g, int max_recent, SortMode mode) {
    auto by_title = [&](std::vector<int>& v) {
        std::stable_sort(v.begin(), v.end(), [&](int a, int b) {
            if (g[a].pinned != g[b].pinned) return g[a].pinned;
            std::string x = lower(g[a].title), y = lower(g[b].title);
            if (x != y) return x < y;
            return g[a].key < g[b].key;
        });
    };
    std::vector<Shelf> out;
    std::vector<unsigned long long> lp;
    for (auto& e : g) lp.push_back(e.last_played);
    Shelf recent{"recent", "Continue Playing", pick_recent(lp, max_recent)};
    if (!recent.items.empty()) out.push_back(recent);
    Shelf favs{"favs", "Favorites", {}};
    for (size_t i = 0; i < g.size(); ++i)
        if (g[i].favorite) favs.items.push_back((int)i);
    by_title(favs.items);
    if (!favs.items.empty()) out.push_back(favs);
    std::vector<ListItem> li;
    for (auto& e : g) li.push_back({e.key, e.title, e.system, e.pinned, e.last_played, e.play_seconds, e.added});
    for (auto& cs : group_by_console(li, nullptr, mode)) {
        Shelf sh{"console:" + cs.system, cs.title, cs.items};
        out.push_back(sh);
    }
    return out;
}

int rect_hit(const std::vector<FRect>& rects, float x, float y) {
    for (size_t i = 0; i < rects.size(); ++i)
        if (x >= rects[i].x0 && x < rects[i].x1 && y >= rects[i].y0 && y < rects[i].y1) return (int)i;
    return -1;
}
float shelf_wheel_delta(float wx, float wy, float tile_w, float gap) { return (wx - wy) * 1.5f * (tile_w + gap); }
bool drag_exceeds(float dx, float dy, float t) { return dx * dx + dy * dy > t * t; }
float shelf_page_step(float view_w, float tile_w, float gap, float bleed) {
    int n = std::max(1, (int)((view_w - bleed * 2 + gap) / (tile_w + gap)));
    return n * (tile_w + gap);
}
float shelf_clamp(float v, float m) { return std::max(0.f, std::min(std::max(0.f, m), v)); }
float shelf_reveal(float left, float right, float target, float view_w, float margin, float max_scroll) {
    if (left - target < margin) target = left - margin;
    else if (right - target > view_w - margin) target = right - view_w + margin;
    return shelf_clamp(target, max_scroll);
}
float shelf_max_scroll(int count, float tw, float gap, float bleed, float view_w) {
    float content = bleed * 2 + count * tw + std::max(0, count - 1) * gap;
    return std::max(0.f, content - view_w);
}

std::vector<int> pick_unowned(const std::vector<bool>& owned, const std::vector<bool>& online, int max_n) {
    std::vector<int> v;
    for (int pass = 0; pass < 2; ++pass)
        for (size_t i = 0; i < owned.size(); ++i) {
            bool on = i < online.size() && online[i];
            if (owned[i] || on != (pass == 0)) continue;
            if ((int)v.size() >= max_n) return v;
            v.push_back((int)i);
        }
    return v;
}

int pick_hero(const std::vector<ShelfGame>& g, const std::string& preferred_key) {
    if (g.empty()) return -1;
    for (size_t i = 0; i < g.size(); ++i)
        if (!preferred_key.empty() && g[i].key == preferred_key) return (int)i;
    int best = -1;
    for (size_t i = 0; i < g.size(); ++i)
        if (g[i].last_played > 0 && (best < 0 || g[i].last_played > g[best].last_played)) best = (int)i;
    if (best >= 0) return best;
    for (size_t i = 0; i < g.size(); ++i)
        if (g[i].favorite) return (int)i;
    return 0;
}

GridLayout grid_layout(float avail, float min_w, float max_w, float gap) {
    GridLayout r;
    r.cols = std::max(1, (int)((avail + gap) / (min_w + gap)));
    r.tile_w = std::min(max_w, (avail - gap * (r.cols - 1)) / r.cols);
    if (r.tile_w < 1.f) r.tile_w = 1.f;
    return r;
}

float shelf_tile_w(float avail, float min_w, float max_w, float gap) {
    int n = std::max(2, (int)((avail + gap) / (max_w + gap)));
    float w = (avail - gap * n) / (n + 0.4f);
    return std::max(min_w, std::min(max_w, w));
}

float approach(float v, float target, float dt, float rate) {
    if (dt <= 0) return v;
    return v + (target - v) * (1.f - std::exp(-rate * dt));
}

ArtFit fit_art(float aw, float ah, float bw, float bh, float lo, float hi) {
    ArtFit f;
    if (aw <= 0 || ah <= 0 || bw <= 0 || bh <= 0) { f.w = bw; f.h = bh; return f; }
    float art_a = aw / ah, box_a = bw / bh, r = art_a / box_a;
    if (r >= lo && r <= hi) {   /* crop to fill */
        f.cover = true;
        f.w = bw; f.h = bh;
        if (art_a > box_a) {   // art wider: crop left/right
            float keep = box_a / art_a;
            f.u0 = (1 - keep) * 0.5f; f.u1 = f.u0 + keep;
        } else {               // art taller: crop top/bottom
            float keep = art_a / box_a;
            f.v0 = (1 - keep) * 0.5f; f.v1 = f.v0 + keep;
        }
        return f;
    }
    f.cover = false;
    if (art_a > box_a) { f.w = bw; f.h = bw / art_a; }
    else { f.h = bh; f.w = bh * art_a; }
    f.x = (bw - f.w) * 0.5f;
    f.y = (bh - f.h) * 0.5f;
    return f;
}

bool sidebar_collapsed(int mode, float window_w, float narrow_below) {
    if (mode == 1) return false;
    if (mode == 2) return true;
    return window_w < narrow_below;
}

std::string format_play_time(unsigned long long s) {
    if (s == 0) return "Never played";
    if (s < 60) return "Under a minute";
    if (s < 3600) return std::to_string(s / 60) + " min";
    char b[32];
    std::snprintf(b, sizeof b, "%lluh %02llum", s / 3600, (s % 3600) / 60);
    return b;
}

std::string friend_hours_text(int m) {
    if (m < 1) return "";
    if (m < 60) return std::to_string(m) + "m";
    if (m % 60 == 0 || m >= 10 * 60) return std::to_string(m / 60) + "h";
    return std::to_string(m / 60) + "h " + std::to_string(m % 60) + "m";
}

std::string friends_playtime_line(const std::map<std::string, std::vector<PlaytimeEntry>>& by_uid, const std::map<std::string, std::string>& names,
                                  const std::string& game_key, size_t max_n) {
    std::vector<std::pair<int, std::string>> v;   // (minutes, name)
    for (const auto& kv : by_uid) {
        auto n = names.find(kv.first);
        if (n == names.end() || n->second.empty()) continue;
        for (const auto& g : kv.second) if (g.game_key == game_key && g.minutes >= 1) { v.push_back({g.minutes, n->second}); break; }
    }
    std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
    std::string o;
    for (size_t i = 0; i < v.size() && i < max_n; ++i) { if (i) o += " \xC2\xB7 "; o += v[i].second + " " + friend_hours_text(v[i].first); }
    return o;
}

std::string friend_top_games_text(const std::vector<PlaytimeEntry>& games, size_t max_n) {
    std::vector<PlaytimeEntry> v;
    for (const auto& g : games) if (g.minutes >= 1) v.push_back(g);
    std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.minutes > b.minutes; });
    std::string o;
    for (size_t i = 0; i < v.size() && i < max_n; ++i) { o += (i ? ", " : "Top games: ") + (v[i].title.empty() ? v[i].game_key : v[i].title) + " " + friend_hours_text(v[i].minutes); }
    return o;
}

std::string sanitize_player_name(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7f || c == '"' || c == '\\') continue;
        if (o.size() >= 16 && (c & 0xC0) != 0x80) break;   // never cut inside a UTF-8 sequence
        o += (char)c;
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    while (!o.empty() && o[0] == ' ') o.erase(0, 1);
    return o.empty() ? "Player" : o;
}

/* ---------------- INI ---------------- */

static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r");
    size_t e = s.find_last_not_of(" \t\r");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}
static bool section_line(const std::string& line, std::string* name) {
    std::string t = trim(line);
    if (t.size() >= 2 && t.front() == '[' && t.back() == ']') { *name = lower(t.substr(1, t.size() - 2)); return true; }
    return false;
}
static bool key_line(const std::string& line, const std::string& lkey, std::string* val) {
    size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    if (lower(trim(line.substr(0, eq))) != lkey) return false;
    if (val) *val = trim(line.substr(eq + 1));
    return true;
}
static std::vector<std::string> split_lines(const std::string& t) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : t) {
        if (c == '\n') { v.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    if (!cur.empty()) v.push_back(cur);
    return v;
}

std::string ini_get(const std::string& text, const std::string& section, const std::string& key, const std::string& def) {
    std::string cur, val, ls = lower(section), lk = lower(key);
    for (auto& line : split_lines(text)) {
        std::string n;
        if (section_line(line, &n)) { cur = n; continue; }
        if (cur == ls && key_line(line, lk, &val)) return val;
    }
    return def;
}

std::string ini_set(const std::string& text, const std::string& section, const std::string& key, const std::string& value) {
    std::vector<std::string> lines = split_lines(text);
    std::string ls = lower(section), lk = lower(key), cur;
    int sec_end = -1;   // index just after the last line of the target section
    bool in_sec = false, done = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string n;
        if (section_line(lines[i], &n)) {
            if (in_sec) { sec_end = (int)i; in_sec = false; }
            cur = n;
            if (cur == ls) in_sec = true;
            continue;
        }
        if (in_sec && key_line(lines[i], lk, nullptr)) { lines[i] = key + " = " + value; done = true; break; }
    }
    if (!done) {
        if (in_sec) sec_end = (int)lines.size();
        if (sec_end >= 0) {
            int at = sec_end;
            while (at > 0 && trim(lines[at - 1]).empty()) --at;   // insert before trailing blank lines
            lines.insert(lines.begin() + at, key + " = " + value);
        } else {
            if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back("");
            lines.push_back("[" + section + "]");
            lines.push_back(key + " = " + value);
        }
    }
    std::string o;
    for (auto& l : lines) o += l + "\n";
    return o;
}

/* ---------------- vault meta ---------------- */

bool parse_vault_meta(const std::string& text, VaultMeta* out) {
    Json j;
    if (!json::parse(text, &j) || j.type != Json::Object) return false;
    out->key = j.str("key");
    out->title = j.str("title");
    out->system = j.str("system");
    out->device = j.str("device");
    out->updated = (unsigned long long)j.num("updated", 0);
    return !out->key.empty();
}

/* ---------------- friends ---------------- */

static std::string lower_str(std::string s) {
    for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

static int online_rank(const FriendRow& r) {
    if (r.state == FS_InLobby && r.joinable) return 0;
    if (r.state == FS_InGame && r.joinable) return 1;
    if (r.state == FS_InGame) return 2;
    if (r.state == FS_InLobby) return 3;
    return 4;
}

FriendGroups group_friends(const std::vector<FriendRow>& rows) {
    FriendGroups g;
    std::set<std::string> seen;   // each person (uid) is placed exactly once
    for (int i = 0; i < (int)rows.size(); ++i) {
        if (!rows[i].uid.empty() && !seen.insert(rows[i].uid).second) continue;
        (rows[i].state != FS_Offline ? g.online : g.offline).push_back(i);
    }
    std::stable_sort(g.online.begin(), g.online.end(), [&](int a, int b) {
        int ra = online_rank(rows[a]), rb = online_rank(rows[b]);
        if (ra != rb) return ra < rb;
        std::string x = lower_str(rows[a].name), y = lower_str(rows[b].name);
        return x != y ? x < y : rows[a].uid < rows[b].uid;
    });
    std::stable_sort(g.offline.begin(), g.offline.end(), [&](int a, int b) {
        if (rows[a].updated_ms != rows[b].updated_ms) return rows[a].updated_ms > rows[b].updated_ms;
        std::string x = lower_str(rows[a].name), y = lower_str(rows[b].name);
        return x != y ? x < y : rows[a].uid < rows[b].uid;
    });
    return g;
}

std::string format_ago_ms(long long now_ms, long long then_ms) {
    long long s = (now_ms - then_ms) / 1000;
    if (s < 60) return "just now";
    if (s < 3600) return std::to_string(s / 60) + " min ago";
    if (s < 86400) return std::to_string(s / 3600) + "h ago";
    return std::to_string(s / 86400) + "d ago";
}

std::string friend_status_line(const FriendRow& r, long long now_ms) {
    if (r.in_my_room && r.state != FS_Offline) {
        std::string s = "In your room";
        if (r.state == FS_InGame) s += " Â· Playing";
        return s;
    }
    switch (r.state) {
        case FS_InGame: {
            std::string s = r.game_title.empty() ? "Playing" : "Playing " + r.game_title;
            if (r.joinable && r.max_players > 0) s += " \xC2\xB7 " + std::to_string(r.players) + "/" + std::to_string(r.max_players);
            return s;
        }
        case FS_InLobby: {
            std::string s = "In lobby";
            if (!r.game_title.empty()) s += " \xC2\xB7 " + r.game_title;
            if (r.max_players > 0) s += " \xC2\xB7 " + std::to_string(r.players) + "/" + std::to_string(r.max_players);
            return s;
        }
        case FS_Online: return "Online";
        default: break;
    }
    if (r.updated_ms > 0 && now_ms > r.updated_ms) return "Last seen " + format_ago_ms(now_ms, r.updated_ms);
    return "Offline";
}

std::string format_dur_short(long long ms) {
    long long m = ms / 60000;
    if (ms <= 0 || m < 1) return "";
    if (m < 60) return std::to_string(m) + " min";
    return std::to_string(m / 60) + "h" + (m % 60 ? " " + std::to_string(m % 60) + "m" : std::string());
}

std::string friend_presence_line(const FriendRow& r, long long now_ms) {
    if (r.state != FS_InLobby && r.state != FS_InGame) return friend_status_line(r, now_ms);
    if (r.in_my_room) return friend_status_line(r, now_ms);
    std::string s = r.game_title.empty() ? (r.state == FS_InGame ? "Playing" : "In lobby") : r.game_title;
    if (r.max_players > 0 && r.players > 0) s += " \xC2\xB7 " + std::to_string(r.players) + "/" + std::to_string(r.max_players);
    std::string d = r.updated_ms > 0 ? format_dur_short(now_ms - r.updated_ms) : "";
    if (!d.empty()) s += " \xC2\xB7 " + d;
    return s;
}

std::vector<int> friends_playing(const std::vector<FriendRow>& rows, size_t max_n) {
    std::vector<int> idx;
    std::set<std::string> seen;
    for (int i = 0; i < (int)rows.size(); ++i) {
        const FriendRow& r = rows[i];
        if (r.in_my_room || (r.state != FS_InLobby && r.state != FS_InGame) || r.game_title.empty()) continue;
        if (!r.uid.empty() && !seen.insert(r.uid).second) continue;
        idx.push_back(i);
    }
    std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
        bool ja = friend_can_join(rows[a]), jb = friend_can_join(rows[b]);
        if (ja != jb) return ja;
        if (rows[a].updated_ms != rows[b].updated_ms) return rows[a].updated_ms > rows[b].updated_ms;
        return lower_str(rows[a].name) < lower_str(rows[b].name);
    });
    if (idx.size() > max_n) idx.resize(max_n);
    return idx;
}

std::string ask_toast_title(const std::string& name, const std::string& game) {
    return name + " wants to join your game" + (game.empty() ? std::string() : " (" + game + ")");
}
std::string ask_sent_text(const std::string& name) { return "Asked " + name + " to join. They will get a request to invite you."; }
std::string ask_limited_text(const std::string& name, long long wait_ms) {
    if (wait_ms > 0) return "You already asked " + name + " - try again in " + std::to_string((wait_ms + 999) / 1000) + "s.";
    return "You have asked a lot of people just now - wait a few minutes.";
}

PingClass ping_class(int rtt_ms, bool relay) {
    if (relay) return PingClass::Relay;
    if (rtt_ms < 0) return PingClass::Unknown;
    if (rtt_ms < 80) return PingClass::Great;
    if (rtt_ms < 160) return PingClass::Ok;
    return PingClass::Poor;
}
const char* ping_class_label(PingClass c) {
    switch (c) {
        case PingClass::Great: return "Great";
        case PingClass::Ok: return "OK";
        case PingClass::Poor: return "Poor";
        case PingClass::Relay: return "Relay";
        default: return "...";
    }
}
std::string ping_text(int rtt_ms, bool relay) {
    if (relay) return "relay";
    return rtt_ms < 0 ? "-" : std::to_string(rtt_ms) + " ms";
}
int PingBook::rtt(int slot) const {
    auto it = s_.find(slot);
    if (it == s_.end() || it->second.empty()) return -1;
    std::vector<int> v = it->second;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

std::string queued_join_text(const std::string& friend_name) {
    return "Will join " + (friend_name.empty() ? std::string("that") : friend_name + "'s") + " room when your game closes.";
}

std::string queued_join_replaced_text(const std::string& old_who, const std::string& new_who) {
    return "Replaced the queued join (" + (old_who.empty() ? std::string("another room") : old_who + "'s room") + "). " + queued_join_text(new_who);
}

std::string friends_error_text(const std::string& r, const std::string& d) {
    if (r == "bad-request") {
        if (d == "friends-full") return "Your friends list is full.";
        if (d == "target-full") return "That player's friends list is full.";
        if (d == "too-many-requests") return "You have too many pending requests - cancel some first.";
        if (d == "already-requested") return "You already sent that player a request.";
        if (d == "undeliverable") return "That message could not be delivered.";
        if (d == "no-such-request") return "That request is no longer there.";
        if (d == "too-many-blocked") return "Your block list is full.";
        if (d == "retired-elsewhere") return "That account was already moved to another identity.";
        return "";   // internal / retire-failed / bad-secret / text / too-large / none: retried or not user-actionable
    }
    if (r == "unknown-code") return d == "unknown-username" ? "No player has that username." : "No player has that friend code.";
    if (r == "unknown-username") return "No player has that username.";
    if (r == "self") return "That is your own friend code.";
    if (r == "already-friends") return "You are already friends.";
    if (r == "not-friends") return "You can only do that with friends.";
    if (r == "rate-limited") return "Slow down a little - too many requests.";
    if (r == "blocked") return "That player is blocked.";
    return "Friends: " + r;
}

std::string invite_result_text(const std::vector<std::string>& ok, const std::vector<std::string>& bad,
                               const std::string& reason, const std::string& code) {
    auto join = [](const std::vector<std::string>& v) { std::string s; for (auto& n : v) { if (!s.empty()) s += ", "; s += n; } return s; };
    std::string t;
    if (!ok.empty()) t = "Invited " + join(ok) + " to room " + code + ".";
    if (!bad.empty()) {
        if (!t.empty()) t += " ";
        t += std::string(reason == "rate-limited" ? "Slow down - could not invite " : "Could not invite ") + join(bad) + ".";
    }
    return t;
}

std::string normalize_friend_code(const std::string& in) {
    std::string t;
    for (unsigned char ch : in)
        if (ch != ' ' && ch != '-' && ch != '\t' && ch != '_') t += (char)std::toupper(ch);
    if (t.size() == 10 && t.compare(0, 2, "RH") == 0) t = t.substr(2);
    if (t.size() != 8) return "";
    for (char ch : t)
        if (!std::isalnum((unsigned char)ch)) return "";
    return "RH-" + t.substr(0, 4) + "-" + t.substr(4);
}

int invite_seconds_left(long long expires_at_ms, long long now_ms) {
    if (expires_at_ms <= 0 || now_ms >= expires_at_ms) return 0;
    return (int)((expires_at_ms - now_ms + 999) / 1000);
}

} // namespace hubui
