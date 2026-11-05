/* Plan 4 Wave 3 X3: Pal Park / DS Slot-2. A GBA game from the library sits in the DS's GBA slot (retrorun --slot2-rom / --slot2-save), so Pokemon
 * Diamond / Pearl / Platinum / HeartGold / SoulSilver can move Pokemon over from the Gen 3 games. Offline only. Logic: palpark_logic.h. */
#include "app.h"
#include "wave3_hooks.h"
#include "palpark_logic.h"
#include "savestore.h"
#include "cloud_engine.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {

namespace fs = std::filesystem;
namespace pp = hubui::palpark;

std::string choices_path() { return hub::data_dir() + "palpark.json"; }

/* The stored choices, loaded once (and again after we changed them). */
pp::Choices& choices() {
    static pp::Choices c;
    static bool loaded = false;
    if (!loaded) { loaded = true; c = pp::parse_choices(read_text_file(choices_path())); }
    return c;
}

void save_choices() {
    const std::string text = pp::write_choices(choices());
    const std::string path = choices_path();
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(fs::u8path(tmp), std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << text;
        if (!f.flush()) return;
    }
    std::error_code ec;
    fs::rename(fs::u8path(tmp), fs::u8path(path), ec);
    if (ec) { fs::remove(fs::u8path(path), ec); fs::rename(fs::u8path(tmp), fs::u8path(path), ec); }
}

/* The player's own GBA games (ROM hacks excluded: their saves are separate and their path is the unpatched base game). */
std::vector<pp::Gba> gba_library() {
    std::vector<pp::Gba> v;
    for (const hub::GameEntry& g : A.lib) {
        if (g.system != hub::System::GBA || !g.hack_patch.empty() || g.path.empty()) continue;
        v.push_back({g.key, g.title, g.catalog_id, g.path, g.save_stem});
    }
    return v;
}

bool ds_slot2_game(const hub::GameEntry& e) { return e.system == hub::System::NDS && pp::ds_uses_slot2(e.catalog_id); }

/* The GBA game that was in the slot for the run that is going (set at launch, consumed when the process ends). */
struct Pending { std::string ds_key, gba_key, gba_title; } g_pending;

}  // namespace

std::string palpark_launch_args(const hub::GameEntry& e) {
    g_pending = Pending();
    if (!ds_slot2_game(e)) return std::string();
    const std::vector<pp::Gba> lib = gba_library();
    const pp::Gba* g = pp::find_gba(lib, pp::get_choice(choices(), e.key));
    if (!g || !file_exists(g->path)) return std::string();   // None, the game left the library, or its ROM is gone: start the DS game as usual
    /* The GBA game's exact vault battery file, the one the GBA runner itself reads and writes on every PC. */
    const std::string dir = hub::saves::battery_dir("gba", g->key);
    std::vector<std::string> names;
    std::error_code ec;
    for (auto& de : fs::directory_iterator(fs::u8path(dir), ec)) if (de.is_regular_file(ec)) names.push_back(de.path().filename().u8string());
    const std::string file = pp::pick_save_file(names, pp::srm_stem(*g));
    const std::string args = pp::build_args(g->path, file.empty() ? std::string() : dir + file);
    if (!args.empty()) g_pending = {e.key, g->key, g->title};
    return args;
}

/* Called by poll_process() when the game process has ended: the GBA game's save may have changed, so it is queued for the cloud like a normal play. */
void palpark_process_ended() {
    if (g_pending.gba_key.empty()) return;
    Pending p = g_pending;
    g_pending = Pending();
    if (A.run.key != p.ds_key) return;   // not the run we armed (the launch failed, or another game ran since)
    hub::saves::touch_game("gba", p.gba_key, p.gba_title);
    hub::saves::history_snapshot_game("gba", p.gba_key);
    hub::saves::GameId cid{"gba", p.gba_key};
    if (!cloud_bg_run(cid.gkey(), [cid]() { hub::saves::cloud_game_exited(cid); }))
        hub::saves::cloud_game_exited(cid);
    A.vault_dirty = true;
}

void palpark_details_section(const hub::GameEntry& e) {
    if (!ds_slot2_game(e)) return;
    const std::vector<pp::Gba> lib = gba_library();
    const std::vector<size_t> order = pp::order_choices(lib);
    const std::string cur_key = pp::get_choice(choices(), e.key);
    const bool cur_ok = pp::find_gba(lib, cur_key) != nullptr;
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    ImGui::TextUnformatted("GBA cartridge (Slot-2)");
    ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
    ImGui::TextWrapped(order.empty()
        ? "Add a Game Boy Advance game to your library to put it in the DS's GBA slot (Pal Park)."
        : "Put a Game Boy Advance game in the DS's GBA slot. Pokemon Ruby, Sapphire, Emerald, FireRed and LeafGreen can send Pokemon over with Pal Park. "
          "Offline only. That game's save is backed up before each run.");
    ImGui::PopStyleColor();
    const float avail = ImGui::GetContentRegionAvail().x;
    float x = 0;
    size_t shown = 0;
    auto chip = [&](const std::string& label, bool cur, const std::string& gba_key, int id) {
        const float w = ImGui::CalcTextSize(label.c_str()).x + sp(4.f);
        if (id && x + w + sp(1.f) < avail) { ImGui::SameLine(0, sp(1.f)); x += w + sp(1.f); } else x = w;
        ImGui::PushID(id);
        if (button(label.c_str(), ImVec2(w, sp(4.f)), cur) && !cur) {
            pp::set_choice(&choices(), e.key, gba_key);
            save_choices();
        }
        ImGui::PopID();
    };
    chip("None", !cur_ok, std::string(), 0);
    for (size_t i : order) {
        if (++shown > 24) break;
        chip(lib[i].title, cur_ok && lib[i].key == cur_key, lib[i].key, (int)shown);
    }
}

/* Screenshot aid: HUB_SHOT_PALPARK=1 swaps the library for Pokemon Diamond + a few GBA games and opens Diamond's details (Ruby already chosen, in memory only).
 * =none leaves the choice on None. Headless only; nothing is written. */
void palpark_shot_init() {
    const char* m = std::getenv("HUB_SHOT_PALPARK");
    if (!m || !g_headless) return;
    A.lib.clear();
    auto add = [](hub::System sys, const char* key, const char* title, const char* cat) {
        hub::GameEntry g;
        g.system = sys; g.key = key; g.title = title; g.catalog_id = cat; g.path = std::string("C:/fake/") + key + ".rom";
        A.lib.push_back(g);
    };
    add(hub::System::NDS, "nds:cpuE0", "Pokemon Diamond", "pokediamond");
    add(hub::System::GBA, "gba:mfusion", "Metroid Fusion", "mfusion");
    add(hub::System::GBA, "gba:pokeemerald", "Pokemon Emerald", "pokeemerald");
    add(hub::System::GBA, "gba:pokeruby", "Pokemon Ruby", "pokeruby");
    add(hub::System::GBA, "gba:aw", "Advance Wars", "");
    add(hub::System::NDS, "nds:mkds", "Mario Kart DS", "");
    if (std::string(m) != "none") pp::set_choice(&choices(), "nds:cpuE0", "gba:pokeruby");
    A.sel_game = "nds:cpuE0";
    A.show_details = true;
}
