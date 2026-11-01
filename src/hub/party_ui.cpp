#include "app.h"
#include "friends_ui.h"
#include "party_closer.h"
#include "party_history.h"
#include "party_logic.h"
#include "party_ui.h"
#include "runner_ctl.h"

/* Party mode flow (Plan 4 P1-P3): the lobby messages, the finished-match watcher, the round lifecycle on every PC, the OSD sequence
 * that closes a round's game, polls (vote for the next game / who won), the couch party and the party-night history.
 * The drawing lives in party_view.cpp. Pure logic and its tests: party_logic.h / party_closer.h / party_history.h, test_party.cpp.
 *
 * Who does what in a room:
 *   - the HOST owns the State: results (its own game's result file), the round timer, polls, membership; every change goes through the
 *     pure reducer and out as one pt_state message (never on a timer).
 *   - EVERY PC reacts to the State it has: when a round ends it closes its own game (OsdMsg, OsdCorner, Quit, window close as a last
 *     resort), readies itself for the next game (auto-ready, once per round) and shows the standings when the after-game rule allows.
 * Nothing here raises, focuses or flashes a window; announcements during a game go to the game's OSD only. */

namespace pl = hubui::party;

void partyui_qa_drive(unsigned long long now);   // below: the RETROHUB_AUTOTEST_PARTY script

