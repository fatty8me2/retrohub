// plat_proc on posix_spawn / libproc / NSWorkspace / CoreGraphics (docs/MACOS.md). Objective-C++, ARC on.
#include "plat_proc.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Foundation/Foundation.h>

#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/proc_info.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

extern char** environ;

namespace plat {
namespace proc {
namespace {

using Clock = std::chrono::steady_clock;

NSString* ns(const std::string& s) {
    NSString* r = [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding];
    return r ? r : @"";
}

std::string str(NSString* s) {
    const char* u = s.UTF8String;
    return u ? u : "";
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Exit codes of children reaped by running() / wait() / kill_tree(), so a later wait() still gets the status.
std::mutex g_mu;
std::map<int, int> g_reaped;

int decode_status(int st) {
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return st;
}

// 1 = reaped now (*code set), 0 = still running, -1 = not our child (or already reaped)
int reap(int pid, int* code) {
    int st = 0;
    pid_t r;
    do { r = waitpid(pid, &st, WNOHANG); } while (r < 0 && errno == EINTR);
    if (r == pid) {
        int c = decode_status(st);
        std::lock_guard<std::mutex> lk(g_mu);
        g_reaped[pid] = c;
        if (code) *code = c;
        return 1;
    }
    if (r == 0) return 0;
    return -1;
}

bool cached_code(int pid, int* code) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_reaped.find(pid);
    if (it == g_reaped.end()) return false;
    if (code) *code = it->second;
    return true;
}

// Resolves a .app bundle (or returns the path itself when it is a plain executable / a file inside a bundle).
std::string resolve_exe(const std::string& exe) {
    if (exe.size() >= 4 && exe.compare(exe.size() - 4, 4, ".app") == 0) {
        NSBundle* b = [NSBundle bundleWithPath:ns(exe)];
        NSString* p = b.executablePath;
        if (p) return str(p);
    } else if (exe.size() >= 5 && exe.compare(exe.size() - 5, 5, ".app/") == 0) {
        return resolve_exe(exe.substr(0, exe.size() - 1));
    }
    return exe;
}

}  // namespace

bool spawn(const SpawnOptions& o, Child* out, std::string* err) {
    auto fail = [&](int e) {
        if (err) *err = strerror(e);
        return false;
    };
    @autoreleasepool {
        std::string exe = resolve_exe(o.exe);
        if (exe.empty()) return fail(ENOENT);

        std::vector<std::string> envs;
        for (char** e = environ; e && *e; ++e) envs.emplace_back(*e);
        auto key_is = [](const std::string& kv, const std::string& k) {
            return kv.size() > k.size() && kv.compare(0, k.size(), k) == 0 && kv[k.size()] == '=';
        };
        for (auto& k : o.env_unset) envs.erase(std::remove_if(envs.begin(), envs.end(), [&](const std::string& kv) { return key_is(kv, k); }), envs.end());
        for (auto& kv : o.env_set) {
            envs.erase(std::remove_if(envs.begin(), envs.end(), [&](const std::string& e) { return key_is(e, kv.first); }), envs.end());
            envs.push_back(kv.first + "=" + kv.second);
        }
        std::vector<char*> envp;
        for (auto& s : envs) envp.push_back(const_cast<char*>(s.c_str()));
        envp.push_back(nullptr);

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(exe.c_str()));
        for (auto& a : o.args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        posix_spawnattr_t attr;
        posix_spawn_file_actions_t fa;
        posix_spawnattr_init(&attr);
        posix_spawn_file_actions_init(&fa);
        short flags = POSIX_SPAWN_CLOEXEC_DEFAULT;
        if (o.own_group) {
            flags |= POSIX_SPAWN_SETPGROUP;
            posix_spawnattr_setpgroup(&attr, 0);
        }
        // Reset signal state: the hub may block / ignore signals that emulators expect at default.
        sigset_t none, all;
        sigemptyset(&none);
        sigfillset(&all);
        posix_spawnattr_setsigmask(&attr, &none);
        posix_spawnattr_setsigdefault(&attr, &all);
        flags |= POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
        posix_spawnattr_setflags(&attr, flags);

        // CLOEXEC_DEFAULT closes everything not named here, so inherit (or redirect) 0, 1, 2 explicitly.
        posix_spawn_file_actions_addinherit_np(&fa, 0);
        if (!o.stdout_path.empty())
            posix_spawn_file_actions_addopen(&fa, 1, o.stdout_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        else
            posix_spawn_file_actions_addinherit_np(&fa, 1);
        if (!o.stderr_path.empty())
            posix_spawn_file_actions_addopen(&fa, 2, o.stderr_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        else
            posix_spawn_file_actions_addinherit_np(&fa, 2);
        if (!o.cwd.empty()) posix_spawn_file_actions_addchdir_np(&fa, o.cwd.c_str());

        // activate=false: nothing extra. A directly spawned executable is not launched through LaunchServices, so AppKit apps do
        // not get brought to the front by the OS; whether the app activates itself is up to the app.
        pid_t pid = 0;
        int rc = posix_spawn(&pid, exe.c_str(), &fa, &attr, argv.data(), envp.data());
        posix_spawn_file_actions_destroy(&fa);
        posix_spawnattr_destroy(&attr);
        if (rc != 0) return fail(rc);

        if (out) {
            out->pid = pid;
            out->pgid = o.own_group ? pid : (int)getpgid(pid);
            out->start_time_us = pid_start_time_us(pid);
        }
        { std::lock_guard<std::mutex> lk(g_mu); g_reaped.erase(pid); }
        return true;
    }
}

bool wait(const Child& c, int timeout_ms, int* exit_code) {
    if (!c.valid()) return false;
    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        int code = 0;
        int r = reap(c.pid, &code);
        if (r == 1) { if (exit_code) *exit_code = code; return true; }
        if (r < 0) {
            if (cached_code(c.pid, &code)) { if (exit_code) *exit_code = code; return true; }
            // Not our child: only liveness is known.
            if (!pid_alive(c.pid)) { if (exit_code) *exit_code = 0; return true; }
        }
        if (timeout_ms >= 0 && Clock::now() >= deadline) return false;
        sleep_ms(5);
    }
}

bool running(const Child& c) {
    if (!c.valid()) return false;
    int code = 0;
    int r = reap(c.pid, &code);
    if (r == 1) return false;
    if (r == 0) return true;
    if (cached_code(c.pid, nullptr)) return false;
    return pid_alive(c.pid);
}

void kill_tree(const Child& c, int grace_ms) {
    if (!c.valid()) return;
    int pg = c.pgid > 0 ? c.pgid : c.pid;
    bool group = c.pgid > 0 && c.pgid == c.pid;   // only signal the group when it is ours (never the hub's own group)
    auto gone = [&]() {
        int code;
        reap(c.pid, &code);
        bool child_gone = !pid_alive(c.pid) || cached_code(c.pid, nullptr);
        if (child_gone && group) {
            if (kill(-pg, 0) == 0 || errno == EPERM) return false;
        }
        return child_gone;
    };
    if (group) kill(-pg, SIGTERM); else kill(c.pid, SIGTERM);
    auto deadline = Clock::now() + std::chrono::milliseconds(std::max(0, grace_ms));
    while (!gone() && Clock::now() < deadline) sleep_ms(5);
    if (gone()) return;
    if (group) kill(-pg, SIGKILL); else kill(c.pid, SIGKILL);
    auto d2 = Clock::now() + std::chrono::milliseconds(1000);
    while (!gone() && Clock::now() < d2) sleep_ms(5);
}

bool pid_alive(int pid) {
    if (pid <= 0) return false;
    if (kill(pid, 0) == 0) {
        // A zombie child counts as gone.
        struct proc_bsdinfo bi;
        if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof(bi)) == (int)sizeof(bi) && bi.pbi_status == 5) return false;
        return true;
    }
    return errno == EPERM;
}

uint64_t pid_start_time_us(int pid) {
    if (pid <= 0) return 0;
    struct proc_bsdinfo bi;
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof(bi)) != (int)sizeof(bi)) return 0;
    return (uint64_t)bi.pbi_start_tvsec * 1000000ull + bi.pbi_start_tvusec;
}

