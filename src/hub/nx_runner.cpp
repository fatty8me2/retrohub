#include "nx_runner.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "lichii.h"

namespace fs = std::filesystem;

namespace hubemu {

static fs::path P(const std::string& s) { return fs::u8path(s); }
static std::string S(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }
static std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char c = a.back();
    return (c == '\\' || c == '/') ? a + b : a + "\\" + b;
}
static bool dir_ok(const std::string& p) { std::error_code ec; return !p.empty() && fs::is_directory(P(p), ec); }
static std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

NxRunner nx_runner_value(const hub::gs::Values& v) {
    auto it = v.find(kRunnerKey);
    if (it == v.end()) return NxRunner::Lichii;
    return it->second == "fritchii" ? NxRunner::Fritchii : it->second == "hookshot" ? NxRunner::Hookshot : NxRunner::Lichii;
}

bool fritchii_selected(const hub::GameEntry& e, const hub::gs::Values& v) {
    return e.system == hub::System::Switch && nx_runner_value(v) == NxRunner::Fritchii;
}

Kind nx_emulator_for(const hub::GameEntry& e, const hub::gs::Values& v, bool online_or_room) {
    return !online_or_room && fritchii_selected(e, v) ? Kind::Fritchii : Kind::Lichii;
}

const std::vector<std::string>& nx_skip_keys(Kind k) {
    // TODO(audit): the compatibility audit is still pinning down which keys Fritchii resets at every start (gpu_accuracy, vsync_mode, resolution_setup,
    // scaling_filter, docked mode ... "Section.key"). Add them here and RetroHub stops writing them for Fritchii; nothing else changes.
    static const std::vector<std::string> none;
    (void)k;
    return none;
}

std::vector<std::pair<std::string, std::string>> nx_env(Kind k, const hub::gs::Values& v, uint64_t ram) {
    if (k != Kind::Fritchii) return lichii_env(v, ram);   // Lichii: exactly as before
    std::vector<std::pair<std::string, std::string>> env;
    if (lichii_fast_reads(v)) env.push_back({"EDEN_NX2_NO_REACTIVE", "1"});   // same helper as lichii_env
    auto it = v.find("switch.preload_shaders");
    if (it != v.end() && it->second == "all") env.push_back({"EDEN_NX2_PRELOAD_ALL_PIPELINES", "1"});
    env.push_back({"EDEN_UPDATE_URL", kFritchiiUpdateUrl});
    return env;
}

const char* nx_manifest_suffix(Kind k) { return k == Kind::Fritchii ? "-fritchii" : ""; }

/* ------------------------------------------------------------------ keys + firmware */

static std::string keys_dir_of(const std::string& install) { return join(lichii_user_dir(install), "keys"); }
static std::string registered_of(const std::string& install) { return join(lichii_user_dir(install), "nand\\system\\Contents\\registered"); }

bool nx_seed_setup_needed(const std::string& from, const std::string& to) {
    if (!dir_ok(lichii_user_dir(from)) || to.empty()) return false;
    const NxSetup a = lichii_setup_status(from), b = lichii_setup_status(to);
    return (a.prod_keys && !b.prod_keys) || (a.firmware && !b.firmware);
}

std::string nx_seed_setup(const std::string& from, const std::string& to, bool* did) {
    if (did) *did = false;
    if (!dir_ok(lichii_user_dir(from)) || to.empty()) return "";
    const NxSetup a = lichii_setup_status(from), b = lichii_setup_status(to);
    if (a.prod_keys && !b.prod_keys) {
        std::string e = lichii_import_keys(to, keys_dir_of(from));
        if (!e.empty()) return "keys: " + e;
        if (did) *did = true;
    }
    if (a.firmware && !b.firmware) {
        std::string e = lichii_import_firmware(to, registered_of(from));
        if (!e.empty()) return "firmware: " + e;
        if (did) *did = true;
    }
    return "";
}

/* ------------------------------------------------------------------ Fritchii settings pin */

