#include "hookshot_ui.h"
#include "hookshot.h"
#include "hookshot_build.h"
#include "app.h"
#include "winutil.h"

/* The native-version card (game page > About). Contract: hookshot_build.h (the builds) and hookshot.h (what is built, and decide()).
 * The card starts with "Which version plays" (always: what Play starts now, and one button per other runner), then one section per native
 * flavor (Hookshot, Longshot). Each section has four looks: not built (what it is, what is missing, Build it), building (stage, progress,
 * Cancel), ready (built date; Rebuild) and failed (the reason, Try again, Open the log). The build tools ("Remove build tools") are shared, so
 * that button sits once at the bottom. Only ONE build runs at a time: the other flavor's build buttons are off meanwhile.
 * All of it is ordinary ImGui widgets, so the pad and the keyboard reach every button. Nothing here opens a system window except what the
 * player clicks (the download page, Notepad). */

namespace fs = std::filesystem;

namespace {

namespace hs = hookshot;

enum class Mode { NotBuilt, Building, Ready, Failed };

struct Fake {
    bool on = false;
    std::string mode;
};

struct FV {   // one flavor's section
    Mode mode = Mode::NotBuilt;
    bool ready = false;          // built (a Rebuild in progress still counts): the "Play with <Name> instead" button may be offered
    int64_t built = 0;
    bool older = false;          // the build is from the flavor's previous audited source: a newer one can be built (never forced)
    bool cancelled_note = false;
};

struct View {
    FV f[hs::kFlavorCount];
    hs::BuildStatus st;          // build_status(): st.flavor says which flavor the running / last build is
    bool running = false;        // any build runs
    hs::Requirements req;
    std::string cur = "lichii";  // what Play starts: lichii | fritchii | hookshot | longshot (the same decision as Play)
    bool cur_missing = false;    // cur is a native flavor that is not built yet
    int64_t kit_bytes = -1;
    bool kit_there = false;
};

struct UI {
    std::string lang = "en-US";
    bool lang_init = false;
    bool confirm_cancel = false, confirm_rebuild[hs::kFlavorCount] = {false, false}, confirm_remove = false;
    bool probe_set = false, recovered = false;
    bool was_running = false;
    bool req_asked = false;
    std::string req_for;
    std::string last_key;        // the game whose card was drawn last (the finished-build toast looks its runner up)
    double kit_asked_at = -100.0;
    Fake fake;

    std::string fake_runner = "lichii";   // the fake state's runner (the switch buttons set it)
    bool fake_read = false;
    std::string remove_error;
};
UI HU;
std::atomic<bool> g_game_running{false};

void read_fake() {
    if (HU.fake_read) return;
    HU.fake_read = true;
    const char* m = g_headless ? std::getenv("HUB_SHOT_HOOKSHOT") : nullptr;
    if (m && *m && !(m[0] == '0' && !m[1])) {
        HU.fake.on = true;
        HU.fake.mode = m;
        const std::string& k = HU.fake.mode;
        HU.fake_runner = k == "done_on" ? "hookshot" : (k == "ls_done_on" || k == "ls_missing") ? "longshot" : (k == "done_fritchii" || k == "both_done") ? "fritchii" : "lichii";
    }
}

std::string date_text(int64_t unix_t) {
    if (unix_t <= 0) return std::string();
    std::time_t t = (std::time_t)unix_t;
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[48];
    std::strftime(b, sizeof b, "%b %d, %Y", &tm);
    return b;
}

std::string gb_text(int64_t bytes) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    return b;
}

/* The language the game was last built in (language.txt in the build: the last line that is not a comment); the first flavor that has one */
void init_language() {
    if (HU.lang_init) return;
    HU.lang_init = true;
    for (const hs::Flavor f : hs::kFlavors) {
        std::ifstream in(fs::u8path(hs::game_dir(f)) / "language.txt", std::ios::binary);
        std::string line, found;
        while (in && std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty() && line[0] != '#') found = line;
        }
        if (hs::language_valid(found)) { HU.lang = found; return; }
    }
}

