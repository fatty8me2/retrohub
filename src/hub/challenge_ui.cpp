#include "challenge_ui.h"

#include "app.h"
#include "attach_xfer.h"
#include "games.h"
#include "tex_cache_logic.h"

#include <SDL.h>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "third_party/stb_image.h"

/* Friend challenges (Plan 4 p4-social). See challenge_logic.h (cards, rules), challenge_flow.h (state machines), docs/FRIENDS.md "Friend challenges".
 * Nothing here opens a window over a running game: the sender works inside the game menu, the receiver in the hub's chat, and a running
 * challenge only talks to the game through runctl (osd_corner timer). */

namespace hubui {
namespace {

using namespace hub::challenge;
namespace fs = std::filesystem;

int64_t now_ms() { return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
int64_t tick_ms() { return (int64_t)GetTickCount64(); }
fs::path u8(const std::string& s) { return fs::u8path(s); }
std::string data() { return hub::data_dir(); }
std::string scratch(const std::string& name) { return scratch_path(data(), name); }
bool exists(const std::string& p) { std::error_code ec; return fs::exists(u8(p), ec); }
int64_t fsize(const std::string& p) { std::error_code ec; auto n = fs::file_size(u8(p), ec); return ec ? -1 : (int64_t)n; }
std::string trim_end_sep(std::string d) {
    while (!d.empty() && (d.back() == '\\' || d.back() == '/')) d.pop_back();
    return d;
}
std::string me_name() {
    std::string n = FR.cli.username();
    return n.empty() ? A.st.name : n;
}

ChallengeHost g_host;
bool g_account_set = false;
void ensure_account() {
    if (g_account_set) return;
    g_account_set = true;
    hub::attach::set_account_provider([]() {
        hub::attach::Account a;
        a.base_url = hub::attach::http_base_from_server(server_url());
        a.user_id = FR.cli.my_uid();
        a.secret = FR.cli.my_secret();
        return a;
    });
}

/* ---------------- results (leaderboards) ---------------- */
std::map<std::string, std::vector<Card>> g_results;   // challenge id -> result cards
std::set<std::string> g_seen_msgs;
std::map<std::string, Card> g_decoded;               // message id -> card
bool g_results_loaded = false;
std::string results_path() { return data() + "challenge_results.txt"; }

void results_save() {
    std::ofstream f(u8(results_path()), std::ios::binary | std::ios::trunc);
    int n = 0;
    for (auto& kv : g_results) for (auto& c : kv.second) { if (n++ > 400) return; f << encode_card(c) << "\n"; }
}
bool results_add(const Card& c) {
    if (c.kind != CardKind::Result || c.id.empty()) return false;
    auto& v = g_results[c.id];
    for (auto& o : v) if (o.player == c.player && o.at == c.at && o.time_ms == c.time_ms && o.score == c.score && o.outcome == c.outcome) return false;
    v.push_back(c);
    return true;
}
void results_load() {
    if (g_results_loaded) return;
    g_results_loaded = true;
    std::ifstream f(u8(results_path()), std::ios::binary);
    std::string line;
    while (std::getline(f, line)) { Card c; if (decode_card(line, &c)) results_add(c); }
}

/* ---------------- pictures ---------------- */
struct Pic { SDL_Texture* t = nullptr; int w = 0, h = 0; };
hubtex::TexCache<Pic> g_pics(40, [](Pic& p) { if (p.t) SDL_DestroyTexture(p.t); });   // LRU; evicted textures are freed next frame (tex_cache_logic.h)
/* Per-path probe: the file's mtime+size, re-checked at most every 500 ms. A missing file is never cached (the download may land later),
 * and the stamp is part of the cache key, so a replaced picture is decoded again. */
struct PicProbe { int64_t next_ms = 0; std::string stamp; };
std::map<std::string, PicProbe> g_probe;
std::string pic_stamp(const std::string& path) {
    std::error_code ec;
    auto t = fs::last_write_time(u8(path), ec);
    if (ec) return std::string();
    auto sz = fs::file_size(u8(path), ec);
    if (ec) return std::string();
    return std::to_string((long long)t.time_since_epoch().count()) + ":" + std::to_string((long long)sz);
}
Pic* pic_for(const std::string& path) {
    const int64_t now = tick_ms();
    if (g_probe.size() > 200) g_probe.clear();
    PicProbe& pr = g_probe[path];
    if (now >= pr.next_ms) { pr.next_ms = now + 500; pr.stamp = pic_stamp(path); }
    if (pr.stamp.empty()) return nullptr;   // not there (yet): nothing cached, asked again in half a second
    const std::string key = path + "|" + pr.stamp;
    if (Pic* hit = g_pics.find(key)) return hit->t ? hit : nullptr;
    Pic p;
    SDL_Renderer* ren = A.win ? SDL_GetRenderer(A.win) : nullptr;
    if (ren) {
        std::ifstream f(u8(path), std::ios::binary);
        std::string d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        int w = 0, h = 0, n = 0;
        unsigned char* px = d.empty() ? nullptr : stbi_load_from_memory((const unsigned char*)d.data(), (int)d.size(), &w, &h, &n, 4);
        if (px) {
            p.t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
            if (p.t) { SDL_UpdateTexture(p.t, nullptr, px, w * 4); SDL_SetTextureBlendMode(p.t, SDL_BLENDMODE_BLEND); p.w = w; p.h = h; }
            stbi_image_free(px);
        }
    }
    Pic* stored = g_pics.insert(key, p);
    return stored->t ? stored : nullptr;
}

/* ---------------- sender ---------------- */
struct SendState {
    ChallengeSendView v;
    std::string key, system, state_dir, id;
    int slot = 0;
    bool had_backup = false;
    std::string slot_state, bak_state, bak_png, bak_json;
    int64_t slot_stamp = -1;
    int64_t save_asked_ms = 0, stable_since = 0, stable_size = -1;
    std::string state_copy;      // scratch send state
    int job_state = 0, job_shot = 0;
    hubnet::Attach ref_state, ref_shot;
    bool shot_ok = false;
    std::vector<std::string> targets;
    hub::challenge::Card card;
} S;

int64_t file_stamp(const std::string& p) {
    std::error_code ec;
    auto t = fs::last_write_time(u8(p), ec);
    return ec ? -1 : (int64_t)t.time_since_epoch().count();
}
void copy_file(const std::string& a, const std::string& b) { std::error_code ec; fs::copy_file(u8(a), u8(b), fs::copy_options::overwrite_existing, ec); }
std::string slot_file(int slot, const char* ext) { return trim_end_sep(S.state_dir) + "\\slot" + std::to_string(slot) + ext; }

void send_fail(const std::string& why) {
    S.v.error = why;
    S.v.phase = send_next(S.v.phase, SendEvent::Fail);
}
void restore_slot() {
    if (!S.slot_state.empty() && S.had_backup) {   // put the player's own slot back
        copy_file(S.bak_state, S.slot_state);
        if (exists(S.bak_png)) copy_file(S.bak_png, slot_file(S.slot, ".png"));
        if (exists(S.bak_json)) copy_file(S.bak_json, slot_file(S.slot, ".json"));
    } else if (!S.slot_state.empty()) {            // the slot was empty before: leave no trace of the scratch save
        std::error_code ec;
        fs::remove(u8(S.slot_state), ec); fs::remove(u8(slot_file(S.slot, ".png")), ec); fs::remove(u8(slot_file(S.slot, ".json")), ec); fs::remove(u8(slot_file(S.slot, ".bmp")), ec);
    }
    S.slot_state.clear();
}

void send_tick() {
    using P = SendPhase;
    switch (S.v.phase) {
    case P::Saving: {
        const int64_t t = tick_ms();
        const int64_t st = file_stamp(S.slot_state), sz = fsize(S.slot_state);
        if (st >= 0 && st != S.slot_stamp && sz > 0) {
            if (sz != S.stable_size) { S.stable_size = sz; S.stable_since = t; }
            else if (t - S.stable_since > 600) {   // size unchanged for 0.6 s: the runner finished writing
                std::string why;
                if (!challenge_eligible(S.system, sz, &why)) { restore_slot(); send_fail(why); return; }
                S.state_copy = scratch(scratch_state_name(S.id, false));
                std::error_code ec; fs::create_directories(u8(scratch_dir(data())), ec);
                copy_file(S.slot_state, S.state_copy);
                std::string png = slot_file(S.slot, ".png");
                if (!exists(png)) png = slot_file(S.slot, ".bmp");
                S.v.thumb.clear();
                if (png.size() > 4 && png.compare(png.size() - 4, 4, ".png") == 0 && exists(png)) { std::string d = scratch(scratch_shot_name(S.id)); copy_file(png, d); S.v.thumb = d; }
                restore_slot();
                if (fsize(S.state_copy) <= 0) { send_fail("Couldn't save the game state."); return; }
                S.v.status.clear();
                S.v.phase = send_next(S.v.phase, SendEvent::StateSaved);
            }
        } else if (t - S.save_asked_ms > 15000) { restore_slot(); send_fail("The game didn't save a state in time. Try again."); }
        break;
    }
    case P::Uploading: {
        hub::attach::Result r;
        bool st_done = false, sh_done = !S.job_shot;
        if (hub::attach::poll(S.job_state, &r)) {
            if (r.done) { if (!r.ok) { send_fail(r.error); return; } S.ref_state = r.ref; st_done = true; }
        }
        if (S.job_shot) {
            hub::attach::Result r2;
            if (hub::attach::poll(S.job_shot, &r2) && r2.done) { if (r2.ok) { S.ref_shot = r2.ref; S.shot_ok = true; } sh_done = true; }   // a failed picture is not fatal
        }
        S.v.status = "Sending the save to your friends...";
        if (st_done && sh_done) S.v.phase = send_next(S.v.phase, SendEvent::UploadsDone);
        break;
    }
    case P::Sending: {
        Card c;
        c.kind = CardKind::Challenge; c.id = S.id; c.game_key = S.key; c.game_title = S.v.title; c.system = S.system; c.rom_key = S.key;
        c.sender = me_name(); c.goal = S.v.goal; c.state = S.ref_state;
        if (S.shot_ok) { c.has_shot = true; c.shot = S.ref_shot; }
        std::string enc = encode_card(c);
        if (enc.empty()) { send_fail("That challenge is too big to send."); return; }
        for (auto& uid : S.targets) FR.cli.send_message(uid, card_fallback_text(c), &S.ref_state, enc);
        S.card = c;
        results_load();
        S.v.status = "Challenge sent to " + std::to_string(S.targets.size()) + (S.targets.size() == 1 ? " friend." : " friends.");
        S.v.phase = send_next(S.v.phase, SendEvent::Delivered);
        break;
    }
    default: break;
    }
}

/* ---------------- receiver / run ---------------- */
struct RunState {
    RunPhase phase = RunPhase::Idle;
    Card card;
    std::string challenger_uid, state_path, err;
    int job = 0;
    RunClock clock;
    int64_t last_corner = 0;
    bool no_ra_pending = false;
    bool seen_proc = false;
} R;

const hub::GameEntry* local_game_for(const Card& c) {
    if (c.game_key.empty()) return nullptr;
    static const char* shot = std::getenv("HUB_SHOT_CHALLENGE");   // screenshot aid: pretend the game is in the library
    if (shot && g_headless && std::string(shot) != "cant") { static hub::GameEntry fake; fake.key = c.game_key; fake.title = c.game_title; return &fake; }
    return find_entry(c.game_key);
}

void run_stop_overlay() {
    if (g_host.corner) g_host.corner(1, "");
}
void run_reset() {
    R.phase = RunPhase::Idle; R.no_ra_pending = false; R.seen_proc = false; R.job = 0; R.clock = RunClock();
}
void post_result(bool did_it, int64_t score, int64_t elapsed) {
    Card r;
    r.kind = CardKind::Result; r.id = R.card.id; r.game_key = R.card.game_key; r.game_title = R.card.game_title; r.system = R.card.system;
    r.player = me_name(); r.outcome = did_it ? Outcome::Done : Outcome::GaveUp; r.at = now_ms();
    if (did_it && R.card.goal.kind == GoalKind::FastestTime) r.time_ms = elapsed;
    if (did_it && R.card.goal.kind == GoalKind::Score) r.score = score;
    results_load(); results_add(r); results_save();
    const std::string enc = encode_card(r);
    if (!enc.empty() && !R.challenger_uid.empty()) FR.cli.send_message(R.challenger_uid, card_fallback_text(r), nullptr, enc);
    push_toast(did_it ? "Result sent: " + result_text(r, R.card.goal) : std::string("You gave up. The result was sent."), did_it ? COL_GOOD : COL_WARN, 5.0);
}

void run_tick(bool menu_open) {
    const int64_t t = tick_ms();
    if (R.phase == RunPhase::Downloading) {
        hub::attach::Result r;
        if (!hub::attach::poll(R.job, &r)) { R.err = "The download was lost."; R.phase = run_next(R.phase, RunEvent::Fail); return; }
        if (!r.done) return;
        hub::attach::forget(R.job);
        if (!r.ok) { R.err = r.error; R.phase = run_next(R.phase, RunEvent::Fail); push_toast(r.error, COL_BAD, 6.0); return; }
        const hub::GameEntry* g = local_game_for(R.card);
        if (!g) { R.err = rom_check_message(R.card.game_title); R.phase = run_next(R.phase, RunEvent::Fail); push_toast(R.err, COL_BAD, 6.0); return; }
        if (A.proc) { R.err = "Finish your current game first."; R.phase = run_next(R.phase, RunEvent::Fail); push_toast(R.err, COL_WARN, 5.0); return; }
        R.state_path = r.path;
        R.phase = run_next(R.phase, RunEvent::Downloaded);
        A.resume_key = g->key;               // launch.cpp passes --load-state for exactly this game, once
        A.resume_state = R.state_path;       // the scratch copy, never one of the player's own slots
        R.no_ra_pending = true;              // achievements_launch_args() consumes this: no RetroAchievements (hardcore refuses state loads)
        hub::GameEntry c = *g;
        play_entry(c);
        return;
    }
    if (R.phase == RunPhase::Launching || R.phase == RunPhase::Running) {
        if (A.proc && !R.card.game_key.empty() && A.run.key == R.card.game_key && !A.run.online) {
            if (!R.seen_proc) {
                R.seen_proc = true; R.no_ra_pending = false;
                R.clock.start(t);
                R.phase = run_next(R.phase, RunEvent::Started);
                if (g_host.osd) g_host.osd("Challenge: " + goal_text(R.card.goal) + ". Timer started.");
            }
            R.clock.set_menu_open(menu_open, t);
            if (t - R.last_corner >= 1000 && !A.run.ctl_path.empty()) {
                R.last_corner = t;
                if (g_host.corner) g_host.corner(1, "Challenge " + osd_timer_text(R.clock.elapsed(t)));
            }
        } else if (R.seen_proc && !A.proc) {   // the game closed without an answer: counts as giving up
            R.phase = run_next(R.phase, RunEvent::GameEnded);
            post_result(false, -1, 0);
            run_reset();
        } else if (!R.seen_proc && !A.proc && !A.resume_key.empty()) {
            // still being started (cloud gate etc.)
        }
    }
}

} // namespace

/* ---------------- public: host + tick ---------------- */
void challenge_set_host(ChallengeHost h) { g_host = std::move(h); }
void challenge_tick(bool menu_open) {
    ensure_account();
    if (S.v.phase == SendPhase::Saving || S.v.phase == SendPhase::Uploading || S.v.phase == SendPhase::Sending) send_tick();
    run_tick(menu_open);
}
bool challenge_pending_no_ra() { return R.no_ra_pending; }
bool challenge_running() { return R.phase == RunPhase::Running; }
bool challenge_busy() { return R.phase == RunPhase::Downloading || R.phase == RunPhase::Launching || R.phase == RunPhase::Running; }
bool challenge_goal_is_score() { return R.card.goal.kind == GoalKind::Score; }

void challenge_run_finish(bool did_it, const std::string& typed_score) {
    if (R.phase != RunPhase::Running) return;
    int64_t score = -1;
    if (did_it && R.card.goal.kind == GoalKind::Score && !parse_score(typed_score, &score)) { push_toast("Type your score as a number.", COL_WARN, 4.0); return; }
    const int64_t el = R.clock.elapsed(tick_ms());
    R.phase = run_next(R.phase, did_it ? RunEvent::Done : RunEvent::GaveUp);
    run_stop_overlay();
    post_result(did_it, score, el);
    run_reset();
}

/* ---------------- public: sender ---------------- */
const ChallengeSendView& challenge_send_view() { return S.v; }
bool challenge_send_begin(const std::string& game_key, const std::string& title, const std::string& system, const std::string& state_dir) {
    if (send_busy(S.v.phase)) return false;
    ensure_account();
    S = SendState();
    S.v.phase = send_next(SendPhase::Idle, SendEvent::Begin);
    S.v.title = title; S.v.status = "Saving the game...";
    S.key = game_key; S.system = system; S.state_dir = state_dir; S.id = new_challenge_id();
    // the first empty slot, else slot 10 (index 9) whose owner is backed up and put back afterwards
    S.slot = 9;
    for (int i = 0; i < 10; i++) if (!exists(slot_file(i, ".state"))) { S.slot = i; break; }
    S.slot_state = slot_file(S.slot, ".state");
    std::error_code ec; fs::create_directories(u8(scratch_dir(data())), ec);
    if (exists(S.slot_state)) {
        S.had_backup = true;
        S.bak_state = scratch("ch-" + S.id + "-bak.state"); S.bak_png = scratch("ch-" + S.id + "-bak.png"); S.bak_json = scratch("ch-" + S.id + "-bak.json");
        copy_file(S.slot_state, S.bak_state);
        if (exists(slot_file(S.slot, ".png"))) copy_file(slot_file(S.slot, ".png"), S.bak_png);
        if (exists(slot_file(S.slot, ".json"))) copy_file(slot_file(S.slot, ".json"), S.bak_json);
    }
    S.slot_stamp = file_stamp(S.slot_state);
    S.save_asked_ms = tick_ms();
    if (g_host.save_state) g_host.save_state(S.slot);
    return true;
}
void challenge_send_goal(GoalKind k) {
    if (S.v.phase != SendPhase::PickGoal) return;
    S.v.goal.kind = k; S.v.goal.target = 0;
    S.v.phase = send_next(S.v.phase, k == GoalKind::Score ? SendEvent::GoalScore : k == GoalKind::FastestTime ? SendEvent::GoalFastest : SendEvent::GoalJust);
}
bool challenge_send_score(const std::string& typed) {
    int64_t v = 0;
    if (S.v.phase != SendPhase::TypeScore || !parse_score(typed, &v)) return false;
    S.v.goal.target = v;
    S.v.phase = send_next(S.v.phase, SendEvent::ScoreOk);
    return true;
}
bool challenge_send_toggle_friend(const std::string& uid) { return friend_toggle(&S.v.selected, uid); }
void challenge_send_go() {
    if (S.v.phase != SendPhase::PickFriends || S.v.selected.empty()) return;
    S.targets = S.v.selected;
    const std::string nm = hub::attach::sanitize_name(S.v.title + " challenge.state");
    S.job_state = hub::attach::upload(S.state_copy, "state", nm, S.targets);
    S.job_shot = S.v.thumb.empty() ? 0 : hub::attach::upload(S.v.thumb, "shot", hub::attach::sanitize_name(S.v.title + ".png"), S.targets);
    S.v.status = "Sending the save to your friends...";
    S.v.phase = send_next(S.v.phase, SendEvent::FriendsOk);
}
void challenge_send_back() { S.v.phase = send_next(S.v.phase, SendEvent::Back); }
void challenge_send_cancel() { if (S.v.phase == SendPhase::Saving) restore_slot(); S.v.phase = send_next(S.v.phase, SendEvent::Cancel); }
void challenge_send_reset() { if (!send_busy(S.v.phase)) S = SendState(); }

/* ---------------- chat card ---------------- */
void challenge_on_message(const hubnet::Message& m) {
    if (m.card.empty() || !looks_like_card(m.card) || !g_seen_msgs.insert(m.id).second) return;
    results_load();
    Card c;
    if (!decode_card(m.card, &c)) return;
    if (c.kind == CardKind::Result && results_add(c)) results_save();
}

namespace {
void text_col(ImU32 col, const std::string& s, bool wrapped = false, float wrap_w = 0) {
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    if (wrapped) { ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_w); ImGui::TextUnformatted(s.c_str()); ImGui::PopTextWrapPos(); }
    else ImGui::TextUnformatted(s.c_str());
    ImGui::PopStyleColor();
}
bool g_dl_started = false;
std::map<std::string, int> g_shot_jobs;   // challenge id -> job (picture download)

void ensure_picture(const Card& c) {
    if (!c.has_shot || c.shot.id.empty()) return;
    const std::string dest = scratch(scratch_shot_name(c.id));
    if (exists(dest) || c.shot.expires < now_ms()) return;
    auto it = g_shot_jobs.find(c.id);
    if (it == g_shot_jobs.end()) { if (g_shot_jobs.size() < 6) g_shot_jobs[c.id] = hub::attach::download(c.shot, dest); return; }
    hub::attach::Result r;
    if (hub::attach::poll(it->second, &r) && r.done) hub::attach::forget(it->second);
}
}  // namespace

bool challenge_draw_card(const hubnet::Message& m, float width) {
    if (m.card.empty() || !looks_like_card(m.card)) return false;
    auto it = g_decoded.find(m.id);
    if (it == g_decoded.end()) { Card c; if (!decode_card(m.card, &c)) return false; it = g_decoded.emplace(m.id, c).first; }
    const Card& c = it->second;
    results_load();
    challenge_on_message(m);
    const std::string idk = m.id.empty() ? c.id + c.player : m.id;
    ImGui::PushID(idk.c_str());
    if (c.kind == CardKind::Result) {   // one tidy line
        text_col(ui::TEXT3, (m.mine ? "You: " : c.player + ": ") + result_text(c, Goal{}) + (c.game_title.empty() ? std::string() : "  -  " + c.game_title));
        ImGui::PopID();
        return true;
    }
    const float pad = sp(1.25f);
    ImGui::BeginChild("##chcard", ImVec2(width, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted(("Challenge: " + c.game_title).c_str());
    ImGui::PopFont();
    text_col(ui::TEXT2, (m.mine ? std::string("You") : (m.from_name.empty() ? c.sender : m.from_name)) + " challenges you - " + goal_text(c.goal));
    if (!c.note.empty()) text_col(ui::TEXT2, c.note);
    ensure_picture(c);
    if (Pic* p = pic_for(scratch(scratch_shot_name(c.id)))) {
        const float tw = std::min(width - pad * 2, sp(30.f)), th = tw * (float)p->h / std::max(1, p->w);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(tw, th));
        ui::draw_art(ImGui::GetWindowDrawList(), p->t, p->w, p->h, p0, ImVec2(p0.x + tw, p0.y + th), sp(0.75f));
    }
    // leaderboard
    auto rows = leaderboard(c.goal, c.id, g_results.count(c.id) ? g_results[c.id] : std::vector<Card>());
    if (!rows.empty()) {
        ImGui::Dummy(ImVec2(0, sp(0.25f)));
        text_col(ui::TEXT3, "Leaderboard");
        for (auto& r : rows) {
            char b[160];
            if (r.gave_up) std::snprintf(b, sizeof b, "   -   %s: gave up", r.player.c_str());
            else std::snprintf(b, sizeof b, "%3d.  %s: %s", r.rank, r.player.c_str(), r.text.c_str());
            text_col(r.gave_up ? ui::TEXT3 : (r.rank == 1 ? ui::GOOD : ui::TEXT), b);
        }
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (m.mine) {
        text_col(ui::TEXT3, "Results from your friends show up here.");
    } else if (c.state.expires < now_ms()) {
        text_col(ui::TEXT3, "This challenge has expired (files last 7 days).");
    } else {
        const hub::GameEntry* g = local_game_for(c);
        if (!g || rom_check(g->key, c.rom_key) == RomCheck::Different) {
            text_col(ui::WARN, rom_check_message(c.game_title), true, width - pad * 2);   // nothing else is offered
        } else if (challenge_busy()) {
            text_col(ui::TEXT3, R.card.id == c.id && R.phase == RunPhase::Downloading ? "Getting the challenge..." : (R.card.id == c.id ? "You're playing this challenge." : "You're already in a challenge."));
        } else if (A.proc) {
            text_col(ui::TEXT3, "Finish your current game to take this challenge.");
        } else {
            if (ui::button("Accept", ImVec2(sp(12.f), sp(5.f)), true)) {
                ensure_account();
                results_load();
                R = RunState();
                R.card = c; R.challenger_uid = m.from;
                R.phase = run_next(RunPhase::Idle, RunEvent::Accept);
                std::error_code ec; fs::create_directories(u8(scratch_dir(data())), ec);
                R.job = hub::attach::download(c.state, scratch(scratch_state_name(c.id, true)));
            }
            ImGui::SameLine();
            text_col(ui::TEXT3, "Plays offline from a copy of the save. " + std::string("Achievements are off for it."));
        }
        if (R.phase == RunPhase::Failed && R.card.id == c.id && !R.err.empty()) text_col(ui::BAD, R.err);
    }
    ImGui::EndChild();
    ImGui::PopID();
    (void)g_dl_started;
    return true;
}

/* ---------------- screenshot aid ---------------- */
void challenge_shot_draw() {
    static const char* mode = std::getenv("HUB_SHOT_CHALLENGE");
    if (!mode || !g_headless) return;
    static bool prepared = false;
    static hubnet::Message ch, mine, res1, res2, res3;
    if (!prepared) {
        prepared = true;
        Card c;
        c.id = "1a2b3c4d5e6f7a8b"; c.game_key = A.lib.empty() ? "n64:shot" : A.lib[0].key; c.game_title = A.lib.empty() ? "Super Mario 64" : A.lib[0].title;
        c.system = "n64"; c.rom_key = std::string(mode) == "cant" ? "different-rom" : c.game_key; c.sender = "Mickey";
        c.goal.kind = GoalKind::FastestTime;
        c.state.id = "0123456789abcdef0123456789abcdef"; c.state.expires = now_ms() + 5 * 86400000LL; c.state.kind = "state";
        ch.id = "m1"; ch.from = "u-mickey"; ch.from_name = "Mickey"; ch.card = encode_card(c); ch.at = now_ms();
        mine = ch; mine.id = "m2"; mine.mine = true;
        const char* who[] = {"Mickey", "Mia", "Jules"};
        int64_t tm[] = {83450, 75210, 99990};
        hubnet::Message* rm[] = {&res1, &res2, &res3};
        for (int i = 0; i < 3; i++) {
            Card r; r.kind = CardKind::Result; r.id = c.id; r.game_title = c.game_title; r.player = who[i]; r.time_ms = tm[i]; r.at = 1000 + i;
            if (std::string(mode) == "results" || i < 2) { rm[i]->id = "r" + std::to_string(i); rm[i]->card = encode_card(r); }
        }
    }
    const bool results = std::string(mode) == "results";
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(sp(70.f), io.DisplaySize.x * 0.9f), 0));
    ImGui::Begin("Mickey (chat preview)##chshot", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::TextDisabled("Mickey: look at this!");
    if (results) { challenge_draw_card(mine, w); for (auto* r : {&res1, &res2, &res3}) challenge_draw_card(*r, w); }
    else { challenge_draw_card(ch, w); if (!res1.card.empty()) { challenge_draw_card(res1, w); challenge_draw_card(res2, w); } }
    ImGui::End();
}

} // namespace hubui

void challenge_textures_begin_frame() { hubui::g_pics.begin_frame(); }
