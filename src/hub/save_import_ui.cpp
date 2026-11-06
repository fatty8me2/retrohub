/* Plan 4 Wave 3 L4: "Bring your saves from other emulators" (Saves page).
 * Looks (in the background) for the save files RetroArch, Project64, Dolphin, PCSX2, mGBA, VisualBoyAdvance and melonDS left on
 * this PC, next to the library's ROMs and in any folder or file the player picks, matches them to library games and shows a preview.
 * Importing writes exactly the vault file the runner reads (src/common/save_import.*), backs up what it replaces, and queues a
 * cloud push. Screenshot aid: HUB_SHOT_SAVEIMPORT=card|preview|done|none (fake data, nothing is scanned or written). */
#include "app.h"
#include "wave3_hooks.h"
#include "save_import.h"
#include "osk.h"

#include <atomic>
#include <ctime>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace si = hub::saveimport;

namespace {

struct ScanOut {
    std::atomic<bool> done{false};
    std::vector<si::Row> rows;
    std::vector<si::GameRef> games;
    std::vector<std::string> sources;   // "RetroArch", ... that were present on this PC
    int states = 0;                     // other emulators' save states left alone
    std::string ini;
};
struct ApplyOut {
    std::atomic<bool> done{false};
    int games = 0, written = 0, backed_up = 0;
    std::vector<std::string> errors;
    std::vector<std::pair<std::string, std::string>> changed;   // system, key
    std::vector<std::string> titles;
};

struct State {
    bool started = false, open = false, shot = false;
    std::shared_ptr<ScanOut> scan;
    std::shared_ptr<ApplyOut> apply;
    std::vector<si::Row> rows;
    std::vector<si::GameRef> games;
    std::vector<std::string> sources;
    int states = 0;
    std::string ini;
    std::vector<si::Found> extra;          // picked by hand
    std::string note, result;
    bool result_ok = true;
} G;

std::string slash_trim(std::string s) { while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back(); return s; }

std::string date_text(int64_t t) {
    if (t <= 0) return "unknown date";
    std::time_t tt = (std::time_t)t;
    std::tm lt{};
    localtime_s(&lt, &tt);
    static const char* mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char b[40];
    std::snprintf(b, sizeof b, "%d %s %d, %02d:%02d", lt.tm_mday, mon[lt.tm_mon % 12], lt.tm_year + 1900, lt.tm_hour, lt.tm_min);
    return b;
}
std::string size_text(int64_t n) {
    char b[32];
    if (n < 1024) std::snprintf(b, sizeof b, "%lld B", (long long)n);
    else if (n < (1 << 20)) std::snprintf(b, sizeof b, "%.0f KB", n / 1024.0);
    else std::snprintf(b, sizeof b, "%.1f MB", n / 1048576.0);
    return b;
}

std::string n64_ini_text() {
    for (const std::string& p : {hub::exe_dir() + "emu\\mupen64plus.ini", find_upward("deps\\emu\\mupen64plus.ini"), find_upward("emu\\mupen64plus.ini")}) {
        if (p.empty() || !file_exists(p)) continue;
        std::ifstream f(std::filesystem::u8path(p), std::ios::binary);
        if (f) return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    return "";
}

std::vector<si::GameRef> library_games() {
    std::vector<si::GameRef> v;
    for (const hub::GameEntry& e : A.lib) {
        if (is_native(e) || !e.hack_patch.empty()) continue;   // hacks keep their saves under their own key: never guess
        std::string sys = sys_short(e);
        if (sys.empty() || sys == "unknown") continue;
        si::GameRef g;
        g.key = e.key;
        g.system = sys;
        g.title = e.title;
        g.rom_path = e.path;
        g.serial = e.serial;
        g.save_stem = !e.save_stem.empty() ? e.save_stem : std::filesystem::u8path(e.path).stem().u8string();   // what retrorun names the .srm
        v.push_back(std::move(g));
    }
    return v;
}

si::Env make_env(const std::string& docs) {
    si::Env env;
    env.appdata = env_var(L"APPDATA");
    env.localappdata = env_var(L"LOCALAPPDATA");
    env.userprofile = env_var(L"USERPROFILE");
    env.documents = docs;
    env.programfiles = env_var(L"ProgramFiles");
    env.programfiles_x86 = env_var(L"ProgramFiles(x86)");
    env.system_drive = env_var(L"SystemDrive");
    env.own_data_dir = hub::data_dir();
    return env;
}

si::Ctx make_ctx(const std::string& ini) {
    si::Ctx c;
    c.n64_ini = ini;
    c.battery_dir = [](const si::GameRef& g) { return hub::saves::battery_dir(g.system, g.key); };
    return c;
}

void si_start_scan() {
    G.started = true;
    G.scan = std::make_shared<ScanOut>();
    auto out = G.scan;
    std::vector<si::GameRef> games = library_games();
    std::vector<si::Found> extra = G.extra;
    std::string up = env_var(L"USERPROFILE");
    std::vector<std::string> docs = {up.empty() ? std::string() : up + "\\Documents"};
    for (const wchar_t* n : {L"OneDrive", L"OneDriveConsumer"}) { std::string od = env_var(n); if (!od.empty()) docs.push_back(od + "\\Documents"); }
    std::string ini = n64_ini_text();
    std::thread([out, games, extra, docs, ini]() {
        std::vector<si::Folder> folders;
        std::set<std::string> seen;
        for (const std::string& d : docs) {
            for (auto& f : si::detect_folders(make_env(d))) if (seen.insert(f.dir).second) folders.push_back(f);
        }
        std::set<std::string> src_names;
        std::vector<si::Found> all;
        std::set<std::string> paths;
        auto add = [&](const si::Found& f) { if (paths.insert(f.path).second) all.push_back(f); };
        for (auto& f : folders) {
            auto v = si::scan_folder(f);
            if (!v.empty()) src_names.insert(si::source_name(f.src));
            for (auto& x : v) add(x);
        }
        std::set<std::string> stems_done;
        for (const si::GameRef& g : games) {
            if (g.rom_path.empty()) continue;
            std::string stem = g.save_stem.empty() ? std::filesystem::u8path(g.rom_path).stem().u8string() : g.save_stem;
            if (!stems_done.insert(g.rom_path + "|" + stem).second) continue;
            for (auto& x : si::scan_next_to_rom(g.rom_path, stem)) add(x);
        }
        for (auto& x : extra) add(x);
        si::Ctx ctx = make_ctx(ini);
        out->rows = si::build_rows(all, games, ctx);
        for (auto& r : out->rows) if (r.problem.empty()) src_names.insert(si::source_name(r.src()));
        out->sources.assign(src_names.begin(), src_names.end());
        for (auto& f : folders) if (f.src == si::Source::RetroArch) {
            std::string base = std::filesystem::u8path(f.dir).parent_path().u8string();
            out->states += si::count_files_with_ext(base + "\\states", ".state");
        }
        out->games = games;
        out->ini = ini;
        out->done = true;
    }).detach();
}

int count_good(const std::vector<si::Row>& rows) { int n = 0; for (auto& r : rows) if (r.problem.empty()) ++n; return n; }
int count_selected(const std::vector<si::Row>& rows) { int n = 0; for (auto& r : rows) if (r.selected && r.problem.empty()) ++n; return n; }

std::string sources_text(const std::vector<std::string>& s) {
    std::string t;
    for (size_t i = 0; i < s.size(); ++i) t += (i ? (i + 1 == s.size() ? " and " : ", ") : "") + s[i];
    return t;
}

void fake_data(const std::string& mode) {   // HUB_SHOT_SAVEIMPORT: made-up rows for screenshots
    G.started = true;
    G.games.clear();
    G.rows.clear();
    auto game = [&](const char* title, const char* sys) { si::GameRef g; g.title = title; g.system = sys; g.key = std::string(sys) + ":" + title; G.games.push_back(g); return G.games.size() - 1; };
    const int64_t now = (int64_t)std::time(nullptr);
    auto row = [&](size_t gi, si::Source s, const char* file, int64_t size, int64_t age, bool vault, int64_t vsize, int64_t vage, bool sel) {
        si::Row r;
        si::Found f; f.src = s; f.name = file; f.size = size; f.mtime = now - age; f.ext = ".srm";
        r.files.push_back(f); r.game = gi; r.src_mtime = f.mtime; r.src_size = size; r.selected = sel;
        r.vault_has = vault; r.vault_size = vsize; r.vault_mtime = now - vage;
        r.newer = !vault ? si::Row::NoVault : (r.src_mtime > r.vault_mtime ? si::Row::SourceNewer : si::Row::VaultNewer);
        G.rows.push_back(r);
        return G.rows.size() - 1;
    };
    row(game("Pokemon Emerald", "gba"), si::Source::RetroArch, "Pokemon Emerald (USA).srm", 131072, 3600 * 5, true, 131072, 86400 * 12, true);
    row(game("Super Mario 64", "n64"), si::Source::Project64, "SUPER MARIO 64.eep", 512, 86400 * 40, true, 2048, 86400 * 3, false);
    row(game("Metroid Fusion", "gba"), si::Source::MGBA, "Metroid Fusion.sav", 65536, 86400 * 2, false, 0, 0, true);
    row(game("Super Smash Bros. Melee", "gc"), si::Source::Dolphin, "MemoryCardA.USA.raw", 524288, 86400 * 9, false, 0, 0, true);
    row(game("Kingdom Hearts", "ps2"), si::Source::PCSX2, "Mcd001.ps2", 8388608, 86400 * 20, true, 8388608, 86400 * 30, true);
    row(game("Pokemon Black", "nds"), si::Source::MelonDS, "Pokemon Black.sav", 524288, 86400 * 6, false, 0, 0, true);
    size_t amb = row(game("Tetris", "gb"), si::Source::VBA, "Tetris.sav", 32768, 86400 * 60, false, 0, 0, false);
    G.rows[amb].ambiguous = true;
    size_t bad = row(game("Mario Kart 64", "n64"), si::Source::Project64, "MARIOKART64.sra", 1234, 86400 * 90, false, 0, 0, false);
    G.rows[bad].problem = "That is not an N64 SRAM save.";
    G.sources = {"Dolphin", "melonDS", "mGBA", "PCSX2", "Project64", "RetroArch", "VisualBoyAdvance"};
    G.states = 4;
    G.open = mode == "preview" || mode == "done";
    if (mode == "done") { G.result = "Imported 5 saves. The ones they replaced were backed up first, and the cloud will pick them up."; G.result_ok = true; }
    if (mode == "none") { G.rows.clear(); G.sources.clear(); G.states = 0; }
}

void si_poll() {
    if (G.scan && G.scan->done) {
        auto s = G.scan;
        G.scan.reset();
        G.rows = std::move(s->rows);
        G.games = std::move(s->games);
        G.sources = std::move(s->sources);
        G.states = s->states;
        G.ini = std::move(s->ini);
        if (G.rows.empty()) G.open = false;
    }
    if (G.apply && G.apply->done) {
        auto a = G.apply;
        G.apply.reset();
        for (auto& c : a->changed) {   // the vault changed outside a run: refresh meta.json and queue the cloud push (saves-F13)
            std::string title = c.second;
            for (auto& e : A.lib) if (e.key == c.second) title = e.title;
            hub::saves::touch_game(c.first, c.second, title);
            hub::saves::cloud_game_changed(hub::saves::GameId{c.first, c.second});
        }
        A.vault_dirty = true;
        if (!a->errors.empty()) {
            G.result = a->errors[0] + (a->games ? " The others were imported." : "");
            G.result_ok = false;
        } else {
            G.result = "Imported " + std::to_string(a->written) + (a->written == 1 ? " save" : " saves") + " for " + std::to_string(a->games) +
                       (a->games == 1 ? " game" : " games") + ". " + (a->backed_up ? "The ones they replaced were backed up first. " : "") +
                       "They will go to your cloud saves like any other save.";
            G.result_ok = true;
        }
        if (a->written) set_toast("Imported saves for " + std::to_string(a->games) + (a->games == 1 ? " game." : " games."), true, 5.0);
        si_start_scan();   // refresh: imported rows now read "already the same"
    }
}

void start_import() {
    auto out = std::make_shared<ApplyOut>();
    G.apply = out;
    G.result.clear();
    std::vector<si::Row> rows;
    for (auto& r : G.rows) if (r.selected && r.problem.empty()) rows.push_back(r);
    std::vector<si::GameRef> games = G.games;
    std::string ini = G.ini, vault = hub::saves::vault_dir();
    std::thread([out, rows, games, ini, vault]() {
        si::Ctx ctx = make_ctx(ini);
        std::set<std::string> done_games;
        for (const si::Row& r : rows) {
            const si::GameRef& g = games[r.game];
            si::ApplyReport rep = si::apply_row(r, g, ctx, vault, (int64_t)std::time(nullptr));
            out->written += rep.written;
            out->backed_up += rep.backed_up;
            if (rep.written && done_games.insert(g.key).second) { out->changed.push_back({g.system, g.key}); ++out->games; }   // even a partly written game goes to the cloud
            if (!rep.ok) out->errors.push_back(g.title + ": " + rep.error);
        }
        out->done = true;
    }).detach();
}

/* A table cell's text on one line: too long gets "..." and the whole text in a tooltip. */
void cell_text(const std::string& t) {
    const std::string shown = ui::ellipsize(ui::F.body, ui::F.body->FontSize, t, ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(shown.c_str());
    if (shown != t && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t.c_str());
}

void draw_table(bool can_act) {
    const float lh = ImGui::GetTextLineHeight();
    if (!ImGui::BeginTable("si_tbl", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) return;
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, lh * 1.8f);
    ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch, 2.4f);
    ImGui::TableSetupColumn("Found in", ImGuiTableColumnFlags_WidthStretch, 1.5f);
    ImGui::TableSetupColumn("Their save", ImGuiTableColumnFlags_WidthStretch, 1.9f);
    ImGui::TableSetupColumn("Your save here", ImGuiTableColumnFlags_WidthStretch, 1.9f);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < G.rows.size(); ++i) {
        si::Row& r = G.rows[i];
        const si::GameRef& g = G.games[r.game];
        ImGui::PushID((int)i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const bool can_tick = r.problem.empty() && !r.same_as_vault && can_act;
        ImGui::BeginDisabled(!can_tick);
        ImGui::Checkbox("##pick", &r.selected);
        ImGui::EndDisabled();
        ImGui::TableNextColumn();
        cell_text(g.title);
        std::string sub;
        if (!r.problem.empty()) sub = r.problem;
        else if (r.same_as_vault) sub = "Already the same as your save here.";
        else if (r.ambiguous) sub = "Fits more than one version of this game, so it is not ticked.";
        else if (r.newer == si::Row::VaultNewer) sub = "Your save here is newer, so it is not ticked.";
        if (!sub.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, r.problem.empty() ? ui::TEXT2 : ui::WARN);
            cell_text(sub);   // one line (an ellipsis + the full text on hover), so every row has the same height and nothing runs into the next column
            ImGui::PopStyleColor();
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(si::source_name(r.src()));
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
        cell_text(r.files.empty() ? "" : (r.files.size() > 1 ? std::to_string(r.files.size()) + " save files" : r.files[0].name));
        ImGui::PopStyleColor();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(date_text(r.src_mtime).c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
        ImGui::TextUnformatted(size_text(r.src_size).c_str());
        ImGui::PopStyleColor();
        if (r.vault_has && r.newer == si::Row::SourceNewer) { ImGui::SameLine(0, sp(0.75f)); ImGui::PushStyleColor(ImGuiCol_Text, ui::GOOD); ImGui::TextUnformatted("newer"); ImGui::PopStyleColor(); }
        ImGui::TableNextColumn();
        if (!r.vault_has) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
            ImGui::TextUnformatted("No save yet");
            ImGui::PopStyleColor();
        } else {
            ImGui::TextUnformatted(date_text(r.vault_mtime).c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
            ImGui::TextUnformatted(size_text(r.vault_size).c_str());
            ImGui::PopStyleColor();
            if (r.newer == si::Row::VaultNewer) { ImGui::SameLine(0, sp(0.75f)); ImGui::PushStyleColor(ImGuiCol_Text, ui::GOOD); ImGui::TextUnformatted("newer"); ImGui::PopStyleColor(); }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

}  // namespace

void save_import_section() {
    if (!G.shot && g_headless) {
        G.shot = true;
        if (const char* m = std::getenv("HUB_SHOT_SAVEIMPORT")) fake_data(m);
    }
    si_poll();
    if (!g_headless && !G.started && !A.lib.empty() && !G.scan) si_start_scan();

    const bool busy = G.scan != nullptr || G.apply != nullptr;
    const bool game_running = A.proc != nullptr || A.job.running;
    const int good = count_good(G.rows);

    /* pickers (a folder or a single save file: the same pad-friendly browsers as elsewhere) */
    auto pickers = [&](bool primary_first) {
        (void)primary_first;
        std::string folder = osk::folder("si_folder", button("Pick a folder...", ImVec2(0, 0), false, !busy));
        ImGui::SameLine();
        std::string file = osk::file("si_file", button("Pick a save file...", ImVec2(0, 0), false, !busy),
                                     {".srm", ".sav", ".eep", ".sra", ".fla", ".mpk", ".gci", ".raw", ".gcp", ".mcd", ".ps2"});
        if (!folder.empty()) {
            std::string f = slash_trim(folder);
            for (auto& x : si::scan_folder({si::Source::Picked, f})) G.extra.push_back(x);
            G.note = "Looked in " + hubui::short_path(f) + ".";
            si_start_scan();
        }
        if (!file.empty()) {
            si::Found x;
            if (si::found_from_path(file, si::Source::Picked, &x)) { G.extra.push_back(x); G.note = "Looked at " + x.name + "."; si_start_scan(); }
            else G.note = "That is not a save file RetroHub knows how to read.";
        }
    };

    if (good == 0 && !G.open) {
        /* nothing found: just a small button */
        ImGui::BeginGroup();
        std::string folder = osk::folder("si_folder", button("Bring saves from another emulator...", ImVec2(0, 0), false, !busy));
        if (!folder.empty()) {
            std::string f = slash_trim(folder);
            for (auto& x : si::scan_folder({si::Source::Picked, f})) G.extra.push_back(x);
            G.note = "Nothing in " + hubui::short_path(f) + " matches your games yet.";
            si_start_scan();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pick the folder where RetroArch, Project64, Dolphin, PCSX2 or another emulator keeps its saves.");
        if (!G.note.empty()) { ImGui::SameLine(0, sp(1.f)); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", G.note.c_str()); }
        ImGui::EndGroup();
        if (!G.result.empty()) result_line(G.result, G.result_ok);
        ImGui::Dummy(ImVec2(0, sp(0.75f)));
        return;
    }

    begin_panel("si_card");
    section_title("Bring your saves from other emulators");
    if (!G.open) {
        std::string s = "Found " + std::to_string(good) + (good == 1 ? " save" : " saves") + " from " + sources_text(G.sources) + " that match games in your library.";
        text_wrapped_disabled(s + " You choose what to bring over, and nothing is replaced without a backup.");
        if (button("Look at them...", ImVec2(0, 0), true, !busy)) { G.open = true; G.result.clear(); }
        ImGui::SameLine();
        pickers(false);
    } else {
        text_wrapped_disabled("Tick the saves you want. Only in-game saves move over. Another emulator's save states can't be used here" +
                              std::string(G.states > 0 ? " (" + std::to_string(G.states) + " were left alone)." : "."));
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        draw_table(!game_running && !busy);
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        const int n = count_selected(G.rows);
        const std::string label = n ? "Bring over " + std::to_string(n) + (n == 1 ? " save" : " saves") : std::string("Bring over");
        if (button(label.c_str(), ImVec2(0, 0), true, n > 0 && !game_running && !busy)) start_import();
        ImGui::SameLine();
        if (button("Close", ImVec2(0, 0))) G.open = false;
        ImGui::SameLine();
        pickers(false);
        if (game_running) text_wrapped_disabled("A game is running. Close it first, then bring your saves over.");
        else text_wrapped_disabled("The save you have now is copied aside first, so nothing is lost.");
    }
    if (busy) text_wrapped_disabled(G.apply ? "Bringing your saves over..." : "Looking for saves...");
    if (!G.result.empty()) result_line(G.result, G.result_ok);
    if (!G.note.empty() && !G.open) text_wrapped_disabled(G.note);
    end_panel();
    ImGui::Dummy(ImVec2(0, sp(1.f)));
}
