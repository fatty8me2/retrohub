#include "app.h"

#include <xinput.h>

#include "cheat_store.h"
#include "cheats.h"
#include "cheats_db.h"
#include "dolphin_cheats.h"

namespace { int g_rev = 1; }   // bumped on every change of the store: invalidates the row caches

/* ---------- Cheats: panel (details > Cheats, Y on a pad), in-game overlay, online room strip ----------
 * Choices live in the save vault (cheat_store.h, _settings\cheats.json) and follow the player to every PC. The runner gets
 * `--cheats <file>` and live-reloads it, so a toggle while a game runs just rewrites that file. Online, the host's list
 * is announced to the room ("cheats" message), travels in the launch message and is applied identically to everyone;
 * guests cannot toggle. Headless screenshot runs never touch the real vault (a temp folder is used). */

namespace {

cheatstore::Store g_store;
bool g_loaded = false;
hub::GameEntry g_ent;            // the game the panel is open for
bool g_have = false, g_open_req = false;
char g_search[64] = {0};
bool g_add = false;
char g_add_name[80] = {0}, g_add_codes[512] = {0};
std::string g_add_err;
bool g_confirm_restore = false;
std::vector<std::string> g_room_names;   // guest: what the host announced
std::string g_room_game;
std::string g_sent_sig;
/* Value editor (cheats with X/? placeholders): one cheat at a time, replaces the list while open. */
bool g_val_open = false;
cheatstore::Row g_val_row;
char g_val_buf[24] = {0};
bool g_val_dec = true;
cheats::Target target_of(const hub::GameEntry& e);

std::string vault() {
    if (g_headless) {   // screenshots/tests: never touch the player's vault
        std::string d = env_var(L"TEMP");
        if (d.empty()) d = ".";
        return with_slash(d) + "hub_cheats_shot";
    }
    return hub::saves::vault_dir();
}

void ensure_loaded() {
    if (g_loaded) return;
    g_loaded = true;
    g_store.load(vault());
}
void reload() { g_loaded = false; ++g_rev; ensure_loaded(); }
void persist() { ++g_rev; g_store.save(vault()); A.vault_dirty = true; }

std::string safe_name(std::string s);
/* The game's vault folder and battery folder (headless runs use a temp folder instead of the real vault). */
std::string gdir_of(const hub::GameEntry& e) {
    if (!g_headless) return hub::saves::game_dir(sys_short(e), e.key);
    std::string d = with_slash(vault()) + "games\\" + safe_name(sys_short(e) + "_" + e.key) + "\\";
    make_dirs(d);
    return d;
}
std::string battery_of(const hub::GameEntry& e) {
    if (!g_headless) return hub::saves::battery_dir(sys_short(e), e.key);
    std::string d = gdir_of(e) + "battery\\";
    make_dirs(d);
    return d;
}

std::string safe_name(std::string s) {
    for (auto& c : s) if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) c = '_';
    return s;
}
std::string run_file(const std::string& key) {
    std::string d = hub::data_dir();
    if (g_headless) d = with_slash(vault());
    make_dirs(d);
    return with_slash(d) + "cheats_run_" + safe_name(key) + ".json";
}
std::string stamp_now() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[40];
    std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm);
    return b;
}
int64_t now_s() { return (int64_t)std::time(nullptr); }

/* Stable id from the cheat's own text (the database's positional ids would shift when the bundle is updated). */
std::string stable_id(const cheats::Cheat& c, std::map<std::string, int>* seen) {
    uint64_t h = 1469598103934665603ull;
    auto mix1 = [&](const std::string& t) { for (unsigned char ch : t) { h ^= ch; h *= 1099511628211ull; } h ^= 0x1F; h *= 1099511628211ull; };
    mix1(c.desc);
    for (auto& l : c.codes) mix1(l);
    char b[24];
    std::snprintf(b, sizeof b, "c%012llx", (unsigned long long)(h & 0xFFFFFFFFFFFFull));
    int n = (*seen)[b]++;
    return n ? std::string(b) + "-" + std::to_string(n) : std::string(b);
}

/* The database list for one game, computed once (lists can have 12 000 entries). Masters + Popular lead and count as featured. */
/* GameCube: the disc's version byte (header 0x07) picks Dolphin's per-revision code list. Plain / WIA / RVZ / CISO discs; -1 otherwise. */
int gc_disc_revision(const hub::GameEntry& e) {
    std::string p = launch_path_for(e);
    if (p.empty()) return -1;
    std::string ext = p.size() > 4 ? p.substr(p.size() - 4) : p;
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    long off = ext == ".iso" || ext == ".gcm" ? 7 : ext == ".rvz" || ext == ".wia" ? 0x58 + 7 : ext == "ciso" ? 0x8000 + 7 : -1;
    if (off < 0) return -1;
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "rb") != 0 || !f) return -1;
    unsigned char b = 0;
    int r = -1;
    if (std::fseek(f, off, SEEK_SET) == 0 && std::fread(&b, 1, 1, f) == 1) r = b;
    std::fclose(f);
    return r;
}

