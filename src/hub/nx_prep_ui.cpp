#include "app.h"

#include "nx_runner.h"

/* ---------- Fritchii: the one-time copy job that runs between Play and the game starting ----------
 * Fritchii has its own user\ folder, so before it starts RetroHub COPIES (never links) what Lichii already holds:
 *   - the player's keys and firmware, when Fritchii has none (a few hundred MB for the firmware),
 *   - Lichii's per-game shader cache (vulkan.fork-v53 / v54, up to ~1.4 GB), when Fritchii has none or an older one.
 * That can take a while, so it runs on its own thread with a small progress dialog; the game starts when it ends. A failed or skipped copy never stops the game
 * (Fritchii just builds its own caches / asks for keys); only "Cancel" does. It never runs while an emulator runs (launch.cpp checks first). The save comparison is
 * quick and runs inline in launch.cpp (nx_sync_saves_before_launch). */

namespace {

struct Prep {
    std::atomic<bool> running{false}, done{false}, cancel{false};
    std::atomic<uint64_t> got{0}, total{0};
    std::mutex mu;
    std::string status, error;   // guarded by mu
    std::thread th;
    std::string key;             // the game being prepared
    std::string ready_key;       // set when the job ended: the next launch_external for this key passes the gate
} PREP;

}  // namespace

/* launch.cpp: true when the launch must wait (a job was started or is running). false = go on with the launch. */
bool nxprep_gate(const hub::GameEntry& e, const std::string& fritchii_dir) {
    if (PREP.ready_key == e.key) { PREP.ready_key.clear(); return false; }
    if (PREP.running) return true;
    const hubemu::Install li = hubemu::find(hubemu::Kind::Lichii, "", "");
    if (li.exe.empty() || li.dir.empty()) return false;   // no Lichii: nothing to copy
    const uint64_t tid = hubemu::title_id_from_key(e.key);
    std::vector<hubemu::ShaderSeedItem> plan = hubemu::shader_seed_plan(li.dir, fritchii_dir, tid);
    const bool seed = hubemu::nx_seed_setup_needed(li.dir, fritchii_dir);
    if (plan.empty() && !seed) return false;
    if (PREP.th.joinable()) PREP.th.join();
    PREP.cancel = false;
    PREP.done = false;
    PREP.got = PREP.total = 0;
    PREP.key = e.key;
    { std::lock_guard<std::mutex> lk(PREP.mu); PREP.status = "Getting Fritchii ready..."; PREP.error.clear(); }
    PREP.running = true;
    const std::string from = li.dir, to = fritchii_dir;
    autotest_log("fritchii: prep job started (keys/firmware " + std::string(seed ? "yes" : "no") + ", shader files " + std::to_string(plan.size()) + ")");
    PREP.th = std::thread([from, to, seed, plan]() {
        std::string err;
        if (seed) {
            { std::lock_guard<std::mutex> lk(PREP.mu); PREP.status = "Copying your keys and firmware from Lichii..."; }
            err = hubemu::nx_seed_setup(from, to);
        }
        if (!plan.empty() && !PREP.cancel) {
            { std::lock_guard<std::mutex> lk(PREP.mu); PREP.status = "Copying Lichii's shader cache (one time, this can take a minute)..."; }
            std::string e2 = hubemu::shader_seed_run(plan, [](uint64_t d, uint64_t t) { PREP.got = d; PREP.total = t; }, &PREP.cancel);
            if (!e2.empty() && err.empty()) err = e2;
        }
        if (PREP.cancel && err.empty()) err = "cancelled";
        { std::lock_guard<std::mutex> lk(PREP.mu); PREP.error = err; }
        PREP.done = true;
    });
    return true;
}

void poll_nxprep() {
    if (!PREP.running || !PREP.done) return;
    if (PREP.th.joinable()) PREP.th.join();
    PREP.running = false;
    std::string err;
    { std::lock_guard<std::mutex> lk(PREP.mu); err = PREP.error; }
    autotest_log("fritchii: prep job ended" + (err.empty() ? std::string(" ok") : " | " + err));
    if (err == "cancelled") { set_toast("Cancelled. The game was not started.", false); return; }
    if (!err.empty()) set_toast(err.compare(0, 8, "skipped:") == 0 ? "Fritchii starts without Lichii's shader cache (" + err.substr(9) + ")."
                                                                  : "Could not copy everything from Lichii (" + err + "). Fritchii starts anyway.", false, 10.0);
    PREP.ready_key = PREP.key;
    if (const hub::GameEntry* g = find_entry(PREP.key)) { hub::GameEntry copy = *g; if (!A.proc) play_entry(copy); }
}

void draw_nxprep_dialog() {
    if (!PREP.running) return;
    bool open = false;
    emu_modal_begin("Getting Fritchii ready", &open);
    if (!open) return;
    std::string st;
    { std::lock_guard<std::mutex> lk(PREP.mu); st = PREP.status; }
    ImGui::TextWrapped("%s", st.c_str());
    const uint64_t got = PREP.got, tot = PREP.total;
    if (tot) {
        char ov[48];
        std::snprintf(ov, sizeof ov, "%.2f / %.2f GB", got / 1073741824.0, tot / 1073741824.0);
        ImGui::ProgressBar((float)((double)got / (double)tot), ImVec2(-1, 24 * S), ov);
    }
    text_wrapped_disabled("Fritchii keeps its own copy, so Lichii is not changed. Your game starts as soon as this is done.");
    ImGui::Dummy(ImVec2(0, 6 * S));
    if (button("Cancel", ImVec2(160 * S, 44 * S)) || ui::modal_back()) PREP.cancel = true;
    ImGui::EndPopup();
}
