#include "test_input.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "buttons.h"

namespace n64run {

static uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

uint32_t TestInput::parse_buttons(const std::string& in, bool* ok) {
    *ok = true;
    std::string s = trim(in);
    if (s.empty()) return 0;
    if (std::isdigit((unsigned char)s[0])) {
        char* e = nullptr;
        unsigned long v = std::strtoul(s.c_str(), &e, 0);
        if (*e) *ok = false;
        return (uint32_t)v & 0xFFFFu;
    }
    uint32_t w = 0;
    std::string tok;
    auto flush = [&] {
        std::string t;
        for (char c : tok) t += (char)std::toupper((unsigned char)c);
        tok.clear();
        if (t.empty()) return;
        static const struct { const char* n; uint32_t b; } kN[] = {
            {"A", BTN_A}, {"B", BTN_B}, {"Z", BTN_Z}, {"START", BTN_START}, {"L", BTN_L}, {"R", BTN_R},
            {"CU", BTN_C_UP}, {"CD", BTN_C_DOWN}, {"CL", BTN_C_LEFT}, {"CR", BTN_C_RIGHT},
            {"UP", BTN_U_DPAD}, {"DOWN", BTN_D_DPAD}, {"LEFT", BTN_L_DPAD}, {"RIGHT", BTN_R_DPAD}, {"NONE", 0}};
        for (auto& n : kN) if (t == n.n) { w |= n.b; return; }
        *ok = false;
    };
    for (char c : s) { if (c == '+' || c == '|') flush(); else tok += c; }
    flush();
    return w;
}

bool TestInput::configure(const std::string& spec, int slot, std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = m; mode_ = Mode::None; return false; };
    if (spec.rfind("random:", 0) == 0) {
        char* e = nullptr;
        std::string num = spec.substr(7);
        unsigned long long v = std::strtoull(num.c_str(), &e, 0);
        if (num.empty() || *e) return fail("bad seed in --test-input random:<seed>");
        seed_ = v * 0x100000001B3ull + (uint64_t)(slot + 1) * 0x9E3779B97F4A7C15ull;
        mode_ = Mode::Random;
        for (int i = 0; i < 4; i++) { gen_init_[i] = false; gen_sample_[i] = -1; }
        sample_ = -1;
        return true;
    }
    if (spec.rfind("file:", 0) == 0) {
        std::ifstream f(spec.substr(5));
        if (!f) return fail("cannot open --test-input file");
        rows_.clear();
        std::string line;
        int ln = 0;
        while (std::getline(f, line)) {
            ln++;
            size_t h = line.find('#');
            if (h != std::string::npos) line.resize(h);
            if (trim(line).empty()) continue;
            std::vector<std::string> p;
            std::stringstream ss(line);
            std::string t;
            while (std::getline(ss, t, ',')) p.push_back(t);
            if (p.size() < 2 || p.size() > 4) return fail("--test-input file line " + std::to_string(ln) + ": want sample,buttons[,x,y]");
            bool ok = true;
            char* e = nullptr;
            long long s = std::strtoll(trim(p[0]).c_str(), &e, 10);
            if (*e || s < 0) return fail("--test-input file line " + std::to_string(ln) + ": bad sample");
            uint32_t w = parse_buttons(p[1], &ok);
            if (!ok) return fail("--test-input file line " + std::to_string(ln) + ": bad buttons");
            long x = p.size() > 2 ? std::strtol(trim(p[2]).c_str(), nullptr, 10) : 0;
            long y = p.size() > 3 ? std::strtol(trim(p[3]).c_str(), nullptr, 10) : 0;
            x = std::max(-80L, std::min(80L, x)); y = std::max(-80L, std::min(80L, y));
            if (!rows_.empty() && s < rows_.back().sample) return fail("--test-input file line " + std::to_string(ln) + ": samples must ascend");
            rows_.push_back({s, w | pack_stick((int)x, (int)y)});
        }
        mode_ = Mode::File;
        sample_ = -1;
        return true;
    }
    return fail("--test-input wants random:<seed> or file:<path>");
}

uint32_t TestInput::step(Gen& g) {
    const int kStartEvery = 240, kStartHold = 6;
    if (g.start_left > 0) { g.start_left--; return (g.word & 0xFFFF0000u) | BTN_START; }
    if (--g.next_start <= 0) { g.next_start = kStartEvery; g.start_left = kStartHold - 1; return (g.word & 0xFFFF0000u) | BTN_START; }
    if (g.left > 0) { g.left--; return g.word; }
    g.left = 4 + (int)(splitmix(g.rng) % 36);   // total span 5..40
    uint64_t r = splitmix(g.rng);
    uint32_t w = 0;
    static const uint32_t kBtn[] = {BTN_A, BTN_B, BTN_Z, BTN_L, BTN_R, BTN_C_UP, BTN_C_DOWN, BTN_C_LEFT, BTN_C_RIGHT,
                                    BTN_START, BTN_A, BTN_A, BTN_U_DPAD, BTN_D_DPAD, BTN_L_DPAD, BTN_R_DPAD};
    int n = (int)(r & 3);                    // 0..3 buttons
    for (int i = 0; i < n; i++) w |= kBtn[splitmix(g.rng) % (sizeof kBtn / sizeof kBtn[0])];
    int mode = (int)(splitmix(g.rng) % 4);   // 0 neutral, 1 axis-aligned, 2 free, 3 full tilt
    int x = 0, y = 0;
    if (mode == 1) { int d = (int)(splitmix(g.rng) % 4); int m = 80; x = d == 0 ? m : d == 1 ? -m : 0; y = d == 2 ? m : d == 3 ? -m : 0; }
    else if (mode == 2) { x = (int)(splitmix(g.rng) % 161) - 80; y = (int)(splitmix(g.rng) % 161) - 80; }
    else if (mode == 3) { x = (splitmix(g.rng) & 1) ? 57 : -57; y = (splitmix(g.rng) & 1) ? 57 : -57; }
    g.word = w | pack_stick(x, y);
    return g.word;
}

void TestInput::update() { sample_++; }

uint32_t TestInput::read(int idx) {
    if (idx < 0 || idx > 3) return 0;
    if (mode_ == Mode::File) {
        uint32_t w = 0;
        for (const Row& r : rows_) { if (r.sample <= sample_) w = r.word; else break; }
        return w;
    }
    if (mode_ != Mode::Random) return 0;
    if (!gen_init_[idx]) {
        uint64_t s = seed_ + (uint64_t)idx * 0xD1B54A32D192ED03ull;
        gens_[idx] = Gen();
        gens_[idx].rng = s;
        gens_[idx].next_start = 60 + (int)(splitmix(gens_[idx].rng) % 120);   // first Start soon, staggered per peer
        gen_init_[idx] = true;
    }
    if (gen_sample_[idx] != sample_) { gen_sample_[idx] = sample_; cached_[idx] = step(gens_[idx]); }
    return cached_[idx];
}

}  // namespace n64run
