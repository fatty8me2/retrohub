#include "app.h"

#include "customize_logic.h"
#include "osk.h"
#include "prefs.h"
#include "savestore.h"

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "third_party/stb_image.h"

/* ---------- Make it yours: per-game rename + custom cover (L3) and custom shelves (L4) ----------
 * Data: synced prefs (common/prefs.h): renames[key], covers[key], user_shelves. This file only uses the setters; the merge is prefs.cpp'CU.
 *  - A rename is applied to A.lib[].title (so tiles, hero, details and search all show it); the scanned name is kept here
 *    (custom_original_title) for art/genre lookups and so search still matches it (ListItem::alt_title).
 *  - A custom cover replaces the box art in art_for_entry (tiles.cpp).
 *  - User shelves are appended to Home after the built-in shelves (home_shelves hook); the built-ins are not touched.
 * UI = small modals: Customize (rename / cover / reset), Screenshot cover picker, Add to shelf, Manage shelves. Every control is a
 * ui::button / osk::input, so all of it is pad-navigable. Modals are drawn where their opener lives (custom_draw(loc)) so they nest. */

namespace {

using hubui::cust::Shelves;

enum Loc { LocRoot = 0, LocDetails = 1, LocCustomize = 2, LocPicker = 3 };

struct Tex { SDL_Texture* t = nullptr; int w = 0, h = 0; std::string stamp; };

struct Popup { bool want = false; int parent = LocRoot; };

struct State {
    hub::prefs::Prefs P;
    std::string sig;
    double checked = -100;
    std::map<std::string, std::string> orig;   // key -> scanned title
    std::map<std::string, Tex> tex;            // cover / thumbnail cache by path
    Popup cust, shots, pick, mgr;
    std::string key;                           // game the Customize / picker / shots modals are about
    char name_buf[128] = {0};
    std::string name_key;
    char new_buf[64] = {0};
    char edit_buf[64] = {0};
    int edit = -1, del = -1;                   // Manage shelves: row being renamed / awaiting delete confirmation
    std::vector<std::string> shot_files;
    std::string shot_dir;
    bool shot_once = false;
};
State CU;

double now_s() { return (double)SDL_GetTicks() / 1000.0; }

std::string file_stamp(const std::string& p) {
    std::error_code ec;
    auto fp = std::filesystem::u8path(p);
    auto t = std::filesystem::last_write_time(fp, ec);
    if (ec) return std::string();
    auto sz = std::filesystem::file_size(fp, ec);
    return std::to_string((long long)t.time_since_epoch().count()) + ":" + std::to_string((long long)sz);
}

/* PNG / BMP file -> texture, cached by path + stamp (a missing or unreadable file gives null). */
Tex* tex_for(const std::string& path) {
    SDL_Renderer* ren = A.win ? SDL_GetRenderer(A.win) : nullptr;
    if (!ren || path.empty()) return nullptr;
    std::string st = file_stamp(path);
    if (st.empty()) return nullptr;
    auto it = CU.tex.find(path);
    if (it != CU.tex.end() && it->second.stamp == st) return it->second.t ? &it->second : nullptr;
    if (it != CU.tex.end() && it->second.t) SDL_DestroyTexture(it->second.t);
    if (CU.tex.size() > 64) { for (auto& kv : CU.tex) if (kv.second.t) SDL_DestroyTexture(kv.second.t); CU.tex.clear(); }
    Tex nt;
    nt.stamp = st;
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!data.empty() && data.size() < (48u << 20)) {
        const bool bmp = path.size() > 4 && _stricmp(path.c_str() + path.size() - 4, ".bmp") == 0;
        if (bmp) {
            SDL_RWops* rw = SDL_RWFromConstMem(data.data(), (int)data.size());
            SDL_Surface* s = rw ? SDL_LoadBMP_RW(rw, 1) : nullptr;
            if (s) { nt.t = SDL_CreateTextureFromSurface(ren, s); nt.w = s->w; nt.h = s->h; SDL_FreeSurface(s); }
        } else {
            int w = 0, h = 0, n = 0;
            unsigned char* px = stbi_load_from_memory((const unsigned char*)data.data(), (int)data.size(), &w, &h, &n, 4);
            if (px) {
                nt.t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
                if (nt.t) { SDL_UpdateTexture(nt.t, nullptr, px, w * 4); SDL_SetTextureBlendMode(nt.t, SDL_BLENDMODE_BLEND); nt.w = w; nt.h = h; }
                stbi_image_free(px);
            }
        }
    }
    CU.tex[path] = nt;
    return nt.t ? &CU.tex[path] : nullptr;
}

