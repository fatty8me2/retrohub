/* Plan 4 Wave 3, X1: Transfer Pak (offline). A Game Boy / Game Boy Color game from the library plugs into controller 1 of Pokemon Stadium 1 and 2,
 * Mario Golf, Mario Tennis and Perfect Dark. Pure rules: transferpak_logic.h. n64run side: --tpak-rom / --tpak-save (src/n64run/frontend.cpp).
 *
 * The save is the exact vault battery file retrorun uses for that GB game ("<stem>.srm" in battery_dir). It is backed up before the run (last 3, in
 * <data_dir>\tpak_backups, outside the vault so they are not synced), and when the run ends the GB game gets the same bookkeeping as a GB play:
 * meta.json touched, history snapshot, cloud push queued. */
#include "app.h"
#include "wave3_hooks.h"
#include "transferpak_logic.h"
#include "savestore.h"
#include "json.h"
#include "winutil.h"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;
namespace tp = hubui::tpak;

namespace {

std::string choices_path() { return hub::data_dir() + "transferpak.json"; }

std::map<std::string, std::string>& choices() {   // N64 game key -> GB game key ("" = None)
    static std::map<std::string, std::string> m;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::ifstream f(fs::u8path(choices_path()), std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        json::Value v;
        if (f && json::parse(ss.str(), &v))
            if (const json::Value* c = v.get("choices"))
                for (auto& kv : c->o) if (kv.second.is_string()) m[kv.first] = kv.second.s;
    }
    return m;
}

void save_choices() {
    json::Value root = json::make_object(), c = json::make_object();
    for (auto& kv : choices()) c.o.push_back({kv.first, json::make_string(kv.second)});
    root.o.push_back({"choices", c});
    std::string tmp = choices_path() + ".tmp";
    { std::ofstream f(fs::u8path(tmp), std::ios::binary | std::ios::trunc); f << json::stringify(root); if (!f) return; }
    std::error_code ec;
    fs::rename(fs::u8path(tmp), fs::u8path(choices_path()), ec);
    if (ec) { fs::remove(fs::u8path(choices_path()), ec); fs::rename(fs::u8path(tmp), fs::u8path(choices_path()), ec); }
}

std::vector<tp::GbGame> gb_library() {
    std::vector<tp::GbGame> v;
    for (const hub::GameEntry& g : A.lib) {
        if (g.system != hub::System::GB && g.system != hub::System::GBC) continue;
        if (!g.hack_patch.empty()) continue;   // a hack's path is its base game's file
        v.push_back({g.key, g.title, g.catalog_id, g.system == hub::System::GBC});
    }
    return v;
}

const hub::GameEntry* chosen_entry(const hub::GameEntry& n64) {
    auto fits = tp::compatible(n64.catalog_id, gb_library());
    auto it = choices().find(n64.key);
    const std::string stored = it == choices().end() ? std::string() : it->second;
    std::string key = tp::resolve_choice(it == choices().end() ? nullptr : &stored, fits);
    return key.empty() ? nullptr : find_entry(key);
}

std::vector<std::string> list_names(const std::string& dir) {
    std::vector<std::string> v;
    std::error_code ec;
    for (auto& de : fs::directory_iterator(fs::u8path(dir), ec)) if (de.is_regular_file(ec)) v.push_back(de.path().filename().u8string());
    return v;
}

std::string vault_save_path(const hub::GameEntry& gb) {
    std::string dir = no_slash(hub::saves::battery_dir(sys_short(gb), gb.key));
    return dir + "\\" + tp::save_file_name(gb.path, gb.save_stem, list_names(dir));
}

bool same_bytes(const fs::path& a, const fs::path& b) {
    std::ifstream x(a, std::ios::binary), y(b, std::ios::binary);
    if (!x || !y) return false;
    std::stringstream sa, sb; sa << x.rdbuf(); sb << y.rdbuf();
    return sa.str() == sb.str();
}

/* Copy the existing save aside (skipped when the newest backup is identical), keep the last 3. False = could not back up. */
bool backup_save(const hub::GameEntry& gb, const std::string& save) {
    std::error_code ec;
    if (!fs::exists(fs::u8path(save), ec)) return true;   // nothing to lose yet
    fs::path dir = fs::u8path(hub::data_dir() + "tpak_backups\\" + gb.key);
    fs::create_directories(dir, ec);
    std::string name = fs::u8path(save).filename().u8string();
    std::vector<std::string> have;
    for (auto& n : list_names(dir.u8string())) if (n.rfind(name + ".", 0) == 0) have.push_back(n);
    std::sort(have.begin(), have.end());
    if (!have.empty() && same_bytes(fs::u8path(save), dir / fs::u8path(have.back()))) return true;
    char stamp[32]; std::time_t t = std::time(nullptr); std::tm lt{}; localtime_s(&lt, &t);
    std::strftime(stamp, sizeof stamp, "%Y%m%d%H%M%S", &lt);
    std::string bn = tp::backup_name(name, stamp);
    for (int i = 1; fs::exists(dir / fs::u8path(bn), ec) && i < 100; ++i) bn = tp::backup_name(name, std::string(stamp) + "-" + std::to_string(i));   // same second: never overwrite a backup
    fs::copy_file(fs::u8path(save), dir / fs::u8path(bn), fs::copy_options::none, ec);
    if (ec) return false;
    have.clear();
    for (auto& n : list_names(dir.u8string())) if (n.rfind(name + ".", 0) == 0) have.push_back(n);
    for (auto& d : tp::backups_to_prune(have)) fs::remove(dir / fs::u8path(d), ec);
    return true;
}

std::vector<uint8_t> read_header(const std::string& rom) {
    std::vector<uint8_t> h(0x150, 0);
    std::ifstream f(fs::u8path(rom), std::ios::binary);
    f.read((char*)h.data(), (std::streamsize)h.size());
    if (f.gcount() < 0x14A) h.clear();
    return h;
}

struct Pending { bool on = false; std::string n64_key, sys, key, title; } g_pending;

}  // namespace

