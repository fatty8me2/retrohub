#include "hack_ui.h"
#include "hack_core.h"
#include "hack_logic.h"
#include "osk.h"
#include "savestore.h"
#include "attach_xfer.h"
#include "friend_pick_ui.h"

#include <atomic>
#include <ctime>
#include <map>
#include <filesystem>
#include <thread>

/* ROM hacks and translations in the library (Plan 4 H2-H4). Contract: hack_logic.h (identity), hack_core.h (files), common/patcher.h (patch engine),
 * GameEntry::hack_patch / hack_base. Screenshot aid: HUB_SHOT_HACKS=add|versions|tile (with HUB_SHOT_FAKE_LIB=6). */

namespace fs = std::filesystem;
namespace hk = hubui::hacks;

namespace {

std::string data_root() {
    std::string d = hub::data_dir();
    while (!d.empty() && (d.back() == '\\' || d.back() == '/')) d.pop_back();
    return d;
}
std::string cache_dir_for(const std::string& key) { return data_root() + "\\rom-cache\\hacks\\" + hk::cache_folder(key); }

bool g_shot = false;
std::string g_shot_mode;

std::map<std::string, std::string> g_ribbon;   // key -> "HACK" | "TRANSLATION" (a hack's kind never changes)

hub::GameEntry base_of(const hub::GameEntry& e, bool* found) {
    const hub::GameEntry* b = is_hack(e) ? find_entry(e.hack_base) : &e;
    if (found) *found = b != nullptr;
    return b ? *b : e;
}

/* ---------------- adding a hack ---------------- */

struct Add {
    enum St { Closed, Working, Choose, Details, Building, Fail } st = Closed;
    std::string patch, pname;
    bool has_hint = false;
    hub::GameEntry hint;
    std::vector<hk::Cand> libsnap;
    hk::FindResult fr;
    hub::patch::Info info;
    hub::patch::Format fmt = hub::patch::Format::Unknown;
    hk::Cand base;
    std::string prep, msg, tmp_dir, tmp_out;
    char title[96] = {};
    char lang[16] = {};
    bool translation = false, focus_first = true;
    uint32_t patch_crc = 0;
    std::thread th;
    std::atomic<bool> done{false}, cancel{false};
    std::atomic<double> prog{0};
    hk::BuildResult br;
    bool want_pick = false, pick_click = false;
} AD;

void add_reset() {
    if (AD.th.joinable()) { AD.cancel = true; AD.th.join(); }
    if (!AD.tmp_dir.empty()) { std::error_code ec; fs::remove_all(fs::u8path(AD.tmp_dir), ec); }
    AD.st = Add::Closed;
    AD.patch.clear(); AD.pname.clear(); AD.has_hint = false; AD.libsnap.clear(); AD.fr = hk::FindResult();
    AD.tmp_dir.clear(); AD.tmp_out.clear(); AD.msg.clear(); AD.translation = false;
    AD.title[0] = 0; AD.lang[0] = 0;
    AD.cancel = false; AD.done = false; AD.prog = 0; AD.focus_first = true;
}

void add_fail(const std::string& m) { AD.st = Add::Fail; AD.msg = m; AD.focus_first = true; }

std::string stem_title(const std::string& path) {
    std::string s = fs::u8path(path).stem().u8string();
    for (char& c : s) if (c == '_') c = ' ';
    return s.substr(0, 90);
}

void to_details() {
    AD.st = Add::Details;
    AD.focus_first = true;
    std::string t = stem_title(AD.patch);
    std::snprintf(AD.title, sizeof AD.title, "%s", t.c_str());
}

const char* kind_word(bool tr) { return tr ? "translation" : "hack"; }

void start_find() {
    AD.st = Add::Working;
    AD.done = false;
    AD.th = std::thread([]() {
        std::string err;
        uint64_t ext = 0;
        if (hub::patch::inspect_file(AD.patch, &AD.info, &err)) {
            if (AD.info.format == hub::patch::Format::Ips) ext = hk::ips_extent_file(AD.patch);
            AD.fr = hk::find_base(AD.libsnap, AD.info, AD.pname, ext);
        } else {
            AD.fr = hk::FindResult();
            AD.fr.status = hk::FindStatus::NotFound;
            AD.fr.message = err.empty() ? "That does not look like a patch file." : err;
        }
        AD.done = true;
    });
}

void start_build() {
    AD.st = Add::Building;
    AD.done = false;
    AD.cancel = false;
    AD.prog = 0;
    AD.tmp_dir = data_root() + "\\rom-cache\\hacks\\_new-" + std::to_string((unsigned long long)GetTickCount64());
    AD.tmp_out = AD.tmp_dir + "\\" + hk::patched_name(AD.base.system, AD.base.path);
    AD.th = std::thread([]() {
        AD.br = hk::build_patched(AD.base.path, AD.prep, AD.patch, AD.tmp_out, [](double p) { AD.prog = p; return !AD.cancel.load(); });
        std::string err;
        if (AD.br.ok && !hub::patch::crc32_file(AD.patch, &AD.patch_crc, &err)) AD.patch_crc = 0;
        AD.done = true;
    });
}

void finish_build() {
    AD.th.join();
    if (AD.cancel) { add_reset(); return; }
    if (!AD.br.ok) { std::string m = AD.br.message; std::error_code ec; fs::remove_all(fs::u8path(AD.tmp_dir), ec); AD.tmp_dir.clear(); add_fail(m); return; }
    hk::HackDef d;
    d.base_key = AD.base.key;
    d.system = hub::system_short(AD.base.system);
    d.out_crc = AD.br.out_crc;
    d.out_size = AD.br.out_size;
    d.key = hk::make_key(d.base_key, d.out_crc);
    d.title = AD.title;
    d.kind = kind_word(AD.translation);
    d.lang = AD.translation ? AD.lang : "";
    d.prep = AD.prep;
    d.patch_crc = AD.patch_crc;
    d.added = (int64_t)time(nullptr);
    d.format = hub::patch::format_name(AD.info.format);
    for (char& c : d.format) c = (char)std::tolower((unsigned char)c);
    if (d.format == "xdelta") d.format = "vcdiff";
    std::string pf = fs::u8path(AD.patch).filename().u8string();
    d.patch_file = hk::valid_patch_file_name(pf) ? pf : "patch." + d.format;
    if (find_entry(d.key)) {
        std::string t = find_entry(d.key)->title;
        std::error_code ec; fs::remove_all(fs::u8path(AD.tmp_dir), ec); AD.tmp_dir.clear();
        add_fail("You already have this " + std::string(kind_word(AD.translation)) + " in your library as " + t + ".");
        return;
    }
    std::error_code ec;
    const std::string dest = cache_dir_for(d.key);
    fs::remove_all(fs::u8path(dest), ec);
    fs::rename(fs::u8path(AD.tmp_dir), fs::u8path(dest), ec);   // the image is ready for the first Play
    if (!ec) AD.tmp_dir.clear();
    std::string err;
    if (!hk::install_hack(hub::saves::game_dir(d.system, d.key), d, AD.patch, &err)) { add_fail(err); return; }
    push_toast("Added " + d.title + ".", ui::GOOD, 8.0);
    A.env_select = d.key;
    std::string key = d.key;
    add_reset();
    start_scan();
}

void begin_add(const std::string& patch, const hub::GameEntry* hint) {
    if (AD.st != Add::Closed) { set_notice("Finish adding the current hack first."); return; }
    hub::patch::Format f = hub::patch::format_from_ext(patch);
    if (f == hub::patch::Format::Unknown) { set_notice("That is not a patch file. RetroHub reads .ips, .ups, .bps and .xdelta patches."); return; }
    add_reset();
    AD.patch = patch;
    AD.pname = fs::u8path(patch).filename().u8string();
    AD.fmt = f;
    AD.libsnap = hk::candidates_from(A.lib);
    if (hint) {
        AD.has_hint = true;
        AD.hint = *hint;
        std::vector<hk::Cand> one;
        for (auto& c : AD.libsnap) if (c.key == hint->key) one.push_back(c);
        AD.libsnap = one;
    }
    if (AD.libsnap.empty()) {
        add_fail(hint ? "This game's image cannot be patched here." : "Add the original game to your library first, then add the patch.");
        return;
    }
    start_find();
}

void poll_add() {
    if (AD.st == Add::Working && AD.done) {
        AD.th.join();
        if (AD.fr.status == hk::FindStatus::Found) { AD.base = AD.fr.base; AD.prep = AD.fr.prep; to_details(); }
        else if (AD.fr.status == hk::FindStatus::Choose) {
            if (AD.has_hint) {
                if (AD.fr.choices.empty()) add_fail("This patch does not fit " + AD.hint.title + ".");
                else { AD.base = AD.fr.choices[0]; AD.prep = hk::default_prep(AD.base); to_details(); }
            } else { AD.st = Add::Choose; AD.focus_first = true; }
        } else add_fail(AD.fr.message);
    }
    if (AD.st == Add::Building && AD.done) finish_build();
}

void modal_frame_begin(const char* id, float w) {
    ImGui::OpenPopup(id);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(sp(w), ImGui::GetIO().DisplaySize.x * 0.94f), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(3.f), sp(3.f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, sp(2.5f));
}

void add_modal() {
    modal_frame_begin("##hackadd", 84.f);
    bool open = ImGui::BeginPopupModal("##hackadd", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    if (!open) return;
    const float bh = sp(5.f), bw = ImGui::GetContentRegionAvail().x;
    const bool busy = AD.st == Add::Working || AD.st == Add::Building;
    if (!busy && ui::modal_back()) { add_reset(); ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    auto focus_first = [&]() { if (AD.focus_first) { ImGui::SetKeyboardFocusHere(0); AD.focus_first = false; } };

    ImGui::PushFont(ui::F.title);
    ImGui::TextUnformatted(AD.st == Add::Fail ? "Can't add this patch" : AD.st == Add::Building ? "Building your game" : "Add a hack or translation");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));

    if (AD.st == Add::Working) {
        text_wrapped_disabled("Looking for the original game this patch is for...");
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        ImGui::ProgressBar(-1.f * (float)ImGui::GetTime(), ImVec2(-1, sp(1.5f)), "");
    } else if (AD.st == Add::Choose) {
        text_wrapped_disabled("This kind of patch does not say which game it is for. Pick the game it was made for. RetroHub only lists games it could fit.");
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        ImGui::BeginChild("##hackbase", ImVec2(0, sp(28.f)), ImGuiChildFlags_NavFlattened);
        for (size_t i = 0; i < AD.fr.choices.size(); ++i) {
            const hk::Cand& c = AD.fr.choices[i];
            std::string label = c.title + "   -   " + hub::system_name(c.system);
            ImGui::PushID((int)i);
            if (i == 0) focus_first();
            if (ImGui::Selectable(label.c_str(), false, 0, ImVec2(0, sp(3.5f)))) {
                AD.base = c;
                AD.prep = hk::default_prep(c);
                to_details();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        if (button("Cancel", ImVec2(sp(14.f), bh), false, true, ui::Icon::Close)) { add_reset(); ImGui::CloseCurrentPopup(); }
    } else if (AD.st == Add::Details) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT2);
        ImGui::TextWrapped("Original game: %s (%s)", AD.base.title.c_str(), hub::system_name(AD.base.system));
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        ImGui::TextUnformatted("Name");
        ImGui::SetNextItemWidth(-1);
        osk::input("##hacktitle", "Name of the hack or translation", AD.title, sizeof AD.title);
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        focus_first();   // a pad lands on Hack, not on the name field (that would open the on-screen keyboard at once)
        if (button("Hack", ImVec2(sp(14.f), sp(4.5f)), !AD.translation)) AD.translation = false;
        ImGui::SameLine(0, sp(1.f));
        if (button("Translation", ImVec2(sp(14.f), sp(4.5f)), AD.translation)) AD.translation = true;
        if (AD.translation) {
            ImGui::Dummy(ImVec2(0, sp(0.5f)));
            ImGui::TextUnformatted("Language");
            static const char* langs[][2] = {{"English", "en"}, {"Spanish", "es"}, {"French", "fr"}, {"German", "de"}, {"Portuguese", "pt-br"}, {"Japanese", "ja"}};
            for (int i = 0; i < 6; ++i) {
                if (i) ImGui::SameLine(0, sp(0.75f));
                ImGui::PushID(i);
                if (button(langs[i][0], ImVec2(0, sp(4.f)), std::string(AD.lang) == langs[i][1])) std::snprintf(AD.lang, sizeof AD.lang, "%s", langs[i][1]);
                ImGui::PopID();
            }
            ImGui::SetNextItemWidth(sp(24.f));
            osk::input("##hacklang", "Other language", AD.lang, sizeof AD.lang);
        }
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextUnformatted("It becomes its own game in your library, with its own saves. Your original game is not changed. Cheats and achievements start empty.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, sp(1.5f)));
        bool ok = AD.title[0] != 0 && (!AD.translation || AD.lang[0] != 0);
        if (button("Add to library", ImVec2(sp(18.f), bh), true, ok && !g_shot, ui::Icon::Plus)) start_build();
        ImGui::SameLine(0, sp(1.f));
        if (button("Cancel", ImVec2(sp(14.f), bh), false, true, ui::Icon::Close)) { add_reset(); ImGui::CloseCurrentPopup(); }
    } else if (AD.st == Add::Building) {
        text_wrapped_disabled("Applying the patch. This takes a few seconds, longer for big games.");
        ImGui::Dummy(ImVec2(0, sp(1.f)));
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ui::ACCENT);
        ImGui::ProgressBar((float)AD.prog.load(), ImVec2(-1, sp(2.5f)), "");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, sp(1.5f)));
        if (button("Cancel", ImVec2(sp(14.f), bh), false, true, ui::Icon::Close)) AD.cancel = true;
    } else {   // Fail
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WARN);
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextUnformatted(AD.msg.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, sp(1.5f)));
        focus_first();
        if (button("OK", ImVec2(sp(14.f), bh), true)) { add_reset(); ImGui::CloseCurrentPopup(); }
    }
    ImGui::EndPopup();
}

