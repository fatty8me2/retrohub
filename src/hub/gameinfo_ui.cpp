#include "app.h"
#include "art.h"
#include "gameinfo.h"
#include "gameinfo_queue.h"
#include "gameinfo_net.h"
#include "gametdb.h"
#include "games.h"
#include "winutil.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <optional>
#include <set>
#include <vector>
#include <thread>

/* ---------- Game info section of the details modal ----------
 * Facts (developer, publisher, genre, release, players, age rating, Famitsu/Edge scores) come from the bundled libretro-database file
 * (data\gameinfo.tsv) and show instantly. The description and review scores are fetched on demand from Wikidata/Wikipedia by ONE
 * background thread (>= 1 s between requests), cached per game under <data>\gameinfo\, and refreshed rarely; offline the cached copy is
 * used and nothing blocks. Headless runs never touch the network (fixtures only: HUB_GAMEINFO_FIXTURES). Logic: common/gameinfo.cpp.
 *
 * Every game gets a description without opening anything: gameinfo_prefetch() queues a lookup (urgent ones first = what the cases view shows),
 * and once the library has loaded the rest trickle through the same single thread (>= 1 s between requests, stops while offline for 5 minutes
 * or while "Download cover art" is off). Results are cached on disk exactly like the details window's; gameinfo_generation() ticks whenever
 * one lands. When no description exists anywhere, gameinfo_case_info() returns a factual generated line from the metadata. */

namespace {

using gameinfo::Meta;
using gameinfo::Online;
using gameinfo::Query;

struct Slot {
    Query q;
    Meta meta;
    Online on;
    enum { Loading, Done, Offline } st = Loading;
    double retry_at = 0;   // Offline: ImGui time of the next attempt
    bool queued = false;
    bool expanded = false;
};

struct JobData {
    Query q;
    bool dl = false;   // the details window / a focused card asked: this job may start a one-off GameTDB database download (never the trickle)
};
using JobQueue = gameinfo::PrefetchQueue<JobData>;
struct Result { std::string name; gameinfo::ResolveResult r; Online on; };

struct Worker {
    std::mutex mu;
    std::condition_variable cv;
    JobQueue jobs;                  // urgent first, deduplicated, handled names remembered (prefetch is called every frame)
    std::vector<std::pair<std::string, JobData>> tdb_wait;   // finished without a description because a GameTDB database was still downloading
    std::deque<Result> results;
    bool started = false;
    std::string cache_dir;
    std::chrono::steady_clock::time_point pause_until{};   // background trickle waits here after a network failure
};
std::atomic<uint64_t> g_generation{1};
Worker* W() { static Worker* w = new Worker();   // leaked on purpose: the thread outlives static destruction
              return w; }

std::map<std::string, Slot> g_slots;

int64_t now_s() { return (int64_t)std::time(nullptr); }

std::string cache_dir() {
    if (const char* e = std::getenv("HUB_GAMEINFO_CACHE")) {
        std::string d = e;
        if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
        return d;
    }
    return hub::data_dir() + "gameinfo/";
}

bool network_allowed() { return !g_headless || !gameinfo_net::fixture_dir().empty(); }
bool gametdb_net_fixture() { return !gameinfo_net::fixture_dir().empty(); }

bool bg_allowed(Worker* w) {   // the trickle: online, downloads enabled, not paused
    return hubart::enabled() && std::chrono::steady_clock::now() >= w->pause_until && network_allowed();
}

thread_local bool t_tdb_pending = false;   // set by tdb_find when the answer would have needed a database that is not on disk yet

void tdb_ready(const std::string& system);

/* GameTDB record for a query. Reads only an index that is already on disk; the one thing it may do for a `dl` job is START a background
 * download (once per database, on its own thread), so this worker never waits on a multi-MB download and the trickle never starts one. */
std::optional<gametdb::Rec> tdb_find(const Query& q, bool dl) {
    if (gametdb::db_name(q.system).empty()) return std::nullopt;
    if (gametdb::dir().empty()) gametdb::set_dir(hub::data_dir() + "art/tdb/");
    const gametdb::Index* idx = gametdb::cached_index(q.system);
    if (!idx) {
        if (dl && !gametdb_net_fixture()) {
            const std::string sys = q.system;
            gametdb::request_download(sys, [](const std::string& u, std::string* b, int* st) { return gameinfo_net::http_get(u, b, st); }, [sys] { tdb_ready(sys); });
            t_tdb_pending = true;
        }
        return std::nullopt;
    }
    return gametdb::find_record(*idx, q.system, q.serial, gameinfo::title_variants(q));
}

void worker_main() {
    Worker* w = W();
    for (;;) {
        JobQueue::Entry j;
        {
            std::unique_lock<std::mutex> l(w->mu);
            for (;;) {   // urgent jobs sit in front; a background job only runs while bg_allowed()
                if (!w->jobs.empty() && (w->jobs.front_urgent() || bg_allowed(w))) break;
                w->cv.wait_for(l, std::chrono::seconds(1));
            }
            j = w->jobs.pop();
        }
        Online on;
        std::string txt;
        Online cached;
        bool fresh = winutil::read_file(w->cache_dir + j.name, &txt) && gameinfo::cache_from_json(txt, &cached) && !gameinfo::cache_stale(cached, now_s());
        if (fresh) continue;   // already on disk and recent: nothing to do
        gameinfo::Fetcher f = [](const std::string& u, std::string* b, int* s) { return gameinfo_net::fetch(u, b, s); };
        t_tdb_pending = false;
        const bool dl = j.payload.dl;
        auto r = gameinfo::resolve_description(j.payload.q, f, [dl](const Query& q) { return tdb_find(q, dl); }, &on);
        const bool waiting_db = t_tdb_pending && on.description.empty();
        if (r != gameinfo::ResolveResult::NetworkError) {
            on.fetched = now_s();
            on.rev = waiting_db ? 0 : gameinfo::kCacheRev;   // rev 0 = looked up again once the database is there (cache_stale)
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::u8path(w->cache_dir), ec);
            winutil::write_file_atomic(w->cache_dir + j.name, gameinfo::cache_to_json(on));
            g_generation.fetch_add(1);
        } else if (!j.urgent) {   // offline: leave the trickle alone for 5 minutes and keep the job
            std::lock_guard<std::mutex> l(w->mu);
            w->pause_until = std::chrono::steady_clock::now() + std::chrono::minutes(5);
            w->jobs.requeue(j);
            continue;
        } else {
            std::lock_guard<std::mutex> l(w->mu);
            w->jobs.forget(j.name);   // the details window asks again after its own back-off
        }
        std::lock_guard<std::mutex> l(w->mu);
        if (waiting_db) w->tdb_wait.emplace_back(j.name, j.payload);
        if (j.urgent) w->results.push_back({j.name, r, on});   // only requested-by-the-UI jobs have a slot waiting; background results are on disk and nothing else
    }
}