std::string frizti_ini_pin(const std::string& existing) {
    std::string out = "# RetroHub pins the Fritchii settings migration: RetroHub writes the video settings before every launch.\nrevision=" + std::to_string(kFriztiPinRevision) + "\n";
    size_t p = 0;
    while (p < existing.size()) {
        size_t e = existing.find('\n', p);
        std::string l = existing.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? existing.size() : e + 1;
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        const size_t a = l.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        const std::string t = l.substr(a);
        if (t[0] == '#') continue;   // our own header replaces the old comments
        const size_t eq = t.find('=');
        std::string key = eq == std::string::npos ? t : t.substr(0, eq);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        if (key.compare(0, 7, "global.") == 0 || key == "revision") continue;   // Fritchii refills the globals itself; the revision is ours
        out += t + "\n";   // game.* lines (and anything unknown) stay
    }
    return out;
}


static std::string read_all(const std::string& p) {
    std::ifstream f(P(p), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static bool write_all(const std::string& p, const std::string& t) {
    std::error_code ec;
    fs::create_directories(P(p).parent_path(), ec);
    const fs::path tmp = P(p + ".retrohub-new");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(t.data(), (std::streamsize)t.size());
        if (!f) return false;
    }
    fs::rename(tmp, P(p), ec);
    if (ec) { std::error_code e2; fs::remove(tmp, e2); return false; }
    return true;
}

std::string prepare_fritchii(const std::string& install) {
    const std::string cfg = join(lichii_user_dir(install), "config");
    const std::string ini = join(cfg, "qt-config.ini"), fz = join(cfg, "frizti.ini");
    if (!write_all(ini, fritchii_ini_pin(read_all(ini)))) return "could not write qt-config.ini";
    if (!write_all(fz, frizti_ini_pin(read_all(fz)))) return "could not write frizti.ini";
    return "";
}

/* ------------------------------------------------------------------ shader seed */

static const char* const kShaderFiles[] = {"vulkan.fork-v53.bin", "vulkan.fork-v54.bin"};

/* The folder below `base` named like the title id (any letter case), "" when there is none. */
static std::string tid_dir_name(const std::string& base, uint64_t tid) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", (unsigned long long)tid);
    std::error_code ec;
    for (fs::directory_iterator it(P(base), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2) && lower(S(it->path().filename())) == b) return S(it->path().filename());
    }
    return "";
}

std::vector<ShaderSeedItem> shader_seed_plan(const std::string& from, const std::string& to, uint64_t tid) {
    std::vector<ShaderSeedItem> out;
    if (!tid || from.empty() || to.empty()) return out;
    const std::string sbase = join(lichii_user_dir(from), "cache\\shader");
    if (!dir_ok(sbase)) return out;
    const std::string name = tid_dir_name(sbase, tid);
    if (name.empty()) return out;
    for (const char* f : kShaderFiles) {
        const fs::path src = P(join(join(sbase, name), f));
        std::error_code ec;
        if (!fs::is_regular_file(src, ec)) continue;
        const uint64_t sz = fs::file_size(src, ec);
        if (ec || sz == 0) continue;
        const std::string dst = join(join(join(lichii_user_dir(to), "cache\\shader"), name), f);
        std::error_code e2;
        bool need = !fs::exists(P(dst), e2);
        if (!need) {
            auto ts = fs::last_write_time(src, ec), td = fs::last_write_time(P(dst), e2);
            need = !ec && !e2 && td < ts;   // only when the destination is older
        }
        if (need) out.push_back({S(src), dst, sz});
    }
    return out;
}