/* ---------------- removing a hack ---------------- */

struct Remove { bool open = false, focus = true; std::string key, title, system; } RM;

void do_remove(bool with_saves) {
    if (A.proc && (A.run.key == RM.key || (find_entry(RM.key) && A.run.key == find_entry(RM.key)->hack_base))) { set_notice("That game is running. Close it first."); return; }   // other games are safe to change
    std::string gd = hub::saves::game_dir(RM.system, RM.key);
    hk::remove_hack_files(gd);
    std::error_code ec;
    fs::remove_all(fs::u8path(cache_dir_for(RM.key)), ec);
    if (with_saves) fs::remove_all(fs::u8path(gd), ec);
    if (const hub::GameEntry* h = find_entry(RM.key)) A.sel_game = h->hack_base;
    push_toast("Removed " + RM.title + (with_saves ? " and its saves." : ". Its saves were kept."), ui::GOOD, 8.0);
    start_scan();
}

void remove_modal() {
    modal_frame_begin("##hackrm", 64.f);
    bool open = ImGui::BeginPopupModal("##hackrm", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    if (!open) return;
    if (ui::modal_back()) { RM.open = false; ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
    ImGui::PushFont(ui::F.title);
    ImGui::TextUnformatted("Remove this hack?");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    ImGui::TextWrapped("%s will leave your library. Its patch file is deleted from RetroHub (your original game is not touched).", RM.title.c_str());
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    text_wrapped_disabled("Its saves are kept by default, so adding the same patch again brings them back.");
    ImGui::Dummy(ImVec2(0, sp(1.5f)));
    const ImVec2 bs(ImGui::GetContentRegionAvail().x, sp(5.f));
    if (RM.focus) { ImGui::SetKeyboardFocusHere(0); RM.focus = false; }
    if (button("Remove, keep saves", bs, true)) { do_remove(false); RM.open = false; ImGui::CloseCurrentPopup(); }
    if (button("Remove and delete its saves", bs, false)) { do_remove(true); RM.open = false; ImGui::CloseCurrentPopup(); }
    if (button("Cancel", bs)) { RM.open = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

/* ---------------- preparing the patched image for Play ---------------- */

struct Prep {
    bool active = false, waiting = false;
    std::thread th;
    std::atomic<bool> done{false}, cancel{false};
    std::atomic<double> prog{0};
    hub::GameEntry e;
    hk::HackDef def;
    std::string dest;
    hk::BuildResult br;
} PR;

std::string dest_for(const hub::GameEntry& e, const std::string& key) { return cache_dir_for(key) + "\\" + hk::patched_name(e.system, e.path); }

void poll_prep() {
    if (!PR.active || !PR.done) return;
    PR.th.join();
    PR.active = false;
    std::error_code ec;
    if (PR.cancel) { fs::remove(fs::u8path(PR.dest), ec); return; }
    if (PR.br.ok && PR.br.out_crc != PR.def.out_crc) {
        fs::remove(fs::u8path(PR.dest), ec);
        PR.br.ok = false;
        PR.br.message = "This " + std::string(PR.def.kind) + " did not build the same game it was added with. Is your original game a different version now?";
    }
    if (!PR.br.ok) { set_notice(PR.br.message, 12.0); return; }
    const hub::GameEntry* cur = find_entry(PR.e.key);
    if (PR.waiting && cur && !A.proc && A.focused && !A.in_room && !game_active()) {
        hub::GameEntry copy = *cur;
        play_entry(copy);
    } else {
        push_toast(PR.e.title + " is ready to play.", ui::GOOD, 8.0);
    }
}

void prep_card() {
    if (!PR.active) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - sp(2.f), io.DisplaySize.y - sp(2.f)), ImGuiCond_Always, ImVec2(1.f, 1.f));
    ImGui::SetNextWindowSize(ImVec2(sp(40.f), 0));
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sp(2.f), sp(2.f)));
    ImGui::Begin("##hackprep", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PopStyleVar();
    ImGui::PushFont(ui::F.bold);
    ImGui::TextUnformatted(ui::ellipsize(ui::F.bold, ImGui::GetFontSize(), "Getting " + PR.e.title + " ready", sp(36.f)).c_str());
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ui::ACCENT);
    ImGui::ProgressBar((float)PR.prog.load(), ImVec2(-1, sp(1.5f)), "");
    ImGui::PopStyleColor();
    if (PR.waiting) text_wrapped_disabled("It starts by itself when it is ready.");
    if (button("Cancel", ImVec2(sp(12.f), sp(4.f)), false, true, ui::Icon::Close)) { PR.cancel = true; PR.waiting = false; }
    ImGui::End();
}

}  // namespace