std::string transferpak_launch_args(const hub::GameEntry& e) {
    g_pending = Pending();
    if (e.system != hub::System::N64 || !tp::uses_transfer_pak(e.catalog_id)) return std::string();
    const hub::GameEntry* gb = chosen_entry(e);
    if (!gb || gb->path.empty() || rom_missing(*gb)) return std::string();
    std::string save = vault_save_path(*gb);
    std::error_code ec;
    const bool exists = fs::exists(fs::u8path(save), ec);
    const size_t expected = tp::expected_ram_size(read_header(gb->path));
    if (!tp::save_size_ok(expected, exists, exists ? (size_t)fs::file_size(fs::u8path(save), ec) : 0)) {
        set_notice(gb->title + ": its save is not the size the Transfer Pak expects, so it was left out to keep the save safe.", 12.0);
        return std::string();
    }
    if (!backup_save(*gb, save)) {
        set_notice("Could not back up the " + gb->title + " save, so the Transfer Pak was left out.", 12.0);
        return std::string();
    }
    fs::create_directories(fs::u8path(save).parent_path(), ec);
    g_pending = {true, e.key, sys_short(*gb), gb->key, gb->title};
    return tp::launch_args(gb->path, save, [](const std::string& s) { return winutil::quote_arg(s); });
}

/* Called when the game process ended (launch.cpp poll_process). */
void transferpak_run_finished() {
    Pending p = g_pending;
    g_pending = Pending();
    if (!p.on || A.run.key != p.n64_key) return;
    hub::saves::touch_game(p.sys, p.key, p.title);
    hub::saves::history_snapshot_game(p.sys, p.key);
    hub::saves::GameId gid{p.sys, p.key};
    if (!cloud_bg_run(gid.gkey(), [gid]() { hub::saves::cloud_game_exited(gid); })) hub::saves::cloud_game_exited(gid);   // queues the push
}

void transferpak_details_section(const hub::GameEntry& e) {
    if (e.system != hub::System::N64 || !tp::uses_transfer_pak(e.catalog_id)) return;
    auto fits = tp::compatible(e.catalog_id, gb_library());
    auto it = choices().find(e.key);
    const std::string stored = it == choices().end() ? std::string() : it->second;
    const std::string cur = tp::resolve_choice(it == choices().end() ? nullptr : &stored, fits);
    std::string label = "None";
    for (auto& g : fits) if (g.key == cur) label = g.title;
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    ImGui::TextUnformatted("Game Boy cartridge");
    ImGui::PushID("tpak");
    ImGui::SetNextItemWidth(sp(26.f));
    if (ImGui::BeginCombo("##gbcart", label.c_str())) {
        if (ImGui::Selectable("None", cur.empty())) { choices()[e.key] = ""; save_choices(); }
        for (auto& g : fits) {
            ImGui::PushID(g.key.c_str());
            if (ImGui::Selectable(g.title.c_str(), g.key == cur)) { choices()[e.key] = g.key; save_choices(); }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
    ImGui::PushTextWrapPos(0.f);
    if (fits.empty()) ImGui::TextWrapped("No matching Game Boy game in your library yet. Add one to plug it into controller 1.");
    else ImGui::TextWrapped("Plugged into controller 1's Transfer Pak. Its save is backed up first and kept in your save vault. Offline play only.");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    if (g_headless && std::getenv("HUB_SHOT_TPAK")) ImGui::SetScrollHereY(1.0f);   // screenshot aid: scroll the row into view
}
