#pragma once
/* "Finish setup" card: which first-run steps are still open. Pure. */
#include <string>
#include <vector>

namespace hubhelp {

struct SetupInputs { bool setup_done = false, have_games = false, username_claimed = false, net_probe_done = false, have_pad = true; };

/* Steps still open, in order. Empty = nothing to finish (the Home card hides). A missing pad is not a step: the keyboard works. */
inline std::vector<std::string> open_steps(const SetupInputs& s) {
    std::vector<std::string> v;
    if (!s.have_games) v.push_back("Add your games");
    if (!s.username_claimed) v.push_back("Pick a username");
    if (!s.net_probe_done) v.push_back("Allow online play");
    if (!s.setup_done && v.empty()) v.push_back("Finish the setup tour");
    return v;
}

}  // namespace hubhelp