/* A thread still running at exit would terminate the process: cancel and join. */
struct ThreadGuard {
    ~ThreadGuard() {
        AD.cancel = true; PR.cancel = true;
        if (AD.th.joinable()) AD.th.join();
        if (PR.th.joinable()) PR.th.join();
    }
} g_thread_guard;

/* ---------------- hooks ---------------- */

void hack_scan_append(std::vector<hub::GameEntry>* out, const std::vector<std::string>& folders) {
    (void)folders;
    try { hk::append_hacks(hub::saves::vault_dir(), out); } catch (...) {}
}

bool hack_ensure_rom(const hub::GameEntry& e, std::string* patched_path) {
    hk::HackDef def;
    std::string why = hk::load_def_next_to(e.hack_patch, &def);
    if (!why.empty()) { set_notice(why, 10.0); return false; }
    const std::string dest = dest_for(e, e.key);
    std::error_code ec;
    if (fs::is_regular_file(fs::u8path(dest), ec) && fs::file_size(fs::u8path(dest), ec) == def.out_size) {
        if (patched_path) *patched_path = dest;
        return true;
    }
    if (PR.active) {
        if (PR.e.key == e.key) { PR.waiting = true; set_notice("Still getting " + e.title + " ready...", 4.0); }
        else set_notice("Still getting " + PR.e.title + " ready. Try again in a moment.", 6.0);
        return false;
    }
    if (!fs::is_regular_file(fs::u8path(e.path), ec)) { set_notice("The original game for " + e.title + " is missing. Reconnect its drive or locate the game first.", 10.0); return false; }
    if (!fs::is_regular_file(fs::u8path(e.hack_patch), ec)) { set_notice("The patch file for " + e.title + " is missing.", 10.0); return false; }
    PR.e = e;
    PR.def = def;
    PR.dest = dest;
    PR.cancel = false;
    PR.done = false;
    PR.prog = 0;
    PR.waiting = true;
    PR.active = true;
    PR.th = std::thread([]() {
        PR.br = hk::build_patched(PR.e.path, PR.def.prep, PR.e.hack_patch, PR.dest, [](double p) { PR.prog = p; return !PR.cancel.load(); });
        PR.done = true;
    });
    return false;
}

