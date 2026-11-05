/* Plan 4 Wave 3 R1 "Resume more" (see resume_more.h, resume_logic.h, docs/SAVES.md). */
#include "app.h"
#include "wave3_hooks.h"
#include "resume_logic.h"
#include "resume_more.h"
#include <atomic>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

namespace {

namespace fs = std::filesystem;
using namespace hubui;

/* ---------- files ---------- */
long long file_time(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(W(path).c_str(), GetFileExInfoStandard, &fa)) return 0;
    ULARGE_INTEGER u;
    u.LowPart = fa.ftLastWriteTime.dwLowDateTime;
    u.HighPart = fa.ftLastWriteTime.dwHighDateTime;
    return (long long)(u.QuadPart / 10000000ULL) - 11644473600LL;
}

std::string no_sep(std::string s) {
    while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    return s;
}

long long newest_battery(const std::string& dir) {
    long long best = 0;
    std::error_code ec;
    for (fs::directory_iterator it(fs::u8path(dir), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        std::string ext = it->path().extension().u8string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext == ".bak" || ext == ".tmp") continue;   // kept copies, half writes: not the live save
        best = std::max(best, file_time(it->path().u8string()));
    }
    return best;
}

bool read_all_text(const std::string& p, std::string* out) {
    std::ifstream f(fs::u8path(p), std::ios::binary);
    if (!f) return false;
    out->assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

/* ---------- this PC ---------- */
std::string pc_name() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 2] = {};
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(buf, &n)) return "this PC";
    return U(std::wstring(buf, n));
}

/* A random id kept in the data folder: it names the PC that wrote a resume point. Never synced, never leaves the PC. */
std::string pc_id() {
    static std::string id;
    if (!id.empty()) return id;
    const std::string path = hub::data_dir() + "pc_id.txt";
    std::string t;
    if (read_all_text(path, &t)) {
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
        if (t.size() >= 8 && t.size() <= 64) return id = t;
    }
    std::random_device rd;
    char b[40];
    std::snprintf(b, sizeof b, "%08x%08x", (unsigned)rd(), (unsigned)(rd() ^ GetTickCount()));
    id = b;
    std::ofstream f(fs::u8path(path), std::ios::binary | std::ios::trunc);
    if (f) f << id;
    return id;
}

/* ---------- which systems, what the sidecar says ---------- */
bool resumes(const std::string& sys) {
    return sys == "n64" || sys == "gb" || sys == "gbc" || sys == "gba" || sys == "nes" || sys == "snes";
}

std::string sidecar_path(const std::string& states) { return states + "\\auto.resume.json"; }

struct Facts {
    std::string sys, key, battery, states;
    ResumeInput in;
};

Facts facts_for(const hub::GameEntry& e) {
    Facts f;
    f.sys = sys_short(e);
    f.key = e.key;
    f.battery = no_sep(hub::saves::battery_dir(f.sys, e.key));
    f.states = no_sep(hub::saves::states_dir(f.sys, e.key));
    f.in.game_key = e.key;
    f.in.this_pc = pc_id();
    f.in.state_time = file_time(runctl::state_file(f.states, runctl::kAutoSlot));
    f.in.newest_battery_time = newest_battery(f.battery);
    std::string t;
    if (read_all_text(sidecar_path(f.states), &t)) f.in.sidecar = parse_resume_sidecar(t);
    return f;
}

/* ---------- the player's answer, remembered for as long as the newest save stays the same ---------- */
struct Answer { ResumeChoice c = ResumeChoice::Ask; long long battery_time = 0; };
std::map<std::string, Answer> g_answers;
bool g_answers_loaded = false;
std::string answers_path() { return hub::data_dir() + "resume_choice.txt"; }

void load_answers() {
    if (g_answers_loaded) return;
    g_answers_loaded = true;
    std::string t;
    if (!read_all_text(answers_path(), &t)) return;
    size_t p = 0;
    while (p < t.size()) {
        size_t e = t.find('\n', p);
        if (e == std::string::npos) e = t.size();
        std::string line = t.substr(p, e - p);
        p = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b == std::string::npos) continue;
        Answer an;
        an.c = line.substr(a + 1, b - a - 1) == "this" ? ResumeChoice::ThisPc : ResumeChoice::Newest;
        an.battery_time = std::atoll(line.c_str() + b + 1);
        g_answers[line.substr(0, a)] = an;
    }
}

