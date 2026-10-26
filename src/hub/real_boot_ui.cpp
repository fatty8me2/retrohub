#include "app.h"
#include "real_boot.h"
#include "pcsx2.h"

#include <filesystem>
#include <fstream>
#include <iterator>

/* "Real console startup": launch glue + Settings block. The pure logic (file detection, config text, options) is in common/real_boot.*;
 * nothing of any console (logo, sound, firmware) ships with RetroHub: only the player's own files are copied into the runner's system folder. */

namespace fs = std::filesystem;

namespace {

fs::path P(const std::string& s) { return fs::u8path(s); }

std::string read_all(const std::string& p) {
    std::ifstream f(P(p), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
bool write_all(const std::string& p, const std::string& s) {
    std::error_code ec;
    fs::create_directories(P(p).parent_path(), ec);
    std::ofstream f(P(p), std::ios::binary | std::ios::trunc);
    f << s;
    return (bool)f;
}

/* Where the player's boot files may live: the BIOS folders already known to Settings, the game folders and their BIOS siblings. */
std::vector<std::string> search_dirs() {
    std::vector<std::string> d;
    auto add = [&](const std::string& x) { if (!x.empty() && std::find(d.begin(), d.end(), x) == d.end()) d.push_back(x); };
    add(A.st.ps2_bios_dir);
    add(A.st.ps1_bios);
    for (const std::string& f : A.st.rom_folders) {
        add(f);
        std::string par = P(f).parent_path().u8string();
        if (!par.empty() && par != f) { add((P(par) / "GamesBios").u8string()); add((P(par) / "BIOS").u8string()); }
    }
    if (const char* up = std::getenv("USERPROFILE")) { add((P(up) / "Desktop" / "Everything" / "GamesBios").u8string()); add((P(up) / "Desktop" / "GamesBios").u8string()); }
    add(hub::data_dir() + "system");
    add(hub::exe_dir() + "system");
    hubemu::Install dol = hubemu::find(hubemu::Kind::Dolphin, "", "");
    if (!dol.dir.empty()) add(dol.dir + "\\User\\GC");
    return d;
}

std::string sound_dir() { return hub::data_dir() + "boot_sounds"; }

}  // namespace

bool ps1_real_bios_present();   // ps1_ui.cpp

RealBootPlan real_boot_plan(const hub::GameEntry& e) {
    RealBootPlan p;
    p.supported = hub::realboot::supported(e.system);
    if (!p.supported) return p;
    p.enabled = hub::realboot::enabled_for(A.st.real_boot_off, e.system);
    switch (e.system) {
        case hub::System::PS1: p.have = ps1_real_bios_present(); break;
        case hub::System::PS2: p.have = !hubemu::pcsx2_find_bios(hubemu::pcsx2_bios_search_dirs(A.st.ps2_bios_dir, A.st.rom_folders)).file.empty(); break;
        default: p.stage = hub::realboot::find(e.system, search_dirs(), e.serial); p.have = p.stage.ok; break;
    }
    p.use = p.enabled && p.have;
    return p;
}

/* Retrorun arguments for a plan; copies the player's boot files into an isolated system folder first (online runners never see it).
 * `drop_resume` is set when --resume must go (a resumed state would skip the startup). */
std::string real_boot_retro_args(const hub::GameEntry& e, const RealBootPlan& p, bool* drop_resume) {
    if (drop_resume) *drop_resume = false;
    if (!p.supported) return "";
    std::string a;
    const bool gambatte = A.st.gb_core != "sameboy";
    hub::gs::Values gv = gs_values_for(e);
    const bool user_skip = gv.count("gba.skip_bios") && gv["gba.skip_bios"] == "true";
    if (e.system == hub::System::PS1) {   // BIOS and system dir are staged by ps1_play; only the skip switch is ours
        if (p.use || (p.have && !p.enabled)) for (const std::string& o : hub::realboot::retro_opts(e.system, p.use, false, false)) a += " --opt " + o;
        return a;
    }
    if (e.system == hub::System::PS2 || e.system == hub::System::GC) return "";
    if (!p.use) return "";
    const std::string dir = hub::data_dir() + "system\\realboot_" + hub::system_short(e.system);
    std::error_code ec;
    fs::create_directories(P(dir), ec);
    for (auto& c : p.stage.copies) fs::copy_file(P(c.first), P(dir) / c.second, fs::copy_options::overwrite_existing, ec);
    a += " --system-dir " + quote(dir);
    for (const std::string& o : hub::realboot::retro_opts(e.system, true, user_skip, gambatte)) a += " --opt " + o;
    if (drop_resume) *drop_resume = hub::realboot::drops_resume(e.system);
    return a;
}

/* Dolphin: SkipIPL off for this launch (prepare_dolphin writes True on every launch, so the next non-real launch is back to normal)
 * and the player's IPL copied to User\GC\<region>\IPL.bin. */
void real_boot_dolphin_apply(const std::string& install_dir, const RealBootPlan& p) {
    if (!p.use || p.stage.copies.empty()) return;
    std::error_code ec;
    const fs::path dst = P(install_dir) / "User" / "GC" / p.stage.region / "IPL.bin";
    fs::create_directories(dst.parent_path(), ec);
    if (!fs::exists(dst, ec) || fs::file_size(dst, ec) != fs::file_size(P(p.stage.copies[0].first), ec))
        fs::copy_file(P(p.stage.copies[0].first), dst, fs::copy_options::overwrite_existing, ec);
    if (ec) return;
    const std::string ini = install_dir + "\\User\\Config\\Dolphin.ini";
    write_all(ini, hub::realboot::dolphin_skip_ipl(read_all(ini), false));
}

/* PCSX2: fast boot off for real startup, on otherwise (every launch). */
void real_boot_pcsx2_apply(const std::string& install_dir, const RealBootPlan& p) {
    const std::string ini = install_dir + "\\inis\\PCSX2.ini";
    write_all(ini, hub::realboot::pcsx2_fast_boot(read_all(ini), !p.use));
}

bool real_boot_will_use(const hub::GameEntry& e) { return real_boot_plan(e).use; }

void real_boot_settings_ui() {
    settings_label("Real console startup");
    text_wrapped_disabled("Starts the game through your own console's boot files, so the genuine startup logo and sound play. "
                          "RetroHub contains none of them: put the files from your own console in your GamesBios folder. "
                          "Not used in online games.");
    static double looked = -10;
    static std::vector<std::pair<hub::System, RealBootPlan>> rows;
    if (ImGui::GetTime() - looked > 3.0) {
        looked = ImGui::GetTime();
        rows.clear();
        for (hub::System s : {hub::System::GC, hub::System::PS1, hub::System::PS2, hub::System::GBA, hub::System::GB, hub::System::GBC, hub::System::NDS}) {
            hub::GameEntry g;
            g.system = s;
            g.serial = "GALE01";
            rows.push_back({s, real_boot_plan(g)});
        }
    }
    for (auto& r : rows) {
        bool on = hub::realboot::enabled_for(A.st.real_boot_off, r.first);
        std::string label = std::string(hub::system_name(r.first)) + "##rb" + hub::system_short(r.first);
        if (ImGui::Checkbox(label.c_str(), &on)) { A.st.real_boot_off = hub::realboot::set_enabled(A.st.real_boot_off, r.first, on); looked = -10; mark_dirty(); }
        text_wrapped_disabled(r.second.have ? (r.second.stage.detail.empty() ? std::string("Your BIOS was found: the real startup plays.") : r.second.stage.detail)
                                            : std::string(hub::realboot::file_hint(r.first)));
    }
    text_wrapped_disabled("Nintendo 64, SNES, NES, 3DS and Wii U have no startup sequence to show.");

    settings_label("Custom startup sounds");
    text_wrapped_disabled("Drop your own sound (wav, ogg or mp3, up to 15 seconds) named after the console, e.g. gc.wav or ps1.ogg, into the folder below. "
                          "It replaces RetroHub's built-in sound. The startup volume applies.");
    static double looked2 = -10;
    static std::vector<hub::System> have;
    if (ImGui::GetTime() - looked2 > 3.0) { looked2 = ImGui::GetTime(); have = hub::realboot::custom_sounds_present(sound_dir()); }
    std::string list;
    for (hub::System s : have) list += (list.empty() ? "" : ", ") + std::string(hub::system_short(s));
    text_wrapped_disabled(list.empty() ? std::string("No custom sounds yet. Names: n64, nes, snes, gb, gbc, gba, nds, 3ds, gc, wiiu, ps1, ps2.") : "Custom sound for: " + list);
    if (button("Open folder##bootsounds", ImVec2(0, 0))) {
        std::error_code ec;
        fs::create_directories(P(sound_dir()), ec);
        ShellExecuteW(nullptr, L"open", P(sound_dir()).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}