namespace {

/* ------------------------------------------------------------------ runtime state */

struct Runtime {
    pl::State st, seen;                       // what this PC believes / the State the reactions last ran for
    bool have = false;
    bool ov = false;                          // the standings / podium / vote overlay is wanted (party_view.cpp shows it when allowed)
    std::string my_vote, my_vote_what;        // my pick in the open poll (instant feedback; the tally comes from the host)
    int my_vote_round = -1;
    hubui::results::Watcher watch;
    ULONGLONG watch_run = 0, watch_next = 0;
    ULONGLONG round_t0 = 0, consumed_tick = 0, inter_at = 0, member_next = 0, poll_all_at = 0, ack_next = 0, corner_next = 0, corner_sent = 0;
    pl::Closer closer;
    pl::AckInfo ack;
    std::string corner_text;
    long long corner_seq = 0;
    bool corner_unsupported = false;
    int toast_marks = 0;
    std::vector<int> last_slots;
    std::string auto_key;
    std::string saved_id;
    partyui::Reel reel;
    // couch party
    partyui::CouchSetup couch;
    std::map<int, std::string> couch_names;
    bool couch_launching = false;
    ULONGLONG couch_launch_at = 0;
    // QA driver (RETROHUB_AUTOTEST_PARTY)
    int qa_stage = 0;
    ULONGLONG qa_t = 0, qa_next = 0, qa_vote_t = 0;
    std::string qa_logged, qa_why;
} R;

constexpr ULONGLONG kWatchMs = 250;   // the finished-match file is stat'ed about 4 times a second, not every frame

bool in_room_live() { return A.in_room && room_connected(); }

uint32_t random_seed() { return std::random_device{}(); }

/* slot -> name of everyone in the party now: the room's members, or the couch players */
std::map<int, std::string> roster_map() {
    std::map<int, std::string> m;
    if (R.have && R.st.couch) return R.couch_names;
    if (!A.in_room) return m;
    for (auto& mem : room_members()) m[mem.slot] = mem.name;
    return m;
}

std::vector<int> seat_slots() {
    std::vector<int> v;
    for (auto& kv : roster_map()) v.push_back(kv.first);
    return v;
}

uint32_t tiebreak() { return R.st.seed ^ ((uint32_t)R.st.rev * 2654435761u); }

void broadcast() {
    if (!R.st.couch && in_room_live() && !g_fake_room) A.lobby.send_msg(pl::encode_state(R.st));   // (screenshots fake a room: no socket)
}

/* Host side: take the reducer's result and tell the room. Reducers return the State unchanged (same rev) when nothing happened. */
bool commit(const pl::State& ns) {
    if (ns.rev == R.st.rev && ns.phase == R.st.phase && ns.seed == R.st.seed) return false;
    R.st = ns;
    R.have = true;
    broadcast();
    return true;
}

bool couch_mode() { return R.have && R.st.couch; }
bool host_mode() { return couch_mode() || (R.have && in_room_live() && is_host()); }

/* Only this PC's room game counts as the party's: an online room game, or (couch) an offline one. */
bool party_game_running() {
    if (!A.proc) return false;
    return R.st.couch ? !A.run.online : (A.run.room && A.run.online);
}

std::string now_ascii_log(const pl::State& s) {
    std::string out = "party: rev=" + std::to_string(s.rev) + " phase=" + pl::phase_name(s.phase) + " round=" + std::to_string(s.round) + "/" + std::to_string(s.rounds) +
                      " cur=" + s.cur + " poll=" + s.poll + " points=";
    for (const auto& kv : s.points) out += std::to_string(kv.first) + ":" + std::to_string(kv.second) + ",";
    return out;
}

/* ------------------------------------------------------------------ the control channel (OSD) */

pl::AckInfo read_ack() {
    pl::AckInfo ai;
    if (A.run.ctl_path.empty()) return ai;
    std::string txt;
    runctl::Ack a;
    if (runctl::read_text(runctl::ack_path_for(A.run.ctl_path), &txt) && runctl::parse_ack(txt, &a)) { ai.seq = a.seq; ai.ok = a.ok; ai.msg = a.msg; }
    return ai;
}

long long send_osd_msg(const std::string& text) {
    if (A.run.ctl_path.empty() || text.empty()) return 0;
    runctl::Command c;
    c.cmd = runctl::Cmd::OsdMsg;
    c.text = text.substr(0, 118);
    return runctl::send_command_ex(A.run.ctl_path, c);
}

long long send_osd_corner(const std::string& text, int corner, int keep_secs) {
    if (A.run.ctl_path.empty()) return 0;
    runctl::Command c;
    c.cmd = runctl::Cmd::OsdCorner;
    c.slot = corner;
    c.value = keep_secs;
    c.text = text.substr(0, 118);
    return runctl::send_command_ex(A.run.ctl_path, c);
}

/* Start the "round over" sequence for the game running on this PC. */
void start_closer(ULONGLONG now) {
    if (!party_game_running()) return;
    pl::Closer::Plan plan;
    plan.has_ctl = !A.run.ctl_path.empty();
    plan.msg = pl::osd_round_over(R.st);
    plan.corner = pl::osd_standings(R.st);
    plan.fallback = ("Standings: " + plan.corner).substr(0, 118);
    R.closer.start(now, plan);
    R.corner_text.clear();
    R.corner_seq = 0;
    autotest_log("party: closing the game (" + std::string(plan.has_ctl ? "osd + quit" : "window close") + "): " + plan.msg + " | " + plan.corner);
}

void run_closer(ULONGLONG now) {
    if (!R.closer.active()) return;
    if (!party_game_running()) { R.closer.finish(); return; }
    if (now >= R.ack_next) { R.ack = read_ack(); R.ack_next = now + 100; }
    const pl::CloseCmd c = R.closer.step(now, R.ack);
    switch (c.kind) {
        case pl::CloseCmd::Msg: R.closer.sent(send_osd_msg(c.text)); break;
        case pl::CloseCmd::Corner: R.closer.sent(send_osd_corner(c.text, c.corner, c.keep_secs)); break;
        case pl::CloseCmd::Quit: R.closer.sent(runctl::send_command(A.run.ctl_path, runctl::Cmd::Quit, 0)); break;
        case pl::CloseCmd::WindowClose: request_game_close(); autotest_log("party: window close requested (the game ignored Quit)"); break;
        default: break;
    }
}

/* The running round's countdown in the game's corner, and a toast at 5 min / 1 min / 30 s for runners that do not know OsdCorner. */
void run_timer_corner(ULONGLONG now) {
    const pl::State& s = R.st;
    if (s.phase != pl::Phase::Playing || s.round_secs <= 0 || R.closer.active() || !party_game_running() || A.run.ctl_path.empty()) return;
    if (now - R.round_t0 < 5000 || now < R.corner_next) return;   // the runner ignores commands written before it started polling
    const int left = partyui::secs_left();
    if (R.corner_seq) {   // settle the previous corner write first: the file holds one command at a time
        if (now >= R.ack_next) { R.ack = read_ack(); R.ack_next = now + 100; }
        if (R.ack.seq >= R.corner_seq) {
            if (!R.ack.ok) { R.corner_unsupported = true; autotest_log("party: the runner has no OsdCorner (" + R.ack.msg + "): countdown by toast"); }
            R.corner_seq = 0;
        } else if (now - R.corner_sent > 1500) R.corner_seq = 0;   // never acked (a busy runner): carry on
        else return;
    }
    if (R.corner_unsupported) {
        static const int marks[] = {300, 60, 30};
        for (int i = R.toast_marks; i < 3; ++i)
            if (left <= marks[i]) {
                R.toast_marks = i + 1;
                if (left > 0 && s.round_secs > marks[i]) send_osd_msg(pl::osd_timer(left, s.round, s.rounds));
                R.corner_next = now + 700;
                break;
            }
        return;
    }
    const std::string text = pl::osd_timer(left, s.round, s.rounds);
    if (text == R.corner_text) return;
    const long long seq = send_osd_corner(text, 1, 90);
    R.corner_next = now + 1000;
    if (seq) { R.corner_text = text; R.corner_seq = seq; R.corner_sent = now; }
}

/* ------------------------------------------------------------------ finished matches (every PC, any running game with a result file) */

void on_match(const std::string& text, const hubui::results::RoundResult& r) {
    rivals_ingest_text(text);   // rivalries now count EVERY match of a session, not only the last one read at exit
    bracket_on_result(r);
    autotest_log("party: match " + r.id + " source=" + r.source + " local=" + (r.local ? "1" : "0"));
    if (R.have && host_mode() && R.st.phase == pl::Phase::Playing) {
        std::map<int, std::string> names = roster_map();
        commit(pl::on_result(R.st, r, names));
    }
}

void watch_results(ULONGLONG now) {
    const bool live = A.proc && !A.run.result_file.empty();
    if (!live) {
        if (!R.watch.path().empty()) R.watch.reset("");
        R.watch_run = 0;
        return;
    }
    if (R.watch_run != A.run.start_tick || R.watch.path() != A.run.result_file) {   // a new run: reset at launch
        R.watch.reset(A.run.result_file);
        R.watch_run = A.run.start_tick;
        R.watch_next = 0;
    }
    if (now < R.watch_next) return;
    R.watch_next = now + kWatchMs;
    std::string text;
    hubui::results::RoundResult r;
    if (R.watch.poll(&text, &r)) on_match(text, r);
}

/* ------------------------------------------------------------------ the reactions every PC runs when the State moves */

void preselect(const std::string& id) {
    if (id.empty() || R.st.couch || !is_host() || A.selected_game == id) return;
    const hub::GameEntry* g = find_room_entry(id);
    if (!g || is_remote_stand_in(*g)) return;   // the host needs its own copy
    A.selected_game = id;
    A.ready.clear();
    if (!g_fake_room) A.lobby.send_msg("{\"kind\":\"select\",\"game\":\"" + json::escape(id) + "\"}");
}

void save_history(const pl::State& s) {
    const std::string id = pl::night_id(A.code, s.seed, s.couch);
    if (R.saved_id == id) return;
    R.saved_id = id;
    pl::Night n = pl::night_from_state(s, id, wall_ms(), s.couch ? -1 : room_my_slot());
    if (!pl::night_worth_saving(n)) return;
    shot_trace("party-night", id + " rounds=" + std::to_string(n.played) + " winner=" + (n.rows.empty() ? std::string() : n.rows[0].name));
    autotest_log("party: night saved " + id);
    if (g_headless) return;   // screenshots never write the user's data
    pl::save_night(n, hub::data_dir(), hub::saves::vault_dir());
}

void react(ULONGLONG now) {
    const pl::State& s = R.st;
    const pl::State& p = R.seen;
    if (s.rev == p.rev && s.seed == p.seed) return;
    const bool fresh = s.seed != p.seed;
    const pl::Phase from = fresh ? pl::Phase::Off : p.phase;
    const std::string was_poll = fresh ? std::string() : p.poll;
    autotest_log(now_ascii_log(s));

    if (s.phase == pl::Phase::Playing && from != pl::Phase::Playing) {   // a round began on this PC
        R.round_t0 = now;
        R.closer.finish();
        R.corner_text.clear();
        R.corner_seq = 0;
        R.corner_unsupported = false;
        R.toast_marks = 0;
        R.ov = false;
        R.auto_key.clear();
    }
    /* The round is over: close MY game. Also when two updates arrived in one frame (Playing, then Intermission of a later round). */
    const bool round_over = (from == pl::Phase::Playing && (s.phase == pl::Phase::Intermission || s.phase == pl::Phase::Done)) ||
                            (!fresh && from == pl::Phase::Intermission && s.phase == pl::Phase::Intermission && s.round > p.round);
    if (round_over) {
        start_closer(now);
        R.inter_at = now;
        R.ov = true;
    }
    if (s.phase == pl::Phase::Intermission && from != pl::Phase::Intermission && !round_over) R.inter_at = now;
    if (s.phase == pl::Phase::Intermission && s.round == 0 && from == pl::Phase::Lobby) R.ov = false;   // "the night starts" is the room page, not a popup
    if (s.phase == pl::Phase::Done && from != pl::Phase::Done) { save_history(s); R.ov = true; }
    if (s.phase == pl::Phase::Off && from != pl::Phase::Off) { R.ov = false; R.closer.finish(); }
    if (!s.poll.empty() && s.poll != was_poll) { R.my_vote.clear(); R.poll_all_at = 0; R.ov = true; }
    if (s.poll.empty() && !was_poll.empty()) R.my_vote.clear();
    if (!s.tied.empty() && s.tied != p.tied) {   // a tie was settled by the seeded pick: everyone spins the same reel
        R.reel.on = true;
        R.reel.tied = s.tied;
        R.reel.pick = s.pick;
        R.reel.t0 = ImGui::GetTime();
        R.ov = true;
    }
    if (host_mode() && !s.couch) {   // the host keeps the room's game in step with the party's next game
        if (s.phase == pl::Phase::Intermission && s.round < s.rounds) preselect(s.cur);
        else if (!s.pick.empty() && s.pick != p.pick && s.poll.empty()) preselect(s.pick);   // a plain "vote for the next game" in a room without a party
    }
    R.seen = s;
}

/* ------------------------------------------------------------------ the host's duties */

void membership(ULONGLONG now) {
    if (now < R.member_next) return;
    R.member_next = now + 500;
    const std::map<int, std::string> m = roster_map();
    std::vector<int> slots;
    for (auto& kv : m) slots.push_back(kv.first);
    if (slots == R.last_slots) return;
    const bool first = R.last_slots.empty();
    R.last_slots = slots;
    const pl::State ns = pl::reconcile(R.st, m);
    if (ns.rev != R.st.rev) commit(ns);
    else if (!first && R.st.phase != pl::Phase::Off) broadcast();   // someone joined: they get the State (older RetroHubs ignore it)
}

void detect_round_start(ULONGLONG now) {
    (void)now;
    if (R.st.phase != pl::Phase::Intermission || R.st.cur.empty() || !A.proc || A.run.start_tick == R.consumed_tick) return;
    const bool room_game = !R.st.couch && A.run.room && A.run.online;
    const bool couch_game = R.st.couch && R.couch_launching && !A.run.online;
    if (!room_game && !couch_game) return;
    R.consumed_tick = A.run.start_tick;
    R.couch_launching = false;
    commit(pl::next_round(R.st, seat_slots()));
}

void check_timer(ULONGLONG now) {
    if (R.st.phase != pl::Phase::Playing || R.st.round_secs <= 0) return;
    if (now - R.round_t0 >= (ULONGLONG)R.st.round_secs * 1000) commit(pl::on_round_end(R.st, "timer"));
}

void auto_polls(ULONGLONG now) {
    if (R.st.poll.empty() || R.st.couch) { R.poll_all_at = 0; return; }
    if (!pl::all_voted(R.st, seat_slots())) { R.poll_all_at = 0; return; }
    if (!R.poll_all_at) R.poll_all_at = now;
    if (now - R.poll_all_at < 1400) return;   // let people see the tally before it settles
    R.poll_all_at = 0;
    if (R.st.poll == "next") commit(pl::resolve_next(R.st, tiebreak()));
    else {
        const pl::Tally t = pl::tally(R.st);
        if (t.leaders.size() == 1) commit(pl::resolve_winner(R.st, {std::atoi(t.leaders[0].c_str())}, roster_map()));   // a tie waits for the host
    }
}

void maybe_done(ULONGLONG now) {
    if (R.st.phase != pl::Phase::Intermission || R.st.round < 1 || R.st.round < R.st.rounds || !R.st.poll.empty() || A.proc) return;
    if (now - R.inter_at < 1500) return;
    commit(pl::next_round(R.st));
}

void host_tick(ULONGLONG now) {
    if (!R.st.couch) membership(now);
    detect_round_start(now);
    check_timer(now);
    auto_polls(now);
    maybe_done(now);
}

/* ------------------------------------------------------------------ every PC: auto-ready between rounds */

void run_auto_ready() {
    const pl::State& s = R.st;
    if (s.couch || !in_room_live() || is_host() || s.phase != pl::Phase::Intermission || !s.auto_ready || s.cur.empty()) return;
    if (A.proc || A.prev_proc || session_active() || A.selected_game != s.cur || A.my_ready) return;
    const std::string key = std::to_string(s.seed) + ":" + std::to_string(s.round) + ":" + s.cur;
    if (R.auto_key == key) return;   // once per round and game: a player who un-readies stays un-ready
    if (!room_guest_ready_block().empty()) return;   // no game / BIOS / version: they have to sort that out themselves
    R.auto_key = key;
    A.my_ready = true;
    if (!g_fake_room) A.lobby.send_msg("{\"kind\":\"ready\",\"ready\":true}");
    autotest_log("party: auto-ready for round " + std::to_string(s.round + 1) + " " + s.cur);
}

}  // namespace