void save_answers() {
    std::string t;
    for (auto& kv : g_answers) t += kv.first + "\t" + (kv.second.c == ResumeChoice::ThisPc ? "this" : "newest") + "\t" + std::to_string(kv.second.battery_time) + "\n";
    std::ofstream f(fs::u8path(answers_path()), std::ios::binary | std::ios::trunc);
    if (f) f << t;
}

ResumeChoice answer_for(const std::string& key, long long battery_time) {
    load_answers();
    auto it = g_answers.find(key);
    if (it == g_answers.end() || it->second.battery_time != battery_time) return ResumeChoice::Ask;   // a newer save since: ask again
    return it->second.c;
}

void set_answer(const std::string& key, long long battery_time, ResumeChoice c) {
    load_answers();
    g_answers[key] = {c, battery_time};
    save_answers();
}

/* ---------- "from <PC>": the cloud's device name of the newest save, fetched off the frame thread ---------- */
std::mutex g_from_mu;
std::map<std::string, std::string> g_from;   // game key -> device name ("" = asked, unknown)
std::map<std::string, bool> g_from_asked;

void ask_from_pc(const std::string& sys, const std::string& key) {
    {
        std::lock_guard<std::mutex> lk(g_from_mu);
        if (g_from_asked[key]) return;
        g_from_asked[key] = true;
    }
    if (!A.st.cloud_enabled || g_headless) return;
    std::thread([sys, key]() {
        std::string name;
        long long best = -1;
        for (auto& f : hub::saves::cloud_versions({sys, key}))
            if (!f.second.empty()) {
                const auto& v = f.second[0];
                long long ts = v.ts ? v.ts : v.mtime;
                if (ts > best && !v.device_name.empty()) { best = ts; name = v.device_name; }
            }
        std::lock_guard<std::mutex> lk(g_from_mu);
        g_from[key] = name;
    }).detach();
}

std::string from_pc(const std::string& key) {
    if (g_headless && std::getenv("HUB_SHOT_RESUME")) return "LIVING-ROOM-PC";
    std::lock_guard<std::mutex> lk(g_from_mu);
    auto it = g_from.find(key);
    return it == g_from.end() ? std::string() : it->second;
}

std::string newest_label(const std::string& key) {
    std::string n = from_pc(key);
    return n.empty() ? "Start from your newest save" : "Start from your newest save (from " + n + ")";
}

/* ---------- the Play choice ---------- */
struct Ask {
    bool open = false, seen = false;
    std::string key, title, sys;
    long long battery_time = 0;
} G;

void finish_choice(ResumeChoice c) {
    set_answer(G.key, G.battery_time, c);
    G.open = false;
    if (const hub::GameEntry* g = find_entry(G.key)) { hub::GameEntry copy = *g; play_entry(copy); }   // the gate now passes
}

}  // namespace

std::string resume_launch_arg(const hub::GameEntry& e, const std::string&, const std::string&, bool blocked) {
    Facts f = facts_for(e);
    if (!resumes(f.sys)) return "";
    f.in.real_boot = blocked;
    const ResumeChoice c = answer_for(f.key, f.in.newest_battery_time);
    switch (decide_resume(f.in, c)) {
        case ResumeResult::Resume:
            if (newer_save_exists(f.in)) {   // the player chose the older resume point: keep the newer save first, never lose it
                hub::saves::history_snapshot_game(f.sys, f.key);
                set_toast("Kept a copy of your newest save in the save history, then continued from this PC's resume point.");
            }
            return " --resume";
        case ResumeResult::NewerSave:
            if (c == ResumeChoice::Ask)   // not asked (e.g. an automatic launch): the newest save wins, as before
                set_toast("Your save is newer than your resume point (it came from another PC or a later session), so the game starts from your save. "
                          "The resume point is still on the game's page.");
            return "";
        default: return "";
    }
}

bool resume_choice_gate(const hub::GameEntry& e, bool blocked) {
    if (A.proc || game_active()) return false;   // never over a running game
    if (A.fresh_key == e.key) return false;      // "Start fresh": no resume to ask about
    Facts f = facts_for(e);
    if (!resumes(f.sys)) return false;
    f.in.real_boot = blocked;
    if (!resume_needs_choice(f.in, answer_for(f.key, f.in.newest_battery_time))) return false;
    G = Ask();
    G.open = true;
    G.key = f.key;
    G.title = e.title;
    G.sys = f.sys;
    G.battery_time = f.in.newest_battery_time;
    ask_from_pc(f.sys, f.key);
    return true;
}