/* A GameTDB database finished downloading: the games that were waiting for it are looked up again (their cached rev-0 entry is stale). */
void tdb_ready(const std::string& system) {
    Worker* w = W();
    std::lock_guard<std::mutex> l(w->mu);
    for (size_t i = 0; i < w->tdb_wait.size();) {
        if (w->tdb_wait[i].second.q.system == system) {
            JobData d = w->tdb_wait[i].second;
            d.dl = false;
            w->jobs.forget(w->tdb_wait[i].first);
            w->jobs.add(w->tdb_wait[i].first, d, true);
            w->tdb_wait.erase(w->tdb_wait.begin() + (long)i);
        } else ++i;
    }
    g_generation.fetch_add(1);
    w->cv.notify_one();
}

void enqueue(const std::string& name, const Query& q, bool urgent, bool dl = false) {
    Worker* w = W();
    std::lock_guard<std::mutex> l(w->mu);
    if (w->cache_dir.empty()) w->cache_dir = cache_dir();
    if (!w->started) { w->started = true; std::thread(worker_main).detach(); }
    if (w->jobs.add(name, JobData{q, dl}, urgent)) w->cv.notify_one();
}

void enqueue(const std::string& name, Slot& s) {
    enqueue(name, s.q, true, true);   // the details window / a focused card: a missing GameTDB synopsis may start its (single) background download
    s.queued = true;
}

void drain_results() {
    Worker* w = W();
    std::deque<Result> got;
    {
        std::lock_guard<std::mutex> l(w->mu);
        got.swap(w->results);
    }
    for (auto& r : got) {
        auto it = g_slots.find(r.name);
        if (it == g_slots.end()) continue;
        Slot& s = it->second;
        s.queued = false;
        if (r.r == gameinfo::ResolveResult::NetworkError) {
            s.st = s.on.status == Online::Unknown ? Slot::Offline : Slot::Done;   // keep an older cached copy on screen
            s.retry_at = ImGui::GetTime() + 300.0;
        } else {
            s.on = r.on;
            s.st = Slot::Done;
        }
    }
}