const std::vector<cheatstore::DbItem>& db_items(const hub::GameEntry& e0) {
    static std::map<std::string, std::vector<cheatstore::DbItem>> cache;
    auto it = cache.find(e0.key);
    if (it != cache.end()) return it->second;
    hub::GameEntry e = e0;
    if (e.system == hub::System::GC) e.gc_rev = gc_disc_revision(e);
    std::vector<cheatstore::DbItem> v;
    size_t prefix = 0;
    std::vector<cheats::Cheat> all = hub::cheats_for(e, &prefix);
    std::map<std::string, int> seen;
    for (size_t i = 0; i < all.size(); ++i) {
        cheatstore::DbItem d;
        d.cheat = all[i];
        d.cheat.id = stable_id(all[i], &seen);
        d.featured = i < prefix;
        v.push_back(std::move(d));
    }
    return cache.emplace(e0.key, std::move(v)).first->second;
}
const std::vector<cheatstore::Row>& rows_cached(const hub::GameEntry& e) {
    static std::string key;
    static int rev = -1;
    static std::vector<cheatstore::Row> rows;
    ensure_loaded();
    if (key != e.key || rev != g_rev) { rows = cheatstore::build_rows(g_store, e.key, db_items(e), target_of(e)); key = e.key; rev = g_rev; }
    return rows;
}
std::vector<cheatstore::Row> rows_for(const hub::GameEntry& e) { return rows_cached(e); }
cheats::Target target_of(const hub::GameEntry& e) {
    cheats::Target t = cheats::Target::N64;
    cheats::target_for_system(sys_short(e), &t);
    return t;
}

/* "Activate" presses per game+cheat this hub session (the runner fires GameShark-button codes on each rise). */
std::map<std::string, int> g_fire;

void write_run(const hub::GameEntry& e) {
    std::string err;
    cheats::CheatSet cs = cheatstore::runner_set(rows_cached(e));
    for (auto& c : cs.cheats) { auto f = g_fire.find(e.key + "|" + c.id); if (f != g_fire.end()) c.fire = f->second; }
    cheats::save_file(run_file(e.key), cs, &err);
}

/* "42 (0x2A)" */
std::string value_text(const cheatstore::Row& r) {
    return std::to_string(r.value) + " (0x" + cheats::value_format(r.value, false, r.spec.digits) + ")";
}

void open_value_editor(const cheatstore::Row& r) {
    g_val_row = r;
    g_val_dec = !r.spec.prefer_hex;
    std::snprintf(g_val_buf, sizeof g_val_buf, "%s", cheats::value_format(r.value, g_val_dec, r.spec.digits).c_str());
    g_val_open = true;
}

bool value_buf_number(uint32_t* v) { return cheats::value_parse(g_val_buf, g_val_dec, v); }

void value_buf_set(uint32_t v) { std::snprintf(g_val_buf, sizeof g_val_buf, "%s", cheats::value_format(v, g_val_dec, g_val_row.spec.digits).c_str()); }

bool in_room_for(const hub::GameEntry& e) { return A.in_room && A.selected_game == room_id(e); }
bool guest_locked(const hub::GameEntry& e) { return in_room_for(e) && !is_host(); }
bool running_online(const hub::GameEntry& e) { return A.proc && A.run.online && A.run.key == e.key; }

/* First time cheats go on in a game: copy its save aside, once. */
void first_enable_backup(const hub::GameEntry& e) {
    const cheatstore::GameState* gs = g_store.game(e.key);
    std::string gd = gdir_of(e);
    if (gs && cheatstore::has_backup(gd, gs->backup)) return;
    std::string bat = battery_of(e);
    int files = 0;
    uint64_t bytes = 0;
    count_files(bat, &files, &bytes);
    std::string name = stamp_now(), err;
    if (!cheatstore::backup_save(gd, bat, name, &err)) {
        push_toast("Could not back up your save (" + err + "). Cheats are on anyway.", COL_WARN, 8.0);
        return;
    }
    g_store.set_backup(e.key, name, now_s());
    push_toast(files > 0 ? "Your save was backed up" : "No save yet, so there was nothing to back up", COL_GOOD, 5.0);
}

void toggle(const hub::GameEntry& e, const cheatstore::Row& r) {
    bool now = !r.cheat.enabled;
    if (now) first_enable_backup(e);
    g_store.set_on(e.key, r.cheat.id, now, now_s());
    persist();
    write_run(e);
    push_toast((now ? "Cheat on: " : "Cheat off: ") + r.cheat.desc, now ? COL_GOOD : COL_PANEL, 3.5);
    if (now && r.spec.valid && !r.value_set) open_value_editor(r);   // a value cheat needs its number
}

/* Store the typed number (clamped to the cheat's range), switch the cheat on, hand it to the runner. */
void value_commit(const hub::GameEntry& e) {
    uint32_t v = 0;
    if (!value_buf_number(&v)) v = g_val_row.value;
    v = cheats::value_clamp(g_val_row.spec, v);
    if (!g_store.is_on(e.key, g_val_row.cheat.id)) { first_enable_backup(e); g_store.set_on(e.key, g_val_row.cheat.id, true, now_s()); }
    g_store.set_value(e.key, g_val_row.cheat.id, v, now_s());
    persist();
    write_run(e);
    cheatstore::Row r = g_val_row;
    r.value = v;
    push_toast("Cheat on: " + g_val_row.cheat.desc + "  (" + value_text(r) + ")", COL_GOOD, 3.5);
    g_val_open = false;
}

const char* fmt_label(cheats::Format f) {
    switch (f) {
        case cheats::Format::GameShark: return "GameShark";
        case cheats::Format::ActionReplay: return "Action Replay";
        case cheats::Format::CodeBreaker: return "CodeBreaker";
        case cheats::Format::GameGenie: return "Game Genie";
        case cheats::Format::Gecko: return "Gecko";
        case cheats::Format::ActionReplayGC: return "Action Replay";
        default: return "Raw";
    }
}