/* ---- prefs cache ---- */

std::string prefs_sig() {
    return file_stamp(hub::prefs::prefs_path(hub::saves::vault_dir()));
}

void apply_titles() {
    for (auto& e : A.lib) {
        auto it = CU.orig.find(e.key);
        if (it == CU.orig.end()) it = CU.orig.emplace(e.key, e.title).first;
        e.title = hubui::cust::display_title(CU.P.renames, e.key, it->second);
    }
    A.vis_dirty = true;
}

void reload(bool force) {
    double t = now_s();
    if (!force && t - CU.checked < 1.5) return;
    CU.checked = t;
    std::string sg = prefs_sig();
    if (!force && sg == CU.sig) return;
    CU.sig = sg;
    hub::prefs::Prefs p;
    hub::prefs::load(hub::saves::vault_dir(), &p);   // missing / corrupt = empty prefs
    CU.P = p;
    apply_titles();
}

/* Load the file fresh, let `f` change it with the prefs setters (true = something changed), save, refresh the cache. */
bool commit(const std::function<bool(hub::prefs::Prefs&)>& f) {
    const std::string root = hub::saves::vault_dir();
    hub::prefs::Prefs p;
    hub::prefs::load(root, &p);
    bool ok = true;
    if (f(p)) ok = hub::prefs::save(root, p);
    reload(true);
    return ok;
}

bool commit_shelves(const std::function<bool(Shelves*)>& edit) {
    return commit([&](hub::prefs::Prefs& p) {
        Shelves v = p.user_shelves;
        if (!edit(&v)) return false;
        return hub::prefs::set_user_shelves(&p, v);
    });
}

const hub::GameEntry* entry_of(const std::string& key) { return find_entry(key); }

/* ---- modal frame ---- */

