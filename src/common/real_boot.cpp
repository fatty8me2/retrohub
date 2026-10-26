#include "real_boot.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "third_party/dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "third_party/dr_mp3.h"
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "third_party/stb_vorbis.c"

namespace fs = std::filesystem;

namespace hub::realboot {
namespace {

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
fs::path P(const std::string& s) { return fs::u8path(s); }

struct Cand { std::string path, name_l, region; uint64_t size; };

std::string region_of(const std::string& path) {   // from folder / file name words
    std::string p = lower(path);
    auto has = [&](const char* w) { return p.find(w) != std::string::npos; };
    if (has("usa") || has("ntsc-u") || has("ntscu") || has("(u)")) return "USA";
    if (has("eur") || has("pal")) return "EUR";
    if (has("jap") || has("ntsc-j") || has("ntscj") || has("(j)") || has("jpn")) return "JAP";
    return "";
}

void collect(const std::string& dir, int depth, std::vector<Cand>* out) {
    std::error_code ec;
    if (!fs::is_directory(P(dir), ec)) return;
    for (fs::directory_iterator it(P(dir), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2)) { if (depth > 0 && !it->is_symlink(e2)) collect(it->path().u8string(), depth - 1, out); continue; }
        Cand c;
        c.path = it->path().u8string();
        c.name_l = lower(it->path().filename().u8string());
        c.size = (uint64_t)it->file_size(e2);
        c.region = region_of(c.path);
        out->push_back(c);
    }
}

std::vector<Cand> scan(const std::vector<std::string>& dirs) {
    std::vector<Cand> v;
    for (const std::string& d : dirs) {
        std::string leaf = lower(fs::u8path(d).filename().u8string());
        if (leaf.empty()) leaf = lower(fs::u8path(d).parent_path().filename().u8string());
        const bool deep = leaf.find("bios") != std::string::npos || leaf.find("gc") == 0 || leaf == "user" || leaf == "system";
        collect(d, deep ? 4 : 0, &v);
        for (const char* sub : {"BIOS", "bios", "Bios", "GamesBios", "GC"}) collect((fs::u8path(d) / sub).u8string(), 4, &v);
    }
    return v;
}

const Cand* pick(const std::vector<Cand>& v, std::initializer_list<const char*> names, std::initializer_list<uint64_t> sizes, bool prefix = false) {
    for (const char* n : names) {
        for (const Cand& c : v) {
            bool nm = prefix ? c.name_l.rfind(n, 0) == 0 && c.name_l.size() > 4 && c.name_l.compare(c.name_l.size() - 4, 4, ".bin") == 0 : c.name_l == n;
            if (!nm) continue;
            if (sizes.size() == 0 || std::find(sizes.begin(), sizes.end(), c.size) != sizes.end()) return &c;
        }
    }
    return nullptr;
}

std::string ini_set(const std::string& in, const std::string& section, const std::string& key, const std::string& value) {
    std::istringstream is(in);
    std::string line, out, cur;
    bool in_sec = false, sec_seen = false, done = false;
    auto trim = [](std::string s) { while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back(); size_t i = 0; while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; return s.substr(i); };
    auto flush_add = [&]() { if (in_sec && !done) { out += key + " = " + value + "\n"; done = true; } };
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = trim(line);
        if (!t.empty() && t[0] == '[') {
            flush_add();
            in_sec = lower(t) == lower("[" + section + "]");
            if (in_sec) sec_seen = true;
            out += line + "\n";
            continue;
        }
        if (in_sec && !done) {
            size_t eq = t.find('=');
            if (eq != std::string::npos && lower(trim(t.substr(0, eq))) == lower(key)) { out += key + " = " + value + "\n"; done = true; continue; }
        }
        out += line + "\n";
    }
    flush_add();
    if (!sec_seen) out += "[" + section + "]\n" + key + " = " + value + "\n";
    return out;
}

}  // namespace

bool supported(System s) {
    switch (s) { case System::GC: case System::PS1: case System::PS2: case System::GBA: case System::GB: case System::GBC: case System::NDS: return true; default: return false; }
}
const char* console_label(System s) { return system_name(s); }

const char* file_hint(System s) {
    switch (s) {
        case System::GC: return "Put your GameCube's IPL.bin (from your own console, in a USA / EUR / JAP folder or named like IPL (USA).bin) in your GamesBios folder to hear the real startup.";
        case System::PS1: return "Needs your PlayStation BIOS (like scph1001.bin), the same file the games already use.";
        case System::PS2: return "Needs your PS2 BIOS, the same file the games already use.";
        case System::GBA: return "Put your Game Boy Advance's gba_bios.bin in your GamesBios folder to see the logo and chime.";
        case System::GB: return "Put your Game Boy's dmg_boot.bin in your GamesBios folder to see the Nintendo logo and ding.";
        case System::GBC: return "Put your Game Boy Color's cgb_boot.bin in your GamesBios folder to see the Nintendo logo.";
        case System::NDS: return "Put your DS's bios7.bin, bios9.bin and firmware.bin in your GamesBios folder to start through the DS menu.";
        default: return "This console has no startup sequence to play.";
    }
}