/* One big row with a switch on the right. Returns true when toggled. `removed` (custom rows) gets a Remove button. */
bool cheat_row(const cheatstore::Row& r, bool enabled, bool* removed, bool* activate = nullptr, bool* edit = nullptr) {
    float h = sp(7.f), w = ImGui::GetContentRegionAvail().x;
    float rm_w = r.custom ? sp(11.f) : 0.f;
    float act_w = (activate ? sp(13.f) : 0.f) + (edit ? sp(14.f) + (activate ? sp(1.f) : 0.f) : 0.f);
    ImGui::PushID(r.cheat.id.c_str());
    ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + w, p0.y + h);
    ImGui::InvisibleButton("##row", ImVec2(w - (r.custom ? rm_w + sp(1.f) : 0.f), h), ImGuiButtonFlags_EnableNav);
    bool pressed = ImGui::IsItemClicked(ImGuiMouseButton_Left) || (ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false)));
    bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && (A.pad_mode || ImGui::GetIO().NavVisible);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float rad = sp(1.75f);
    dl->AddRectFilled(p0, p1, hov || foc ? ui::PANEL3 : ui::PANEL2, rad);
    if (r.cheat.enabled) dl->AddRect(p0, p1, ui::with_alpha(ui::GOOD, 0.55f), rad, 0, 1.5f);
    if (foc) ui::focus_ring(dl, p0, p1, rad);
    // switch
    float sw = sp(8.f), sh = sp(4.4f);
    ImVec2 s0(p1.x - sw - sp(2.f), p0.y + (h - sh) * 0.5f), s1(s0.x + sw, s0.y + sh);
    bool on = r.cheat.enabled;
    dl->AddRectFilled(s0, s1, on ? ui::GOOD : ui::PANEL3, sh * 0.5f);
    float kr = sh * 0.5f - sp(0.4f);
    dl->AddCircleFilled(ImVec2(on ? s1.x - sh * 0.5f : s0.x + sh * 0.5f, s0.y + sh * 0.5f), kr, IM_COL32_WHITE);
    if (!enabled) dl->AddRectFilled(p0, p1, IM_COL32(16, 17, 22, 110), rad);
    // text
    float tx = p0.x + sp(2.f), tw = s0.x - tx - sp(1.5f) - (act_w > 0 ? act_w + sp(1.f) : 0.f);
    ImFont* bf = ui::F.bold;
    std::string t = ui::ellipsize(bf, bf->FontSize, r.cheat.desc, tw);
    dl->AddText(bf, bf->FontSize, ImVec2(tx, p0.y + sp(1.1f)), ui::TEXT, t.c_str());
    char sub[96];
    std::snprintf(sub, sizeof sub, "%s  -  %d line%s", fmt_label(r.cheat.format), (int)r.cheat.codes.size(), r.cheat.codes.size() == 1 ? "" : "s");
    if (r.spec.valid) {
        std::string vt = std::string(sub) + "  -  Value: " + (r.value_set ? value_text(r) : "not set yet");
        std::snprintf(sub, sizeof sub, "%s", vt.c_str());
    }
    dl->AddText(ui::F.cap, ui::F.cap->FontSize, ImVec2(tx, p0.y + sp(1.1f) + bf->FontSize + sp(0.4f)), ui::TEXT2, r.custom ? (std::string("Your code  -  ") + sub).c_str() : sub);
    {
        float bx = s0.x - act_w - sp(1.f);
        if (edit) {   // value cheats: change the number
            ImGui::SetCursorScreenPos(ImVec2(bx, p0.y + (h - sp(4.4f)) * 0.5f));
            if (button("Set value", ImVec2(sp(14.f), sp(4.4f)), false, enabled)) *edit = true;
            bx += sp(15.f);
        }
        if (activate && r.cheat.enabled) {   // GameShark-button codes: fire again on demand
            ImGui::SetCursorScreenPos(ImVec2(bx, p0.y + (h - sp(4.4f)) * 0.5f));
            if (button("Activate", ImVec2(sp(13.f), sp(4.4f)), false, enabled)) *activate = true;
        }
    }
    if (r.custom) {
        ImGui::SameLine(0, sp(1.f));
        if (button("Remove", ImVec2(rm_w, h), false, enabled)) *removed = true;
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + sp(0.75f)));
    return pressed && enabled;
}

void group_title(const char* t, int n) {
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    ImGui::PushFont(ui::F.h2);
    ImGui::TextUnformatted(t);
    ImGui::PopFont();
    if (n >= 0) { ImGui::SameLine(0, sp(1.f)); ImGui::TextDisabled("%d", n); }
}

}  // namespace

/* ======================= public API ======================= */

bool cheats_supported(const hub::GameEntry& e) {
    if (is_native(e) || is_kanto(e)) return false;
    return e.system == hub::System::N64 || e.system == hub::System::GBA || e.system == hub::System::GB || e.system == hub::System::GBC ||
           e.system == hub::System::NDS || e.system == hub::System::PS1 || e.system == hub::System::NES || e.system == hub::System::SNES ||   // DS / PS1: libretro retro_cheat_set through retrorun
           e.system == hub::System::GC;                                      // GameCube: Dolphin GameSettings ini, written at launch
}