bool begin_modal(const char* name, Popup& pp, float width_sp) {
    if (pp.want) { ImGui::OpenPopup(name); pp.want = false; }
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(sp(width_sp), io.DisplaySize.x * 0.92f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(3.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
    bool open = ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    return open;
}

void heading(const char* t) {
    ImGui::PushFont(ui::F.title);
    ImGui::TextUnformatted(t);
    ImGui::PopFont();
}
void sub_heading(const char* t) {
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted(t);
    ImGui::PopFont();
}

/* "New shelf name [____] (Create)" row. Returns true when a shelf was made (name of it in *made). */
bool new_shelf_row(char* buf, size_t sz, const char* idsuffix, std::string* made) {
    bool go = false;
    ImGui::PushID(idsuffix);
    ImGui::SetNextItemWidth(sp(26.f));
    if (osk::input("##newshelf", "New shelf name", buf, sz, ImGuiInputTextFlags_EnterReturnsTrue)) go = buf[0] != 0;
    ImGui::SameLine(0, sp(1.f));
    if (ui::button("Create shelf", ImVec2(0, sp(4.f)), false, true, ui::Icon::Plus)) go = true;
    ImGui::PopID();
    if (!go) return false;
    std::string nm;
    bool ok = commit_shelves([&](Shelves* v) { return hubui::cust::add_shelf(v, buf, &nm); });
    if (!ok || nm.empty()) { push_toast(nm.empty() ? "You can have up to 24 shelves" : "Couldn't save the shelf", COL_BAD, 4.0); return false; }
    buf[0] = 0;
    if (made) *made = nm;
    return true;
}

/* ---------- screenshot cover picker ---------- */

void draw_shots() {
    if (!begin_modal("Cover from a screenshot##cust", CU.shots, 96.f)) return;
    const hub::GameEntry* e = entry_of(CU.key);
    if (!e || ui::modal_back()) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    heading("Cover from a screenshot");
    ImGui::TextDisabled("%s", e->title.c_str());
    if (CU.shot_files.empty()) {
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        ImGui::TextWrapped("No screenshots yet. Take one in game from the quick menu (View + Y), then come back.");
    }
    const float tw = sp(18.f), th = tw / 1.6f;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int per_row = std::max(1, (int)((avail + sp(1.f)) / (tw + sp(1.f))));
    ImGui::BeginChild("##cust_shots", ImVec2(0, std::min(sp(40.f), (th + sp(1.f)) * 3.4f)), ImGuiChildFlags_NavFlattened);
    for (size_t i = 0; i < CU.shot_files.size() && i < 40; ++i) {
        const std::string path = CU.shot_dir + "\\" + CU.shot_files[i];
        ImGui::PushID((int)i);
        if (i % per_row) ImGui::SameLine(0, sp(1.f));
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool pick = ImGui::InvisibleButton("shot", ImVec2(tw, th));
        bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        if (i == 0) ImGui::SetItemDefaultFocus();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p1(p.x + tw, p.y + th);
        dl->AddRectFilled(p, p1, IM_COL32(0, 0, 0, 255), sp(1.f));
        if (Tex* t = tex_for(path)) ui::draw_art(dl, t->t, t->w, t->h, p, p1, sp(1.f));
        if (hot) ui::focus_ring(dl, p, p1, sp(1.f));
        ImGui::PopID();
        if (pick) {
            bool ok = commit([&](hub::prefs::Prefs& pr) { auto m = pr.covers; m[CU.key] = path; return hub::prefs::set_covers(&pr, m); });
            push_toast(ok ? "Cover set from the screenshot" : "Couldn't save the cover", ok ? COL_GOOD : COL_BAD, 3.0);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    if (ui::button("Cancel", ImVec2(sp(14.f), sp(4.5f)))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

/* ---------- Add to shelf ---------- */

void draw_manager();

void draw_picker() {
    if (!begin_modal("Add to shelf##cust", CU.pick, 56.f)) return;
    const hub::GameEntry* e = entry_of(CU.key);
    if (!e || ui::modal_back()) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    heading("Add to shelf");
    ImGui::TextDisabled("%s", e->title.c_str());
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    const Shelves shelves = CU.P.user_shelves;
    if (shelves.empty()) ImGui::TextWrapped("You have no shelves yet. Name one below and this game goes in it.");
    ImGui::BeginChild("##pick_list", ImVec2(0, std::min(sp(30.f), (sp(4.5f) + sp(0.75f)) * (float)std::max<size_t>(1, shelves.size()) + sp(0.5f))), ImGuiChildFlags_NavFlattened);
    for (size_t i = 0; i < shelves.size(); ++i) {
        const bool in = hubui::cust::has_game(shelves, (int)i, CU.key);
        std::string lbl = shelves[i].name + "  (" + std::to_string(shelves[i].keys.size()) + ")";
        ImGui::PushID((int)i);
        if (ui::button(lbl.c_str(), ImVec2(-FLT_MIN, sp(4.5f)), in, true, in ? ui::Icon::Check : ui::Icon::Plus)) {
            const std::string key = CU.key;
            bool ok = commit_shelves([&](Shelves* v) { return hubui::cust::toggle_game(v, (int)i, key); });
            if (ok) push_toast(in ? "Removed from " + shelves[i].name : "Added to " + shelves[i].name, COL_GOOD, 2.5);
            else push_toast("Couldn't save the shelf", COL_BAD, 4.0);
        }
        if (i == 0) ImGui::SetItemDefaultFocus();
        ImGui::PopID();
    }
    ImGui::EndChild();
    sub_heading("New shelf");
    std::string made;
    if (new_shelf_row(CU.new_buf, sizeof CU.new_buf, "pick", &made)) {
        const std::string key = CU.key;
        commit_shelves([&](Shelves* v) { int at = hubui::cust::find_shelf(*v, made); return at >= 0 && hubui::cust::add_game(v, at, key); });
        push_toast("Added to " + made, COL_GOOD, 2.5);
    }
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    if (ui::button("Manage shelves", ImVec2(0, sp(4.5f)), false, true, ui::Icon::Gear)) { CU.mgr.want = true; CU.mgr.parent = LocPicker; }
    ImGui::SameLine(0, sp(1.f));
    if (ui::button("Done", ImVec2(sp(14.f), sp(4.5f)), true)) ImGui::CloseCurrentPopup();
    if (CU.mgr.parent == LocPicker) draw_manager();   // opened from here: nests inside this modal (see custom_draw)
    ImGui::EndPopup();
}

/* ---------- Manage shelves: new / rename / reorder / delete ---------- */

void draw_manager() {
    if (!begin_modal("Manage shelves##cust", CU.mgr, 70.f)) return;
    if (ui::modal_back()) { CU.edit = CU.del = -1; ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    heading("Your shelves");
    ImGui::TextDisabled("They show on Home after the built-in shelves.");
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    std::string made;
    new_shelf_row(CU.new_buf, sizeof CU.new_buf, "mgr", &made);
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    const Shelves shelves = CU.P.user_shelves;
    if (shelves.empty()) ImGui::TextDisabled("No shelves yet.");
    ImGui::BeginChild("##mgr_list", ImVec2(0, std::min(sp(34.f), (sp(13.5f)) * (float)std::max<size_t>(1, shelves.size()))), ImGuiChildFlags_NavFlattened);
    for (int i = 0; i < (int)shelves.size(); ++i) {
        ImGui::PushID(i);
        ImGui::PushFont(ui::F.bold);
        ImGui::Text("%s", shelves[i].name.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("%d game%s", (int)shelves[i].keys.size(), shelves[i].keys.size() == 1 ? "" : "s");
        if (CU.edit == i) {
            ImGui::SetNextItemWidth(sp(26.f));
            bool go = osk::input("##rename_shelf", "Shelf name", CU.edit_buf, sizeof CU.edit_buf, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine(0, sp(1.f));
            if (ui::button("Save", ImVec2(0, sp(4.f)), true)) go = true;
            ImGui::SameLine(0, sp(1.f));
            if (ui::button("Cancel", ImVec2(0, sp(4.f)))) CU.edit = -1;
            if (go) {
                const std::string nn = CU.edit_buf;
                commit_shelves([&](Shelves* v) { return hubui::cust::rename_shelf(v, i, nn); });
                CU.edit = -1;
            }
        } else if (CU.del == i) {
            ImGui::TextWrapped("Delete this shelf? The games stay in your library.");
            if (ui::button("Yes, delete", ImVec2(0, sp(4.f)), true)) { commit_shelves([&](Shelves* v) { return hubui::cust::delete_shelf(v, i); }); CU.del = -1; }
            ImGui::SameLine(0, sp(1.f));
            if (ui::button("Keep it", ImVec2(0, sp(4.f)))) CU.del = -1;
        } else {
            if (ui::button("Up", ImVec2(0, sp(4.f)), false, i > 0, ui::Icon::ChevronL)) commit_shelves([&](Shelves* v) { return hubui::cust::move_shelf(v, i, -1); });
            ImGui::SameLine(0, sp(1.f));
            if (ui::button("Down", ImVec2(0, sp(4.f)), false, i + 1 < (int)shelves.size(), ui::Icon::ChevronR)) commit_shelves([&](Shelves* v) { return hubui::cust::move_shelf(v, i, +1); });
            ImGui::SameLine(0, sp(1.f));
            if (ui::button("Rename", ImVec2(0, sp(4.f)))) { CU.edit = i; CU.del = -1; std::snprintf(CU.edit_buf, sizeof CU.edit_buf, "%s", shelves[i].name.c_str()); }
            ImGui::SameLine(0, sp(1.f));
            if (ui::button("Delete", ImVec2(0, sp(4.f)), false, true, ui::Icon::Close)) { CU.del = i; CU.edit = -1; }
        }
        ImGui::Dummy(ImVec2(0, sp(0.75f)));
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, sp(1.f)));
    if (ui::button("Done", ImVec2(sp(14.f), sp(4.5f)), true)) { CU.edit = CU.del = -1; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

/* ---------- Customize (rename / cover / reset) ---------- */

void refresh_shots(const hub::GameEntry& e) {
    CU.shot_dir = hub::saves::game_dir(sys_short(e), e.key);
    while (!CU.shot_dir.empty() && (CU.shot_dir.back() == '\\' || CU.shot_dir.back() == '/')) CU.shot_dir.pop_back();
    CU.shot_dir += "\\shots";
    CU.shot_files.clear();
    std::error_code ec;
    for (auto& it : std::filesystem::directory_iterator(std::filesystem::u8path(CU.shot_dir), ec)) {
        if (!it.is_regular_file(ec)) continue;
        std::string n = it.path().filename().u8string(), x = n.size() > 4 ? n.substr(n.size() - 4) : "";
        for (auto& c : x) c = (char)std::tolower((unsigned char)c);
        if (x == ".png" || x == ".bmp") CU.shot_files.push_back(n);
    }
    std::sort(CU.shot_files.begin(), CU.shot_files.end(), std::greater<std::string>());   // newest first (timestamp names)
}

void draw_customize() {
    if (!begin_modal("Customize##cust", CU.cust, 64.f)) return;
    const hub::GameEntry* ep = entry_of(CU.key);
    if (!ep || ui::modal_back()) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    const hub::GameEntry& e = *ep;
    const std::string orig = custom_original_title(e);
    if (CU.name_key != e.key) { CU.name_key = e.key; std::snprintf(CU.name_buf, sizeof CU.name_buf, "%s", e.title.c_str()); }
    const bool renamed = custom_is_renamed(e.key);
    auto cit = CU.P.covers.find(e.key);
    const bool has_cover = cit != CU.P.covers.end() && !cit->second.empty();

    heading("Customize");
    ImGui::TextDisabled("%s", orig.c_str());

    sub_heading("Name");
    ImGui::SetNextItemWidth(sp(34.f));
    bool go = osk::input("##cust_name", "Game name", CU.name_buf, sizeof CU.name_buf, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0, sp(1.f));
    if (ui::button("Rename", ImVec2(0, sp(4.f)), true)) go = true;
    if (go) {
        const std::string nn = CU.name_buf, key = e.key;
        bool ok = commit([&](hub::prefs::Prefs& p) { return hub::prefs::set_renames(&p, hubui::cust::with_rename(p.renames, key, orig, nn)); });
        push_toast(ok ? "Name saved" : "Couldn't save the name", ok ? COL_GOOD : COL_BAD, 2.5);
        CU.name_key.clear();
    }
    if (renamed) {
        if (ui::button("Back to original name", ImVec2(0, sp(4.f)))) {
            const std::string key = e.key;
            commit([&](hub::prefs::Prefs& p) { auto m = p.renames; m.erase(key); return hub::prefs::set_renames(&p, m); });
            CU.name_key.clear();
        }
    }

    sub_heading("Cover");
    if (has_cover) ImGui::TextDisabled("Using your own cover picture.");
    else ImGui::TextDisabled("Using the normal box art.");
    {
        std::string f = osk::file("cust_cover", ui::button("From a file...", ImVec2(0, sp(4.f)), false, true, ui::Icon::Folder), {".png", ".bmp"});
        if (!f.empty()) {
            if (!tex_for(f)) push_toast("Couldn't read that picture (PNG or BMP)", COL_BAD, 4.0);
            else {
                const std::string key = e.key;
                bool ok = commit([&](hub::prefs::Prefs& p) { auto m = p.covers; m[key] = f; return hub::prefs::set_covers(&p, m); });
                push_toast(ok ? "Cover set" : "Couldn't save the cover", ok ? COL_GOOD : COL_BAD, 3.0);
            }
        }
    }
    ImGui::SameLine(0, sp(1.f));
    {   // the screenshot list: re-read every 2 s or when the game changes
        static std::string s_k; static double s_at = -10;
        if (CU.shots.want || s_k != e.key || now_s() - s_at > 2.0) { s_k = e.key; s_at = now_s(); refresh_shots(e); }
    }
    const bool have_shots = !CU.shot_files.empty();
    if (ui::button("From a screenshot...", ImVec2(0, sp(4.f)), false, have_shots, ui::Icon::Play)) { CU.shots.want = true; CU.shots.parent = LocCustomize; }
    if (!have_shots && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("No screenshots of this game yet");
    if (has_cover) {
        ImGui::SameLine(0, sp(1.f));
        if (ui::button("Back to box art", ImVec2(0, sp(4.f)))) {
            const std::string key = e.key;
            commit([&](hub::prefs::Prefs& p) { auto m = p.covers; m.erase(key); return hub::prefs::set_covers(&p, m); });
        }
    }

    sub_heading("Shelves");
    {
        int n = hubui::cust::membership_count(CU.P.user_shelves, e.key);
        ImGui::TextDisabled(n ? "On %d of your shelves." : "Not on any of your shelves.", n);
        if (ui::button("Add to shelf...", ImVec2(0, sp(4.f)), false, true, ui::Icon::Plus)) { CU.pick.want = true; CU.pick.parent = LocCustomize; }
    }

    ImGui::Dummy(ImVec2(0, sp(1.5f)));
    if (renamed || has_cover) {
        if (ui::button("Reset everything", ImVec2(0, sp(4.5f)))) {
            const std::string key = e.key;
            commit([&](hub::prefs::Prefs& p) {
                auto r = p.renames; r.erase(key);
                auto c = p.covers; c.erase(key);
                bool a = hub::prefs::set_renames(&p, r), b = hub::prefs::set_covers(&p, c);
                return a || b;
            });
            CU.name_key.clear();
            push_toast("Back to the original name and cover", COL_GOOD, 2.5);
        }
        ImGui::SameLine(0, sp(1.f));
    }
    if (ui::button("Done", ImVec2(sp(14.f), sp(4.5f)), true)) ImGui::CloseCurrentPopup();
    if (CU.shots.parent == LocCustomize) draw_shots();
    if (CU.pick.parent == LocCustomize) draw_picker();
    ImGui::EndPopup();
}

}  // namespace

/* ======================= public hooks (declared in app.h) ======================= */

void custom_reset_originals() {
    CU.orig.clear();
    CU.name_key.clear();
    reload(true);   // loads prefs and applies the renames to the fresh A.lib
}

std::string custom_original_title(const hub::GameEntry& e) {
    auto it = CU.orig.find(e.key);
    return it == CU.orig.end() ? e.title : it->second;
}

bool custom_is_renamed(const std::string& key) { return CU.P.renames.count(key) != 0 && CU.orig.count(key) != 0 && CU.P.renames.at(key) != CU.orig.at(key); }

bool custom_cover(const std::string& key, SDL_Texture** tex, int* w, int* h) {
    auto it = CU.P.covers.find(key);
    if (it == CU.P.covers.end() || it->second.empty()) return false;
    Tex* t = tex_for(it->second);
    if (!t) return false;
    *tex = t->t; *w = t->w; *h = t->h;
    return true;
}

void custom_open_customize(const std::string& key) {
    CU.key = key;
    CU.cust.want = true;
    CU.cust.parent = LocDetails;
}

void custom_open_picker(const std::string& key, bool from_details) {
    if (!entry_of(key)) return;
    CU.key = key;
    CU.pick.want = true;
    CU.pick.parent = from_details ? LocDetails : LocRoot;
}

void custom_open_manager() { CU.mgr.want = true; CU.mgr.parent = LocRoot; }

void custom_draw(int loc) {
    if (loc == LocRoot) {
        reload(false);
        if (g_headless && !CU.shot_once && ImGui::GetFrameCount() > 8) {   // screenshot aid: HUB_SHOT_CUSTOM=customize|picker|manager|shots
            const char* c = std::getenv("HUB_SHOT_CUSTOM");
            if (c && !A.sel_game.empty() && entry_of(A.sel_game)) {
                CU.shot_once = true;
                std::string m = c;
                if (m == "customize" || m == "shots") { A.show_details = true; custom_open_customize(A.sel_game); CU.shots.parent = LocCustomize; CU.shots.want = m == "shots"; }
                else if (m == "picker") custom_open_picker(A.sel_game, false);
                else if (m == "manager") custom_open_manager();
            }
        }
    }
    if (CU.cust.parent == loc) draw_customize();
    if (CU.pick.parent == loc) draw_picker();
    if (CU.mgr.parent == loc) draw_manager();
}

/* Home: the user's shelves, after the built-in ones. Tiles are the same as everywhere (spec_for_entry / draw_tile). */
void custom_home_shelves(float avail, float tw, float th, float gap, float bleed) {
    if (A.lib.empty()) return;
    std::map<std::string, int> idx;
    for (const auto& sh : CU.P.user_shelves)
        for (const auto& k : sh.keys)
            if (const hub::GameEntry* e = find_entry(k)) idx[k] = (int)(e - A.lib.data());
    const std::vector<hubui::Shelf> view = hubui::cust::compose_user_shelves(CU.P.user_shelves, idx);
    for (const hubui::Shelf& sh : view) {
        bool overflow = hubui::shelf_max_scroll((int)sh.items.size(), tw, gap, bleed, avail) > 0;
        shelf_header(sh.title.c_str(), "", bleed, (int)sh.items.size(), nullptr, overflow);
        shelf_strip("shelf_" + sh.id, (int)sh.items.size(), tw, th, [&](int i, ImVec2 sz) {
            const hub::GameEntry& e = A.lib[sh.items[i]];
            TileSpec spec = spec_for_entry(e, sh.id.c_str(), false, true);
            TileResult r = draw_tile(spec, sz);
            handle_entry_result(e, spec, r);
            return r;
        });
        ImGui::Dummy(ImVec2(0, sp(1.f)));
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + bleed);
    if (ui::button(CU.P.user_shelves.empty() ? "Make your own shelf..." : "Edit your shelves...", ImVec2(0, 0), false, true, ui::Icon::Plus)) custom_open_manager();
    ImGui::Dummy(ImVec2(0, sp(1.f)));
}
