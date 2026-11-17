#pragma once
/* What the game page reads from the disk, cached per game (docs/ARCHITECTURE.md, "UI kit and layouts", "Page data").
 *
 * The full-window page (Traditional) used to read some facts on every frame it was open; the Library desk keeps the selected game's page
 * drawn all the time, also behind a game it started. So every disk fact the page shows is cached here:
 *   - a fact is read once per game and kept until something that can change it happens (invalidate(): controls saved, an install, build
 *     or unpack finished, a game ended, the hub window got the focus back, the library was rescanned, the settings were re-read from the
 *     vault). Facts the full-window page always re-read on a timer (play log 30 s, screenshots 2 s, save history 3 s, resume facts
 *     1.5 s, achievements 4 s, save info 4 s, Wii U details 4 s) keep that timer there, and only there;
 *   - in the desk pane the heavy facts of a game wait until the list cursor has stayed on it for kSettleSecs (stepping through the list
 *     stays smooth: the header and the tabs switch at once, the rest fills in), and the pane never times a re-read;
 *   - while a game (or another session) runs, the pane reads nothing at all: it shows what is cached.
 * Code outside a page draw (launching, Settings) does not go through here and keeps its own rules.
 * need() is the one decision; PerGame / One hold a value. QA: qa_diskio.h counts what is still read (qa\desk_idle_io.ps1). */
#include "qa_diskio.h"

#include <string>
#include <unordered_map>

namespace pagecache {

constexpr double kSettleSecs = 0.2;

enum class Host { None, Full, Pane };

/* Around one draw of the game page (gamepage::draw_view). `settled`: the pane has shown this game for kSettleSecs (the full page: always). */
struct PageScope {
    PageScope(Host h, bool settled);
    ~PageScope();
    PageScope(const PageScope&) = delete;
    PageScope& operator=(const PageScope&) = delete;
    Host prev_host;
    bool prev_settled;
};
Host host();               // Host::None outside a page draw
bool settled();
bool frozen();             // a game or another session runs (session_active)
bool pane_frozen();        // drawing the desk pane while frozen: start no scan, no worker
bool may_read();           // a one-time read (a file kept in memory after) may happen now: not in the pane while it waits or a game runs
unsigned generation();
void invalidate(const char* why);
void tick();               // once per main-loop iteration: a game ending, the window getting the focus back, the vault settings re-read, an install finishing
double now();

enum class Need { Use, Load, Periodic };
/* One cached fact. `have`: the cache holds a value for this game; `gen` / `at`: when it was read; `ttl`: the full-window page's timed
 * re-read (0 = none). Use = show what is cached (maybe nothing yet), Load = read it now, Periodic = the full page's timed re-read. */
Need need(bool have, unsigned gen, double at, double ttl);

/* QA scope name for a read need() asked for ("periodic" is reported apart by qa_diskio). */
const char* scope_name(Need n);

/* A value per game key, read by `load()` when need() says so. get() returns nullptr while there is nothing for that game yet. */
template <class T>
struct PerGame {
    struct E { T v{}; unsigned gen = 0; double at = 0.0; };
    std::unordered_map<std::string, E> m;
    template <class F>
    const T* get(const std::string& key, double ttl, F load) {
        auto it = m.find(key);
        const bool have = it != m.end();
        const Need n = need(have, have ? it->second.gen : 0u, have ? it->second.at : 0.0, ttl);
        if (n == Need::Use) return have ? &it->second.v : nullptr;
        if (!have && m.size() >= 512) m.clear();   // bounded: a huge library stepped through once
        T v;
        {
            qadisk::Scope qs(scope_name(n), false);
            v = load();
        }
        E& e = m[key];
        e.v = std::move(v);
        e.gen = generation();
        e.at = now();
        return &e.v;
    }
    void forget(const std::string& key) { m.erase(key); }
};

/* One value for every game (controls, the GBA BIOS ...), keyed by a signature of the settings it depends on. */
template <class T>
struct One {
    std::string sig;
    bool have = false;
    T v{};
    unsigned gen = 0;
    double at = 0.0;
    template <class F>
    const T* get(const std::string& signature, double ttl, F load) {
        const bool same = have && signature == sig;
        const Need n = need(same, same ? gen : 0u, same ? at : 0.0, ttl);
        if (n == Need::Use) return have ? &v : nullptr;   // (a changed signature while frozen: the old value until the game ends)
        {
            qadisk::Scope qs(scope_name(n), false);
            v = load();
        }
        sig = signature;
        have = true;
        gen = generation();
        at = now();
        return &v;
    }
};

}  // namespace pagecache