void hack_draw() {
    poll_add();
    poll_prep();
    prep_card();
    if (AD.want_pick) {   // the pad file browser / Windows dialog for "Add a hack or translation"
        std::string p = osk::file("hack_patch", AD.pick_click, {".ips", ".ups", ".bps", ".xdelta", ".vcdiff", ".delta"});
        AD.pick_click = false;
        if (!p.empty()) {
            AD.want_pick = false;
            hub::GameEntry h = AD.hint;
            AD.has_hint = false;
            begin_add(p, &h);
        }
    }
    if (AD.st != Add::Closed) add_modal();
    if (RM.open) remove_modal();
}

void hack_shot_init() {
    const char* m = std::getenv("HUB_SHOT_HACKS");
    if (!m || !g_headless) return;
    g_shot = true;
    g_shot_mode = m;
    if (g_shot_mode == "add") {
        AD.st = Add::Details;
        AD.base.title = "Pokemon Emerald";
        AD.base.system = hub::System::GBA;
        std::snprintf(AD.title, sizeof AD.title, "%s", "Emerald Kaizo");
        AD.focus_first = true;
    } else if (g_shot_mode == "versions") {
        A.sel_game = "fake:0";
        A.show_details = true;
    } else if (g_shot_mode == "tile") {
        A.view = LibView::All;
    }
}