std::string pid_exe_path(int pid) {
    if (pid <= 0) return std::string();
    char buf[PROC_PIDPATHINFO_MAXSIZE];
    int n = proc_pidpath(pid, buf, sizeof(buf));
    return n > 0 ? std::string(buf, (size_t)n) : std::string();
}

std::vector<int> pids_by_exe_name(const std::string& name) {
    std::vector<int> res;
    int cap = proc_listallpids(nullptr, 0);
    if (cap <= 0) return res;
    std::vector<int> pids((size_t)cap + 64);
    int n = proc_listallpids(pids.data(), (int)(pids.size() * sizeof(int)));
    if (n <= 0) return res;
    auto lower = [](std::string s) { for (auto& ch : s) ch = (char)tolower((unsigned char)ch); return s; };
    std::string want = lower(name);
    for (int i = 0; i < n && i < (int)pids.size(); ++i) {
        if (pids[i] <= 0) continue;
        std::string p = pid_exe_path(pids[i]);
        if (p.empty()) continue;
        size_t sl = p.find_last_of('/');
        if (lower(sl == std::string::npos ? p : p.substr(sl + 1)) == want) res.push_back(pids[i]);
    }
    return res;
}

bool kill_pid(int pid, bool force) {
    if (pid <= 0) return false;
    return kill(pid, force ? SIGKILL : SIGTERM) == 0;
}