Slot& slot_for(const Query& q, const std::string& name) {
    auto it = g_slots.find(name);
    if (it != g_slots.end()) return it->second;
    Slot& s = g_slots[name];
    s.q = q;
    s.meta = gameinfo::shared_db(hub::exe_dir() + "data/").lookup(q);
    W()->cache_dir = cache_dir();
    std::string txt;
    Online cached;
    bool have = winutil::read_file(W()->cache_dir + name, &txt) && gameinfo::cache_from_json(txt, &cached);
    if (have) { s.on = cached; s.st = Slot::Done; }
    if (!have || gameinfo::cache_stale(cached, now_s())) {
        if (network_allowed()) enqueue(name, s);
        else if (!have) s.st = Slot::Offline;
    }
    return s;
}

ImU32 score_color(const std::string& label, const std::string& value) {
    double num = std::atof(value.c_str()), den = 100;
    auto sl = value.find('/');
    if (sl != std::string::npos) den = std::atof(value.c_str() + sl + 1);
    else if (label == "Edge") den = 10;
    else if (label == "Famitsu") den = 40;
    double f = den > 0 ? num / den : 0;
    if (f >= 0.75) return IM_COL32(36, 128, 82, 255);
    if (f >= 0.5) return IM_COL32(160, 120, 30, 255);
    return IM_COL32(160, 60, 60, 255);
}

