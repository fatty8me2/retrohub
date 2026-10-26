/* Console test: real console startup detection, config edits, custom sound decoding. Scratch dirs only. */
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "real_boot.h"

namespace fs = std::filesystem;
using namespace hub;
using namespace hub::realboot;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static void put(const fs::path& p, size_t n, const char* bytes = nullptr) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    std::string b(n, '\0');
    if (bytes) b.replace(0, std::min(n, std::strlen(bytes)), bytes);
    f.write(b.data(), (std::streamsize)n);
}

static std::string wav_bytes(int rate, int ch, int frames) {
    std::string b;
    auto u32 = [&](unsigned v) { b.append((const char*)&v, 4); };
    auto u16 = [&](unsigned short v) { b.append((const char*)&v, 2); };
    int data = frames * ch * 2;
    b += "RIFF"; u32(36 + data); b += "WAVEfmt "; u32(16); u16(1); u16((unsigned short)ch); u32(rate); u32(rate * ch * 2); u16((unsigned short)(ch * 2)); u16(16);
    b += "data"; u32(data);
    for (int i = 0; i < frames; ++i) for (int c = 0; c < ch; ++c) { short s = (short)(std::sin(i * 0.05) * 12000); b.append((const char*)&s, 2); }
    return b;
}

int main() {
    fs::path root = fs::temp_directory_path() / "retrohub_real_boot_test";
    fs::remove_all(root);

    CHECK(supported(System::GC) && supported(System::PS1) && supported(System::PS2) && supported(System::GBA) && supported(System::NDS));
    CHECK(!supported(System::N64) && !supported(System::N3DS) && !supported(System::WiiU) && !supported(System::NES) && !supported(System::SNES));
    CHECK(gc_region_for_serial("GALE01") == "USA" && gc_region_for_serial("GALP01") == "EUR" && gc_region_for_serial("GALJ01") == "JAP" && gc_region_for_serial("") == "USA");

    /* GC: region folders, wrong size ignored, other region not used */
    put(root / "bios" / "USA" / "IPL.bin", 2097152);
    put(root / "bios" / "EUR" / "IPL.bin", 1000);   // wrong size
    Stage g = find(System::GC, {(root / "bios").u8string()}, "GALE01");
    CHECK(g.ok && g.region == "USA" && g.copies.size() == 1 && g.copies[0].second == "IPL.bin");
    CHECK(!find(System::GC, {(root / "bios").u8string()}, "GALP01").ok);
    CHECK(!find(System::GC, {(root / "none").u8string()}, "GALE01").ok);
    put(root / "b2" / "IPL (JAP).bin", 2097152);
    CHECK(find(System::GC, {(root / "b2").u8string()}, "GALJ01").ok && !find(System::GC, {(root / "b2").u8string()}, "GALE01").ok);
    put(root / "b3" / "ipl.bin", 2097152);   // region unknown: any
    CHECK(find(System::GC, {(root / "b3").u8string()}, "GALP01").ok);

    /* GBA / GB / GBC / NDS */
    put(root / "GamesBios" / "gba_bios.bin", 16384);
    put(root / "GamesBios" / "sub" / "dmg_boot.bin", 256);
    put(root / "GamesBios" / "cgb_boot.bin", 2304);
    std::string gb = (root / "GamesBios").u8string();
    CHECK(find(System::GBA, {gb}).ok && find(System::GB, {gb}).ok && find(System::GB, {gb}).copies.size() == 2 && find(System::GBC, {gb}).ok);
    CHECK(!find(System::NDS, {gb}).ok);
    put(root / "GamesBios" / "bios7.bin", 16384);
    put(root / "GamesBios" / "bios9.bin", 4096);
    CHECK(!find(System::NDS, {gb}).ok);
    put(root / "GamesBios" / "firmware.bin", 262144);
    CHECK(find(System::NDS, {gb}).ok && find(System::NDS, {gb}).copies.size() == 3);
    put(root / "wrong" / "gba_bios.bin", 100);
    CHECK(!find(System::GBA, {(root / "wrong").u8string()}).ok);
    CHECK(!find(System::N64, {gb}).ok);

    /* settings list */
    std::string off;
    CHECK(enabled_for(off, System::GC));
    off = set_enabled(off, System::GC, false);
    off = set_enabled(off, System::PS1, false);
    CHECK(!enabled_for(off, System::GC) && !enabled_for(off, System::PS1) && enabled_for(off, System::PS2));
    off = set_enabled(off, System::GC, true);
    CHECK(enabled_for(off, System::GC) && off == "ps1");
    off = set_enabled(off, System::PS1, false);   // no duplicates
    CHECK(off == "ps1");

    /* config text */
    std::string d = dolphin_skip_ipl("[Core]\nSkipIPL = True\nSlotA = 8\n", false);
    CHECK(d.find("SkipIPL = False") != std::string::npos && d.find("SkipIPL = True") == std::string::npos && d.find("SlotA = 8") != std::string::npos);
    CHECK(dolphin_skip_ipl("", true).find("[Core]\nSkipIPL = True") != std::string::npos);
    std::string p = pcsx2_fast_boot("[UI]\nA = 1\n[EmuCore]\nEnableFastBoot = true\nEnableCheats = false\n", false);
    CHECK(p.find("EnableFastBoot = false") != std::string::npos && p.find("EnableCheats = false") != std::string::npos);
    CHECK(pcsx2_fast_boot("[UI]\nA=1\n", false).find("[EmuCore]\nEnableFastBoot = false") != std::string::npos);

    /* retro options */
    auto has = [](const std::vector<std::string>& v, const char* s) { for (auto& x : v) if (x == s) return true; return false; };
    CHECK(has(retro_opts(System::PS1, true, false, false), "beetle_psx_skip_bios=disabled"));
    CHECK(has(retro_opts(System::PS1, false, false, false), "beetle_psx_skip_bios=enabled"));
    CHECK(has(retro_opts(System::GBA, true, false, false), "mgba_skip_bios=OFF") && has(retro_opts(System::GBA, true, true, false), "mgba_skip_bios=ON"));
    CHECK(has(retro_opts(System::GB, true, false, true), "gambatte_gb_bootloader=enabled") && retro_opts(System::GB, true, false, false).empty());
    CHECK(drops_resume(System::GBA) && !drops_resume(System::GC));
    CHECK(allowed(false) && !allowed(true));

    /* custom sounds */
    fs::path sd = root / "boot_sounds";
    CHECK(custom_sound_path(sd.u8string(), System::GC).empty());
    std::string w = wav_bytes(22050, 1, 22050);   // 1 s mono 22.05k
    { fs::create_directories(sd); std::ofstream f(sd / "gc.wav", std::ios::binary); f.write(w.data(), (std::streamsize)w.size()); }
    CHECK(!custom_sound_path(sd.u8string(), System::GC).empty() && custom_sound_path(sd.u8string(), System::PS1).empty());
    std::vector<float> pcm;
    CHECK(decode_sound((sd / "gc.wav").u8string(), 44100, &pcm));
    CHECK(pcm.size() >= 2 * 44000 && pcm.size() <= 2 * 44200);   // resampled to 44.1k stereo
    std::string longw = wav_bytes(8000, 2, 8000 * 20);   // 20 s stereo: capped at 15 s
    { std::ofstream f(sd / "ps1.wav", std::ios::binary); f.write(longw.data(), (std::streamsize)longw.size()); }
    CHECK(decode_sound((sd / "ps1.wav").u8string(), 44100, &pcm) && pcm.size() == (size_t)(15.0 * 44100) * 2);
    CHECK(std::fabs(pcm[pcm.size() - 2]) < 0.01f);   // faded out at the cut
    { std::ofstream f(sd / "gba.mp3", std::ios::binary); f << "not an mp3 at all"; }
    CHECK(!decode_sound((sd / "gba.mp3").u8string(), 44100, &pcm));
    CHECK(!decode_sound((sd / "missing.wav").u8string(), 44100, &pcm));
    CHECK(!decode_sound_mem((const unsigned char*)"abc", 3, ".xyz", 44100, &pcm));
    CHECK(custom_sounds_present(sd.u8string()).size() == 3);

    fs::remove_all(root);
    if (fails) { std::printf("%d FAILED\n", fails); return 1; }
    std::printf("real_boot_test OK\n");
    return 0;
}
