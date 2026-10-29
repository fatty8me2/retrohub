#include "app.h"
#include <atomic>
#include <chrono>
#include <list>
#include <mutex>
#include "cloud_internal.h"   // cloud_prelaunch_ex and the engine hooks (not part of the UI contract)

/* ---------- save jobs (sync / export / import) ---------- */

void set_toast(const std::string& s, bool good, double secs) {
    push_toast(s, good ? COL_GOOD : COL_BAD, secs);
}


/* Cloud problems that are not shown as toasts go to cloud_saves.log in the data dir (job thread only). */
static void log_cloud(const std::string& msg) {
    std::ofstream f(hub::data_dir() + "cloud_saves.log", std::ios::app);
    if (!f) return;
    std::time_t t = std::time(nullptr);
    char ts[32] = {};
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    f << ts << "  " << msg << "\n";
}

/* The Worker is reached over https; server_url() is the wss:// lobby URL. */
static std::string cloud_base_url() {
    std::string base = server_url();
    if (base.rfind("wss://", 0) == 0) base = "https://" + base.substr(6);
    else if (base.rfind("ws://", 0) == 0) base = "http://" + base.substr(5);
    return base;
}

static bool g_cloud_on = false;

/* ---------- tracked background cloud jobs ---------- */
namespace {
struct BgJob { std::thread th; std::string gkey; std::atomic<bool> done{false}; };
std::mutex g_bg_mu;
std::list<std::shared_ptr<BgJob>> g_bg;
std::atomic<int> g_bg_gen{0};
bool g_bg_closed = false;
}
bool cloud_bg_run(const std::string& gkey, std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(g_bg_mu);
    if (g_bg_closed) return false;
    auto j = std::make_shared<BgJob>();
    j->gkey = gkey;
    j->th = std::thread([j, fn]() { fn(); j->done = true; });
    g_bg.push_back(j);
    return true;
}
bool cloud_bg_busy(const std::string& gkey) {
    std::lock_guard<std::mutex> lk(g_bg_mu);
    for (auto& j : g_bg) if (!j->done && (gkey.empty() || j->gkey == gkey)) return true;
    return false;
}
int cloud_bg_generation() { return g_bg_gen; }
void cloud_bg_poll() {
    std::vector<std::shared_ptr<BgJob>> fin;
    {
        std::lock_guard<std::mutex> lk(g_bg_mu);
        for (auto it = g_bg.begin(); it != g_bg.end();) {
            if ((*it)->done) { fin.push_back(*it); it = g_bg.erase(it); } else ++it;
        }
    }
    for (auto& j : fin) if (j->th.joinable()) j->th.join();
    if (!fin.empty()) ++g_bg_gen;
}
void cloud_bg_join_all() {
    std::list<std::shared_ptr<BgJob>> all;
    { std::lock_guard<std::mutex> lk(g_bg_mu); g_bg_closed = true; all.swap(g_bg); }
    for (auto& j : all) if (j->th.joinable()) j->th.join();
}
/* Gate / worker threads only: waits (bounded) until no background job touches this game. */
static void bg_wait_idle(const std::string& gkey) {
    for (int i = 0; i < 600 && cloud_bg_busy(gkey); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void cloud_boot() {
    /* Screenshot/QA runs normally never touch the network. HUB_SHOT_CLOUD_REAL=1 lets a headless run start the REAL engine, but only
     * against a loopback server (qa\cloud_ui_shots_real.ps1 uses a local Worker). */
    const bool real_shot = g_headless && std::getenv("HUB_SHOT_CLOUD_REAL") && cloud_base_url().rfind("http://127.0.0.1", 0) == 0;
    if ((g_headless && !real_shot) || std::getenv("RETROHUB_NO_CLOUD")) return;
    hub::saves::cloud_engine_start(cloud_base_url());   // automatic for every account: no opt-in, identity = the account
    g_cloud_on = true;
}
void cloud_shutdown() {
    cloud_bg_join_all();   // Resolve / Keep / Remove / exit processing finish before the engine state is reset
    if (!g_cloud_on) return;
    g_cloud_on = false;
    hub::saves::cloud_engine_stop();
}
bool cloud_active() { return g_cloud_on; }

hub::saves::GameId cloud_id_for(const hub::GameEntry& e) {
    if (is_native(e)) return hub::saves::GameId{hubui::kSmashSaveSystem, hubui::kSmashSaveKey};
    return hub::saves::GameId{sys_short(e), e.key};
}

/* Legacy status struct (Saves page line) filled from the engine, no network. */
hub::saves::CloudStatus cloud_status_now() {
    hub::saves::CloudStatus s;
    if (!g_cloud_on) { s.unavailable = true; s.error = "cloud saves are off"; return s; }
    hub::saves::CloudAccountInfo a = hub::saves::cloud_account();
    s.enabled = a.known && a.online;
    s.unavailable = a.needs_attention || a.paused_by_server;
    s.used = a.used;
    s.quota = a.cap;
    for (auto& g : hub::saves::cloud_games()) if (g.cloud_bytes > 0) s.files++;
    if (!s.enabled && !s.unavailable) s.error = a.known ? "offline" : "";
    return s;
}

/* ---------- pre-launch gate (see CloudGate in app.h) ---------- */

static void gate_finish() {
    cloud_ui_checking("", false);
    A.cloud_gate.phase = CloudGate::Idle;
    A.cloud_gate.res = hub::saves::PrelaunchResult();
    std::function<void()> go;
    go.swap(A.cloud_gate.proceed);
    if (go) go();
}
static void gate_task(std::function<void()> f) {
    if (A.cloud_gate.th.joinable()) A.cloud_gate.th.join();
    A.cloud_gate.phase = CloudGate::Running;
    A.cloud_gate.done = false;
    A.cloud_gate.th = std::thread([f]() { f(); A.cloud_gate.done = true; });
}

bool cloud_gate_begin(const hub::GameEntry& e, std::function<void()> proceed) {
    if (A.cloud_gate.pass_key == e.key) { A.cloud_gate.pass_key.clear(); return false; }   // re-entered after the gate
    if (!g_cloud_on || g_headless || A.cloud_gate.phase != CloudGate::Idle) return false;
    hub::saves::GameId id = cloud_id_for(e);
    if (id.system.empty() || id.key.empty()) return false;
    A.cloud_gate.game = id;
    A.cloud_gate.title = e.title;
    A.cloud_gate.mode = hub::saves::SyncMode::ReadWrite;   // offline play from the library always uses the vault save
    A.cloud_gate.proceed = std::move(proceed);
    cloud_ui_checking(e.title, true);   // "Getting your save from the cloud..." strip (Lane U)
    /* A Resolve / Keep / Remove or the previous run's exit processing may still be working on this game: the gate thread waits for it
     * (the strip shows "Getting your save from the cloud...") so the lease and the save files are never touched by two sessions. */
    gate_task([id]() { bg_wait_idle(id.gkey()); A.cloud_gate.res = hub::saves::cloud_prelaunch(id, hub::saves::SyncMode::ReadWrite); });
    return true;
}

/* Decides what the last answer needs: continue, or ask (UI lane dialogs), or apply the default when nobody asks. */
static void gate_handle_result() {
    using hub::saves::PrelaunchResult;
    const PrelaunchResult& r = A.cloud_gate.res;
    cloud_ui_checking("", false);
    cloud_ui_prelaunch_notice(r);   // offline / "pulled N files" notices
    if (r.action == PrelaunchResult::NeedConflictDecision) {
        A.cloud_gate.phase = CloudGate::AskConflict;
        if (A.cloud_gate.on_ask) A.cloud_gate.on_ask();
        else cloud_ui_ask_conflict(A.cloud_gate.game, A.cloud_gate.title, r.conflicts,
                                   [](hub::saves::ConflictChoice c) { cloud_gate_choose_conflict(c); });
        return;
    }
    if (r.action == PrelaunchResult::NeedLeaseDecision) {
        A.cloud_gate.phase = CloudGate::AskLease;
        if (A.cloud_gate.on_ask) A.cloud_gate.on_ask();
        else cloud_ui_ask_lease(A.cloud_gate.game, A.cloud_gate.title, r.held_by, r.held_since,
                                [](hub::saves::LeaseChoice c) { cloud_gate_choose_lease(c); });
        return;
    }
    gate_finish();
}

void cloud_gate_tick() {
    if (A.cloud_gate.phase == CloudGate::Running && A.cloud_gate.done) {
        if (A.cloud_gate.th.joinable()) A.cloud_gate.th.join();
        gate_handle_result();
    }
}

void cloud_gate_choose_conflict(hub::saves::ConflictChoice c) {
    if (A.cloud_gate.phase != CloudGate::AskConflict) return;
    hub::saves::GameId id = A.cloud_gate.game;
    cloud_ui_checking(A.cloud_gate.title, true);
    gate_task([id, c]() {
        hub::saves::cloud_resolve_conflict(id, c);   // UseCloud downloads, so it runs off the UI thread
        A.cloud_gate.res = hub::saves::PrelaunchResult();
    });
}

void cloud_gate_choose_lease(hub::saves::LeaseChoice c) {
    if (A.cloud_gate.phase != CloudGate::AskLease) return;
    if (c == hub::saves::LeaseChoice::Cancel) {
        hub::saves::cloud_resolve_lease(A.cloud_gate.game, c);
        A.cloud_gate.phase = CloudGate::Idle;
        A.cloud_gate.proceed = nullptr;
        return;
    }
    hub::saves::GameId id = A.cloud_gate.game;
    gate_task([id, c]() { A.cloud_gate.res = hub::saves::cloud_resolve_lease(id, c); });   // continues the pre-launch (pull / conflicts)
}

void cloud_account_changed() {
    if (g_cloud_on) hub::saves::cloud_account_changed();
}

/* Online host: the normal pre-launch pull first, so the save it shares with the room is the freshest. The vault is only read
 * afterwards; guests' throwaway saves never upload. Bounded (2.5 s manifest, 6 s pull) and silent when offline. */
void cloud_host_pull(const hub::GameEntry& e) {
    if (!g_cloud_on || g_headless) return;
    hub::saves::GameId id = cloud_id_for(e);
    if (id.system.empty() || id.key.empty()) return;
    hub::saves::CloudAccountInfo ai = hub::saves::cloud_account();
    if (cloud_bg_busy(id.gkey())) return;   // never wait on the per-game lock a background job holds
    if (ai.known && !ai.online) return;   // the engine already knows the server is unreachable: do not stall the frame thread
    hub::saves::cloud_prelaunch_ex(id, hub::saves::SyncMode::ReadOnly, 1500, 2500);   // runs on the frame thread: keep the worst case short
}

/* spawn(): the session is tracked so the exit pushes the game (read-only sessions use a throwaway save dir: nothing is pushed). */
void cloud_run_started() {
    if (!g_cloud_on || A.run.key.empty() || A.run.system_short.empty()) return;
    A.run.cloud_started = true;
    hub::saves::cloud_game_started(hub::saves::GameId{A.run.system_short, A.run.key},
                                   A.run.online ? hub::saves::SyncMode::ReadOnly : hub::saves::SyncMode::ReadWrite);
}

bool sync_enabled() { return !A.st.sync_folder.empty() || g_cloud_on; }   // the cloud is automatic; the folder is an optional extra

/* Merges the folder and cloud reports of one sync run into the one line the Saves page shows. */
static hub::saves::SyncReport merge_reports(const hub::saves::SyncReport& a, const hub::saves::SyncReport& b) {
    hub::saves::SyncReport r;
    r.pushed = a.pushed + b.pushed;
    r.pulled = a.pulled + b.pulled;
    r.conflicts = a.conflicts + b.conflicts;
    r.ok = a.ok && b.ok;
    r.error = a.error;
    if (!b.error.empty()) r.error += (r.error.empty() ? "" : "; ") + b.error;
    r.log = a.log;
    r.log.insert(r.log.end(), b.log.begin(), b.log.end());
    return r;
}

void start_job(SaveJob::Kind k, const std::string& path, bool manual) {
    if (A.job.running) return;
    if (A.job.th.joinable()) A.job.th.join();
    A.job.kind = k;
    A.job.running = true;
    A.job.done = false;
    A.job.path = path;
    A.job.include_states = k == SaveJob::Export;
    A.job.manual = manual;
    A.job.report = hub::saves::SyncReport();
    A.job.export_ok = false;
    A.job.export_err.clear();
    A.job.cloud = false;
    A.job.cloud_skipped = path.empty();   // nothing for this job to report when only the (automatic) cloud sync runs
    A.job.th = std::thread([]() {
        SaveJob& j = A.job;
        if (j.kind == SaveJob::Sync) {
            hub::saves::SyncReport folder;
            /* order: cloud first (asynchronous, owned by the engine), the optional folder second and never the truth */
            if (j.manual && g_cloud_on) hub::saves::cloud_sync_all_now();
            if (!j.path.empty()) folder = hub::saves::sync_folder(j.path, j.include_states);
            j.report = folder;
        }
        else if (j.kind == SaveJob::Status) {
            j.cloud_stat = cloud_status_now();
        }
        else if (j.kind == SaveJob::Export) j.export_ok = hub::saves::export_zip(j.path, j.include_states, &j.export_err);
        else if (j.kind == SaveJob::Import) j.report = hub::saves::import_zip(j.path);
        j.done = true;
    });
}

void start_cloud_probe() {
    A.cloud_probed = true;
    if (!A.job.running) start_job(SaveJob::Status, "", false);
}

const std::vector<SyncSuggestion>& sync_suggestions() {
    static std::vector<SyncSuggestion> v;
    static bool done = false;
    if (done) return v;
    done = true;
    std::set<std::string> seen;
    auto add = [&](const std::string& label, const std::string& base) {
        if (base.empty() || !dir_exists(base) || !seen.insert(base).second) return;
        v.push_back({label, no_slash(base) + "\\RetroHub"});
    };
    add("OneDrive", env_var(L"OneDriveConsumer"));
    add("OneDrive", env_var(L"OneDrive"));
    add("OneDrive (work)", env_var(L"OneDriveCommercial"));
    std::string up = env_var(L"USERPROFILE");
    if (!up.empty()) {
        add("Dropbox", up + "\\Dropbox");
        add("Google Drive", up + "\\Google Drive");
        add("Google Drive", up + "\\My Drive");
        add("iCloud Drive", up + "\\iCloudDrive");
    }
    for (char d = 'D'; d <= 'Z'; ++d) {
        std::string gd = std::string(1, d) + ":\\My Drive";
        if (dir_exists(gd)) add("Google Drive", gd);
    }
    return v;
}

void start_sync(bool manual) {
    if (!sync_enabled()) return;
    /* never touch battery files under a running emulator: queue until the game exits */
    if (hubui::sync_request(A.sync_gate, manual, A.proc != nullptr, A.job.running) == hubui::SyncAction::Queue) return;
    start_job(SaveJob::Sync, A.st.sync_folder, manual);
}
void run_pending_sync() {
    bool manual = false;
    if (hubui::sync_take_pending(A.sync_gate, A.proc != nullptr, A.job.running, &manual)) start_job(SaveJob::Sync, A.st.sync_folder, manual);
}
void auto_sync() {
    if (hubui::auto_sync_should_run(sync_enabled(), A.st.sync_auto)) start_sync(false);
}

std::string report_summary(const hub::saves::SyncReport& r, const char* sent_word, const char* got_word) {
    if (!r.ok) return "Failed: " + (r.error.empty() ? std::string("unknown error") : r.error);
    if (r.pushed == 0 && r.pulled == 0 && r.conflicts == 0) return "Everything is already up to date.";
    std::string s;
    if (r.pushed) s += std::to_string(r.pushed) + " file(s) " + sent_word;
    if (r.pulled) s += (s.empty() ? "" : ", ") + std::to_string(r.pulled) + " file(s) " + got_word;
    if (r.conflicts) s += (s.empty() ? "" : ", ") + std::to_string(r.conflicts) + " conflict(s) (older copy kept in _conflicts)";
    return s + ".";
}

void poll_job() {
    if (!A.job.running || !A.job.done) return;
    if (A.job.th.joinable()) A.job.th.join();
    SaveJob& j = A.job;
    if (j.kind == SaveJob::Status || (j.kind == SaveJob::Sync && j.cloud)) {
        A.cloud_stat = j.cloud_stat;
        A.cloud_stat_known = true;
    }
    if (j.kind == SaveJob::Status) {
        // probe only: no toast, no result line
    } else if (j.kind == SaveJob::Sync) {
        bool nothing_ran = j.path.empty() && j.cloud_skipped;   // only cloud, and it is unavailable: the status line says why
        A.sync_result = nothing_ran ? std::string() : report_summary(j.report, "sent", "received");
        A.sync_result_ok = j.report.ok;
        bool quiet = j.report.ok && j.report.pushed == 0 && j.report.pulled == 0 && j.report.conflicts == 0 && !j.manual;
        if (nothing_ran || j.cloud_skipped && j.report.ok) {
            // no toast for an unavailable cloud (status line + log only)
        } else if (!j.report.ok) {
            // real transfer errors: one toast per session, the result line keeps showing it
            if (!A.sync_error_toasted) { A.sync_error_toasted = true; set_toast("Save sync " + A.sync_result, false); }
        } else if (!quiet) {
            set_toast("Saves synced: " + A.sync_result, true);
        }
    } else if (j.kind == SaveJob::Export) {
        A.export_ok = j.export_ok;
        A.export_result = j.export_ok ? "Exported to " + j.path : "Export failed: " + j.export_err;
        set_toast(j.export_ok ? "Saves exported." : A.export_result, j.export_ok);
    } else if (j.kind == SaveJob::Import) {
        A.import_ok = j.report.ok;
        A.import_result = report_summary(j.report, "restored", "restored");
        if (j.report.ok && (j.report.pushed + j.report.pulled) > 0)
            A.import_result = std::to_string(j.report.pushed + j.report.pulled) + " file(s) restored" +
                              (j.report.conflicts ? ", " + std::to_string(j.report.conflicts) + " conflict(s) (older copy kept)" : "") + ".";
        set_toast(j.report.ok ? "Saves imported." : A.import_result, j.report.ok);
    }
    if ((j.kind == SaveJob::Sync || j.kind == SaveJob::Import) && j.report.ok) fr_adopt_identity_after_vault_change();
    A.vault_dirty = true;
    A.info_at = -100;
    j.kind = SaveJob::None;
    j.running = false;
    j.done = false;
    std::function<void()> next;
    next.swap(A.after_job);
    if (next) next();
}

