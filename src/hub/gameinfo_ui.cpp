#include "app.h"
#include "gameinfo.h"
#include "gameinfo_net.h"
#include "winutil.h"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <thread>

/* ---------- Game info section of the details modal ----------
 * Facts (developer, publisher, genre, release, players, age rating, Famitsu/Edge scores) come from the bundled libretro-database file
 * (data\gameinfo.tsv) and show instantly. The description and review scores are fetched on demand from Wikidata/Wikipedia by ONE
 * background thread (>= 1 s between requests), cached per game under <data>\gameinfo\, and refreshed rarely; offline the cached copy is
 * used and nothing blocks. Headless runs never touch the network (fixtures only: HUB_GAMEINFO_FIXTURES). Logic: common/gameinfo.cpp. */

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

struct Job { std::string name; Query q; };
struct Result { std::string name; gameinfo::ResolveResult r; Online on; };

struct Worker {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Job> jobs;
    std::deque<Result> results;
    bool started = false;
    std::string cache_dir;
};
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

void worker_main() {
    Worker* w = W();
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> l(w->mu);
            w->cv.wait(l, [&] { return !w->jobs.empty(); });
            j = w->jobs.front();
            w->jobs.pop_front();
        }
        Online on;
        gameinfo::Fetcher f = [](const std::string& u, std::string* b, int* s) { return gameinfo_net::fetch(u, b, s); };
        auto r = gameinfo::resolve_online(j.q, f, &on);
        if (r != gameinfo::ResolveResult::NetworkError) {
            on.fetched = now_s();
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::u8path(w->cache_dir), ec);
            winutil::write_file_atomic(w->cache_dir + j.name, gameinfo::cache_to_json(on));
        }
        std::lock_guard<std::mutex> l(w->mu);
        w->results.push_back({j.name, r, on});
    }
}

void enqueue(const std::string& name, Slot& s) {
    Worker* w = W();
    std::lock_guard<std::mutex> l(w->mu);
    if (!w->started) { w->started = true; std::thread(worker_main).detach(); }
    w->jobs.push_back({name, s.q});
    s.queued = true;
    w->cv.notify_one();
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
        if (!o.page_url.empty()) {
            if (ImGui::SmallButton("Wikipedia (CC BY-SA)")) open_url(o.page_url);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", o.page_url.c_str());
        } else {
            ImGui::TextDisabled("Wikipedia");
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