int cheats_enabled_count(const hub::GameEntry& e) {
    if (!cheats_supported(e)) return 0;
    ensure_loaded();
    return g_store.on_count(e.key);
}

void cheats_open(const hub::GameEntry& e) {
    if (!cheats_supported(e)) return;
    reload();
    g_ent = e;
    g_have = true;
    g_open_req = true;
    g_search[0] = 0;
    g_add = false;
    g_val_open = false;
    g_confirm_restore = false;
}

/* The running game's cheats overlay (pad combo): brings RetroHub forward on the panel. */
void cheats_open_live() {
    if (!A.proc || A.run.key.empty()) return;
    const hub::GameEntry* e = find_entry(A.run.key);
    if (!e || !cheats_supported(*e)) return;
    hub::GameEntry c = *e;
    cheats_open(c);
    focus_hub();
}

/* Console + game-pad watch: hold View (Back) and press Y while a supported game runs. */
void cheats_pad_watch() {
    static bool prev = false;
    bool down = false;
    if (A.proc && !g_headless && !A.run.key.empty()) {
        for (DWORD i = 0; i < 4 && !down; ++i) {
            XINPUT_STATE xs{};
            if (XInputGetState(i, &xs) == ERROR_SUCCESS && (xs.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) && (xs.Gamepad.wButtons & XINPUT_GAMEPAD_Y)) down = true;
        }
        for (auto* c : A.pads) if (!down && SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK) && SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y)) down = true;
    }
    if (down && !prev) qm_open_live();
    prev = down;
}

/* GameCube: write the enabled cheats into Dolphin's User\\GameSettings\\<GAMEID>.ini (and EnableCheats) right before Dolphin starts.
 * Offline and the online host use the player's choices; an online guest uses the list in the host's launch message (the same
 * list Dolphin NetPlay's code sync sends), so every PC holds identical codes. RetroHub's old entries are always removed first,
 * so turning a cheat off (or all of them) cleans the file. Returns the number of codes written (-1: error, logged). */
int cheats_apply_dolphin(const hub::GameEntry& e, const std::string& user_dir, bool online, bool is_host_, const std::string* launch_body) {
    if (e.system != hub::System::GC) return 0;
    std::string id = e.serial;
    if (id.empty()) if (const hub::GameInfo* g = hub::find_game(e.catalog_id)) if (!g->serials.empty()) id = g->serials[0];
    if (id.empty()) return 0;
    cheats::CheatSet cs;
    if (online && !is_host_) {
        if (launch_body) cheatstore::extract_launch(*launch_body, &cs);
    } else {
        ensure_loaded();
        if (autotest_active()) if (const char* want = std::getenv("RETROHUB_AUTOTEST_CHEATS")) {   // QA only: switch on database cheats by description ("a;b")
            std::string w = want;
            if (const cheatstore::GameState* gs0 = g_store.game(e.key)) { std::set<std::string> was = gs0->on; for (auto& id : was) g_store.set_on(e.key, id, false, now_s()); }   // exact set: clear the game's choices first ("-" = none)
            if (w == "-") w.clear();
            size_t from = 0;
            while (from <= w.size()) {
                size_t semi = w.find(';', from);
                std::string part = w.substr(from, semi == std::string::npos ? std::string::npos : semi - from);
                from = semi == std::string::npos ? w.size() + 1 : semi + 1;
                if (part.empty()) continue;
                for (auto& d : db_items(e)) if (d.cheat.desc.find(part) != std::string::npos) g_store.set_on(e.key, d.cheat.id, true, now_s());
            }
            persist();
        }
        cs = cheatstore::enabled_only(rows_cached(e));
    }
    for (auto& c : cs.cheats) c.enabled = true;
    std::string err;
    int n = dolphincheats::apply_for_game(user_dir, id, cs, &err);
    if (n < 0) autotest_log("cheats: could not write Dolphin game ini: " + err);
    else autotest_log("cheats: " + std::to_string(n) + " code set(s) written to Dolphin's " + id + ".ini");
    if (n < 0) push_toast("Could not apply cheats (" + err + "). The game starts without them.", COL_WARN, 8.0);
    else if (n > 0) push_toast(std::to_string(n) + (n == 1 ? " cheat" : " cheats") + " on for this game", COL_GOOD, 3.5);
    return n;
}

/* " --cheats <file>" for the runner. Offline: always (the runner live-reloads the file). Online: only when cheats are on,
 * from the host's list (guest: from the launch message); the runner does not take live changes then. */
std::string cheats_launch_args(const hub::GameEntry& e, bool online, bool is_host_, const std::string* launch_body) {
    if (!cheats_supported(e) || e.system == hub::System::GC || (online && e.system == hub::System::NDS)) return "";   // GC: see cheats_apply_dolphin
    std::string path = run_file(e.key), err;
    if (!online) {
        write_run(e);
        return " --cheats " + quote(path);
    }
    cheats::CheatSet cs;
    if (is_host_) cs = cheatstore::enabled_only(rows_cached(e));
    else if (!launch_body || !cheatstore::extract_launch(*launch_body, &cs)) return "";
    for (auto& c : cs.cheats) c.enabled = true;
    if (cs.cheats.empty()) return "";
    if (!cheats::save_file(path, cs, &err)) return "";
    return " --cheats " + quote(path);
}

