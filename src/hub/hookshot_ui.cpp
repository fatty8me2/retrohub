#include "hookshot_ui.h"
#include "hookshot.h"
#include "hookshot_build.h"
#include "app.h"
#include "winutil.h"

/* The Hookshot card (game page > About). Contract: hookshot_build.h (the build) and hookshot.h (what is built).
 * The card has four looks: not built (what it is, what is missing, Build it), building (stage, progress, Cancel), ready (Play uses it;
 * Rebuild, Remove build tools) and failed (the reason, Try again, Open the log). All of it is ordinary ImGui widgets, so the pad and
 * the keyboard reach every button. Nothing here opens a system window except what the player clicks (the download page, Notepad). */

namespace fs = std::filesystem;

namespace {

namespace hs = hookshot;

enum class Mode { NotBuilt, Building, Ready, Failed };

struct Fake {
    bool on = false;
    std::string mode;
};

struct View {
    Mode mode = Mode::NotBuilt;
    hs::BuildStatus st;
    hs::Requirements req;
    int64_t built = 0;
    bool use_hookshot = false;   // Play starts Hookshot (this game's switch.runner is "hookshot")
    std::string runner = "lichii";   // the game's switch.runner: lichii | fritchii | hookshot
    int64_t kit_bytes = -1;
    bool kit_there = false;
    bool cancelled_note = false;
};

struct UI {
    std::string lang = "en-US";
    bool lang_init = false;
    bool confirm_cancel = false, confirm_rebuild = false, confirm_remove = false;
    bool probe_set = false, recovered = false;
    bool was_running = false;
    bool req_asked = false;
    std::string req_for;
    double kit_asked_at = -100.0;
    Fake fake;
    int scroll_frames = 0;   // screenshots: frames spent scrolling to the card (a few, for layout)
    bool fake_on_hookshot = false;   // the fake done state's runner
    std::string fake_runner = "lichii";   // ...as a name (the switch buttons set it)
    bool fake_read = false;
    std::string remove_error;
};
UI HU;
std::atomic<bool> g_game_running{false};

void read_fake() {
    if (HU.fake_read) return;
    HU.fake_read = true;
    const char* m = g_headless ? std::getenv("HUB_SHOT_HOOKSHOT") : nullptr;
    if (m && *m && !(m[0] == '0' && !m[1])) { HU.fake.on = true; HU.fake.mode = m; HU.fake_on_hookshot = HU.fake.mode == "done_on"; HU.fake_runner = HU.fake_on_hookshot ? "hookshot" : HU.fake.mode == "done_fritchii" ? "fritchii" : "lichii"; }
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

/* The language the game was last built in (language.txt in the build: the last line that is not a comment) */
void init_language() {
    if (HU.lang_init) return;
    HU.lang_init = true;
    std::ifstream f(fs::u8path(hs::game_dir()) / "language.txt", std::ios::binary);
    std::string line, found;
    while (f && std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty() && line[0] != '#') found = line;
    }
    if (hs::language_valid(found)) HU.lang = found;
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

View make_view(const hub::GameEntry& e) {
    View v;
    if (HU.fake.on) {
        const std::string& m = HU.fake.mode;
        v.kit_bytes = (int64_t)(21.4 * 1024 * 1024 * 1024);
        v.kit_there = true;
        if (m == "building" || m == "paused") {
            v.mode = Mode::Building;
            v.st.state = m == "paused" ? hs::BuildState::Paused : hs::BuildState::Building;
            v.st.stage_id = "compile";
            v.st.stage = "Compiling";
            v.st.progress = 0.58f;
            v.st.overall = 0.40f + 0.57f * 0.58f;
            v.st.paused_for_game = m == "paused";
            v.st.started_unix = (int64_t)std::time(nullptr) - 1800;
        } else if (m == "done" || m == "done_on" || m == "done_fritchii") {
            v.mode = Mode::Ready;
            v.built = (int64_t)std::time(nullptr) - 5 * 3600;
            v.runner = HU.fake_runner;
            v.use_hookshot = v.runner == "hookshot";
        } else if (m == "failed") {
            v.mode = Mode::Failed;
            v.st.state = hs::BuildState::Failed;
            v.st.error = "nx2/build.py stopped: error: unknown type name 'VkPhysicalDeviceMeshShaderFeaturesEXT'";
            v.st.log_path = "C:\\RetroHub\\userdata\\emulators\\hookshot-kit\\logs\\build-20261107-091500.log";
        } else {
            v.mode = Mode::NotBuilt;
            v.req = fake_requirements(m == "problems");
            v.kit_there = false;
        }
        if (v.mode != Mode::NotBuilt) v.req = fake_requirements(false);
        return v;
    }
    v.st = hs::build_status();
    v.req = hs::requirements_cached();
    v.kit_there = hs::kit_present();
    v.kit_bytes = hs::kit_size_cached();
    // ready() / read_stamp() touch the disk: not every frame. Cached for 2 s, and re-read at once when a build has just finished.
    static double s_at = -100.0;
    static bool s_ready = false, s_was_running = false;
    static hs::Stamp s_stamp;
    if (hs::build_running()) {
        s_was_running = true;
        v.mode = Mode::Building;
        return v;
    }
    const double now = ImGui::GetTime();
    if (s_was_running) { s_at = -100.0; s_was_running = false; }
    if (now - s_at > 2.0 || now < s_at) {
        s_at = now;
        s_ready = hs::ready() && hs::read_stamp(hs::game_dir(), &s_stamp);
    }
    if (s_ready) {
        v.mode = Mode::Ready;
        v.built = s_stamp.built;
        const hs::Values gv = gs_values_for(e);
        const auto rit = gv.find(hs::kRunnerSetting);
        if (rit != gv.end() && rit->second == "hookshot") v.runner = "hookshot";
        else v.runner = nx_planned_emulator(e, false) == hubemu::Kind::Fritchii ? "fritchii" : "lichii";   // same decision as Play (Ocarina defaults to Fritchii)
        v.use_hookshot = v.runner == "hookshot";
    } else if (v.st.state == hs::BuildState::Failed) {
        v.mode = Mode::Failed;
    } else {
        v.mode = Mode::NotBuilt;
        v.cancelled_note = v.st.state == hs::BuildState::Cancelled;
    }
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

void draw_not_built(const hub::GameEntry& e, const View& v) {
    using ui::sp;
    wrapped("Hookshot is an experimental native Windows version of this game, with no emulator. It may be rough in places, so Lichii stays the "
            "default way to play. RetroHub builds Hookshot on this PC from your own copy of the game. It takes about 45 minutes and needs 70 GB free "
            "while building. You can keep using your PC, and it pauses while you play.",
            ui::TEXT2);
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
        } else {
            problems_list(v.req);
        }
    }
    if (v.cancelled_note) wrapped("The last build was cancelled.", ui::TEXT3);
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    language_row(true);
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    const bool can = v.req.checked && v.req.problem.empty() && !busy;
    if (v.req.checked && !v.req.problem.empty()) wrapped("Build it is off until this is sorted. Press Check again afterwards.", ui::TEXT3);
    if (ui::button("Build it", ImVec2(0, sp(4.f)), true, can, ui::Icon::Bolt) && !HU.fake.on) {
        std::string err;
        if (!hs::start_build(e, HU.lang, &err)) push_toast(err, ui::WARN, 7.0);
    }
    ImGui::SameLine(0, sp(1.f));
    if (ui::button(busy ? "Checking..." : "Check again", ImVec2(0, sp(4.f)), false, !busy, ui::Icon::Refresh) && !HU.fake.on) hs::refresh_requirements_async(e);
}

void draw_building(const View& v) {
    using ui::sp;
    const bool paused = v.st.paused_for_game || v.st.state == hs::BuildState::Paused;
    ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT);
    ImGui::TextUnformatted((v.st.stage.empty() ? std::string("Starting") : v.st.stage).c_str());
    ImGui::PopStyleColor();
    char pct[16];
    std::snprintf(pct, sizeof pct, "%d%%", (int)(v.st.overall * 100.f + 0.5f));
    ImGui::ProgressBar(std::min(1.f, std::max(0.f, v.st.overall)), ImVec2(-1, sp(2.f)), pct);
    if (paused) wrapped("Paused while you play. It carries on when the game closes.", ui::WARN);
    else wrapped("Building the native version. You can keep using your PC; this takes about 45 minutes.", ui::TEXT3);
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (!HU.confirm_cancel) {
        if (ui::button("Cancel", ImVec2(0, sp(4.f)), false, true, ui::Icon::Close)) HU.confirm_cancel = true;
    } else {
        wrapped("Cancel the build? Your saves are not touched; the build starts again from the beginning.", ui::TEXT2);
        if (ui::button("Yes, cancel", ImVec2(0, sp(4.f)), true) && !HU.fake.on) { hs::cancel_build(); HU.confirm_cancel = false; }
        ImGui::SameLine(0, sp(1.f));
        if (ui::button("Keep building", ImVec2(0, sp(4.f)))) HU.confirm_cancel = false;
    }
}