hs::Requirements fake_requirements(bool problems) {
    hs::Requirements r;
    r.checked = true;
    r.game_ok = true;
    r.vs_tools = r.win_sdk = !problems;
    r.vulkan_mesh = true;
    r.gpu_name = "NVIDIA GeForce RTX 4070";
    r.ram_gb = 32;
    r.free_gb = problems ? 41 : 312;
    r.game_path = "D:\\Games\\Switch 2\\Zelda Ocarina of Time.nup";
    hs::fill_requirement_problem(&r);
    return r;
}

/* The fake states (HUB_SHOT_HOOKSHOT, headless only). Hookshot's keep their old meaning; ls_* / both_done show Longshot. */
View make_fake_view() {
    View v;
    const std::string& m = HU.fake.mode;
    bool built[hs::kFlavorCount] = {false, false};
    int building = -1, failed = -1;
    if (m == "building" || m == "paused") building = 0;
    else if (m == "ls_building") building = 1;
    else if (m == "done" || m == "done_on" || m == "done_fritchii") built[0] = true;
    else if (m == "ls_done" || m == "ls_done_on") built[1] = true;
    else if (m == "both_done") built[0] = built[1] = true;
    else if (m == "failed") failed = 0;
    v.kit_bytes = (int64_t)(21.4 * 1024 * 1024 * 1024);
    v.kit_there = building >= 0 || failed >= 0 || built[0] || built[1];
    v.req = fake_requirements(m == "problems");
    v.running = building >= 0;
    if (building >= 0) {
        v.st.flavor = hs::kFlavors[building];
        v.st.state = m == "paused" ? hs::BuildState::Paused : hs::BuildState::Building;
        v.st.stage_id = "compile";
        v.st.stage = "Compiling";
        v.st.progress = 0.58f;
        v.st.overall = 0.40f + 0.57f * 0.58f;
        v.st.paused_for_game = m == "paused";
        v.st.started_unix = (int64_t)std::time(nullptr) - 1800;
    } else if (failed >= 0) {
        v.st.flavor = hs::kFlavors[failed];
        v.st.state = hs::BuildState::Failed;
        v.st.error = "nx2/build.py stopped: error: unknown type name 'VkPhysicalDeviceMeshShaderFeaturesEXT'";
        v.st.log_path = "C:\\RetroHub\\userdata\\emulators\\hookshot-kit\\logs\\build-20261107-091500.log";
    }
    for (int i = 0; i < hs::kFlavorCount; ++i) {
        FV& fv = v.f[i];
        fv.ready = built[i];
        if (building == i) fv.mode = Mode::Building;
        else if (built[i]) { fv.mode = Mode::Ready; fv.built = (int64_t)std::time(nullptr) - 5 * 3600; }
        else if (failed == i) fv.mode = Mode::Failed;
    }
    v.cur = HU.fake_runner;
    hs::Flavor cf;
    if (hs::flavor_from_id(v.cur, &cf)) v.cur_missing = !v.f[(int)cf].ready;
    return v;
}