/* ================================================================== partyui: read */

namespace partyui {

const State& state() { return R.st; }
bool active() { return R.have && (R.st.phase != pl::Phase::Off || !R.st.poll.empty()); }
bool is_host() { return couch_mode() ? true : (in_room_live() && ::is_host()); }
bool couch() { return R.have && R.st.couch; }
int my_slot() { return couch() ? -1 : (A.in_room ? room_my_slot() : -1); }

std::vector<Seat> seats() {
    std::vector<Seat> v;
    if (couch()) {
        for (auto& kv : R.couch_names) { Seat s; s.slot = kv.first; s.name = kv.second; s.ready = true; v.push_back(s); }
        return v;
    }
    if (!A.in_room) return v;
    const std::vector<hubui::PeerInfo> peers = peer_list();
    const int me = room_my_slot();
    for (auto& m : room_members()) {
        Seat s;
        s.slot = m.slot;
        s.name = m.name;
        s.me = m.slot == me;
        s.host = m.slot == 0;
        auto it = A.ready.find(m.slot);
        s.ready = s.host || (s.me ? A.my_ready : (it != A.ready.end() && it->second));
        for (auto& p : peers)
            if (p.slot == m.slot && !s.me && p.hello) s.can_party = pl::peer_can_party(p.app, HUB_VERSION_STR);
        v.push_back(s);
    }
    return v;
}

std::string slot_name(int slot) {
    auto it = R.st.names.find(slot);
    if (it != R.st.names.end() && !it->second.empty()) return it->second;
    auto m = roster_map();
    auto jt = m.find(slot);
    if (jt != m.end() && !jt->second.empty()) return jt->second;
    return "Player " + std::to_string(slot + 1);
}

const hub::GameEntry* entry_of(const std::string& id) {
    if (id.empty()) return nullptr;
    if (couch()) return find_entry(id);
    return find_room_entry(id);
}

std::string title_of(const std::string& id) {
    if (const hub::GameEntry* e = entry_of(id)) return e->title;
    if (const GameInfo* g = hub::find_game(id)) return g->title;
    return id;
}

int secs_left() {
    if (R.st.phase != pl::Phase::Playing || R.st.round_secs <= 0) return -1;
    const long long used = (long long)((GetTickCount64() - R.round_t0) / 1000);
    return (int)std::max<long long>(0, R.st.round_secs - used);
}

std::vector<std::string> need_update() {
    std::vector<std::string> v;
    if (couch()) return v;
    for (const Seat& s : seats()) if (!s.can_party) v.push_back(s.name);
    return v;
}

std::string my_vote() {
    if (R.my_vote.empty() || R.my_vote_round != R.st.round || R.my_vote_what != R.st.poll) return std::string();
    return R.my_vote;
}

bool overlay_wanted() { return R.ov; }
void overlay_close() { R.ov = false; }
void overlay_open() { R.ov = true; }
const Reel& reel() { return R.reel; }
void reel_done() { R.reel.on = false; }

std::vector<std::string> shortlist_for_vote() {
    const std::string first = R.st.phase == pl::Phase::Intermission && !R.st.cur.empty() ? R.st.cur : A.selected_game;
    return room_picker_shortlist(first, 6);
}

bool host_can_start_round(std::string* why) {
    if (couch()) {
        if (A.proc) { if (why) *why = "A game is already running."; return false; }
        if (!entry_of(R.st.cur)) { if (why) *why = "That game is not in this library any more."; return false; }
        return true;
    }
    bool can = false;
    const std::string block = room_host_launch_block(&can);
    if (why) *why = block;
    return can;
}

/* ================================================================== partyui: act */

namespace {
bool room_host_only() { return in_room_live() && ::is_host(); }

std::vector<std::string> pool_now(bool prefer_verified) { return couch() ? couch_pool() : room_picker_pool(prefer_verified); }
}  // namespace

void turn_on() {
    if (!room_host_only() || (R.have && R.st.couch)) return;
    const uint32_t seed = random_seed();
    pl::State s = pl::make_lobby(seed, R.st.rev);
    s = pl::set_playlist(s, pl::shuffled(room_picker_pool(true), seed, s.rounds), true, seed);
    R.have = true;
    R.seen = pl::State();
    R.my_vote.clear();
    R.last_slots.clear();
    commit(s);
    shot_trace("party", "on seed=" + std::to_string(seed));
}

void turn_off() {
    if (!R.have) return;
    if (R.st.couch) {   // the couch party simply ends
        R = Runtime();
        return;
    }
    if (!room_host_only()) return;
    commit(pl::end_party(R.st));
    R.closer.finish();
}

void set_options(int rounds, pl::Scoring scoring, int round_secs, bool auto_ready) {
    if (!host_mode()) return;
    pl::State ns = pl::configure(R.st, rounds, scoring, round_secs, auto_ready);
    if (ns.rounds != R.st.rounds && ns.shuffle) ns = pl::set_playlist(ns, pl::shuffled(pool_now(true), ns.seed, ns.rounds), true, ns.seed);
    if (ns.rounds < (int)ns.list.size() && !ns.shuffle) ns = pl::set_playlist(ns, std::vector<std::string>(ns.list.begin(), ns.list.begin() + ns.rounds), false, ns.seed);
    commit(ns);
}

void set_shuffle(bool on) {
    if (!host_mode() || R.st.phase != pl::Phase::Lobby) return;
    if (on) commit(pl::set_playlist(R.st, pl::shuffled(pool_now(true), R.st.seed, R.st.rounds), true, R.st.seed));
    else commit(pl::set_playlist(R.st, R.st.list, false, R.st.seed));
}

void reshuffle() {
    if (!host_mode() || R.st.phase != pl::Phase::Lobby) return;
    const uint32_t seed = random_seed();
    commit(pl::set_playlist(R.st, pl::shuffled(pool_now(true), seed, R.st.rounds), true, seed));
}

void add_game(const std::string& id) {
    if (!host_mode() || R.st.phase != pl::Phase::Lobby || id.empty()) return;
    std::vector<std::string> l = R.st.shuffle ? std::vector<std::string>() : R.st.list;   // switching from Shuffle to a hand-built list starts it empty
    if ((int)l.size() >= R.st.rounds || (int)l.size() >= pl::kMaxList) return;
    l.push_back(id);
    commit(pl::set_playlist(R.st, l, false, R.st.seed));
}

void remove_game(size_t index) {
    if (!host_mode() || R.st.phase != pl::Phase::Lobby || index >= R.st.list.size()) return;
    std::vector<std::string> l = R.st.list;
    l.erase(l.begin() + (long)index);
    commit(pl::set_playlist(R.st, l, false, R.st.seed));
}

void clear_games() {
    if (!host_mode() || R.st.phase != pl::Phase::Lobby) return;
    commit(pl::set_playlist(R.st, std::vector<std::string>(), false, R.st.seed));
}

void start() {
    if (!host_mode() || R.st.phase != pl::Phase::Lobby) return;
    if (R.st.list.empty()) { set_notice("Pick at least one game for the party first."); return; }
    R.consumed_tick = A.proc ? A.run.start_tick : 0;   // a game already running is not round 1
    R.last_slots.clear();
    commit(pl::start_party(R.st, seat_slots(), roster_map()));
    shot_trace("party", "start rounds=" + std::to_string(R.st.rounds) + " first=" + R.st.cur);
}

void launch_next_round() {
    if (!host_mode() || R.st.phase != pl::Phase::Intermission || R.st.cur.empty()) return;
    std::string why;
    if (!host_can_start_round(&why)) { if (!why.empty()) set_notice(why, 6.0); return; }
    if (couch()) {
        const hub::GameEntry* g = find_entry(R.st.cur);
        if (!g) return;
        hub::GameEntry copy = *g;
        A.sel_game = copy.key;
        R.couch_launching = true;
        R.couch_launch_at = GetTickCount64();
        R.ov = false;
        play_entry(copy);   // may still be waiting (cloud gate / unpack): detect_round_start picks the process up when it appears
        return;
    }
    R.ov = false;
    host_launch_selected();
}

void set_next_game(const std::string& id) {
    if (!host_mode() || R.st.phase != pl::Phase::Intermission) return;
    commit(pl::set_next(R.st, id));
}

void cast_vote(const std::string& pick) {
    if (!R.have || R.st.poll.empty() || pick.empty()) return;
    R.my_vote = pick;
    R.my_vote_what = R.st.poll;
    R.my_vote_round = R.st.round;
    if (host_mode()) commit(pl::on_vote(R.st, std::max(0, my_slot()), R.st.round, R.st.poll, pick));
    else if (in_room_live() && !g_fake_room) A.lobby.send_msg(pl::encode_vote(pl::VoteMsg{R.st.round, R.st.poll, pick}));
    shot_trace("party", "vote " + R.st.poll + " " + pick);
}

void open_next_vote() {
    if (!room_host_only()) return;
    if (R.st.phase == pl::Phase::Playing || R.st.phase == pl::Phase::Done || !R.st.poll.empty()) return;
    const std::vector<std::string> cand = shortlist_for_vote();
    if (cand.size() < 2) { set_notice("There are not enough games everyone can play to hold a vote.", 6.0); return; }
    if (!R.have) {   // a plain vote in a room without Party mode
        R.st = pl::State();
        R.st.seed = random_seed();
        R.have = true;
        R.seen = pl::State();
        R.last_slots.clear();
    }
    commit(pl::open_poll(R.st, "next", cand));
    shot_trace("party", "next-vote open " + std::to_string(cand.size()));
}

void close_vote() {
    if (!host_mode() || R.st.poll.empty()) return;
    if (R.st.poll == "next") commit(pl::resolve_next(R.st, tiebreak()));
    else {
        const pl::Tally t = pl::tally(R.st);
        if (t.leaders.size() == 1) commit(pl::resolve_winner(R.st, {std::atoi(t.leaders[0].c_str())}, roster_map()));
        else if (t.leaders.empty()) commit(pl::resolve_winner(R.st, {}));
    }
}

void decide_winner(const std::vector<int>& slots) {
    if (!host_mode() || R.st.poll != "winner") return;
    commit(pl::resolve_winner(R.st, slots, roster_map()));
}

void skip_round() { decide_winner({}); }

void finish_night() {
    if (!host_mode() || R.st.phase != pl::Phase::Intermission) return;
    commit(pl::next_round(R.st));
}

/* ================================================================== partyui: couch party */

CouchSetup& couch_setup() { return R.couch; }

std::vector<std::string> couch_pool() {
    std::vector<std::string> v;
    for (const hub::GameEntry& e : A.lib) {
        if (!e.issue.empty() && e.issue != "extract") continue;
        if (e.key.empty() || e.key.size() > pl::kMaxId || is_remote_stand_in(e)) continue;
        if (local_players_for(e) < 2) continue;
        v.push_back(e.key);
    }
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    if (v.empty() && g_headless && std::getenv("HUB_SHOT_PARTY")) v = {"mariogolf", "mk64", "mp2", "remix", "smash64"};   // screenshots run without a library
    return v;
}

void couch_begin() {
    const CouchSetup c = R.couch;   // a copy: R is rebuilt below
    const uint32_t seed = random_seed();
    pl::State s = pl::make_lobby(seed, 0);
    s.couch = true;
    s = pl::configure(s, c.rounds, c.scoring, c.round_secs, false);
    std::vector<std::string> list = c.shuffle ? pl::shuffled(couch_pool(), seed, s.rounds) : c.list;
    s = pl::set_playlist(s, list, c.shuffle, seed);
    if (s.list.empty()) { set_notice("Pick at least one game for the couch party first."); return; }
    R = Runtime();
    R.couch = c;
    std::vector<int> players;
    for (int i = 0; i < c.players && i < 4; ++i) {
        R.couch_names[i] = c.names[i].empty() ? "Player " + std::to_string(i + 1) : c.names[i];
        players.push_back(i);
    }
    s = pl::start_party(s, players, R.couch_names);
    R.st = s;
    R.have = true;
    R.couch.open = false;
    R.ov = true;
    shot_trace("party", "couch players=" + std::to_string(players.size()) + " rounds=" + std::to_string(s.rounds));
}

}  // namespace partyui