void open_url(const std::string& url) {
    if (url.rfind("https://", 0) != 0) return;
    ShellExecuteW(nullptr, L"open", std::filesystem::u8path(url).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

/* "Label  value" cell, value cut to the column width. */
void fact(const char* label, const std::string& value, float col_w) {
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine(0, sp(0.75f));
    std::string v = ui::ellipsize(ImGui::GetFont(), ImGui::GetFontSize(), value, col_w - ImGui::CalcTextSize(label).x - sp(1.5f));
    ImGui::TextUnformatted(v.c_str());
    ImGui::EndGroup();
}

}  // namespace

/* Draws the info block for any game (library entry or a friend's game). `id` only disambiguates widget ids. */
bool gameinfo_ui_section_query(const Query& q, const std::string& id, int part) {   // part: 0 all, 1 description only, 2 facts + ratings only
    bool drew_desc = false;
    drain_results();
    {   /* A tall modal opens scrolled to its default-focused Play button (at the bottom); keep the info block in view for the first frames. */
        static double keep_until = 0;
        if (ImGui::IsWindowAppearing()) keep_until = ImGui::GetTime() + 3.0;
        if (ImGui::GetIO().MouseWheel != 0.f) keep_until = 0;   // the player scrolled or moved the focus: hands off
        for (ImGuiKey k : {ImGuiKey_DownArrow, ImGuiKey_UpArrow, ImGuiKey_Tab, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadUp})
            if (ImGui::IsKeyPressed(k, false)) keep_until = 0;
        if (ImGui::GetTime() < keep_until) ImGui::SetScrollY(0.f);
    }
    const std::string name = gameinfo::cache_name(q);
    Slot& s = slot_for(q, name);
    if (s.st == Slot::Offline && ImGui::GetTime() >= s.retry_at && !s.queued && network_allowed()) { s.st = Slot::Loading; enqueue(name, s); }
    const Meta& m = s.meta;
    const Online& o = s.on;
    ImGui::PushID(id.c_str());
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, sp(0.5f)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float avail = ImGui::GetContentRegionAvail().x;

    /* facts: offline data first, Wikidata fills the gaps */
    struct F { const char* label; std::string value; };
    std::vector<F> facts;
    auto add = [&](const char* l, const std::string& v) { if (!v.empty()) facts.push_back({l, v}); };
    add("Developer", !m.developer.empty() ? m.developer : o.developer);
    add("Publisher", !m.publisher.empty() ? m.publisher : o.publisher);
    add("Genre", !m.genre.empty() ? m.genre : o.genre);
    add("Released", gameinfo::release_text(m, o));
    add("Players", gameinfo::players_text(m.players));
    const float col_w = avail * 0.5f;
    if (part == 1) facts.clear();
    for (size_t i = 0; i < facts.size(); ++i) {
        if (avail > sp(50.f)) {
            if (i % 2 == 1) ImGui::SameLine(col_w);
            fact(facts[i].label, facts[i].value, col_w - sp(1.f));
        } else {
            fact(facts[i].label, facts[i].value, avail);
        }
    }

    /* badges: age rating + review scores */
    std::string age = gameinfo::age_badge(m, o);
    auto chips = gameinfo::rating_chips(m, o);
    if (part == 1) { age.clear(); chips.clear(); }
    if (!age.empty() || !chips.empty()) {
        ImGui::Dummy(ImVec2(0, sp(0.75f)));
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float x = 0, rowh = 0;
        ImFont* cf = ui::F.cap ? ui::F.cap : ImGui::GetFont();
        auto badge = [&](const std::string& text, ImU32 bg) {
            float w = cf->CalcTextSizeA(cf->FontSize, FLT_MAX, 0.f, text.c_str()).x + sp(2.f);
            if (x > 0 && x + w > avail) { x = 0; p0.y += rowh + sp(0.5f); }
            float pw = ui::pill_at(dl, ImVec2(p0.x + x, p0.y), text.c_str(), bg);
            rowh = cf->FontSize + sp(1.f);
            x += pw + sp(0.75f);
        };
        if (!age.empty()) badge(age, ui::PANEL3);
        for (const auto& c : chips) badge(c.label + " " + c.value, score_color(c.label, c.value));
        ImGui::Dummy(ImVec2(avail, p0.y + rowh - ImGui::GetCursorScreenPos().y));
    }

    /* description */
    if (part == 2) {}
    else if (s.st == Slot::Loading) {
        drew_desc = true;
        ImGui::Dummy(ImVec2(0, sp(0.75f)));
        for (int i = 0; i < 3; ++i) {
            ImVec2 a = ImGui::GetCursorScreenPos();
            float w = avail * (i == 2 ? 0.55f : 1.f);
            ui::shimmer(dl, a, ImVec2(a.x + w, a.y + sp(1.25f)), sp(0.5f));
            ImGui::Dummy(ImVec2(w, sp(1.25f) + sp(0.75f)));
        }
    } else if (!o.description.empty()) {
        drew_desc = true;
        ImGui::Dummy(ImVec2(0, sp(0.75f)));
        ImFont* f = ImGui::GetFont();
        float fs = ImGui::GetFontSize();
        auto all = ui::wrap_lines(f, fs, o.description, avail, 60);
        bool more = all.size() > 3;
        auto lines = s.expanded || !more ? all : ui::wrap_lines(f, fs, o.description, avail, 3);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
        for (const auto& ln : lines) ImGui::TextUnformatted(ln.c_str());
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        if (more && ui::button(s.expanded ? "Less" : "More", ImVec2(sp(8.f), sp(3.5f)))) s.expanded = !s.expanded;
        if (more) ImGui::SameLine(0, sp(1.f));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("From");
        ImGui::SameLine(0, sp(0.5f));
        const bool tdb_src = o.source == "GameTDB";
        if (!o.page_url.empty()) {
            if (ImGui::SmallButton(tdb_src ? "GameTDB" : "Wikipedia (CC BY-SA)")) open_url(o.page_url);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", o.page_url.c_str());
        } else {
            ImGui::TextDisabled(tdb_src ? "GameTDB" : "Wikipedia");
        }
    } else if (s.st == Slot::Offline && part != 1) {
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        text_wrapped_disabled("A description and review scores load when you are online.");
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
    return drew_desc;
}

bool gameinfo_ui_section(const hub::GameEntry& e, const std::string& catalog_title, int part) {
    if (e.catalog_id == "remix" || e.catalog_id == "kanto") return false;   // hacks: nothing reliable to match
    Query q;
    q.system = hub::system_short(e.system);
    q.title = e.title;
    q.serial = e.serial;
    q.alt_title = e.catalog_id == "smash64" ? "Super Smash Bros." : catalog_title;
    if (q.system.empty()) return false;
    bool d = gameinfo_ui_section_query(q, e.key, part);
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    return d;
}

namespace {

bool is_mod_entry(const hub::GameEntry& e) { return e.catalog_id == "remix" || e.catalog_id == "kanto" || !e.hack_patch.empty(); }

std::string hack_base_title(const hub::GameEntry& e) {
    if (e.catalog_id == "remix") return "Super Smash Bros.";
    if (!e.hack_base.empty())
        for (const auto& g : A.lib)
            if (g.key == e.hack_base) return g.title;
    return "*";
}

bool query_for(const hub::GameEntry& e, const std::string& catalog_title, Query* q) {
    q->system = hub::system_short(e.system);
    q->title = e.title;
    q->serial = e.serial;
    q->alt_title = e.catalog_id == "smash64" ? "Super Smash Bros." : catalog_title;
    return !q->system.empty();
}

void bg_maybe_start() {   // once the library has loaded (or changed): queue every game, slowly, behind whatever the player is looking at
    static bool busy = false;
    static size_t last_n = 0;
    static double next_check = 0;
    if (busy || g_headless || !ImGui::GetCurrentContext()) return;
    double t = ImGui::GetTime();
    if (t < next_check) return;
    next_check = t + 2.0;
    if (A.lib.empty() || A.lib.size() == last_n || !hubart::enabled() || !network_allowed()) return;
    last_n = A.lib.size();
    busy = true;
    for (const hub::GameEntry& e : A.lib) gameinfo_prefetch(e, false);
    busy = false;
}

}  // namespace

void gameinfo_case_info(const hub::GameEntry& e, const std::string& catalog_title, bool with_description, CaseInfo* out) {
    if (!out) return;
    bg_maybe_start();
    Query q;
    if (!query_for(e, catalog_title, &q)) return;
    if (is_mod_entry(e)) {   // a mod / ROM hack: nothing reliable to match, so the line says what it is and nothing else
        if (with_description) {
            out->desc = gameinfo::generated_description(q, Meta(), Online(), e.nx2, hack_base_title(e));
            out->desc_generated = true;
        }
        return;
    }
    if (!with_description) {   // the bundled database only (plus a cached online copy when this session already has one): no slot, no network
        const Meta m = gameinfo::shared_db(hub::exe_dir() + "data/").lookup(q);
        out->genre = m.genre; out->maker = m.developer; out->year = m.year; out->players = m.players;
        out->publisher = m.publisher;
        out->age = gameinfo::age_badge(m, Online());
        auto it = g_slots.find(gameinfo::cache_name(q));
        if (it != g_slots.end()) {
            const Online& o = it->second.on;
            if (out->genre.empty()) out->genre = o.genre;
            if (out->maker.empty()) out->maker = o.developer;
            if (out->publisher.empty()) out->publisher = o.publisher;
            if (!out->year) out->year = o.year;
            out->age = gameinfo::age_badge(m, o);
        }
        return;
    }
    drain_results();
    Slot& s = slot_for(q, gameinfo::cache_name(q));
    const Meta& m = s.meta;
    out->genre = !m.genre.empty() ? m.genre : s.on.genre;
    out->maker = !m.developer.empty() ? m.developer : s.on.developer;
    out->publisher = !m.publisher.empty() ? m.publisher : s.on.publisher;
    out->age = gameinfo::age_badge(m, s.on);
    out->year = m.year ? m.year : s.on.year;
    out->players = m.players;
    out->desc = s.on.description;
    if (out->desc.empty() && s.st != Slot::Loading) {   // looked up and nothing found (or offline): say what we know, never invent plot
        out->desc = gameinfo::generated_description(q, m, s.on, e.nx2);
        out->desc_generated = true;
    }
}

void gameinfo_prefetch(const hub::GameEntry& e, bool urgent) {
    Query q;
    if (!query_for(e, "", &q) || is_mod_entry(e)) return;
    if (const hub::GameInfo* cat = hub::find_game(e.catalog_id)) if (e.catalog_id != "smash64") q.alt_title = cat->title;
    if (e.catalog_id == "smash64") q.alt_title = "Super Smash Bros.";
    if (!network_allowed()) return;
    if (ImGui::GetCurrentContext()) drain_results();
    if (!urgent && !hubart::enabled()) return;
    if (!urgent) bg_maybe_start();
    enqueue(gameinfo::cache_name(q), q, urgent);
}

uint64_t gameinfo_generation() {
    bg_maybe_start();
    if (ImGui::GetCurrentContext()) drain_results();   // never lets finished-job results pile up when no card draws a description
    return g_generation.load();
}

/* Screenshot aid: HUB_SHOT_INFO="n64;Super Smash Bros.;NALE|gba;Pokemon Emerald;BPEE" replaces the fake library with these games
 * (keys fake:info0, fake:info1, ...; select one with HUB_SELECT_GAME and open its details with HUB_SHOT_KEYS=i). */
void gameinfo_ui_shot_lib() {
    const char* spec = std::getenv("HUB_SHOT_INFO");
    if (!spec || !g_headless) return;
    A.lib.clear();
    std::string all = spec;
    size_t pos = 0;
    int idx = 0;
    while (pos <= all.size()) {
        size_t bar = all.find('|', pos);
        std::string one = all.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos);
        std::string part[3];
        size_t s = 0;
        for (int i = 0; i < 3; ++i) {
            size_t sc = one.find(';', s);
            part[i] = one.substr(s, sc == std::string::npos ? std::string::npos : sc - s);
            if (sc == std::string::npos) break;
            s = sc + 1;
        }
        hub::GameEntry g;
        g.system = hub::system_from_short(part[0]);
        g.title = part[1];
        g.serial = part[2];
        g.key = "fake:info" + std::to_string(idx++);
        g.path = "C:/fake/info.rom";
        A.lib.push_back(g);
        if (bar == std::string::npos) break;
        pos = bar + 1;
    }
}