View make_view(const hub::GameEntry& e) {
    if (HU.fake.on) return make_fake_view();
    View v;
    v.st = hs::build_status();
    v.running = hs::build_running();
    v.req = hs::requirements_cached();
    v.kit_there = hs::kit_present();
    v.kit_bytes = hs::kit_size_cached();
    // ready() / read_stamp() touch the disk: not every frame. Cached for 2 s per flavor, and re-read at once when a build has just finished.
    static double s_at[hs::kFlavorCount] = {-100.0, -100.0};
    static bool s_ready[hs::kFlavorCount] = {false, false}, s_was_running = false;
    static hs::Stamp s_stamp[hs::kFlavorCount];
    static std::string s_dkey, s_dpath, s_cur;   // the cached decision (decide() calls ready()): redone with the flavor caches or when the runner setting changes
    static bool s_dvalid = false, s_dmissing = false;
    if (v.running) s_was_running = true;
    else if (s_was_running) { s_at[0] = s_at[1] = -100.0; s_was_running = false; }
    const double now = ImGui::GetTime();
    bool refreshed = false;
    for (int i = 0; i < hs::kFlavorCount; ++i) {
        const hs::Flavor f = hs::kFlavors[i];
        if (v.running && v.st.flavor == f) continue;   // being swapped in: leave its files alone
        if (now - s_at[i] > 2.0 || now < s_at[i]) {
            s_at[i] = now;
            refreshed = true;
            s_ready[i] = hs::ready(nullptr, std::string(), f) && hs::read_stamp(hs::game_dir(f), &s_stamp[i]);
        }
    }
    for (int i = 0; i < hs::kFlavorCount; ++i) {
        const hs::Flavor f = hs::kFlavors[i];
        FV& fv = v.f[i];
        fv.ready = s_ready[i];
        if (v.running && v.st.flavor == f) { fv.mode = Mode::Building; continue; }
        if (s_ready[i]) {
            fv.mode = Mode::Ready;
            fv.built = s_stamp[i].built;
            fv.older = hs::is_older_audited(s_stamp[i], f);
        } else if (v.st.flavor == f && v.st.state == hs::BuildState::Failed) {
            fv.mode = Mode::Failed;
        } else {
            fv.mode = Mode::NotBuilt;
            fv.cancelled_note = v.st.flavor == f && v.st.state == hs::BuildState::Cancelled;
        }
    }
    const hs::Values gv = gs_values_for(e);
    const auto rit = gv.find(hs::kRunnerSetting);
    const std::string key = rit != gv.end() ? rit->second : std::string();
    if (refreshed || !s_dvalid || key != s_dkey || e.path != s_dpath) {   // the same decision as Play: decide(), then the emulator it picks
        const hs::Decision d = hs::decide(e, gv);
        s_dvalid = true; s_dkey = key; s_dpath = e.path;
        s_dmissing = d.choice == hs::Choice::NativeNotBuilt;
        s_cur = d.choice == hs::Choice::Emulator ? (nx_planned_emulator(e, false) == hubemu::Kind::Fritchii ? "fritchii" : "lichii") : hs::info(d.flavor).id;
    }
    v.cur = s_cur;
    v.cur_missing = s_dmissing;
    return v;
}