// ---- desktop (NSWorkspace is safe from any thread) ----

bool open_url(const std::string& url) {
    @autoreleasepool {
        NSURL* u = [NSURL URLWithString:ns(url)];
        if (!u) return false;
        return [[NSWorkspace sharedWorkspace] openURL:u];
    }
}

bool open_path(const std::string& path) {
    @autoreleasepool {
        return [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:ns(path)]];
    }
}

bool reveal_in_finder(const std::string& path) {
    @autoreleasepool {
        NSURL* u = [NSURL fileURLWithPath:ns(path)];
        [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ u ]];
        return true;
    }
}

// ---- other apps' windows ----

bool activate_pid(int pid) {
    @autoreleasepool {
        NSRunningApplication* a = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
        if (!a) return false;
        return [a activateWithOptions:NSApplicationActivateAllWindows];
    }
}

bool hide_pid(int pid) {
    @autoreleasepool {
        NSRunningApplication* a = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
        return a ? [a hide] : false;
    }
}

namespace {
// Front-to-back on-screen normal-layer windows of pid (the list from CoreGraphics is ordered front to back).
std::vector<std::string> windows_of(int pid) {
    std::vector<std::string> titles;
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (!list) return titles;
    NSArray* arr = CFBridgingRelease(list);
    for (NSDictionary* w in arr) {
        if ([w[(__bridge NSString*)kCGWindowOwnerPID] intValue] != pid) continue;
        if ([w[(__bridge NSString*)kCGWindowLayer] intValue] != 0) continue;
        NSString* t = w[(__bridge NSString*)kCGWindowName];
        titles.push_back(t ? str(t) : std::string());
    }
    return titles;
}
}  // namespace

bool pid_has_window(int pid) {
    @autoreleasepool { return pid > 0 && !windows_of(pid).empty(); }
}

std::string pid_window_title(int pid) {
    @autoreleasepool {
        if (pid <= 0) return std::string();
        for (auto& t : windows_of(pid)) if (!t.empty()) return t;
        return std::string();
    }
}

bool accessibility_trusted(bool prompt) {
    NSDictionary* opts = @{(__bridge NSString*)kAXTrustedCheckOptionPrompt : prompt ? @YES : @NO};
    return AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)opts);
}

bool send_key(int pid, int keycode, unsigned mods) {
    if (pid <= 0 || !accessibility_trusted(false)) return false;
    CGEventFlags fl = 0;
    if (mods & 1) fl |= kCGEventFlagMaskShift;
    if (mods & 2) fl |= kCGEventFlagMaskControl;
    if (mods & 4) fl |= kCGEventFlagMaskAlternate;
    if (mods & 8) fl |= kCGEventFlagMaskCommand;
    bool ok = true;
    for (int down = 1; down >= 0; --down) {
        CGEventRef e = CGEventCreateKeyboardEvent(nullptr, (CGKeyCode)keycode, down != 0);
        if (!e) { ok = false; continue; }
        CGEventSetFlags(e, fl);
        CGEventPostToPid(pid, e);
        CFRelease(e);
        if (down) sleep_ms(10);
    }
    return ok;
}

}  // namespace proc
}  // namespace plat