void resume_note_exit(const std::string& system_short, const std::string& key, bool online, unsigned long long start_unix) {
    if (online || !resumes(system_short) || key.empty()) return;
    const std::string states = no_sep(hub::saves::states_dir(system_short, key));
    const long long st = file_time(runctl::state_file(states, runctl::kAutoSlot));
    if (st <= 0 || st + 2 < (long long)start_unix) return;   // no state written this session: the old sidecar still describes it
    ResumeSidecar s;
    s.pc = pc_id();
    s.pc_name = pc_name();
    s.game_key = key;
    s.time = st;
    s.battery_time = newest_battery(no_sep(hub::saves::battery_dir(system_short, key)));
    runctl::write_text_atomic(sidecar_path(states), make_resume_sidecar(s));
}

void resume_choice_draw() {
    if (!G.open) return;
    if (A.proc || game_active()) { G.open = false; return; }   // a game started meanwhile: drop it silently
    bool open = false;
    emu_modal_begin("Which save?", &open);
    if (!open) return;
    if (ui::modal_back()) G.open = false;
    ImGui::TextWrapped("%s", G.title.empty() ? "This game" : G.title.c_str());
    ImGui::Dummy(ImVec2(0, ui::sp(0.5f)));
    text_wrapped_disabled("Your newest save is newer than this PC's resume point. Which one do you want?");
    ImGui::Dummy(ImVec2(0, ui::sp(1.5f)));
    const ImVec2 bs(ImGui::GetContentRegionAvail().x, ui::sp(4.f));
    if (!G.seen) { ImGui::SetKeyboardFocusHere(0); G.seen = true; }   // a pad lands on the default
    if (button(newest_label(G.key).c_str(), bs, true, true)) finish_choice(ResumeChoice::Newest);
    if (button("Continue from this PC's resume point (older)", bs, false, true)) finish_choice(ResumeChoice::ThisPc);
    text_wrapped_disabled("Either way your newest save is kept in the save history.");
    ImGui::Dummy(ImVec2(0, ui::sp(0.5f)));
    if (button("Cancel", ImVec2(ui::sp(14.f), ui::sp(3.5f)))) G.open = false;
    if (!G.open) { ImGui::CloseCurrentPopup(); G.seen = false; }
    ImGui::EndPopup();
}

void resume_choice_shot() {
    G = Ask();
    G.open = true;
    G.key = "fake:shot";
    G.title = "Test Game 3";
    G.sys = "gba";
}

/* Details modal, Saves tab: shown only when another PC's save is newer than this PC's resume point. */
void resume_details_section(const hub::GameEntry& e) {
    static std::string cached_key;
    static double cached_at = -10;
    static Facts f;
    if (cached_key != e.key || ImGui::GetTime() - cached_at > 1.5) {
        cached_key = e.key;
        cached_at = ImGui::GetTime();
        f = facts_for(e);
    }
    const bool shot = g_headless && std::getenv("HUB_SHOT_RESUME");
    if (!resumes(f.sys) && !shot) return;
    if (!shot && !newer_save_exists(f.in)) return;
    ask_from_pc(f.sys, f.key);
    ImGui::Dummy(ImVec2(0, ui::sp(0.5f)));
    ImGui::TextUnformatted("Which save to start from");
    text_wrapped_disabled("Your newest save is newer than this PC's resume point. This is used the next time you press Play.");
    ResumeChoice cur = answer_for(f.key, f.in.newest_battery_time);
    if (shot && cur == ResumeChoice::Ask) cur = ResumeChoice::Newest;
    if (cur == ResumeChoice::Ask) cur = ResumeChoice::Newest;   // the default
    const ImVec2 bs(0, 0);
    if (button(newest_label(f.key).c_str(), bs, cur == ResumeChoice::Newest, true)) set_answer(f.key, f.in.newest_battery_time, ResumeChoice::Newest);
    ImGui::SameLine();
    if (button("Continue from this PC's resume point (older)", bs, cur == ResumeChoice::ThisPc, true)) set_answer(f.key, f.in.newest_battery_time, ResumeChoice::ThisPc);
    ImGui::Dummy(ImVec2(0, ui::sp(0.5f)));
}