/* ---------------- UI entry points ---------------- */

bool hack_is_patch_file(const std::string& path) { return hub::patch::format_from_ext(path) != hub::patch::Format::Unknown; }

void hack_add_patch(const std::string& path, const hub::GameEntry* hint) {
    if (!hint) { AD.has_hint = false; }
    begin_add(path, hint);
}

bool hack_can_add(const hub::GameEntry& e) {
    if (is_native(e) || is_kanto(e)) return false;
    bool ok = false;
    hub::GameEntry b = base_of(e, &ok);
    if (!ok) return false;
    std::vector<hub::GameEntry> one{b};
    return !hk::candidates_from(one).empty();
}

void hack_add_open(const hub::GameEntry& e) {
    if (AD.st != Add::Closed) return;
    bool ok = false;
    hub::GameEntry b = base_of(e, &ok);
    if (!ok) return;
    AD.hint = b;
    AD.want_pick = true;
    AD.pick_click = true;
}

std::string hack_ribbon(const hub::GameEntry& e) {
    if (!is_hack(e)) return std::string();
    auto it = g_ribbon.find(e.key);
    if (it != g_ribbon.end()) return it->second;
    std::string r = "HACK";
    if (g_shot) r = e.key.size() && e.key.back() == '1' ? "TRANSLATION" : "HACK";
    else {
        hk::HackDef d;
        if (hk::load_def_next_to(e.hack_patch, &d).empty()) r = hk::ribbon_for(d);
    }
    g_ribbon[e.key] = r;
    return r;
}

