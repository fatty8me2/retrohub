#include "kanto_pad.h"

#include <filesystem>
#include <set>

#include "winutil.h"

namespace kanto {

namespace {

/* GBA key names in mGBA's order (GBAInputInfo.keyId) and the RetroHub "retro" control that feeds each. */
struct KeyDef { const char* mgba; const char* control; };
const KeyDef kKeys[] = {{"A", "a"},   {"B", "b"},   {"Select", "select"}, {"Start", "start"}, {"Right", "right"},
                        {"Left", "left"}, {"Up", "up"}, {"Down", "down"},  {"R", "r"},        {"L", "l"}};

std::vector<std::string> section_names() {
    std::vector<std::string> v = {"gba.input.SDLC"};
    for (int i = 1; i <= 4; ++i) v.push_back("gba.input-profile.XInput Controller #" + std::to_string(i));
    return v;
}

std::string trim_ws(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

}  // namespace

std::vector<std::pair<std::string, std::string>> pad_bindings(const hubctl::Profile& retro) {
    hubctl::Profile p = retro;
    if (p.same_mapping(hubctl::default_profile("retro"))) {   // libretro's A is Xbox B; mGBA/GBA players expect A=A, B=B
        p.buttons["a"] = "a";
        p.buttons["b"] = "b";
    }
    std::vector<std::pair<std::string, std::string>> keys, axes;
    std::set<std::string> used_axis;
    std::set<int> used_btn;
    for (const KeyDef& k : kKeys) {
        auto it = p.buttons.find(k.control);
        if (it == p.buttons.end()) continue;
        std::vector<hubctl::Term> terms;
        if (!hubctl::parse_expr(it->second, p.axis_threshold, &terms)) continue;
        bool have_btn = false, have_axis = false;
        for (const auto& t : terms) {
            if (!t.axis && !have_btn && !used_btn.count(t.idx)) {
                keys.push_back({std::string("key") + k.mgba, std::to_string(t.idx)});
                used_btn.insert(t.idx);
                have_btn = true;
            } else if (t.axis && !have_axis && t.idx >= 0 && t.idx < 4) {   // sticks only; triggers are not mapped (no turbo / no accidental input)
                std::string tag = std::string(t.sign < 0 ? "-" : "+") + std::to_string(t.idx);
                if (used_axis.count(tag)) continue;
                used_axis.insert(tag);
                axes.push_back({std::string("axis") + k.mgba + "Value", std::to_string(t.sign < 0 ? -t.threshold : t.threshold)});
                axes.push_back({std::string("axis") + k.mgba + "Axis", tag});
                have_axis = true;
            }
        }
    }
    keys.insert(keys.end(), axes.begin(), axes.end());
    return keys;
}

static std::string set_section(const std::string& ini, const std::string& name,
                               const std::vector<std::pair<std::string, std::string>>& kv, const std::string& eol) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < ini.size()) {
        size_t e = ini.find('\n', pos);
        std::string l = ini.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    const std::string header = "[" + name + "]";
    int start = -1, end = (int)lines.size();
    for (int i = 0; i < (int)lines.size(); ++i) {
        std::string t = trim_ws(lines[i]);
        if (start < 0) { if (t == header) start = i; }
        else if (!t.empty() && t[0] == '[') { end = i; break; }
    }
    std::set<std::string> ours;
    for (auto& p : kv) ours.insert(p.first);
    std::vector<std::string> out;
    if (start < 0) {
        out = lines;
        while (!out.empty() && trim_ws(out.back()).empty()) out.pop_back();
        if (!out.empty()) out.push_back("");
        out.push_back(header);
        for (auto& p : kv) out.push_back(p.first + "=" + p.second);
        out.push_back("");
    } else {
        for (int i = 0; i <= start; ++i) out.push_back(lines[i]);
        std::vector<std::string> body;
        for (int i = start + 1; i < end; ++i) {
            size_t eq = lines[i].find('=');
            if (eq != std::string::npos && ours.count(trim_ws(lines[i].substr(0, eq)))) continue;
            body.push_back(lines[i]);
        }
        size_t trailing = 0;   // keep the blank separator after the section
        while (!body.empty() && trim_ws(body.back()).empty()) { body.pop_back(); ++trailing; }
        for (auto& l : body) out.push_back(l);
        for (auto& p : kv) out.push_back(p.first + "=" + p.second);
        if (trailing == 0 && end < (int)lines.size()) trailing = 1;
        for (size_t i = 0; i < trailing; ++i) out.push_back("");
        for (int i = end; i < (int)lines.size(); ++i) out.push_back(lines[i]);
    }
    std::string r;
    for (auto& l : out) r += l + eol;
    return r;
}

std::string mgba_pad_config_patch(const std::string& ini, const hubctl::Profile& retro) {
    auto kv = pad_bindings(retro);
    std::string eol = ini.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::string r = ini;
    for (const auto& s : section_names()) r = set_section(r, s, kv, eol);
    return r;
}

bool has_pad_section(const std::string& ini) {
    size_t p = 0;
    while ((p = ini.find("[gba.input.SDLC]", p)) != std::string::npos) {
        if (p == 0 || ini[p - 1] == '\n') return true;
        ++p;
    }
    return false;
}

bool prepare_pad(const std::string& install_dir, const hubctl::Profile& retro) {
    namespace fs = std::filesystem;
    std::string d = install_dir;
    if (!d.empty() && d.back() != '\\' && d.back() != '/') d += '\\';
    d += "runtime\\mgba\\";
    std::string cfg = d + "config.ini", marker = d + "retrohub-pad.txt";
    std::error_code ec;
    fs::create_directories(fs::path(winutil::to_wide(d)), ec);
    const std::string want = hubctl::hash_hex(hubctl::mapping_hash(retro));
    std::string ini, mk;
    winutil::read_file(cfg, &ini);
    if (winutil::read_file(marker, &mk) && mk.find("profile-hash: " + want) != std::string::npos && has_pad_section(ini)) return false;
    if (!winutil::write_file(cfg, mgba_pad_config_patch(ini, retro))) return false;
    winutil::write_file(marker, "RetroHub wrote the gamepad bindings of Kanto Together's mGBA from its Controllers profile (GBA).\n"
                                "They are rewritten only when that profile changes. Delete this file to have them written again.\n"
                                "profile-hash: " + want + "\n");
    return true;
}

}  // namespace kanto