std::string gc_region_for_serial(const std::string& serial) {
    char r = serial.size() >= 4 ? (char)std::toupper((unsigned char)serial[3]) : 'E';
    if (r == 'P' || r == 'D' || r == 'F' || r == 'S' || r == 'I' || r == 'U' || r == 'X' || r == 'Y') return "EUR";
    if (r == 'J' || r == 'K') return "JAP";
    return "USA";
}

Stage find(System s, const std::vector<std::string>& dirs, const std::string& serial) {
    Stage st;
    std::vector<Cand> v = scan(dirs);
    switch (s) {
        case System::GC: {
            st.region = gc_region_for_serial(serial);
            const Cand* best = nullptr;
            for (const Cand& c : v) {
                if (c.size != 2097152) continue;
                const bool named = c.name_l == "ipl.bin" || c.name_l.rfind("ipl", 0) == 0 || c.name_l.find("ipl") != std::string::npos;
                if (!named || c.name_l.size() < 4 || c.name_l.compare(c.name_l.size() - 4, 4, ".bin") != 0) continue;
                if (c.region == st.region) { best = &c; break; }
                if (c.region.empty() && !best) best = &c;
            }
            if (best) { st.ok = true; st.copies.push_back({best->path, "IPL.bin"}); st.detail = "IPL found: " + best->path; }
            else st.detail = std::string("No ") + st.region + " GameCube IPL.bin found.";
            break;
        }
        case System::GBA: {
            const Cand* c = pick(v, {"gba_bios.bin", "gba_bios.rom", "bios.gba"}, {16384});
            if (c) { st.ok = true; st.copies.push_back({c->path, "gba_bios.bin"}); st.detail = "GBA BIOS found: " + c->path; } else st.detail = "No gba_bios.bin found.";
            break;
        }
        case System::GB: {
            const Cand* c = pick(v, {"dmg_boot.bin", "gb_bios.bin", "dmg_rom.bin"}, {256});
            if (c) { st.ok = true; st.copies.push_back({c->path, "dmg_boot.bin"}); st.copies.push_back({c->path, "gb_bios.bin"}); st.detail = "Game Boy boot ROM found: " + c->path; } else st.detail = "No dmg_boot.bin found.";
            break;
        }
        case System::GBC: {
            const Cand* c = pick(v, {"cgb_boot.bin", "gbc_bios.bin", "cgb_rom.bin"}, {2304, 2048, 4096});
            if (c) { st.ok = true; st.copies.push_back({c->path, "cgb_boot.bin"}); st.copies.push_back({c->path, "gbc_bios.bin"}); st.detail = "Game Boy Color boot ROM found: " + c->path; } else st.detail = "No cgb_boot.bin found.";
            break;
        }
        case System::NDS: {
            const Cand* b7 = pick(v, {"bios7.bin"}, {16384});
            const Cand* b9 = pick(v, {"bios9.bin"}, {4096});
            const Cand* fw = pick(v, {"firmware.bin"}, {131072, 262144, 524288});
            if (b7 && b9 && fw) {
                st.ok = true;
                st.copies = {{b7->path, "bios7.bin"}, {b9->path, "bios9.bin"}, {fw->path, "firmware.bin"}};
                st.detail = "DS BIOS and firmware found.";
            } else st.detail = "DS needs bios7.bin, bios9.bin and firmware.bin.";
            break;
        }
        default: st.detail = "No startup sequence."; break;
    }
    return st;
}

static std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) { if (c == ',') { if (!cur.empty()) v.push_back(cur); cur.clear(); } else if (!std::isspace((unsigned char)c)) cur += c; }
    if (!cur.empty()) v.push_back(cur);
    return v;
}

bool enabled_for(const std::string& off_list, System s) {
    const std::string n = system_short(s);
    for (const std::string& x : split(off_list)) if (x == n) return false;
    return true;
}

std::string set_enabled(const std::string& off_list, System s, bool on) {
    const std::string n = system_short(s);
    std::vector<std::string> v = split(off_list);
    v.erase(std::remove(v.begin(), v.end(), n), v.end());
    if (!on) v.push_back(n);
    std::string out;
    for (const std::string& x : v) out += (out.empty() ? "" : ",") + x;
    return out;
}

