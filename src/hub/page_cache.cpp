/* What the game page reads from the disk, cached per game (see page_cache.h). */
#include "page_cache.h"
#include "app.h"
#include "hookshot_build.h"

namespace pagecache {
namespace {

Host g_host = Host::None;
bool g_settled = true;
unsigned g_gen = 1;

}  // namespace

PageScope::PageScope(Host h, bool settled) : prev_host(g_host), prev_settled(g_settled) {
    g_host = h;
    g_settled = h != Host::Pane || settled;
}
PageScope::~PageScope() {
    g_host = prev_host;
    g_settled = prev_settled;
}

Host host() { return g_host; }
bool settled() { return g_settled; }
bool frozen() { return session_active(); }
bool pane_frozen() { return g_host == Host::Pane && frozen(); }
bool may_read() { return g_host != Host::Pane || (g_settled && !frozen()); }
unsigned generation() { return g_gen; }
double now() { return ImGui::GetTime(); }

void invalidate(const char* why) {
    ++g_gen;
    shot_trace("pagecache-invalidate", std::string(why ? why : "?") + " gen=" + std::to_string(g_gen));
}

Need need(bool have, unsigned gen, double at, double ttl) {
    if (g_host == Host::Pane) {
        if (frozen() || !g_settled) return Need::Use;   // a game runs / the cursor is still moving: what is cached, maybe nothing yet
        return (!have || gen != g_gen) ? Need::Load : Need::Use;
    }
    if (!have || gen != g_gen) return Need::Load;
    if (ttl > 0.0 && now() - at >= ttl) return Need::Periodic;   // the full-window page's timed re-read, as it always was
    return Need::Use;
}

const char* scope_name(Need n) { return n == Need::Periodic ? "periodic" : "load"; }

/* Things that can change what the page reads: when one of them changes, every cached fact is read again (the pane: on its next settled
 * frame, never while a game runs). */
void tick() {
    static bool inited = false, was_session = false, was_focus = true, was_job = false, was_build = false;
    static int was_emu = -1, was_unz = -1;
    static unsigned was_rev = 0;
    const bool s = session_active();
    const bool focus = g_headless || (A.win && (SDL_GetWindowFlags(A.win) & SDL_WINDOW_INPUT_FOCUS) != 0);
    const bool job = A.job_h != nullptr;
    const bool build = hookshot::build_running();
    const int emu = (int)EMU.state, unz = (int)UNZ.state;
    const unsigned rev = hub::saves::cloud_settings_rev();
    if (inited) {
        if (was_session && !s) invalidate("game-ended");
        else if (focus && !was_focus) invalidate("focus");
        else if (was_job != job || was_build != build || was_emu != emu || was_unz != unz) invalidate("install");
        else if (rev != was_rev) invalidate("vault");
    }
    inited = true;
    was_session = s; was_focus = focus; was_job = job; was_build = build; was_emu = emu; was_unz = unz; was_rev = rev;
}

}  // namespace pagecache
