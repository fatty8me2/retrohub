/* Settings > Help: Copy diagnostics. Zips userdata\logs (hub.log, hang and crash reports), version info and the emulator list to the Desktop.
 * identity*.json and anything under _identity is never included (diag_logic.cpp). Opens the folder afterwards: the hub has focus then. */
#include "convenience.h"
#include "diag_logic.h"
#include "hub_version.h"

#include <ctime>
#include <shlobj.h>

namespace {

std::string desktop_dir() {
    PWSTR w = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &w)) && w) out = U(w);
    if (w) CoTaskMemFree(w);
    return out;
}

std::string build_and_save(std::string* where) {
    const std::string root = hub::data_dir();
    /* logs\ holds hub.log / hub.1.log / hub.2.log, hang reports and crash reports (newest 5 only) */
    std::vector<hubdiag::Entry> files = hubdiag::limit_total_bytes(hubdiag::newest_crash_reports_only(hubdiag::collect_dir(root + "logs", "logs/")));
    std::string versions = std::string("RetroHub ") + HUB_VERSION_STR + "\n";
    {   // runner builds sit next to the hub; the VERSION file travels with them
        std::string v = read_text_file(hub::exe_dir() + "VERSION");
        if (!v.empty()) versions += "Package VERSION: " + v + "\n";
    }
    char tb[64];
    std::time_t t = std::time(nullptr);
    std::strftime(tb, sizeof tb, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    versions += std::string("Created: ") + tb + "\n";
    std::string emus;
    static const hubemu::Kind kinds[] = {hubemu::Kind::Azahar, hubemu::Kind::Cemu, hubemu::Kind::Dolphin, hubemu::Kind::Pcsx2};
    for (auto k : kinds) {
        hubemu::Install in = hubemu::find(k, k == hubemu::Kind::Cemu ? A.st.cemu_exe : "", "");
        emus += std::string(hubemu::name(k)) + ": " + (in.exe.empty() ? "not installed" : in.exe + "  (" + in.how + ")") + "\n";
    }
    emus += "n64run.exe: " + std::string(file_exists(n64run_path()) ? "present" : "missing") + "\n";
    emus += "retrorun.exe: " + std::string(file_exists(hub::exe_dir() + "retrorun.exe") ? "present" : "missing") + "\n";
    std::string settings = "games: " + std::to_string(A.lib.size()) + "\nrom folders: " + std::to_string(A.st.rom_folders.size()) +
                           "\nserver: " + server_url() + "\ncontroller family: " + pad_kind_label() + "\n";
    std::string dir = desktop_dir();
    if (dir.empty()) return "Could not find your Desktop.";
    char nb[64];
    std::strftime(nb, sizeof nb, "RetroHub-diagnostics-%Y%m%d-%H%M%S.zip", std::localtime(&t));
    std::string out = with_slash(dir) + nb;
    std::string err;
    if (!hubdiag::write_zip(out, files, {{"versions.txt", versions}, {"emulators.txt", emus}, {"summary.txt", settings}}, &err)) return err;
    *where = out;
    return "";
}

std::string g_result;
bool g_ok = false;

}  // namespace

void diag_settings_ui() {
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    settings_label("Something wrong?");
    text_wrapped_disabled("Saves a zip on your Desktop with your logs, hang reports, version numbers and the list of installed emulators, so you can send it to whoever is helping. "
                          "Your account and identity files are never included.");
    if (button("Copy diagnostics to Desktop", ImVec2(0, 0), false, true, ui::Icon::Folder)) {
        std::string where;
        g_result = build_and_save(&where);
        g_ok = g_result.empty();
        if (g_ok) {
            g_result = "Saved " + file_name(where);
            if (!g_headless) ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + W(where) + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        }
    }
    if (!g_result.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, g_ok ? ui::GOOD : ui::BAD);
        ImGui::TextWrapped("%s", g_result.c_str());
        ImGui::PopStyleColor();
    }
}