/* ================================================================== the app.h hooks and the files the lane owns */

void party_on_msg(int from_slot, const hubui::Json& j) {
    const std::string kind = j.str("kind");
    if (R.have && R.st.couch) return;   // a couch party is its own thing
    if (kind == "pt_state") {
        pl::State in;
        if (!pl::decode_state(j, &in)) return;                                   // malformed / newer than this build
        if (!pl::accept_state_from(from_slot, in_room_live() && is_host(), R.st, in)) return;   // only the host speaks, and only a newer rev counts
        R.st = in;
        R.have = true;
    } else if (kind == "pt_vote") {
        pl::VoteMsg v;
        if (!pl::decode_vote(j, &v)) return;
        if (!pl::accept_vote_from(from_slot, in_room_live() && is_host(), seat_slots())) return;
        if (!R.have) return;
        if (v.what == "next" && std::find(R.st.cand.begin(), R.st.cand.end(), v.pick) == R.st.cand.end()) {   // a game from outside the shortlist: everyone must be able to play it
            const hub::GameEntry* g = find_room_entry(v.pick);
            if (!g) return;
            if (!hubui::game_selectable(v.pick, max_players_for(*g), peer_list(), 0, remote_play_entry(*g)).ok) return;
        }
        commit(pl::on_vote(R.st, from_slot, v));
    }
}