/* ",\"cheats\":{...}" for the host's launch message. */
std::string cheats_launch_fragment(const hub::GameEntry& e) {
    if (!cheats_supported(e) || e.system == hub::System::NDS) return "";   // DS wireless rooms ignore cheats
    return cheatstore::launch_fragment(cheatstore::enabled_only(rows_cached(e)));
}

/* Why the host cannot launch with cheats right now ("" = fine). */
std::string cheats_launch_block(const hub::GameEntry& e, const std::vector<hubui::PeerInfo>& peers) {
    if (!cheats_supported(e) || e.system == hub::System::NDS) return "";
    std::vector<std::string> versions;
    for (auto& p : peers) if (p.slot != room_my_slot()) versions.push_back(p.app);
    return cheatstore::online_block(cheats_enabled_count(e), HUB_VERSION_STR, versions);
}

void cheats_on_room_msg(const std::string& game, const std::vector<std::string>& names) {
    g_room_game = game;
    g_room_names = names;
}

/* Room: "Cheats on: ..." for everyone, "Choose cheats" for the host. Also announces the host's list to the guests. */
void cheats_room_strip(const hub::GameEntry* rg, bool host) {
    if (!rg || !cheats_supported(*rg) || rg->system == hub::System::NDS) return;
    std::vector<std::string> names;
    if (host) {
        names = cheatstore::enabled_names(rows_cached(*rg));
        std::string sig = A.selected_game + "|" + std::to_string(A.peers.size()) + "|" + std::to_string(A.lobby.members().size());
        for (auto& n : names) sig += "|" + n;
        if (!g_fake_room && sig != g_sent_sig) {
            g_sent_sig = sig;
            std::string m = "{\"kind\":\"cheats\",\"game\":" + json::quote(A.selected_game) + ",\"names\":[";
            for (size_t i = 0; i < names.size(); ++i) m += (i ? "," : "") + json::quote(names[i]);
            A.lobby.send_msg(m + "]}");
        }
    } else if (g_room_game == A.selected_game) names = g_room_names;
    if (!host && names.empty()) return;
    begin_panel("room_cheats", ImVec2(0, 0), COL_PANEL);
    ImGui::PushFont(ui::F.bold);
    std::string line = cheatstore::summary_line(names, 4);
    ImGui::TextUnformatted(line.empty() ? "Cheats: off" : line.c_str());
    ImGui::PopFont();
    ImGui::SameLine(0, sp(2.f));
    if (host) {
        if (button("Choose cheats", ImVec2(0, 0), false, A.proc == nullptr, ui::Icon::Bolt)) { hub::GameEntry c = *rg; cheats_open(c); }
        text_wrapped_disabled("Everyone plays with the same cheats. They are fixed when the game starts.");
    } else {
        ImGui::TextDisabled("Chosen by the host - applied for everyone");
    }
    end_panel();
    ImGui::Dummy(ImVec2(0, sp(1.f)));
}

/* ======================= the panel ======================= */

