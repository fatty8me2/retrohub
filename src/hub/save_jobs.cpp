#include "app.h"

/* ---------- save jobs (sync / export / import) ---------- */

void set_toast(const std::string& s, bool good, double secs) {
    push_toast(s, good ? COL_GOOD : COL_BAD, secs);
}


/* Cloud config from the friends identity. The Worker is reached over https; server_url() is the wss:// lobby URL. */
static hub::saves::CloudConfig cloud_config() {
    hub::saves::CloudConfig c;
    std::string base = server_url();
    if (base.rfind("wss://", 0) == 0) base = "https://" + base.substr(6);
    else if (base.rfind("ws://", 0) == 0) base = "http://" + base.substr(5);
    c.base_url = base;
    hubnet::Identity id = hubnet::identity_load_or_create(hub::data_dir(), hub::saves::vault_dir());
    c.user_id = id.user_id;
    c.secret = id.secret;
    c.include_states = A.st.sync_states;
    return c;
}

bool sync_enabled() { return !A.st.sync_folder.empty() || A.st.cloud_saves; }

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
    A.job.include_states = A.st.sync_states;
    A.job.manual = manual;
    A.job.report = hub::saves::SyncReport();
    A.job.export_ok = false;
    A.job.export_err.clear();
    A.job.cloud = (k == SaveJob::Sync && A.st.cloud_saves) || k == SaveJob::Status;
    if (A.job.cloud) A.job.cloud_cfg = cloud_config();
    A.job.th = std::thread([]() {
        SaveJob& j = A.job;
        if (j.kind == SaveJob::Sync) {
            hub::saves::SyncReport folder, cloud;
            if (!j.path.empty()) folder = hub::saves::sync_folder(j.path, j.include_states);
            if (j.cloud) {
                cloud = hub::saves::sync_cloud(j.cloud_cfg);
                j.cloud_stat = hub::saves::cloud_status(j.cloud_cfg);
            }
            j.report = merge_reports(folder, cloud);
        }
        else if (j.kind == SaveJob::Status) j.cloud_stat = hub::saves::cloud_status(j.cloud_cfg);
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
    if (sync_enabled() && A.st.sync_auto) start_sync(false);
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
        A.sync_result = report_summary(j.report, "sent", "received");
        A.sync_result_ok = j.report.ok;
        bool quiet = j.report.ok && j.report.pushed == 0 && j.report.pulled == 0 && j.report.conflicts == 0 && !j.manual;
        if (!quiet) set_toast(j.report.ok ? "Saves synced: " + A.sync_result : "Save sync " + A.sync_result, j.report.ok);
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