void party_on_session_end() {
    R.closer.finish();
    if (!R.have) return;
    /* A.proc is already gone here; A.run still describes the game that ended. While a round is on, the only game this PC runs is the round's. */
    const bool was_round_game = R.st.couch ? !A.run.online : (A.run.room && A.run.online);
    if (host_mode() && R.st.phase == pl::Phase::Playing && was_round_game) commit(pl::on_round_end(R.st, "closed"));
    R.couch_launching = false;
}

void party_tick() {
    const ULONGLONG now = GetTickCount64();
    watch_results(now);
    partyui_qa_drive(now);
    if (!R.have) return;
    if (!R.st.couch && !A.in_room) return;
    react(now);
    if (host_mode()) host_tick(now);
    run_closer(now);
    run_timer_corner(now);
    run_auto_ready();
    if (R.couch_launching && !A.proc && now - R.couch_launch_at > 60000) R.couch_launching = false;   // the launch never happened (cancelled / failed)
}

void party_on_leave_room() {
    if (R.have && R.st.couch) return;   // a couch party does not depend on the room
    const partyui::CouchSetup keep = R.couch;
    R = Runtime();
    R.couch = keep;
}

void party_on_host_change(int promoted_from_slot) {
    if (!R.have || R.st.couch) return;
    R.st = pl::on_host_change(R.st, promoted_from_slot);
    R.seen = pl::on_host_change(R.seen, promoted_from_slot);   // the reactions' baseline follows the renumbering
    R.last_slots.clear();
    R.auto_key.clear();   // every READY was reset with the host change: ready up again for the same round
    R.my_vote.clear();
    if (in_room_live() && is_host()) {   // the new host re-broadcasts what it last received; a guest that saw one update more still accepts it
        R.st = pl::rebroadcast(R.st);
        R.seen.rev = R.st.rev;
        broadcast();
        autotest_log("party: new host re-broadcast rev=" + std::to_string(R.st.rev));
    }
}

