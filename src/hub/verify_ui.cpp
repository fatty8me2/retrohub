/* Plan 4 Wave 3 L3 "Verify my games": the Library health card on All Games and the one-line verdict on a game's page.
 * Logic and data: common/verify.h (known-dump list, hashing, cache, background runner); docs/VERIFY.md.
 * Cartridge ROMs are hashed only when the player presses "Verify my games", on a background thread; a normal scan never hashes.
 * Screenshot aid: HUB_SHOT_VERIFY=card (expanded, made-up results + notes) | collapsed. Use with HUB_SHOT_FAKE_LIB=n and HUB_SHOT_FAKE_DUPES=1. */
#include "app.h"
#include "wave3_hooks.h"
#include "library_scan_info.h"
#include "pcsx2.h"
#include "verify.h"
#include "dump_check.h"
#include "nx.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

bool ps1_real_bios_present();   // ps1_ui.cpp

namespace {

namespace vf = hub::verify;

struct Note {
    std::string head;
    std::vector<std::string> lines;
    ImU32 col = 0;
};

struct VState {
    vf::Runner runner;
    bool expanded = false;
    bool cache_pass_done = false;     // the quiet "what do we already know" pass after the first scan
    bool was_running = false;
    bool ever_checked = false;        // some game has a verdict (this run or the saved cache)
    double refreshed = -100.0;        // ImGui time of the last note rebuild
    std::vector<Note> notes;          // counted: the card's "N notes"
    std::vector<std::string> optional_lines;   // shown inside the card but not counted (the real-startup BIOS files are optional)
    std::string summary;
    int cartridges = 0;               // library games this check can look at
    // BIOS lookups touch the disk: cached
    double bios_at = -100.0;
    std::string bios_sig;
    bool ps1_missing = false, ps2_missing = false;
    std::vector<std::string> optional_missing;
    // screenshot aid
    bool shot = false, shot_init = false;
    std::map<std::string, vf::Result> fake;
};
VState V;

std::string cache_file() { return hub::data_dir() + "verify_cache.tsv"; }


/* Library games the check can look at: cartridge systems, a real file, not a hack (a hack never matches, and that is fine). */
std::vector<vf::Item> candidates() {
    std::vector<vf::Item> v;
    for (const hub::GameEntry& e : A.lib) {
        if (!vf::supported(e.system) || is_hack(e) || !e.issue.empty() || e.path.empty() || e.key == kNativeKey) continue;
        v.push_back({e.key, e.path, e.system});
    }
    return v;
}

bool lookup(const hub::GameEntry& e, vf::Result* r) {
    if (V.shot) { auto f = V.fake.find(e.key); if (f != V.fake.end()) { *r = f->second; return true; } }
    return V.runner.get(e.key, r);
}

void shot_seed() {
    if (V.shot_init) return;
    V.shot_init = true;
    const char* sv = std::getenv("HUB_SHOT_VERIFY");
    if (!sv || !g_headless) return;
    V.shot = true;
    V.expanded = std::string(sv) != "collapsed";
    int i = 0;
    for (const hub::GameEntry& e : A.lib) {   // every third cartridge game is "not a known dump", the rest match
        if (!vf::supported(e.system)) continue;
        vf::Result r;
        r.v = (i++ % 3 == 1) ? vf::Verdict::Unknown : vf::Verdict::Good;
        if (r.v == vf::Verdict::Good) r.title = e.title + " (USA)";
        V.fake[e.key] = r;
    }
    V.ever_checked = true;
}

std::string plural(int n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

void refresh_bios() {
    bool ps1 = false, ps2 = false, gba = false, nds = false;
    for (const hub::GameEntry& e : A.lib) {
        ps1 |= e.system == hub::System::PS1;
        ps2 |= e.system == hub::System::PS2;
        gba |= e.system == hub::System::GBA;
        nds |= e.system == hub::System::NDS;
    }
    std::string sig = std::to_string(ps1) + std::to_string(ps2) + std::to_string(gba) + std::to_string(nds) + A.st.ps1_bios + "|" + A.st.ps2_bios_dir + "|" +
                      std::to_string(A.st.rom_folders.size()) + (A.st.ps1_openbios ? "o" : "-");
    const double now = ImGui::GetTime();
    if (sig == V.bios_sig && now - V.bios_at < 20.0) return;
    V.bios_sig = sig;
    V.bios_at = now;
    V.ps1_missing = ps1 && !A.st.ps1_openbios && !ps1_real_bios_present();   // OpenBIOS (Settings) plays PlayStation games without a BIOS file
    V.ps2_missing = ps2 && hubemu::pcsx2_find_bios(hubemu::pcsx2_bios_search_dirs(A.st.ps2_bios_dir, A.st.rom_folders)).file.empty();
    V.optional_missing.clear();
    auto optional = [&](hub::System s, const char* label) {
        hub::GameEntry probe;
        probe.system = s;
        RealBootPlan p = real_boot_plan(probe);
        if (p.supported && !p.have) V.optional_missing.push_back(label);
    };
    if (gba) optional(hub::System::GBA, "Game Boy Advance");
    if (nds) optional(hub::System::NDS, "Nintendo DS");
}


/* ---- Incomplete game files (dump_check.h): Switch dumps that are cut short or smaller than any complete copy ---- */
namespace dc = hub::dumpcheck;
dc::Cache g_dump_cache;
/* The cached verdict (never blocks: the first ask starts a background check and answers "nothing yet"). */
bool dump_result(const hub::GameEntry& e, dc::Result* r) {
    if (e.system != hub::System::Switch || is_hack(e) || e.path.empty() || e.key == kNativeKey) return false;
    if (!g_dump_cache.get(e.path, hub::nx::title_id_from_key(e.key), r)) return false;
    return r->v != dc::Verdict::Ok;
}

void rebuild_notes() {
    V.notes.clear();
    V.optional_lines.clear();
    V.summary.clear();
    refresh_bios();

    // 1. games that match no known good dump
    int good = 0, unknown = 0;
    std::vector<std::string> bad_titles;
    if (V.shot) {
        for (const auto& kv : V.fake) {
            const hub::GameEntry* e = find_entry(kv.first);
            if (!e) continue;
            if (kv.second.v == vf::Verdict::Good) ++good;
            else if (kv.second.v == vf::Verdict::Unknown) { ++unknown; bad_titles.push_back(e->title + "  (" + hub::system_name(e->system) + ")"); }
        }
    } else {
        for (const auto& kv : V.runner.snapshot()) {
            const hub::GameEntry* e = find_entry(kv.first);
            if (!e) continue;
            if (kv.second.v == vf::Verdict::Good) ++good;
            else if (kv.second.v == vf::Verdict::Unknown) { ++unknown; bad_titles.push_back(e->title + "  (" + hub::system_name(e->system) + ")"); }
        }
    }
    std::sort(bad_titles.begin(), bad_titles.end());
    if (unknown > 0) {
        Note n;
        n.head = plural(unknown, "game doesn't", "games don't") + " match a known good dump";
        n.lines.push_back("They might be a bad or modified copy. Hacks and translations are fine.");
        for (size_t i = 0; i < bad_titles.size() && i < 4; ++i) n.lines.push_back(bad_titles[i]);
        if (bad_titles.size() > 4) n.lines.push_back("and " + std::to_string(bad_titles.size() - 4) + " more");
        n.col = ui::WARN;
        V.notes.push_back(std::move(n));
    }
    if (good + unknown > 0) {
        V.summary = "Checked " + plural(good + unknown, "game", "games") + ": " + std::to_string(good) + " match a known good dump" +
                    (unknown ? ", " + std::to_string(unknown) + " don't." : ".");
        V.ever_checked = true;
    }

    // 2. missing BIOS files
    if (V.ps1_missing || (V.shot && !V.ps1_missing && std::string(std::getenv("HUB_SHOT_VERIFY")) == "card")) {
        Note n;
        n.head = "PlayStation BIOS not found";
        n.lines.push_back("PlayStation games need a BIOS file from your own console. Pick it in Settings.");
        n.col = ui::WARN;
        V.notes.push_back(std::move(n));
    }
    if (V.ps2_missing) {
        Note n;
        n.head = "PlayStation 2 BIOS not found";
        n.lines.push_back("PlayStation 2 games need a BIOS file (scph*.bin) from your own console. Pick its folder in Settings.");
        n.col = ui::WARN;
        V.notes.push_back(std::move(n));
    }
    for (const std::string& s : V.optional_missing)
        V.optional_lines.push_back("Optional: " + s + " BIOS not found. Only needed for the real startup screen; games run without it.");

    // 3. offline or unreadable library folders
    for (const hub::FolderScanStatus& st : hub::scan_folder_status()) {
        if (st.complete) continue;
        Note n;
        n.head = "Folder " + st.folder;
        std::string line = library_folder_status_line(st.folder);
        if (!line.empty()) n.lines.push_back(line);
        n.col = ui::WARN;
        V.notes.push_back(std::move(n));
    }
    if (V.shot && hub::scan_folder_status().empty()) {
        Note n;
        n.head = "Folder D:\\Roms";
        n.lines.push_back("Drive not connected. Showing 42 games from the last scan.");
        n.col = ui::WARN;
        V.notes.push_back(std::move(n));
    }

    // 4. duplicates
    std::vector<std::string> dup_titles;
    for (const auto& kv : A.dupes) {
        if (kv.second.size() < 2) continue;
        const hub::GameEntry* e = find_entry(kv.first);
        dup_titles.push_back(std::string(e ? e->title : kv.first) + "  (" + std::to_string(kv.second.size()) + " copies)");
    }
    if (!dup_titles.empty()) {
        std::sort(dup_titles.begin(), dup_titles.end());
        Note n;
        n.head = plural((int)dup_titles.size(), "game has", "games have") + " more than one copy";
        n.lines.push_back("Open a game's page to choose which copy to use.");
        for (size_t i = 0; i < dup_titles.size() && i < 4; ++i) n.lines.push_back(dup_titles[i]);
        if (dup_titles.size() > 4) n.lines.push_back("and " + std::to_string(dup_titles.size() - 4) + " more");
        n.col = ui::TEXT2;
        V.notes.push_back(std::move(n));
    }

    // disc games cannot be hashed cheaply: say so rather than leave them out silently
    int discs = 0;
    for (const hub::GameEntry& e : A.lib)
        if (e.key != kNativeKey && (e.system == hub::System::PS1 || e.system == hub::System::PS2 || e.system == hub::System::GC || e.system == hub::System::WiiU ||
                                    e.system == hub::System::N3DS || e.system == hub::System::PSP)) ++discs;
    if (discs > 0 && V.ever_checked)
        V.summary += (V.summary.empty() ? "" : "  ") + std::string("Not checked: ") + plural(discs, "disc game", "disc games") + " (PlayStation, GameCube, Wii U, 3DS, PSP).";
    V.cartridges = (int)candidates().size();
}

void start_check(bool cache_only) {
    const vf::KnownDb& db = vf::known_db();
    if (!db.loaded()) { if (!cache_only) set_notice("The list of known good dumps is missing, so games can't be checked.", 6.0); return; }
    V.runner.start(candidates(), &db, cache_file(), cache_only);
    V.was_running = true;
}

void tick() {
    shot_seed();
    // finished a run: the verdicts changed
    const bool running = V.runner.running();
    if (V.was_running && !running) { V.refreshed = -100.0; V.was_running = false; }
    // once after the first scan: pick up verdicts saved by an earlier check, without reading a single ROM
    if (!V.cache_pass_done && A.scanned_once && !A.scanning && !V.shot && !g_headless) {
        V.cache_pass_done = true;
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::path(cache_file()), ec)) start_check(true);
    }
    const double now = ImGui::GetTime();
    if (now - V.refreshed >= (running ? 1.0 : 2.0)) { V.refreshed = now; rebuild_notes(); }
}

}  // namespace

