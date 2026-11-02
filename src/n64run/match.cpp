#include "match.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "remix_data.h"

namespace n64run {

uint8_t ram_r8(const uint8_t* ram, uint32_t a) { return ram[(a & 0x7FFFFFu) ^ 3u]; }
void ram_w8(uint8_t* ram, uint32_t a, uint8_t v) { ram[(a & 0x7FFFFFu) ^ 3u] = v; }
uint32_t ram_r32(const uint8_t* ram, uint32_t a) {
    return ((uint32_t)ram_r8(ram, a) << 24) | ((uint32_t)ram_r8(ram, a + 1) << 16) | ((uint32_t)ram_r8(ram, a + 2) << 8) | ram_r8(ram, a + 3);
}
void ram_w32(uint8_t* ram, uint32_t a, uint32_t v) {
    ram_w8(ram, a, (uint8_t)(v >> 24)); ram_w8(ram, a + 1, (uint8_t)(v >> 16));
    ram_w8(ram, a + 2, (uint8_t)(v >> 8)); ram_w8(ram, a + 3, (uint8_t)v);
}

static bool parse_int(const std::string& s, int* out) {
    if (s.empty()) return false;
    char* e = nullptr;
    long v = std::strtol(s.c_str(), &e, 0);
    if (*e || v < 0 || v > 255) return false;
    *out = (int)v;
    return true;
}

bool parse_match_spec(const std::string& s, MatchSpec* out, std::string* err) {
    MatchSpec m;
    std::stringstream ss(s);
    std::string tok;
    auto fail = [&](const std::string& msg) { if (err) *err = msg; return false; };
    while (std::getline(ss, tok, ';')) {
        if (tok.empty()) continue;
        size_t eq = tok.find('=');
        if (eq == std::string::npos) return fail("missing '=' in '" + tok + "'");
        std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
        if (k == "stage") { if (!parse_int(v, &m.stage)) return fail("bad stage '" + v + "'"); }
        else if (k == "stocks") { if (!parse_int(v, &m.stocks) || m.stocks < 1 || m.stocks > 99) return fail("bad stocks '" + v + "'"); }
        else if (k == "time") { if (!parse_int(v, &m.time) || m.time < 1) return fail("bad time '" + v + "'"); }
        else if (k.size() == 2 && k[0] == 'p' && k[1] >= '0' && k[1] <= '3') {
            int p = k[1] - '0';
            size_t c = v.find(':');
            std::string f = c == std::string::npos ? v : v.substr(0, c);
            std::string co = c == std::string::npos ? "0" : v.substr(c + 1);
            if (!parse_int(f, &m.fighter[p]) || m.fighter[p] > (int)remix::kFighterIdCount - 1) return fail("bad fighter '" + f + "'");
            if (!parse_int(co, &m.costume[p])) return fail("bad costume '" + co + "'");
            m.present[p] = true;
        } else return fail("unknown key '" + k + "'");
    }
    if (!m.valid()) return fail("need at least two players (p0..p3)");
    *out = m;
    return true;
}

void MatchDriver::write_vs(uint8_t* ram) {
    namespace R = remix;
    const uint32_t base = R::kVsBase;
    int humans = 0;
    for (int i = 0; i < 4; i++) humans += spec_.present[i];
    ram_w8(ram, base + R::vs::kGameType, 1);
    ram_w8(ram, base + R::vs::kStage, (uint8_t)spec_.stage);
    ram_w8(ram, base + R::vs::kTeams, 0);
    ram_w8(ram, base + R::vs::kGameRules, 2);          // stock
    ram_w8(ram, base + R::vs::kPlCount, (uint8_t)humans);
    ram_w8(ram, base + R::vs::kCpCount, 0);
    ram_w8(ram, base + R::vs::kTime, (uint8_t)spec_.time);
    ram_w8(ram, base + R::vs::kStocks, (uint8_t)(spec_.stocks - 1));
    ram_w8(ram, base + R::vs::kHandicap, 0);
    ram_w8(ram, base + R::vs::kTeamAttack, 0);
    ram_w8(ram, base + R::vs::kStageSelect, 0);
    ram_w8(ram, base + R::vs::kDamageRatio, 100);
    ram_w8(ram, base + R::vs::kGameStatus, 0);
    for (int i = 0; i < 4; i++) {
        uint32_t pb = R::kVsPlayer1 + (uint32_t)i * R::kVsPlayerStride;
        ram_w8(ram, pb + R::player::kLevel, 1);
        ram_w8(ram, pb + R::player::kHandicapLevel, 9);
        if (!spec_.present[i]) {
            ram_w8(ram, pb + R::player::kPkind, R::kPkindNone);
            ram_w8(ram, pb + R::player::kColor, 4);
            ram_w8(ram, pb + R::player::kTag, 4);
            continue;
        }
        ram_w8(ram, pb + R::player::kPkind, R::kPkindHuman);
        ram_w8(ram, pb + R::player::kFkind, (uint8_t)spec_.fighter[i]);
        ram_w8(ram, pb + R::player::kTeam, (uint8_t)i);
        ram_w8(ram, pb + R::player::kPlayer, (uint8_t)i);
        ram_w8(ram, pb + R::player::kCostume, (uint8_t)spec_.costume[i]);
        ram_w8(ram, pb + R::player::kShade, 0);
        ram_w8(ram, pb + R::player::kColor, (uint8_t)i);
        ram_w8(ram, pb + R::player::kSingleStockIcon, 0);
        ram_w8(ram, pb + R::player::kTag, (uint8_t)i);
    }
    ram_w8(ram, R::kSceneGkind, (uint8_t)spec_.stage);
}

std::string MatchDriver::step(uint8_t* ram, long long vi) {
    namespace R = remix;
    if (!ram) return "";
    std::string log;
    int scene = ram_r8(ram, R::kCurrentScreen);
    char buf[160];
    if (scene != last_scene_) {
        std::snprintf(buf, sizeof buf, "match: vi %lld scene 0x%02X (frame_counter %u)", vi, scene, ram_r32(ram, R::kFrameCounter));
        log = buf;
        last_scene_ = scene;
        scene_vi_ = vi;
    }
    if (phase_ == kWait && scene == R::screen::kTitle && vi - scene_vi_ >= 30) {
        write_vs(ram);
        ram_w8(ram, R::kPreviousScreen, scene);
        ram_w8(ram, R::kCurrentScreen, R::screen::kVsBattle);
        ram_w32(ram, R::kScreenInterrupt, 1);
        phase_ = kDone;
        std::snprintf(buf, sizeof buf, "match: vi %lld jumped to VS battle", vi);
        log += (log.empty() ? "" : "\n") + std::string(buf);
    }
    return log;
}

}  // namespace n64run