void party_on_host_select(const std::string& game_id) {
    if (!R.have || R.st.couch || !is_host()) return;
    if (R.st.phase == pl::Phase::Intermission && R.st.round < R.st.rounds) commit(pl::set_next(R.st, game_id));   // the hand pick becomes the next round's game
}

void party_couch_open() {
    if (R.have && R.st.couch && R.st.phase != pl::Phase::Off) { R.ov = true; return; }
    R.couch.open = true;
    if (R.couch.rounds < 1) R.couch.rounds = 5;
}

bool party_couch_active() { return R.have && R.st.couch && R.st.phase != pl::Phase::Off && R.st.phase != pl::Phase::Done; }

/* launch.cpp (Opus-only) asks for this when it starts a LOCAL Smash game during a couch round, and passes it as the result file
 * (SSB64_RESULT_FILE for the native port, --result-file for n64run). party_tick then reads each finished match while the game runs. */
std::string party_result_file_for_launch() {
    if (!R.have || !R.st.couch || R.st.phase != pl::Phase::Intermission) return std::string();
    const std::string path = hub::data_dir() + "smash_result_local.json";
    std::error_code ec;
    std::filesystem::remove(std::filesystem::u8path(path), ec);
    return path;
}

/* ================================================================== QA driver (RETROHUB_AUTOTEST_PARTY=host|guest) */
/* Presses the same code paths as the party buttons so qa/party_online.ps1 can run a whole night between headless hubs:
 *   host:  RETROHUB_AUTOTEST_PARTY=host  RETROHUB_PARTY_GAMES=<id,id,...> RETROHUB_PARTY_ROUNDS=2 RETROHUB_PARTY_SECS=20 RETROHUB_PARTY_PLAYERS=2
 *   guest: RETROHUB_AUTOTEST_PARTY=guest (votes for the winner of each "who won?" poll: slot = round % players)
 * Every PC logs "party: ..." lines to autotest.log; the script compares the standings across PCs. QA only: needs RETROHUB_AUTOTEST. */
