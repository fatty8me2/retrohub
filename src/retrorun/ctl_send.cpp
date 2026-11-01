/* QA helper: writes commands into a runner's control.json through the same sender the hub uses (runctl::send_command_ex), so the qa\*.ps1 scripts
 * exercise the real "a Quit the runner has not acked yet is never overwritten" rule instead of a re-implementation of it.
 *
 *   retrorun_ctl_send <control.json> <cmd> [slot=N] [value=N] [speed=X|max] [text=...] [-- <cmd> ...]
 *
 * <cmd> is a runner_ctl name (save_state, quit, pause, osd_corner, handoff, ...; docs/RUNNER_CTL.md). "--" chains commands that are written back to
 * back inside this one process (microseconds apart, far inside a runner's 250 ms poll), e.g. `quit -- input value=1` is the hub menu's "Quit to RetroHub"
 * followed by its "input on". One line per command: the seq that was written, or "0" when the sender wrote nothing (it refused because an un-acked
 * Quit is in the file, or the file could not be written). Exit 0 when every command was written, 3 when one was not, 2 on a usage error. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "runner_ctl.h"

static bool parse_cmd(const std::vector<std::string>& w, runctl::Command* c) {
    c->cmd = runctl::cmd_from_name(w[0]);
    if (c->cmd == runctl::Cmd::None) { std::fprintf(stderr, "unknown command: %s\n", w[0].c_str()); return false; }
    for (size_t i = 1; i < w.size(); i++) {
        const size_t eq = w[i].find('=');
        if (eq == std::string::npos) { std::fprintf(stderr, "bad argument (want key=value): %s\n", w[i].c_str()); return false; }
        const std::string k = w[i].substr(0, eq), v = w[i].substr(eq + 1);
        if (k == "slot") c->slot = std::atoi(v.c_str());
        else if (k == "value") c->value = std::atoi(v.c_str());
        else if (k == "speed") { if (!runctl::parse_speed_arg(v, &c->speed)) { std::fprintf(stderr, "bad speed: %s\n", v.c_str()); return false; } }
        else if (k == "text") c->text = v;
        else { std::fprintf(stderr, "unknown key: %s\n", k.c_str()); return false; }
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fputs("usage: retrorun_ctl_send <control.json> <cmd> [slot=N] [value=N] [speed=X|max] [text=...] [-- <cmd> ...]\n", stderr);
        return 2;
    }
    std::vector<std::vector<std::string>> cmds(1);
    for (int i = 2; i < argc; i++) {
        if (std::strcmp(argv[i], "--") == 0) { cmds.emplace_back(); continue; }
        cmds.back().push_back(argv[i]);
    }
    std::vector<runctl::Command> parsed;
    for (auto& w : cmds) {
        runctl::Command c;
        if (w.empty() || !parse_cmd(w, &c)) return 2;
        parsed.push_back(c);
    }
    int rc = 0;
    for (auto& c : parsed) {
        const long long seq = runctl::send_command_ex(argv[1], c);
        std::printf("%lld\n", seq);
        if (seq <= 0) rc = 3;
    }
    return rc;
}