void draw_kit_actions(const hub::GameEntry& e, const View& v) {
    using ui::sp;
    (void)e;
    if (!v.kit_there) return;
    const bool busy = !HU.fake.on && hs::kit_busy();
    std::string label = "Remove build tools";
    if (v.kit_bytes > 0) label += " (frees " + gb_text(v.kit_bytes) + ")";
    if (!HU.confirm_remove) {
        if (ui::button(busy ? "Removing..." : label.c_str(), ImVec2(0, sp(4.f)), false, !busy, ui::Icon::Close)) HU.confirm_remove = true;
    } else {
        wrapped("Remove the build tools? The game stays and so do your saves. Rebuilding later downloads them again.", ui::TEXT2);
        if (ui::button("Yes, remove them", ImVec2(0, sp(4.f)), true) && !HU.fake.on) { hs::remove_kit_async(); HU.confirm_remove = false; }
        ImGui::SameLine(0, sp(1.f));
        if (ui::button("Keep them", ImVec2(0, sp(4.f)))) HU.confirm_remove = false;
    }
}

void draw_ready(const hub::GameEntry& e, const View& v) {
    using ui::sp;
    ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT);
    ImGui::TextUnformatted(v.use_hookshot ? "Play starts Hookshot (native)." : v.runner == "fritchii" ? "Play starts Fritchii, the Ocarina of Time version of Lichii." : "Play starts Lichii (emulator).");
    ImGui::PopStyleColor();
    wrapped("Your saves are shared by Lichii, Fritchii and Hookshot.", ui::TEXT3);
    {   // one button for each runner that is NOT in use now (three runners)
        struct R { const char* id; const char* label; };
        const R all[] = {{"lichii", "Play with Lichii instead"}, {"fritchii", "Play with Fritchii instead"}, {"hookshot", "Play with Hookshot instead"}};
        bool first = true;
        for (const R& r : all) {
            if (v.runner == r.id) continue;
            if (!first) ImGui::SameLine(0, sp(1.f));
            first = false;
            if (ui::button(r.label, ImVec2(0, sp(4.f)), std::string(r.id) == "hookshot" && !v.use_hookshot, true, ui::Icon::Play)) {   // (Rebuild keeps the only refresh icon)
                if (HU.fake.on) { HU.fake_runner = r.id; HU.fake_on_hookshot = HU.fake_runner == "hookshot"; }
                else if (!gs_set_game_value(e, hs::kRunnerSetting, r.id)) push_toast("Could not save that choice.", ui::WARN, 6.0);
            }
        }
    }
    ImGui::Dummy(ImVec2(0, sp(0.75f)));
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted("Native version (Hookshot)");   // its own section: the runner choice above is not part of it
    ImGui::PopFont();
    wrapped(v.use_hookshot ? "Hookshot is experimental. Switch back to Lichii if it misbehaves." : "The native Hookshot version is built and ready.", ui::TEXT3);
    const std::string d = date_text(v.built);
    if (!d.empty()) wrapped("Built on " + d + ".", ui::TEXT3);
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (!HU.confirm_rebuild) {
        if (ui::button("Rebuild", ImVec2(0, sp(4.f)), false, true, ui::Icon::Refresh)) {
            HU.confirm_rebuild = true;
            if (!HU.fake.on && !hs::requirements_cached().checked) hs::refresh_requirements_async(e);
        }
        if (v.kit_there) ImGui::SameLine(0, sp(1.f));
        draw_kit_actions(e, v);
    } else {
        wrapped("Build it again from your game file. It takes about 45 minutes. Your saves are kept.", ui::TEXT2);
        language_row(true);
        if (v.req.checked && !v.req.problem.empty()) problems_list(v.req);
        const bool can = v.req.checked && v.req.problem.empty();
        ImGui::Dummy(ImVec2(0, sp(0.25f)));
        if (ui::button("Rebuild now", ImVec2(0, sp(4.f)), true, can, ui::Icon::Bolt) && !HU.fake.on) {
            std::string err;
            if (hs::start_build(e, HU.lang, &err)) HU.confirm_rebuild = false;
            else push_toast(err, ui::WARN, 7.0);
        }
        ImGui::SameLine(0, sp(1.f));
        if (ui::button("Never mind", ImVec2(0, sp(4.f)))) HU.confirm_rebuild = false;
    }
}