void partyui_qa_drive(unsigned long long now) {
    static const char* mode = std::getenv("RETROHUB_AUTOTEST_PARTY");
    if (!mode || !autotest_active()) return;
    const bool host = std::string(mode) == "host";
    auto env_int = [](const char* k, int def) { const char* v = std::getenv(k); return v && *v ? std::atoi(v) : def; };
    if (R.have && R.st.phase != pl::Phase::Off) {   // log every standing change once (all PCs): the script checks they agree
        const std::string line = now_ascii_log(R.st);
        if (line != R.qa_logged && (R.st.phase == pl::Phase::Intermission || R.st.phase == pl::Phase::Done)) {
            R.qa_logged = line;
            std::string names;
            for (auto& kv : R.st.names) names += std::to_string(kv.first) + "=" + kv.second + ",";
            autotest_log("party-view " + line + " names=" + names);
        }
    }
    if (!in_room_live() || A.scanning) return;
    /* everybody answers a "who won?" poll: the player in seat (round % players) wins, so every PC ends up with the same standings */
    if (R.have && R.st.poll == "winner" && !R.st.cand.empty() && partyui::my_vote().empty() && now >= R.qa_vote_t) {
        R.qa_vote_t = now + 1500;
        partyui::cast_vote(R.st.cand[(size_t)R.st.round % R.st.cand.size()]);
        autotest_log("party: voted in the winner poll (round " + std::to_string(R.st.round) + ")");
    }
    if (!host) return;
    if (R.qa_stage == 0) {
        if (!R.qa_t) R.qa_t = now;
        if ((int)room_members().size() < env_int("RETROHUB_PARTY_PLAYERS", 2) || now - R.qa_t < 3000) return;
        std::vector<std::string> games;
        std::stringstream ss(std::string(std::getenv("RETROHUB_PARTY_GAMES") ? std::getenv("RETROHUB_PARTY_GAMES") : ""));
        std::string tok;
        while (std::getline(ss, tok, ',')) if (!tok.empty()) games.push_back(tok);
        for (auto& g : games) if (!find_room_entry(g)) { autotest_log("error:party game not in the library: " + g); R.qa_stage = 99; return; }
        partyui::turn_on();
        const char* sc = std::getenv("RETROHUB_PARTY_SCORING");
        const pl::Scoring scoring = sc && std::string(sc) == "none" ? pl::Scoring::None : sc && std::string(sc) == "vote" ? pl::Scoring::Vote : pl::Scoring::Auto;
        partyui::set_options(env_int("RETROHUB_PARTY_ROUNDS", 2), scoring, env_int("RETROHUB_PARTY_SECS", 20), true);
        if (!games.empty()) {
            partyui::clear_games();
            for (auto& g : games) partyui::add_game(g);
        }
        autotest_log("party: playlist " + std::to_string(R.st.list.size()) + " games, " + std::to_string(R.st.rounds) + " rounds, timer " + std::to_string(R.st.round_secs) + " s");
        partyui::start();
        R.qa_stage = 1;
        R.qa_next = now + 2500;
        return;
    }
    if (R.qa_stage == 1 && R.st.phase == pl::Phase::Intermission && R.st.round < R.st.rounds && !A.proc && now >= R.qa_next) {
        std::string why;
        if (partyui::host_can_start_round(&why)) {
            autotest_log("party: launching round " + std::to_string(R.st.round + 1) + " (" + R.st.cur + ")");
            partyui::launch_next_round();
            R.qa_next = now + 8000;   // give the launch time before looking again
        } else {
            R.qa_next = now + 1000;
            if (why != R.qa_why) { R.qa_why = why; autotest_log("party: waiting to launch: " + why); }
        }
    }
    if (R.qa_stage == 1 && R.st.phase == pl::Phase::Done) { R.qa_stage = 2; autotest_log("party: night over"); }
}

/* ================================================================== screenshot aid (HUB_SHOT_PARTY) */
/* HUB_SHOT_PARTY=panel|lobbyguest|playing|standings|winner|vote|tie|podium|update|couch|couchround  (+ HUB_SHOT_PARTY_GUEST=1 for the guest's view)
 * Fakes a room of four (Mia, Jake, Kai and "You"/"Ana") and a party State of that mode; no network, nothing written. */