void hack_details_section(const hub::GameEntry& e) {
    const std::string base_key = is_hack(e) ? e.hack_base : e.key;
    std::vector<const hub::GameEntry*> v;
    if (const hub::GameEntry* b = find_entry(base_key)) v.push_back(b);
    for (const hub::GameEntry& g : A.lib) if (is_hack(g) && g.hack_base == base_key) v.push_back(&g);
    if (v.size() > 1) {
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        ImGui::TextUnformatted("Versions");
        float avail = ImGui::GetContentRegionAvail().x, x = 0;
        for (size_t i = 0; i < v.size(); ++i) {
            const bool orig = !is_hack(*v[i]);
            std::string label = orig ? std::string("Original") : v[i]->title;
            if (!orig) { std::string r = hack_ribbon(*v[i]); label += r == "TRANSLATION" ? "  (translation)" : "  (hack)"; }
            float w = ImGui::CalcTextSize(label.c_str()).x + sp(4.f);
            if (i && x + w + sp(1.f) < avail) { ImGui::SameLine(0, sp(1.f)); x += w + sp(1.f); } else x = w;
            ImGui::PushID((int)i);
            const bool cur = v[i]->key == e.key;
            if (button(label.c_str(), ImVec2(w, sp(4.f)), cur) && !cur) A.sel_game = v[i]->key;
            ImGui::PopID();
        }
    }
    if (is_hack(e)) {
        ImGui::Dummy(ImVec2(0, sp(0.5f)));
        ImGui::PushStyleColor(ImGuiCol_Text, ui::TEXT3);
        ImGui::TextWrapped("%s. Its saves are separate from the original game.", hack_ribbon(e) == "TRANSLATION" ? "A translation" : "A ROM hack");
        ImGui::PopStyleColor();
        if (button("Remove hack...", ImVec2(0, sp(4.f)), false, true, ui::Icon::Close)) {
            RM.open = true; RM.focus = true; RM.key = e.key; RM.title = e.title; RM.system = sys_short(e);
        }
        /* Send this hack to a friend: the stored patch file (<= 8 MB) rides a DM; they get "Add this hack". */
        static hubui::FriendPicker send_pick;
        static std::string send_path, send_title;
        ImGui::SameLine(0, sp(1.f));
        if (button("Send this hack to a friend", ImVec2(0, sp(4.f)))) {
            std::error_code ec;
            const std::string pp = e.hack_patch;
            auto sz = fs::file_size(fs::u8path(pp), ec);
            std::string why = ec ? std::string("The patch file for this hack can't be found.") : hub::attach::attach_size_problem((int64_t)sz, "patch");
            if (!why.empty()) set_toast(why, false, 7.0);
            else { send_path = pp; send_title = e.title; send_pick.open("Send this hack", "Pick who should get the patch file. They add it with one click (max 8 MB). Everyone needs the original game."); }
        }
        if (send_pick.draw("Send hack to friends##hackpick") && !send_path.empty())
            hubui::attach_send_start(send_path, "patch", fs::u8path(send_path).filename().u8string(), send_pick.chosen(), "Sent the hack " + send_title);
    }
}