void draw_failed(const hub::GameEntry& e, const View& v) {
    using ui::sp;
    ImGui::PushStyleColor(ImGuiCol_Text, ui::BAD);
    ImGui::TextUnformatted("The build stopped.");
    ImGui::PopStyleColor();
    wrapped(v.st.error.empty() ? std::string("Something went wrong. The log has the details.") : v.st.error, ui::TEXT2);
    if (v.kit_there) wrapped("The build tools stay on this PC so trying again is quicker.", ui::TEXT3);
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    if (ui::button("Try again", ImVec2(0, sp(4.f)), true, true, ui::Icon::Refresh) && !HU.fake.on) {
        std::string err;
        if (!hs::start_build(e, HU.lang, &err)) push_toast(err, ui::WARN, 7.0);
    }
    ImGui::SameLine(0, sp(1.f));
    if (ui::button("Open the log", ImVec2(0, sp(4.f)), false, !v.st.log_path.empty(), ui::Icon::Folder)) open_log(v.st.log_path);
    if (v.kit_there) {
        ImGui::SameLine(0, sp(1.f));
        draw_kit_actions(e, v);
    }
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
    if (!HU.recovered && !hs::build_running()) {   // a swap cut short by a crash: a few renames, once
        HU.recovered = true;
        hs::recover_swap(hs::game_dir() + ".staging", hs::game_dir(), hs::game_dir() + ".old");
    }
    const bool running = hs::build_running();
    if (HU.was_running && !running) {
        const hs::BuildStatus st = hs::build_status();
        if (st.state == hs::BuildState::Done) push_toast("Hookshot is built. To play with it, open the game's page and choose \"Play with Hookshot instead\" (or Settings > Plays with). Play still uses Lichii until then.", ui::GOOD, 12.0);
        else if (st.state == hs::BuildState::Failed) push_toast("The Hookshot build stopped. Open the game's page to see why.", ui::BAD, 12.0);
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
    if (HU.fake.on && HU.scroll_frames < 8 && ImGui::GetFrameCount() > 8) { ImGui::SetScrollHereY(0.f); ++HU.scroll_frames; }   // screenshots: the card sits at the end of the page, bring it to the top
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
    ImGui::TextUnformatted(v.mode == Mode::Ready ? "Which version plays" : "Native version (Hookshot)");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, sp(0.25f)));

    switch (v.mode) {
        case Mode::NotBuilt: draw_not_built(e, v); break;
        case Mode::Building: draw_building(v); break;
        case Mode::Ready: draw_ready(e, v); break;
        case Mode::Failed: draw_failed(e, v); break;
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
}