std::vector<std::string> retro_opts(System s, bool real, bool user_skip_bios, bool gambatte) {
    std::vector<std::string> o;
    switch (s) {
        case System::PS1: o.push_back(std::string("beetle_psx_skip_bios=") + (real ? "disabled" : "enabled")); break;
        case System::GBA:
            o.push_back(std::string("mgba_use_bios=") + (real ? "ON" : "OFF"));
            o.push_back(std::string("mgba_skip_bios=") + ((real && !user_skip_bios) ? "OFF" : "ON"));
            break;
        case System::GB: case System::GBC:
            if (gambatte) o.push_back(std::string("gambatte_gb_bootloader=") + (real ? "enabled" : "disabled"));
            break;
        case System::NDS: if (real) o.push_back("melonds_boot_mode=native"); break;
        default: break;
    }
    return o;
}

std::string dolphin_skip_ipl(const std::string& ini, bool skip) { return ini_set(ini, "Core", "SkipIPL", skip ? "True" : "False"); }
std::string pcsx2_fast_boot(const std::string& ini, bool fast) { return ini_set(ini, "EmuCore", "EnableFastBoot", fast ? "true" : "false"); }

/* ------------------------------------------------------------------ custom sounds */

std::string custom_sound_path(const std::string& dir, System s) {
    for (const char* ext : {".wav", ".ogg", ".mp3"}) {
        fs::path p = P(dir) / (std::string(system_short(s)) + ext);
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) return p.u8string();
    }
    return "";
}

namespace {
void to_stereo_rate(const std::vector<float>& in, int ch, int in_rate, int rate, std::vector<float>* out) {
    out->clear();
    if (ch < 1 || in_rate < 1 || rate < 1 || in.empty()) return;
    const size_t frames = in.size() / (size_t)ch;
    auto at = [&](size_t f, int c) { return in[f * ch + (ch == 1 ? 0 : std::min(c, ch - 1))]; };
    const size_t n = (size_t)((double)frames * rate / in_rate);
    const size_t cap = (size_t)(kMaxSoundSeconds * rate);
    const size_t m = std::min(n, cap);
    out->resize(m * 2);
    for (size_t i = 0; i < m; ++i) {
        double pos = (double)i * in_rate / rate;
        size_t f0 = (size_t)pos, f1 = std::min(f0 + 1, frames - 1);
        float fr = (float)(pos - (double)f0);
        for (int c = 0; c < 2; ++c) (*out)[i * 2 + c] = at(f0, c) + (at(f1, c) - at(f0, c)) * fr;
    }
    if (n > cap) {   // cut: 100 ms fade so it does not click
        size_t fade = std::min(m, (size_t)(0.1 * rate));
        for (size_t i = 0; i < fade; ++i) { float g = (float)(fade - i) / fade; (*out)[(m - fade + i) * 2] *= g; (*out)[(m - fade + i) * 2 + 1] *= g; }
    }
}
}  // namespace

bool decode_sound_mem(const unsigned char* data, size_t n, const std::string& ext_in, int rate, std::vector<float>* stereo) {
    stereo->clear();
    if (!data || !n) return false;
    const std::string ext = lower(ext_in);
    std::vector<float> pcm;
    int ch = 0, sr = 0;
    if (ext == ".wav") {
        unsigned c = 0, r = 0;
        drwav_uint64 frames = 0;
        float* f = drwav_open_memory_and_read_pcm_frames_f32(data, n, &c, &r, &frames, nullptr);
        if (!f) return false;
        pcm.assign(f, f + frames * c);
        drwav_free(f, nullptr);
        ch = (int)c; sr = (int)r;
    } else if (ext == ".mp3") {
        drmp3_config cfg{};
        drmp3_uint64 frames = 0;
        float* f = drmp3_open_memory_and_read_pcm_frames_f32(data, n, &cfg, &frames, nullptr);
        if (!f) return false;
        pcm.assign(f, f + frames * cfg.channels);
        drmp3_free(f, nullptr);
        ch = (int)cfg.channels; sr = (int)cfg.sampleRate;
    } else if (ext == ".ogg") {
        int c = 0, r = 0;
        short* s = nullptr;
        int frames = stb_vorbis_decode_memory(data, (int)n, &c, &r, &s);
        if (frames <= 0 || !s) return false;
        pcm.resize((size_t)frames * c);
        for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = s[i] / 32768.f;
        std::free(s);
        ch = c; sr = r;
    } else return false;
    if (pcm.empty()) return false;
    to_stereo_rate(pcm, ch, sr, rate, stereo);
    return !stereo->empty();
}

bool decode_sound(const std::string& path, int rate, std::vector<float>* stereo) {
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return false;
    std::string b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() > 64u * 1024 * 1024) return false;
    return decode_sound_mem((const unsigned char*)b.data(), b.size(), P(path).extension().u8string(), rate, stereo);
}

std::vector<System> custom_sounds_present(const std::string& dir) {
    std::vector<System> v;
    for (System s : {System::N64, System::NES, System::SNES, System::GB, System::GBC, System::GBA, System::NDS, System::N3DS, System::GC, System::WiiU, System::PS1, System::PS2})
        if (!custom_sound_path(dir, s).empty()) v.push_back(s);
    return v;
}

}  // namespace hub::realboot