void dumpwarn_details_section(const hub::GameEntry& e) {   // an incomplete Switch dump: say so plainly, with what to do, at the TOP of the About tab
    dc::Result r;
    if (!dump_result(e, &r)) return;
    ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextWrapped("%s", dc::warning_text(r).c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, ui::sp(0.5f)));
}

void verify_details_section(const hub::GameEntry& e) {
    if (!vf::supported(e.system) || is_hack(e)) return;   // discs and hacks: nothing to say
    shot_seed();
    vf::Result r;
    if (!lookup(e, &r)) return;                            // not checked yet: nothing
    if (r.v == vf::Verdict::Good) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::GOOD);
        ImGui::TextUnformatted("Verified good dump");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && !r.title.empty()) ImGui::SetTooltip("Matches the known dump \"%s\"", r.title.c_str());
    } else if (r.v == vf::Verdict::Unknown) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
        ImGui::TextUnformatted("Not a known dump");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("It might be a bad or modified copy. Hacks and translations are fine.");
    }
}

void verify_library_card() {
    using ui::sp;
    tick();
    const bool running = V.runner.running();
    const int counted = (int)V.notes.size();
    const bool offer = !V.ever_checked && V.cartridges > 0;   // the way in: until the first check, a quiet one-line card
    if (!running && counted == 0 && !offer && V.summary.empty()) return;   // nothing to say
    if (A.lib.empty()) return;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::PANEL);
    ImGui::PushStyleColor(ImGuiCol_Border, ui::HAIRLINE);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, sp(1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(1.5f), sp(1.f)));
    ImGui::BeginChild("##verify_card", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);

    std::string label;
    if (running) label = "Library health: checking...";
    else if (counted > 0) label = "Library health: " + plural(counted, "note", "notes");
    else if (V.ever_checked) label = "Library health: all good";
    else label = "Library health";
    if (ui::chip(label.c_str(), V.expanded, counted > 0 ? ui::WARN : ui::GOOD)) V.expanded = !V.expanded;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(V.expanded ? "Hide the details" : "Show the details");

    ImGui::SameLine(0, sp(1.5f));
    if (running) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Checking %d of %d", std::min(V.runner.done() + 1, V.runner.total()), V.runner.total());
        ImGui::SameLine(0, sp(1.5f));
        if (ui::button("Stop", ImVec2(0, sp(3.5f)))) V.runner.cancel();
    } else if (ui::button("Verify my games", ImVec2(0, sp(3.5f)), false, !A.scanning && V.cartridges > 0, ui::Icon::Check)) {
        start_check(false);
    }
    if (ImGui::IsItemHovered() && !running) ImGui::SetTooltip("Compare your cartridge games (N64, DS, GBA, GB, GBC, NES, SNES) with the known good dumps. Nothing is changed or sent anywhere.");

    if (V.expanded) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, sp(0.25f)));   // a compact list: the grid still needs the room
        ImGui::Dummy(ImVec2(0, sp(0.25f)));
        if (!V.summary.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextWrapped("%s", V.summary.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, sp(0.5f)));
        } else if (!V.ever_checked) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextWrapped("Not checked yet. \"Verify my games\" compares your cartridge games with the known good dumps. It only reads your files.");
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, sp(0.5f)));
        }
        for (const Note& n : V.notes) {
            ImGui::PushStyleColor(ImGuiCol_Text, n.col ? n.col : ui::TEXT);
            ImGui::Bullet();
            ImGui::PopStyleColor();
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextUnformatted(n.head.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Indent(sp(2.f));
            ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
            ImGui::PushTextWrapPos(0.f);
            for (const std::string& l : n.lines) ImGui::TextWrapped("%s", l.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::Unindent(sp(2.f));
            ImGui::Dummy(ImVec2(0, sp(0.25f)));
        }
        if (!V.optional_lines.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
            ImGui::PushTextWrapPos(0.f);
            for (const std::string& l : V.optional_lines) ImGui::TextWrapped("%s", l.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
}