std::string shader_seed_run(const std::vector<ShaderSeedItem>& plan, const std::function<void(uint64_t, uint64_t)>& progress, const std::atomic<bool>* cancel) {
    uint64_t total = 0, done = 0;
    for (const auto& it : plan) total += it.size;
    if (!total) return "";
    {   // room for the biggest copy plus a margin, on the destination drive
        std::error_code ec;
        const fs::path first = P(plan[0].dst).parent_path();
        fs::create_directories(first, ec);
        auto sp = fs::space(first, ec);
        if (!ec && sp.available < 2 * total + (256ull << 20)) return "skipped: not enough free disk space for the shader cache copy (needs about " + std::to_string(((2 * total) >> 20) + 256) + " MB free)";
    }
    if (progress) progress(0, total);
    for (const auto& it : plan) {
        std::error_code ec;
        fs::create_directories(P(it.dst).parent_path(), ec);
        const fs::path part = P(it.dst + ".retrohub-part");
        uint64_t copied = 0;
        {
            std::ifstream in(P(it.src), std::ios::binary);
            std::ofstream out(part, std::ios::binary | std::ios::trunc);
            if (!in || !out) { fs::remove(part, ec); return "could not open the shader cache files"; }
            std::vector<char> buf(8u << 20);
            while (in) {
                if (cancel && cancel->load()) { out.close(); fs::remove(part, ec); return "cancelled"; }
                in.read(buf.data(), (std::streamsize)buf.size());
                const std::streamsize n = in.gcount();
                if (n <= 0) break;
                out.write(buf.data(), n);
                if (!out) { out.close(); fs::remove(part, ec); return "could not write the shader cache copy (disk full?)"; }
                done += (uint64_t)n;
                copied += (uint64_t)n;
                if (progress) progress(done, total);
            }
            if (in.bad()) { out.close(); fs::remove(part, ec); return "could not read the shader cache (read error)"; }
            out.flush();
            if (!out) { out.close(); fs::remove(part, ec); return "could not write the shader cache copy (disk full?)"; }
        }
        if (copied != it.size) { fs::remove(part, ec); return "the shader cache changed while it was copied (" + std::to_string(copied) + " of " + std::to_string(it.size) + " bytes): not used"; }   // never rename a truncated copy into place
        std::error_code e1;
        auto mt = fs::last_write_time(P(it.src), e1);
        if (!e1) fs::last_write_time(part, mt, e1);   // same time as the source: it is not "older" next time
        fs::rename(part, P(it.dst), ec);   // replaces an existing (older) file in one step on Windows; a failure leaves the old cache as it was
        if (ec) { std::error_code e2; fs::remove(part, e2); return "could not put the shader cache copy in place: " + ec.message(); }
    }
    return "";
}

/* ------------------------------------------------------------------ saves */

std::string default_save_backup_root() {
    const char* la = std::getenv("LOCALAPPDATA");
    if (!la || !*la) return "";
    return join(join(la, "RetroHub"), "save-backups");
}

static std::string stamp(const char* fmt) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[48];
    std::strftime(b, sizeof b, fmt, &tm);
    return b;
}