void open_url(const std::string& url) {
    if (g_headless) return;
    ShellExecuteW(nullptr, L"open", winutil::to_wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void open_log(const std::string& path) {
    if (g_headless || path.empty()) return;
    const std::wstring arg = L"\"" + winutil::to_wide(path) + L"\"";
    ShellExecuteW(nullptr, L"open", L"notepad.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
}

void wrapped(const std::string& s, ImU32 col) {
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextUnformatted(s.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

/* SameLine before the next button only when it fits in the card (ui::button's width: text + icon + padding); otherwise it starts a new row,
 * so a narrow window or a big UI scale wraps the buttons instead of running them off the right edge. Call right after the previous button. */
void same_line_fit(const char* label, bool icon = true) {
    using ui::sp;
    const float w = ImGui::CalcTextSize(label).x + (icon ? sp(2.25f) + sp(1.f) : 0.f) + sp(2.5f) * 2;
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
    if (ImGui::GetItemRectMax().x + sp(1.f) + w <= right) ImGui::SameLine(0, sp(1.f));
}

void language_row(bool enabled) {
    using ui::sp;
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
    ImGui::TextUnformatted("Game language");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, sp(1.f));
    std::string cur = HU.lang;
    for (const hs::Language& l : hs::languages()) if (HU.lang == l.code) cur = l.name;
    ImGui::SetNextItemWidth(sp(24.f));
    ImGui::BeginDisabled(!enabled);
    if (ImGui::BeginCombo("##hs_lang", cur.c_str())) {
        for (const hs::Language& l : hs::languages()) {
            const bool sel = HU.lang == l.code;
            if (ImGui::Selectable(l.name, sel)) HU.lang = l.code;
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
}

void problems_list(const hs::Requirements& r) {
    using ui::sp;
    int i = 0;
    for (const hs::Problem& p : hs::requirement_problems(r)) {
        ImGui::PushID(i++);
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
        ImGui::Bullet();
        ImGui::PopStyleColor();
        wrapped(p.text, ui::TEXT);
        ImGui::Indent(sp(2.f));
        if (!p.hint.empty()) wrapped(p.hint + ".", ui::TEXT3);
        if (!p.url.empty() && ui::button(p.button.c_str(), ImVec2(0, sp(3.5f)), false, true, ui::Icon::Globe)) open_url(p.url);
        ImGui::Unindent(sp(2.f));
        ImGui::Dummy(ImVec2(0, sp(0.25f)));
        ImGui::PopID();
    }
}

/* "Which version plays": what Play starts right now, and one button for each OTHER runner. A native flavor is offered only when it is built
 * (building it is in its own section below). */
void draw_which(const hub::GameEntry& e, const View& v) {
    using ui::sp;
    hs::Flavor cf = hs::Flavor::Hookshot;
    const bool cur_native = hs::flavor_from_id(v.cur, &cf);
    if (v.cur_missing && cur_native) {
        wrapped(std::string(hs::info(cf).name) + " is chosen but not built on this PC yet. Play will ask what to do until it is built.", ui::WARN);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT);
        ImGui::TextUnformatted(cur_native ? (std::string("Play starts ") + hs::info(cf).name + " (native).").c_str()
                               : v.cur == "fritchii" ? "Play starts Fritchii, the Ocarina of Time version of Lichii." : "Play starts Lichii (emulator).");
        ImGui::PopStyleColor();
    }
    wrapped("Your saves are shared by Lichii, Fritchii, Hookshot and Longshot.", ui::TEXT3);
    struct R { const char* id; const char* label; int flavor; };   // flavor: -1 = an emulator
    const R all[] = {{"lichii", "Play with Lichii instead", -1}, {"fritchii", "Play with Fritchii instead", -1},
                     {"hookshot", "Play with Hookshot instead", 0}, {"longshot", "Play with Longshot instead", 1}};
    bool first = true;
    for (const R& r : all) {
        if (v.cur == r.id) continue;
        if (r.flavor >= 0 && !v.f[r.flavor].ready) continue;   // you cannot switch to what is not built
        if (!first) same_line_fit(r.label);
        first = false;
        if (ui::button(r.label, ImVec2(0, sp(4.f)), r.flavor >= 0, true, ui::Icon::Play)) {   // (Rebuild keeps the only refresh icon)
            if (HU.fake.on) HU.fake_runner = r.id;
            else if (!gs_set_game_value(e, hs::kRunnerSetting, r.id)) push_toast("Could not save that choice.", ui::WARN, 6.0);
        }
    }
}

void draw_not_built(const hub::GameEntry& e, const View& v, hs::Flavor f, bool* problems_shown) {
    using ui::sp;
    const FV& fv = v.f[(int)f];
    const hs::FlavorInfo& fi = hs::info(f);
    const bool other_busy = v.running && v.st.flavor != f;
    if (f == hs::Flavor::Longshot) {
        // (v2.0.5 is the Hookshot release Longshot started from; the audited Longshot version itself is fi.source_version)
        wrapped(std::string(fi.name) + " is a fork of Hookshot by another author. It starts from an older Hookshot (v2.0.5) and only takes changes its "
                "author has played through. It fixes night lighting. It prepares its shaders at the first start, inside the game window, so the first "
                "start takes a few minutes. RetroHub builds it on this PC from your own copy of the game, the same way as Hookshot: about 45 minutes, and "
                "70 GB free while building. It is experimental, like Hookshot.",
                ui::TEXT2);
    } else {
        wrapped("Hookshot is an experimental native Windows version of this game, with no emulator. It may be rough in places, so it stays "
                "optional. RetroHub builds Hookshot on this PC from your own copy of the game. It takes about 45 minutes and needs 70 GB free "
                "while building. You can keep using your PC, and it pauses while you play.",
                ui::TEXT2);
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    const bool busy = !HU.fake.on && hs::requirements_busy();
    if (!v.req.checked) {
        wrapped(busy ? "Checking your PC..." : "Checking...", ui::TEXT3);
    } else {
        const std::vector<hs::Problem> pr = hs::requirement_problems(v.req);
        if (pr.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::GOOD);
            ImGui::TextUnformatted(v.req.gpu_name.empty() ? "Your PC can build it." : ("Your PC can build it (" + v.req.gpu_name + ").").c_str());
            ImGui::PopStyleColor();
        } else if (!*problems_shown) {   // the problems are the same for both flavors: listed once
            *problems_shown = true;
            problems_list(v.req);
        } else {
            wrapped("Your PC has the same problems as listed above.", ui::TEXT3);
        }
    }
    if (fv.cancelled_note) wrapped("The last build was cancelled.", ui::TEXT3);
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    language_row(!other_busy);
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    const bool can = v.req.checked && v.req.problem.empty() && !busy && !other_busy;
    if (v.req.checked && !v.req.problem.empty()) wrapped("Build it is off until this is sorted. Press Check again afterwards.", ui::TEXT3);
    if (other_busy) wrapped("Another build is running.", ui::TEXT3);
    if (ui::button("Build it", ImVec2(0, sp(4.f)), true, can, ui::Icon::Bolt) && !HU.fake.on) {
        std::string err;
        if (!hs::start_build(e, HU.lang, &err, f)) push_toast(err, ui::WARN, 7.0);
    }
    const char* chk = busy ? "Checking..." : "Check again";
    same_line_fit(chk);
    if (ui::button(chk, ImVec2(0, sp(4.f)), false, !busy, ui::Icon::Refresh) && !HU.fake.on) hs::refresh_requirements_async(e);
}

void draw_building(const View& v, hs::Flavor f) {
    using ui::sp;
    const bool paused = v.st.paused_for_game || v.st.state == hs::BuildState::Paused;
    ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT);
    ImGui::TextUnformatted((v.st.stage.empty() ? std::string("Starting") : v.st.stage).c_str());
    ImGui::PopStyleColor();
    char pct[16];
    std::snprintf(pct, sizeof pct, "%d%%", (int)(v.st.overall * 100.f + 0.5f));
    ImGui::ProgressBar(std::min(1.f, std::max(0.f, v.st.overall)), ImVec2(-1, sp(2.f)), pct);
    if (paused) wrapped("Paused while you play. It carries on when the game closes.", ui::WARN);
    else wrapped(std::string("Building the native ") + hs::info(f).name + " version. You can keep using your PC; this takes about 45 minutes.", ui::TEXT3);
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (!HU.confirm_cancel) {
        if (ui::button("Cancel", ImVec2(0, sp(4.f)), false, true, ui::Icon::Close)) HU.confirm_cancel = true;
    } else {
        wrapped("Cancel the build? Your saves are not touched; the build starts again from the beginning.", ui::TEXT2);
        if (ui::button("Yes, cancel", ImVec2(0, sp(4.f)), true) && !HU.fake.on) { hs::cancel_build(); HU.confirm_cancel = false; }
        same_line_fit("Keep building", false);
        if (ui::button("Keep building", ImVec2(0, sp(4.f)))) HU.confirm_cancel = false;
    }
}

/* Once, at the bottom of the card: the tools are shared by both flavors, and stay while a build runs. */
void draw_kit_actions(const View& v) {
    using ui::sp;
    if (!v.kit_there) return;
    const bool busy = !HU.fake.on && hs::kit_busy();
    std::string label = "Remove build tools";
    if (v.kit_bytes > 0) label += " (frees " + gb_text(v.kit_bytes) + ")";
    if (!HU.confirm_remove) {
        if (ui::button(busy ? "Removing..." : label.c_str(), ImVec2(0, sp(4.f)), false, !busy && !v.running, ui::Icon::Close)) HU.confirm_remove = true;
        if (v.running) wrapped("The build tools are in use while a build runs.", ui::TEXT3);
    } else {
        wrapped("Remove the build tools? The games stay and so do your saves. Rebuilding later downloads them again.", ui::TEXT2);
        if (ui::button("Yes, remove them", ImVec2(0, sp(4.f)), true, !v.running) && !HU.fake.on) { hs::remove_kit_async(); HU.confirm_remove = false; }
        same_line_fit("Keep them", false);
        if (ui::button("Keep them", ImVec2(0, sp(4.f)))) HU.confirm_remove = false;
    }
}

void draw_ready(const hub::GameEntry& e, const View& v, hs::Flavor f, bool* problems_shown) {
    using ui::sp;
    const FV& fv = v.f[(int)f];
    const hs::FlavorInfo& fi = hs::info(f);
    const bool in_use = v.cur == fi.id;
    const bool other_busy = v.running && v.st.flavor != f;
    wrapped(in_use ? std::string(fi.name) + " is experimental. Switch back to Lichii or Fritchii if it misbehaves." : std::string("The native ") + fi.name + " version is built and ready.", ui::TEXT3);
    const std::string d = date_text(fv.built);
    if (!d.empty()) wrapped("Built on " + d + ".", ui::TEXT3);
    if (fv.older) wrapped(std::string("A newer ") + fi.name + " (" + fi.source_version + ") is available. This build keeps working; Rebuild makes the newer one from your game file and replaces this one only when it succeeds. Your saves are kept.", ui::TEXT2);
    if (other_busy) wrapped("Another build is running.", ui::TEXT3);
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    bool& confirm = HU.confirm_rebuild[(int)f];
    if (!confirm) {
        if (ui::button("Rebuild", ImVec2(0, sp(4.f)), false, !other_busy, ui::Icon::Refresh)) {
            confirm = true;
            if (!HU.fake.on && !hs::requirements_cached().checked) hs::refresh_requirements_async(e);
        }
    } else {
        wrapped("Build it again from your game file. It takes about 45 minutes. Your saves are kept.", ui::TEXT2);
        language_row(true);
        if (v.req.checked && !v.req.problem.empty() && !*problems_shown) { *problems_shown = true; problems_list(v.req); }
        const bool can = v.req.checked && v.req.problem.empty() && !other_busy;
        ImGui::Dummy(ImVec2(0, sp(0.25f)));
        if (ui::button("Rebuild now", ImVec2(0, sp(4.f)), true, can, ui::Icon::Bolt) && !HU.fake.on) {
            std::string err;
            if (hs::start_build(e, HU.lang, &err, f)) confirm = false;
            else push_toast(err, ui::WARN, 7.0);
        }
        same_line_fit("Never mind", false);
        if (ui::button("Never mind", ImVec2(0, sp(4.f)))) confirm = false;
    }
}

void draw_failed(const hub::GameEntry& e, const View& v, hs::Flavor f) {
    using ui::sp;
    ImGui::PushStyleColor(ImGuiCol_Text, ui::BAD);
    ImGui::TextUnformatted("The build stopped.");
    ImGui::PopStyleColor();
    wrapped(v.st.error.empty() ? std::string("Something went wrong. The log has the details.") : v.st.error, ui::TEXT2);
    if (v.kit_there) wrapped("The build tools stay on this PC so trying again is quicker.", ui::TEXT3);
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (ui::button("Try again", ImVec2(0, sp(4.f)), true, !v.running, ui::Icon::Refresh) && !HU.fake.on) {
        std::string err;
        if (!hs::start_build(e, HU.lang, &err, f)) push_toast(err, ui::WARN, 7.0);
    }
    same_line_fit("Open the log");
    if (ui::button("Open the log", ImVec2(0, sp(4.f)), false, !v.st.log_path.empty(), ui::Icon::Folder)) open_log(v.st.log_path);
}

/* Screenshots only (a fake state, headless): the card is taller than the page now, so HUB_SHOT_HOOKSHOT_AT picks the part that is brought to
 * the top: "which" (default, the card's first line), "hookshot" or "longshot" (that flavor's section). Applied every frame: the page's layout
 * keeps settling for a while after it opens, and a few frames of scrolling were not enough. Never outside a fake state. */
float g_fake_anchor_y = -1.f;   // screen y of a section inside the card (the card is a child window: the page scrolls after EndChild)

void fake_anchor(const char* id) {
    if (!HU.fake.on || !g_headless) return;
    static const char* env = std::getenv("HUB_SHOT_HOOKSHOT_AT");
    const std::string at = env && *env ? env : "which";
    if (at == id) g_fake_anchor_y = ImGui::GetCursorScreenPos().y;
}

void fake_anchor_apply() {   // in the page's own window
    if (g_fake_anchor_y < 0.f) return;
    ImGui::SetScrollFromPosY(g_fake_anchor_y - ImGui::GetWindowPos().y, 0.f);
    g_fake_anchor_y = -1.f;
}

/* One flavor's section. PushID per flavor: both sections use the same button labels ("Build it", "Rebuild", ...). */
void draw_flavor(const hub::GameEntry& e, const View& v, hs::Flavor f, bool* problems_shown) {
    using ui::sp;
    ImGui::PushID((int)f + 100);
    ImGui::Dummy(ImVec2(0, sp(0.75f)));
    fake_anchor(hs::info(f).id);
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted((std::string("Native version (") + hs::info(f).name + ")").c_str());   // its own section: the runner choice above is not part of it
    ImGui::PopFont();
    switch (v.f[(int)f].mode) {
        case Mode::NotBuilt: draw_not_built(e, v, f, problems_shown); break;
        case Mode::Building: draw_building(v, f); break;
        case Mode::Ready: draw_ready(e, v, f, problems_shown); break;
        case Mode::Failed: draw_failed(e, v, f); break;
    }
    ImGui::PopID();
}

}   // namespace

void hookshot_ui_tick() {
    read_fake();
    if (g_headless) return;
    if (!HU.probe_set) {
        HU.probe_set = true;
        hs::set_game_running_probe([] { return g_game_running.load(); });   // (a flag the UI thread keeps: the build's thread never reads app state)
    }
    g_game_running = A.proc != nullptr;
    if (!HU.recovered && !hs::build_running()) {   // a swap cut short by a crash: a few renames, once, for each flavor
        HU.recovered = true;
        for (const hs::Flavor f : hs::kFlavors) hs::recover_swap(hs::game_dir(f) + ".staging", hs::game_dir(f), hs::game_dir(f) + ".old");
    }
    const bool running = hs::build_running();
    if (HU.was_running && !running) {
        const hs::BuildStatus st = hs::build_status();
        const std::string name = hs::info(st.flavor).name;
        if (st.state == hs::BuildState::Done) {
            // the player may have chosen this flavor before it was built: then Play starts it now (no "Play still uses ..." claim then)
            const hub::GameEntry* ge = find_entry(HU.last_key.empty() ? std::string("switch:0400486024dec000") : HU.last_key);
            const hs::Decision d = ge && hs::supports(*ge) ? hs::decide(*ge, gs_values_for(*ge)) : hs::Decision();
            if (ge && d.choice == hs::Choice::Native && d.flavor == st.flavor) push_toast(name + " is built. Play now starts " + name + ".", ui::GOOD, 12.0);
            else push_toast(name + " is built. To play with it, open the game's page and choose \"Play with " + name + " instead\" (or Settings > Plays with). Play keeps using the current version until then.", ui::GOOD, 12.0);
        } else if (st.state == hs::BuildState::Failed) push_toast("The " + name + " build stopped. Open the game's page to see why.", ui::BAD, 12.0);
        HU.was_running = false;
        HU.req_asked = false;
    }
    if (running) HU.was_running = true;
}

void hookshot_details_section(const hub::GameEntry& e) {
    using ui::sp;
    if (!hs::supports(e)) return;
    read_fake();
    init_language();
    HU.last_key = e.key;
    // the requirement check runs once per page opening (and on "Check again"), never on the UI thread
    if (!HU.fake.on && !hs::build_running() && !hs::requirements_busy() && (HU.req_for != e.path || (!HU.req_asked && !hs::requirements_cached().checked))) {
        HU.req_for = e.path;
        HU.req_asked = true;
        hs::refresh_requirements_async(e);
    }
    if (!HU.fake.on && hs::kit_present() && hs::kit_size_cached() < 0 && !hs::kit_busy() && ImGui::GetTime() - HU.kit_asked_at > 30.0) {
        HU.kit_asked_at = ImGui::GetTime();
        hs::kit_size_async();
    }
    const View v = make_view(e);

    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    fake_anchor("which");   // screenshots: the card sits at the end of the page, bring it (or one of its sections) to the top
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::PANEL);
    ImGui::PushStyleColor(ImGuiCol_Border, ui::HAIRLINE);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, sp(1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(1.5f), sp(1.f)));
    ImGui::BeginChild("##hookshot_card", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, sp(0.5f)));   // a compact card: the page needs the room

    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted("Which version plays");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, sp(0.25f)));
    draw_which(e, v);

    bool problems_shown = false;   // the requirement problems are the same for both flavors: only the first section that needs them lists them
    for (const hs::Flavor f : hs::kFlavors) draw_flavor(e, v, f, &problems_shown);
    if (v.kit_there) {
        ImGui::Dummy(ImVec2(0, sp(0.75f)));
        draw_kit_actions(v);
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    fake_anchor_apply();
}