void party_shot_init() {
    const char* m = std::getenv("HUB_SHOT_PARTY");
    if (!m || !*m) return;
    const std::string mode = m;
    const bool guest = std::getenv("HUB_SHOT_PARTY_GUEST") != nullptr || mode == "lobbyguest";
    const bool couch = mode == "couch" || mode == "couchround";
    if (A.st.name.empty()) A.st.name = "Ana";
    std::map<int, std::string> names;
    if (couch) names = {{0, "Ana"}, {1, "Ben"}, {2, "Cleo"}};
    else if (guest) names = {{0, "Mia"}, {1, A.st.name}, {2, "Jake"}, {3, "Kai"}};
    else names = {{0, A.st.name}, {1, "Mia"}, {2, "Jake"}, {3, "Kai"}};
    if (!couch) {
        g_fake_room = true;
        g_fake_other_host = guest;
        A.in_room = true;
        A.code = "K7M2QX";
        A.page = Page::Online;
        if (!std::getenv("HUB_SHOT_ROOM_N")) _putenv_s("HUB_SHOT_ROOM_N", "4");
        for (int s = 1; s < 4; ++s) A.ready[s] = true;   // everyone is ready: the host's Start button is live
        if (guest) A.my_ready = true;
    } else {
        A.page = Page::Online;
    }
    const std::vector<std::string> list = {"mk64", "mp2", "smash64", "mariogolf", "remix"};
    std::vector<int> all;
    for (auto& kv : names) all.push_back(kv.first);
    pl::State s = pl::make_lobby(4242, 6);
    s.couch = couch;
    s = pl::configure(s, 5, pl::Scoring::Auto, 600, true);
    s = pl::set_playlist(s, list, mode == "panel" || mode == "lobbyguest" || couch ? mode != "couchround" && mode != "couch" : false, 4242);
    if (mode != "panel" && mode != "lobbyguest" && mode != "couch" && mode != "update") {
        s = pl::start_party(s, all, names);
        auto play = [&](const char* id, const std::vector<std::vector<int>>& ranks) {
            s = pl::next_round(s, all);
            hubui::results::RoundResult r;
            r.id = id;
            r.game = "smash64";
            r.source = "battleship";
            r.local = couch;
            r.ranks = ranks;
            s = pl::on_result(s, r, names);
        };
        const int n = (int)names.size();
        if (mode == "podium") {
            play("r1", n == 3 ? std::vector<std::vector<int>>{{1}, {0}, {2}} : std::vector<std::vector<int>>{{1}, {0}, {3}, {2}});
            play("r2", n == 3 ? std::vector<std::vector<int>>{{1}, {2}, {0}} : std::vector<std::vector<int>>{{1}, {2}, {0}, {3}});
            play("r3", n == 3 ? std::vector<std::vector<int>>{{0}, {1}, {2}} : std::vector<std::vector<int>>{{0}, {1}, {2}, {3}});
            play("r4", n == 3 ? std::vector<std::vector<int>>{{1}, {0, 2}} : std::vector<std::vector<int>>{{3}, {1}, {0}, {2}});
            play("r5", n == 3 ? std::vector<std::vector<int>>{{0}, {1}, {2}} : std::vector<std::vector<int>>{{1}, {0, 3}, {2}});
            s = pl::next_round(s);
        } else if (mode != "playing" && mode != "couchround") {   // (couchround: the night has just started, round 1 is up next)
            play("r1", n == 3 ? std::vector<std::vector<int>>{{1}, {0}, {2}} : std::vector<std::vector<int>>{{1}, {0}, {3}, {2}});
            play("r2", n == 3 ? std::vector<std::vector<int>>{{0}, {1, 2}} : std::vector<std::vector<int>>{{3}, {1}, {0, 2}});
            if (mode == "winner") {
                s = pl::next_round(s, all);
                s = pl::on_round_end(s, "timer");
                s = pl::on_vote(s, 1, s.round, s.poll, "1");
                s = pl::on_vote(s, 2, s.round, s.poll, "3");
                s = pl::on_vote(s, 3, s.round, s.poll, "1");
            } else if (mode == "vote" || mode == "tie") {
                s = pl::open_poll(s, "next", {"smash64", "mp2", "mk64", "mariogolf"});
                s = pl::on_vote(s, 1, s.round, s.poll, "mp2");
                s = pl::on_vote(s, 2, s.round, s.poll, "mk64");
                if (mode == "vote") s = pl::on_vote(s, 0, s.round, s.poll, "mk64");   // three of four have voted: the poll stays open
                if (mode == "tie") s = pl::resolve_next(s, 4);
            }
        } else if (mode == "playing") {
            s = pl::next_round(s, all);   // playing round 1
        }
    }
    if (mode == "update") {
        s = pl::make_lobby(4242, 6);
        s = pl::set_playlist(s, list, true, 4242);
    }
    R.st = s;
    R.seen = s;
    R.have = true;
    R.round_t0 = GetTickCount64() - 95000;   // a minute and a half into the round
    if (mode == "standings" || mode == "winner" || mode == "vote" || mode == "podium" || mode == "tie" || mode == "couchround") R.ov = true;
    if (mode == "tie") {
        R.reel.on = true;
        R.reel.tied = s.tied.empty() ? std::vector<std::string>{"mp2", "mk64"} : s.tied;
        R.reel.pick = s.pick.empty() ? "mp2" : s.pick;
        R.reel.frozen = 1.7;
    }
    if (couch) {
        R.couch_names = names;
        if (mode == "couch") {
            R.have = false;
            R.couch.open = true;
            R.couch.players = 3;
            R.couch.names[0] = "Ana"; R.couch.names[1] = "Ben"; R.couch.names[2] = "Cleo";
            R.couch.rounds = 5;
        }
    }   // (the room page picks the fake room's game itself: whatever online game the scratch library has, else a fake Smash)
    if (!s.poll.empty()) {   // what this PC voted: the State's own record of its seat
        auto v = s.votes.find(guest ? 1 : 0);
        if (v != s.votes.end()) { R.my_vote = v->second; R.my_vote_what = s.poll; R.my_vote_round = s.round; }
    }
    shot_trace("party-shot", mode + (guest ? " guest" : " host"));
}
