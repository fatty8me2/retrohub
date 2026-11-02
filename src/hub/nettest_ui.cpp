/* Settings > Online: connection test (STUN x2 + lobby ping) and the firewall probe (NC-M6).
 * The probe runs `n64run --net-probe` and `retrorun --net-probe`: each binds a UDP port and exits, so the Windows Firewall
 * question appears now, with RetroHub in front, and never over a first online game. Triggered from the setup wizard or the button here. */
#include "convenience.h"
#include "nettest.h"

namespace {

hubnt::Job JOB;
bool job_started = false;

struct Probe {
    HANDLE h[2] = {nullptr, nullptr};
    bool running = false;
} PR;

HANDLE spawn_probe(const std::string& exe) {
    if (!file_exists(exe)) return nullptr;
    std::wstring w = L"\"" + std::wstring(exe.begin(), exe.end()) + L"\" --net-probe";
    std::wstring wexe(exe.begin(), exe.end());
    STARTUPINFOW si;
    std::memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    std::memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(wexe.c_str(), &w[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

}  // namespace

bool netprobe_running() { return PR.running; }

void netprobe_start() {
    if (PR.running) return;
    if (g_headless) { A.st.net_probe_done = true; return; }   // screenshot / test runs never trigger a firewall prompt
    PR.h[0] = spawn_probe(n64run_path());
    PR.h[1] = spawn_probe(hub::exe_dir() + "retrorun.exe");
    PR.running = PR.h[0] || PR.h[1];
    if (!PR.running) { A.st.net_probe_done = true; mark_dirty(); }
}

void netprobe_tick() {
    if (!PR.running) return;
    bool any = false;
    for (auto& h : PR.h) {
        if (!h) continue;
        if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) any = true;
        else { CloseHandle(h); h = nullptr; }
    }
    if (!any) { PR.running = false; A.st.net_probe_done = true; mark_dirty(); }
}

void nettest_settings_ui() {
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    settings_label("Connection test");
    text_wrapped_disabled("Checks whether friends can connect straight to you or the relay will be used, and how fast the lobby answers.");
    if (JOB.running()) {
        text_wrapped_disabled("Testing...");
    } else {
        if (button("Test my connection", ImVec2(0, 0), false, true, ui::Icon::Globe)) { JOB.start(server_url()); job_started = true; }
    }
    if (job_started && !JOB.running() && JOB.has_report()) {
        hubnt::Report r = JOB.report();
        ImU32 col = r.verdict.v == hubnt::Verdict::Direct ? ui::GOOD : r.verdict.v == hubnt::Verdict::Relay ? ui::WARN : ui::BAD;
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::PushFont(ui::F.bold);
        ImGui::TextUnformatted(r.verdict.headline.c_str());
        ImGui::PopFont();
        ImGui::PopStyleColor();
        text_wrapped_disabled(r.verdict.detail);
        text_wrapped_disabled(hubnt::ping_text(r.ping_ms) + (r.ping_ms >= 0 ? " (" + std::to_string(r.ping_ms) + " ms)" : std::string()));
    }
    ImGui::Dummy(ImVec2(0, sp(0.5f)));
    ImGui::BeginDisabled(PR.running);
    if (button(PR.running ? "Waiting for Windows..." : "Allow online play in Windows Firewall", ImVec2(0, 0))) netprobe_start();
    ImGui::EndDisabled();
    text_wrapped_disabled("Windows asks once per program. Doing it here means the question never appears over a game. Choose Allow.");
}