/* Any file of `tree` that is not what this side last agreed on (no entry = never agreed). */
static bool tree_unsynced(const std::string& tree, const std::string& key_prefix, const hub::SyncManifest& man) {
    std::error_code ec;
    if (!dir_ok(tree)) return false;
    for (fs::recursive_directory_iterator it(P(tree), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e0;
        if (!it->is_regular_file(e0) || it->path().extension() == L".hubtmp") continue;
        const std::string key = key_prefix + S(fs::relative(it->path(), P(tree), e0));
        auto f = man.agreed.find(key);
        if (f == man.agreed.end() || f->second != hub::file_hash(S(it->path()))) return true;
    }
    return false;
}

/* Some file exists on both sides with different content. */
static bool trees_differ(const std::string& a, const std::string& b) {
    std::error_code ec;
    if (!dir_ok(a) || !dir_ok(b)) return false;
    for (fs::recursive_directory_iterator it(P(a), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e0;
        if (!it->is_regular_file(e0) || it->path().extension() == L".hubtmp") continue;
        const fs::path other = P(b) / fs::relative(it->path(), P(a), e0);
        if (e0 || !fs::is_regular_file(other, e0)) continue;
        if (hub::file_hash(S(it->path())) != hub::file_hash(S(other))) return true;
    }
    return false;
}

/* A plain copy of every file under src into dst that keeps the file times and REFUSES to overwrite: a file that is already there is an error. */
static bool copy_tree_no_overwrite(const std::string& src, const std::string& dst, std::string* err) {
    std::error_code ec;
    fs::create_directories(P(dst), ec);
    for (fs::recursive_directory_iterator it(P(src), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e0;
        if (!it->is_regular_file(e0)) continue;
        const fs::path rel = fs::relative(it->path(), P(src), e0);
        const fs::path to = P(dst) / rel;
        if (e0 || fs::exists(to, e0)) { *err = "snapshot target exists: " + S(to); return false; }
        fs::create_directories(to.parent_path(), e0);
        fs::copy_file(it->path(), to, fs::copy_options::none, e0);
        if (e0) { *err = "cannot copy " + S(it->path()) + ": " + e0.message(); return false; }
        auto mt = fs::last_write_time(it->path(), e0);
        if (!e0) fs::last_write_time(to, mt, e0);
    }
    if (ec) { *err = "cannot read " + src; return false; }
    return true;
}

static std::string safe_name(std::string s) {
    for (char& c : s) if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) c = '_';
    return s.empty() ? "switch" : s;
}

NxSaveSyncReport nx_sync_saves_before_launch(const NxSaveSync& in) {
    NxSaveSyncReport rep;
    if (!is_nx(in.launching) || !in.title_id || in.battery.empty()) return rep;
    const bool lich_first = in.launching == Kind::Lichii;
    const std::string kdir = lich_first ? in.lichii_dir : in.fritchii_dir;
    const std::string odir = lich_first ? in.fritchii_dir : in.lichii_dir;
    if (odir.empty() || kdir.empty() || !dir_ok(lichii_user_dir(odir))) return rep;   // (an empty dir would make "user" a path relative to the working directory)   // no other emulator: nothing to compare (exactly the old behaviour)
    const Kind other = lich_first ? Kind::Fritchii : Kind::Lichii;
    rep.ran = true;
    std::string battery = in.battery;
    while (!battery.empty() && (battery.back() == '\\' || battery.back() == '/')) battery.pop_back();

    const NxSaveMap km = nx_save_map(kdir, in.title_id, battery, true, true);
    const NxSaveMap om = nx_save_map(odir, in.title_id, battery, false, true);
    if (!km.error.empty()) { rep.ok = false; rep.error = km.error; return rep; }
    if (om.rels.size() != km.rels.size()) { rep.ok = false; rep.error = "internal: save maps differ"; return rep; }

    const fs::path game = P(battery).parent_path(), sysdir = game.parent_path(), vault = sysdir.parent_path();
    const std::string conflicts = S(vault / L"_conflicts" / sysdir.filename() / game.filename() / L"battery");
    const std::string kman_path = emu_manifest_path(battery, nx_manifest_suffix(in.launching)), oman_path = emu_manifest_path(battery, nx_manifest_suffix(other));
    hub::SyncManifest kman, oman;
    hub::manifest_load(kman_path, &kman);
    hub::manifest_load(oman_path, &oman);

    const std::string root = in.backup_root.empty() ? default_save_backup_root() : in.backup_root;
    const std::string label = safe_name(in.game_label.empty() ? S(game.filename()) : in.game_label);
    const std::string tag = stamp("local.%Y%m%d-%H%M%S");

    struct Item { std::string ktree, otree, vtree, vrel, prefix; };
    std::vector<Item> items;
    for (size_t i = 0; i < km.rels.size(); ++i)
        items.push_back({join(km.user_root, km.rels[i]), join(om.user_root, om.rels[i]), join(battery, km.vault_rels[i]), km.vault_rels[i], km.vault_rels[i] + "/"});

    // ---- phase 1: decide and take EVERY snapshot before anything is written. A snapshot that cannot be made ends the launch here, with nothing changed.
    // A tree is kept when this launch (or the session that follows) could replace it or leave it behind holding changes that never synced:
    //   the vault: when it differs from either emulator (step 1 may replace it, or the session's copy-back will);
    //   an emulator: when it differs from the vault or the other one AND holds changes since it last synced.
    // Every snapshot point has its own folder (vault-before-sync, <emulator>-before-sync) under a stamp folder that did not exist before, and the copy refuses
    // to overwrite a file that is already there, so one snapshot can never replace another.
    struct Snap { std::string tree, who, vrel; };
    std::vector<Snap> snaps;
    for (const Item& it : items) {
        if (hub::tree_has_files(it.vtree) && (trees_differ(it.otree, it.vtree) || trees_differ(it.ktree, it.vtree))) snaps.push_back({it.vtree, "vault-before-sync", it.vrel});
        if (hub::tree_has_files(it.ktree) && (trees_differ(it.ktree, it.vtree) || trees_differ(it.ktree, it.otree)) && tree_unsynced(it.ktree, it.prefix, kman))
            snaps.push_back({it.ktree, lower(name(in.launching)) + "-before-sync", it.vrel});
        if (hub::tree_has_files(it.otree) && (trees_differ(it.otree, it.vtree) || trees_differ(it.otree, it.ktree)) && tree_unsynced(it.otree, it.prefix, oman))
            snaps.push_back({it.otree, lower(name(other)) + "-before-sync", it.vrel});
    }
    if (!snaps.empty()) {
        if (root.empty()) { rep.ok = false; rep.error = "no place for save backups (LOCALAPPDATA is not set)"; return rep; }
        const std::string base = join(join(root, label), stamp("%Y%m%d-%H%M%S") + "-before-" + lower(name(in.launching)) + "-launch");
        std::string snap_dir = base;
        std::error_code ec;
        for (int n = 2; fs::exists(P(snap_dir), ec) && n < 1000; ++n) snap_dir = base + "-" + std::to_string(n);   // never reuse (or touch) an existing backup
        if (fs::exists(P(snap_dir), ec)) { rep.ok = false; rep.error = "could not find a free backup folder"; return rep; }
        fs::create_directories(P(snap_dir), ec);
        if (ec || !fs::is_directory(P(snap_dir), ec)) { rep.ok = false; rep.error = "could not create the backup folder " + snap_dir; return rep; }
        for (const Snap& s : snaps) {
            const std::string to = join(join(snap_dir, s.who), s.vrel);
            std::string err;
            if (!copy_tree_no_overwrite(s.tree, to, &err)) { rep.ok = false; rep.error = "could not back up " + s.tree + ": " + err; return rep; }
            rep.backups++;
            rep.backup_dirs.push_back(to);
        }
    }

    // ---- phase 2: the writes. sync_tree_newest keeps its usual last 10 copies in the vault's _conflicts (that folder syncs to every PC and the cloud, so it stays bounded,
    // as for every other emulator); the local snapshots taken above are what is never pruned.
    for (const Item& it : items) {
        // 1. the other emulator -> the vault
        if (hub::tree_has_files(it.otree)) {
            hub::TreeSyncReport r = hub::sync_tree_newest(it.otree, it.vtree, join(conflicts, it.vrel), tag, &oman, it.prefix);
            rep.copied += r.copied;
            if (!r.ok) { rep.ok = false; rep.error = r.error; hub::manifest_save(kman_path, kman); hub::manifest_save(oman_path, oman); return rep; }
        }
        // 2. the vault -> the launching emulator
        if (hub::tree_has_files(it.vtree)) {
            hub::TreeSyncReport r = hub::sync_tree_newest(it.vtree, it.ktree, join(conflicts + "\\emulator", it.vrel), tag, &kman, it.prefix);
            rep.copied += r.copied;
            if (!r.ok) { rep.ok = false; rep.error = r.error; hub::manifest_save(kman_path, kman); hub::manifest_save(oman_path, oman); return rep; }
        }
    }
    hub::manifest_save(kman_path, kman);
    hub::manifest_save(oman_path, oman);
    rep.summary = std::to_string(rep.copied) + " save file(s) brought up to date, " + std::to_string(rep.backups) + " backup(s) made";
    return rep;
}

}  // namespace hubemu
