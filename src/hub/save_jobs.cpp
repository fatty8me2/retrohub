#include "app.h"

/* ---------- save jobs (sync / export / import) ---------- */

void set_toast(const std::string& s, bool good, double secs) {
    push_toast(s, good ? COL_GOOD : COL_BAD, secs);
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
    A.job.th = std::thread([]() {
        SaveJob& j = A.job;
        if (j.kind == SaveJob::Sync) j.report = hub::saves::sync_folder(j.path, j.include_states);
        else if (j.kind == SaveJob::Export) j.export_ok = hub::saves::export_zip(j.path, j.include_states, &j.export_err);
        else if (j.kind == SaveJob::Import) j.report = hub::saves::import_zip(j.path);
        j.done = true;
    });
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
    if (A.st.sync_folder.empty()) return;
    /* never touch battery files under a running emulator: queue until the game exits */
    if (hubui::sync_request(A.sync_gate, manual, A.proc != nullptr, A.job.running) == hubui::SyncAction::Queue) return;
    start_job(SaveJob::Sync, A.st.sync_folder, manual);
}
void run_pending_sync() {
    bool manual = false;
    if (hubui::sync_take_pending(A.sync_gate, A.proc != nullptr, A.job.running, &manual)) start_job(SaveJob::Sync, A.st.sync_folder, manual);
}
void auto_sync() {
    if (!A.st.sync_folder.empty() && A.st.sync_auto) start_sync(false);
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
    if (j.kind == SaveJob::Sync) {
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