void draw_cheats_modal() {
    static bool shot_done = false;
    if (!shot_done) {   // HUB_SHOT_CHEATS=list|add|locked|empty (+ HUB_SHOT_CHEATS_SYS=n64|gba|gb): open on a stand-in game
        shot_done = true;
        if (const char* m = std::getenv("HUB_SHOT_CHEATS")) {
            hub::GameEntry e;
            std::string sy = std::getenv("HUB_SHOT_CHEATS_SYS") ? std::getenv("HUB_SHOT_CHEATS_SYS") : "n64";
            e.system = sy == "gba" ? hub::System::GBA : sy == "gb" ? hub::System::GB : hub::System::N64;
            e.key = sy + ":shot";
            e.title = sy == "gba" ? "Pokemon Emerald" : sy == "gb" ? "Pokemon Red" : "Super Smash Bros.";
            ensure_loaded();
            std::string mode = m;
            if (mode != "empty") {
                { size_t k = 0; for (auto& d : db_items(e)) { if (d.featured && k++ < 2) g_store.set_on(e.key, d.cheat.id, true, 1); } }
                g_store.set_backup(e.key, "20260101-120000", 1);
                make_dirs(gdir_of(e) + "cheat_backups\\20260101-120000");
            }
            persist();
            cheats_open(e);
            if (mode == "add") {
                g_add = true;
                std::snprintf(g_add_name, sizeof g_add_name, "%s", "Infinite health");
                std::snprintf(g_add_codes, sizeof g_add_codes, "%s", sy == "gba" ? "82003000 0000FFFF" : sy == "gb" ? "01FF5CD1" : "8033B21E 00FF");
            }
            if (mode == "value" || mode == "valuehex" || mode == "valuelist") {
                for (auto& r : rows_cached(e))
                    if (r.spec.valid) {
                        cheatstore::Row v = r;
                        v.value_set = true;
                        v.value = mode == "valuehex" ? v.spec.max : std::min<uint32_t>(v.spec.max, 42);
                        if (mode == "valuelist") {
                            v.cheat.desc = "Hold item: 01=Potion 02=Antidote 03=Ether 04=Elixir";
                            v.spec = cheats::value_spec(v.cheat, v.target);
                            v.value = v.spec.options.empty() ? v.value : v.spec.options[1].value;
                        }
                        open_value_editor(v);
                        if (mode == "valuehex") g_val_dec = false, value_buf_set(v.value);
                        break;
                    }
            }
            if (mode == "locked") {
                A.in_room = true; A.selected_game = room_id(e); g_fake_other_host = true;
            }
        }
    }
    if (g_open_req) { g_open_req = false; if (g_have) ImGui::OpenPopup("##cheats"); }
    if (!g_have) return;
    const hub::GameEntry& e = g_ent;
    ImGuiIO& io = ImGui::GetIO();
    float w = std::min(sp(80.f), io.DisplaySize.x * 0.94f), hmax = io.DisplaySize.y * 0.9f;
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, std::min(hmax, sp(110.f))));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(2.5f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    bool open = ImGui::BeginPopupModal("##cheats", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    if (!open) return;
    if (ui::modal_back()) {
        if (g_val_open) g_val_open = false;
        else if (g_add) g_add = false;
        else { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, sp(0.5f)));

    bool locked_guest = guest_locked(e), locked_run = running_online(e);
    bool can_toggle = !locked_guest && !locked_run;
    const auto& rows = rows_cached(e);
    int on_n = g_store.on_count(e.key);

    ImGui::PushFont(ui::F.title);
    ImGui::Text("Cheats");
    ImGui::PopFont();
    ImGui::SameLine(0, sp(1.5f));
    if (on_n) pill((std::to_string(on_n) + " on").c_str(), ui::GOOD);
    ImGui::SameLine(0, sp(1.5f));
    ImGui::TextDisabled("%s", e.title.c_str());

    if (locked_guest) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
        ImGui::TextWrapped("%s", (cheatstore::summary_line(g_room_game == A.selected_game ? g_room_names : std::vector<std::string>{}, 6) + (g_room_names.empty() ? "The host decides the cheats in this room. They apply to everyone." : "  -  chosen by the host, applied for everyone")).c_str());
        ImGui::PopStyleColor();
    } else if (locked_run) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
        ImGui::TextWrapped("Online game running: cheats are fixed for this match.");
        ImGui::PopStyleColor();
    } else if (A.proc && A.run.key == e.key && e.system == hub::System::GC) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
        ImGui::TextWrapped("Dolphin can't change cheats mid-game: your changes apply the next time you start the game.");
        ImGui::PopStyleColor();
    } else if (A.proc && A.run.key == e.key) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::GOOD);
        ImGui::TextWrapped("Game running: changes apply right away.");
        ImGui::PopStyleColor();
    } else if (e.system == hub::System::GC) {
        ImGui::TextDisabled("GameCube cheats are loaded by Dolphin when the game starts. Gecko and Action Replay codes both work.");
    } else {
        ImGui::TextDisabled("In a game, hold View and press Y to open this again.");
    }

    // save safety
    {
        const cheatstore::GameState* gs = g_store.game(e.key);
        std::string gd = gdir_of(e);
        bool has_bk = gs && cheatstore::has_backup(gd, gs->backup);
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        if (has_bk) ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(has_bk ? "Your save was backed up before cheats were first turned on." : "Your save is backed up automatically the first time you turn a cheat on.");
        if (has_bk) {
            ImGui::SameLine(0, sp(1.5f));
            bool ok = A.proc == nullptr && !locked_guest;
            if (g_confirm_restore) {
                if (button("Really restore?", ImVec2(0, 0), true, ok)) {
                    std::string err;
                    int n = cheatstore::restore_save(gd, battery_of(e), gs->backup, stamp_now(), &err);
                    push_toast(n >= 0 ? "Save restored (the previous one was kept as a copy)" : "Could not restore: " + err, n >= 0 ? COL_GOOD : COL_BAD, 6.0);
                    A.vault_dirty = true;
                    g_confirm_restore = false;
                }
                ImGui::SameLine(0, sp(1.f));
                if (button("Cancel", ImVec2(0, 0))) g_confirm_restore = false;
            } else if (button("Restore save", ImVec2(0, 0), false, ok)) g_confirm_restore = true;
            if (!ok && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Close the game first");
        }
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));

    float foot_h = sp(7.5f);
    if (g_val_open) {
        const cheats::ValueSpec& sp_ = g_val_row.spec;
        ImGui::BeginChild("##cheat_val", ImVec2(0, ImGui::GetContentRegionAvail().y - foot_h), ImGuiChildFlags_NavFlattened);
        group_title("Choose a value", -1);
        ImGui::PushFont(ui::F.bold);
        ImGui::TextWrapped("%s", g_val_row.cheat.desc.c_str());
        ImGui::PopFont();
        char range[160];
        std::snprintf(range, sizeof range, "Allowed: %u to %u  (0x%s to 0x%s)", sp_.min, sp_.max, cheats::value_format(sp_.min, false, sp_.digits).c_str(), cheats::value_format(sp_.max, false, sp_.digits).c_str());
        ImGui::TextDisabled("%s", range);
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        // number field + decimal / hex switch
        ImGui::SetNextItemWidth(sp(32.f));
        osk::input("##vval", g_val_dec ? "0" : "00", g_val_buf, sizeof g_val_buf, g_val_dec ? ImGuiInputTextFlags_CharsDecimal : ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase);
        ImGui::SameLine(0, sp(1.5f));
        if (button("Decimal", ImVec2(0, sp(5.f)), g_val_dec, true)) { uint32_t v; bool ok = value_buf_number(&v); g_val_dec = true; if (ok) value_buf_set(v); }
        ImGui::SameLine(0, sp(0.75f));
        if (button("Hex", ImVec2(0, sp(5.f)), !g_val_dec, true)) { uint32_t v; bool ok = value_buf_number(&v); g_val_dec = false; if (ok) value_buf_set(v); }
        if (!sp_.options.empty()) {
            ImGui::Dummy(ImVec2(0, sp(0.5f)));
            uint32_t cur = 0;
            bool have = value_buf_number(&cur);
            std::string pv = "Pick a known value";
            for (auto& o : sp_.options) if (have && o.value == cur) pv = o.label + "  (" + std::to_string(o.value) + ")";
            ImGui::SetNextItemWidth(sp(48.f));
            if (ImGui::BeginCombo("##vopts", pv.c_str())) {
                for (auto& o : sp_.options)
                    if (ImGui::Selectable((o.label + "  (" + std::to_string(o.value) + ")").c_str(), have && o.value == cur)) value_buf_set(o.value);
                ImGui::EndCombo();
            }
        }
        // keypad: 3x4 digits, hex letters beside them
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        const float kw = sp(11.f), kh = sp(6.5f), kg = sp(0.75f);
        const size_t maxlen = g_val_dec ? 10 : (size_t)std::max(1, sp_.digits);
        auto key = [&](const char* label, char ch, float w = 0.f) {
            if (button(label, ImVec2(w > 0 ? w : kw, kh), false, true)) {
                size_t n = std::strlen(g_val_buf);
                if (ch == '<') { if (n) g_val_buf[n - 1] = 0; }
                else if (ch == 'C') g_val_buf[0] = 0;
                else if (n + 1 < sizeof g_val_buf && n < maxlen) {
                    if (n == 1 && g_val_buf[0] == '0') n = 0;
                    g_val_buf[n] = ch; g_val_buf[n + 1] = 0;
                }
            }
        };
        const char* digs[4][3] = {{"7", "8", "9"}, {"4", "5", "6"}, {"1", "2", "3"}, {"0", "Back", "Clear"}};
        const char* hexl[4][2] = {{"A", "B"}, {"C", "D"}, {"E", "F"}, {"", ""}};
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 3; ++c) {
                if (c) ImGui::SameLine(0, kg);
                const char* l = digs[r][c];
                key(l, l[0] == 'B' ? '<' : l[0] == 'C' ? 'C' : l[0]);
            }
            if (!g_val_dec && r < 3)
                for (int c = 0; c < 2; ++c) { ImGui::SameLine(0, c ? kg : kg * 4); key(hexl[r][c], hexl[r][c][0]); }
        }
        // live result
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        uint32_t typed = 0;
        bool ok = value_buf_number(&typed);
        if (!ok) { ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN); ImGui::TextWrapped("Type a number."); ImGui::PopStyleColor(); }
        else {
            uint32_t cl = cheats::value_clamp(sp_, typed);
            cheats::Cheat filled;
            cheats::value_apply(g_val_row.cheat, cl, g_val_row.target, &filled);
            std::string code = filled.codes.empty() ? "" : filled.codes[0];
            if (cl != typed) { ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN); ImGui::TextWrapped("Out of range: %u will be used instead.", cl); ImGui::PopStyleColor(); }
            else ImGui::TextDisabled("%u  =  0x%s", cl, cheats::value_format(cl, false, sp_.digits).c_str());
            ImGui::TextDisabled("Code: %s%s", code.c_str(), filled.codes.size() > 1 ? "  ..." : "");
        }
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        if (button("Set value", ImVec2(sp(18.f), sp(5.5f)), true, ok && can_toggle, ui::Icon::Plus)) value_commit(e);
        ImGui::SameLine(0, sp(1.5f));
        if (button("Cancel", ImVec2(0, sp(5.5f)))) g_val_open = false;
        ImGui::EndChild();
    } else if (g_add) {
        ImGui::BeginChild("##cheat_add", ImVec2(0, ImGui::GetContentRegionAvail().y - foot_h), ImGuiChildFlags_NavFlattened);
        group_title("Add custom code", -1);
        text_wrapped_disabled(e.system == hub::System::GC ? "Paste Gecko or Action Replay codes (8 + 8 hex digits, or XXXX-XXXX-XXXXX). One line per code line."
                                                          : "Paste GameShark, Action Replay, CodeBreaker or Game Genie codes. One code per line. The format is detected for you.");
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        ImGui::TextDisabled("Name");
        ImGui::SetNextItemWidth(-1);
        osk::input("##cname", "e.g. Infinite health", g_add_name, sizeof g_add_name);
        ImGui::TextDisabled("Codes");
        ImGui::SetNextItemWidth(-1);
        osk::input("##ccodes", "8033B21E 00FF", g_add_codes, sizeof g_add_codes);
        cheats::Target tg = target_of(e);
        auto lines = cheatstore::split_codes(g_add_codes);
        cheats::Format f;
        bool okc = !lines.empty();
        for (auto& l : lines) if (!cheatstore::detect_format(tg, l, &f)) okc = false;
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        if (lines.empty()) ImGui::TextDisabled("Waiting for a code...");
        else if (okc) { cheatstore::detect_format(tg, lines[0], &f); ImGui::PushStyleColor(ImGuiCol_Text, ui::GOOD); ImGui::Text("Detected: %s, %d line%s", fmt_label(f), (int)lines.size(), lines.size() == 1 ? "" : "s"); ImGui::PopStyleColor(); }
        else { ImGui::PushStyleColor(ImGuiCol_Text, ui::BAD); ImGui::TextWrapped("Not a valid code for %s.", hub::system_name(e.system)); ImGui::PopStyleColor(); }
        if (!g_add_err.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, ui::BAD); ImGui::TextWrapped("%s", g_add_err.c_str()); ImGui::PopStyleColor(); }
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        if (button("Add cheat", ImVec2(sp(16.f), sp(5.5f)), true, okc, ui::Icon::Plus)) {
            std::string err;
            std::string id = g_store.add_custom(e.key, g_add_name, g_add_codes, tg, now_s(), &err);
            if (id.empty()) g_add_err = err;
            else {
                first_enable_backup(e);
                persist();
                write_run(e);
                push_toast(std::string("Cheat on: ") + g_add_name, COL_GOOD, 3.5);
                g_add = false; g_add_err.clear(); g_add_name[0] = 0; g_add_codes[0] = 0;
            }
        }
        ImGui::SameLine(0, sp(1.5f));
        if (button("Back", ImVec2(0, sp(5.5f)))) { g_add = false; g_add_err.clear(); }
        ImGui::EndChild();
    } else {
        ImGui::SetNextItemWidth(-1);
        osk::input("##csearch", "Search cheats", g_search, sizeof g_search);
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        static std::string q_last;
        static int q_rev = -1;
        static std::string q_key;
        static std::vector<cheatstore::Row> shown;
        if (q_last != g_search || q_rev != g_rev || q_key != e.key) { shown = cheatstore::filter_rows(rows, g_search); q_last = g_search; q_rev = g_rev; q_key = e.key; }
        ImGui::BeginChild("##cheat_list", ImVec2(0, ImGui::GetContentRegionAvail().y - foot_h), ImGuiChildFlags_NavFlattened);
        if (rows.empty()) {
            text_wrapped_disabled("No ready-made cheats for this game yet. Use \"Add custom code\" to type or paste your own.");
        } else if (shown.empty()) {
            ImGui::TextDisabled("Nothing matches \"%s\".", g_search);
        }
        bool searching = g_search[0] != 0;
        const char* titles[3] = {"Popular", "All cheats", "My codes"};
        /* Flat display list (section headers + rows, all one height) so ImGuiListClipper can skip what is off screen. */
        struct Item { int row; int group; int count; };   // row < 0: header
        static std::vector<Item> items;
        static std::string it_key; static int it_rev = -1; static std::string it_q;
        if (it_key != e.key || it_rev != g_rev || it_q != g_search) {
            items.clear();
            int cnt[3] = {0, 0, 0};
            for (auto& r : shown) ++cnt[r.group];
            int last = -1;
            for (size_t i = 0; i < shown.size(); ++i) {
                if (shown[i].group != last) { last = shown[i].group; items.push_back({-1, last, cnt[last]}); }
                items.push_back({(int)i, last, 0});
            }
            it_key = e.key; it_rev = g_rev; it_q = g_search;
        }
        const float row_h = sp(7.75f);
        const bool live_here = A.proc && !A.run.online && A.run.key == e.key && e.system == hub::System::N64;
        int toggled = -1;
        std::string removed_id;
        ImGuiListClipper clip;
        clip.Begin((int)items.size(), row_h);
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const Item& it = items[i];
                if (it.row < 0) {
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    ImGui::PushFont(ui::F.h2);
                    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + sp(3.f)));
                    ImGui::TextUnformatted(searching && it.group == 1 ? "Matches" : titles[it.group]);
                    ImGui::PopFont();
                    ImGui::SameLine(0, sp(1.f));
                    ImGui::TextDisabled("%d", it.count);
                    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + row_h));
                    ImGui::Dummy(ImVec2(0, 0));
                    continue;
                }
                bool rm = false, act = false, ed = false;
                bool btn = live_here && shown[it.row].cheat.enabled && cheats::n64_is_button_cheat(shown[it.row].cheat);
                if (cheat_row(shown[it.row], can_toggle, &rm, btn ? &act : nullptr, shown[it.row].spec.valid && shown[it.row].cheat.enabled ? &ed : nullptr)) toggled = it.row;
                if (ed) open_value_editor(shown[it.row]);
                if (act) { ++g_fire[e.key + "|" + shown[it.row].cheat.id]; write_run(e); push_toast("Activated: " + shown[it.row].cheat.desc, COL_GOOD, 2.5); }
                if (rm) removed_id = shown[it.row].cheat.id;
            }
        if (toggled >= 0) { cheatstore::Row copy = shown[toggled]; toggle(e, copy); }
        if (!removed_id.empty()) { g_store.remove_custom(e.key, removed_id, now_s()); persist(); write_run(e); }
        ImGui::EndChild();
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (!g_add && !g_val_open) {
        if (button("Add custom code", ImVec2(0, sp(5.5f)), false, can_toggle, ui::Icon::Plus)) { g_add = true; g_add_err.clear(); }
        ImGui::SameLine(0, sp(1.5f));
    }
    if (g_ent.system == hub::System::PS1 && ps1_swap_available(g_ent.key)) {   // multi-disc: the same overlay (View+Y) swaps the disc
        if (button("Swap disc", ImVec2(0, sp(5.5f)), false, true)) ps1_swap_disc(g_ent.key);
        ImGui::SameLine(0, sp(1.5f));
    }
    if (button("Close", ImVec2(sp(14.f), sp(5.5f)), true, true, ui::Icon::Close)) ImGui::CloseCurrentPopup();
    ImGui::PopStyleVar();
    ImGui::EndPopup();
}
