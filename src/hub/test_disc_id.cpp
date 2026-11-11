// Console test for PlayStation disc serial parsing (src/common/disc_id.cpp): file-name serials, SYSTEM.CNF boot lines, normalisation.
// A wrong serial breaks PS1/PS2 save keys. Pure string logic, no files, no network.
#include "disc_id.h"
#include <cstdio>
#include <string>
#include <vector>
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
#define CHECK_EQ(a, b) do { std::string _a = (a), _b = (b); if (_a != _b) { std::printf("FAIL %s:%d: %s -> '%s' expected '%s'\n", __FILE__, __LINE__, #a, _a.c_str(), _b.c_str()); ++g_fail; } } while (0)
using namespace hub::disc;

int main() {
    {   // normalize_serial
        struct { const char* in; const char* out; } t[] = {
            {"SLUS_012.34", "SLUS-01234"}, {"SLES-01234", "SLES-01234"}, {"slus-20946", "SLUS-20946"}, {"SLUS20946", "SLUS-20946"},
            {"SCUS_941.63", "SCUS-94163"}, {"SLPM_651.23;1", "SLPM-65123"}, {"  SLUS 209 46", "SLUS-20946"},
            {"[SCUS-94163]", "SCUS-94163"}, {"12 SLUS 20946", "SLUS-20946"}, {"SLUS-2 0 9 4 6", "SLUS-20946"},
            {"", ""}, {"SLUS", ""}, {"SLUS-", ""}, {"SLUS-1234", ""}, {"ABCDE-12345", ""}, {"ABC-12345", ""},
            {"12345", ""}, {"SLUS-12x45", ""}, {"SLUS-1234x", ""}, {"----", ""},
        };
        for (auto& c : t) CHECK_EQ(normalize_serial(c.in), c.out);
        // idempotent on every non-empty result
        for (auto& c : t) { std::string once = normalize_serial(c.in); CHECK_EQ(normalize_serial(once), once); }
        // lower/upper/mixed spellings agree
        CHECK_EQ(normalize_serial("sLuS_209.46"), normalize_serial("SLUS-20946"));
    }
    {   // normalize_serial: the pre-fix function is kept here as a reference. Every valid spelling must give the identical key (PS1/PS2 saves and cards are keyed by it);
        // only inputs with a sixth digit right after the five are now rejected, and serial_from_filename agrees.
        auto old_normalize = [](const std::string& s) -> std::string {
            auto alpha = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
            auto digit = [](char c) { return c >= '0' && c <= '9'; };
            size_t i = 0;
            while (i < s.size() && !alpha(s[i])) ++i;
            std::string letters, digits;
            for (; i < s.size() && alpha(s[i]) && letters.size() < 5; ++i) letters += (s[i] >= 'a' && s[i] <= 'z') ? char(s[i] - 32) : s[i];
            if (letters.size() != 4) return "";
            for (; i < s.size() && digits.size() < 5; ++i) {
                char c = s[i];
                if (digit(c)) digits += c;
                else if (c == '.' || c == '_' || c == '-' || c == ' ') continue;
                else break;
            }
            if (digits.size() != 5) return "";
            return letters + "-" + digits;
        };
        const char* valid[] = {
            "SLUS-00694", "SLUS_006.94", "SLUS_006.94;1", "SLUS00694", "slus-00694", "SLUS 006 94", "SCUS-94163", "SCUS_941.63", "SCUS_941.63;1",
            "SLES-01234", "SLES_523.45", "SLPS-00700", "SLPS_007.00", "SLPM-65123", "SLPM_651.23",
            "SCES-00984", "SCES_009.84;1", "SCED-01234", "SCPS-10001", "SCPS_100.01", "SLKA-25001", "SCAJ-20001", "PBPX-95201", "SIPS-60001", "SCKA-20001", "SLED-02345",
            "SLUS-20946.iso", "SLUS_209.46.chd", "[SLUS-20946]", "(SCUS-94163)", "SLUS-20946 (Disc 1)", "SLUS-20946-1", "SLUS_209.46;1\r\n", "  SLUS 209 46", "12 SLUS 20946", "SLUS-2 0 9 4 6", "sLuS_209.46",
            "SLUS-00001", "SLUS-99999", "SLUS_000.00", "SLUS-20946x", "SLUS-20946.1",
        };
        for (const char* v : valid) {
            std::string a = old_normalize(v), b = normalize_serial(v);
            CHECK(!a.empty());
            CHECK_EQ(b, a);                                   // identical before and after
            CHECK_EQ(normalize_serial(b), b);                 // and idempotent
        }
        // clearly invalid: too many digits or the wrong letter count are rejected
        const char* invalid[] = {"SLUS-209461", "SLUS209461", "SLUS_209.461", "SLUS-2094612", "SLUS-000000", "SLUSS-20946", "SLU-20946", "SL-20946", "SLUS-2094", "SLUS-", "SLUS", "", "12345"};
        for (const char* v : invalid) CHECK_EQ(normalize_serial(v), "");
        // the two functions agree: whatever serial_from_filename accepts, normalize accepts identically, and the six-digit case is rejected by both
        CHECK_EQ(normalize_serial("SLUS-209461"), serial_from_filename("SLUS-209461"));
        CHECK_EQ(serial_from_filename("SLUS-209461.iso"), "");
        CHECK_EQ(serial_from_filename("SLUS_209.461.iso"), "");
        for (const char* v : valid) {
            std::string f = serial_from_filename(v);
            if (!f.empty()) CHECK_EQ(f, normalize_serial(v));
        }
        // exhaustive over digit counts: five digits is valid, four and six are not, with every separator form
        for (const char* sep : {"-", "_", "", " "}) {
            for (int n = 1; n <= 8; ++n) {
                std::string in = std::string("SLUS") + sep + std::string("12345678").substr(0, n);
                CHECK_EQ(normalize_serial(in), n == 5 ? "SLUS-12345" : "");
            }
        }
    }
    {   // serial_from_filename
        struct { const char* in; const char* out; } t[] = {
            {"SLUS_012.34.iso", "SLUS-01234"}, {"SLES-01234.bin", "SLES-01234"}, {"[SCUS-94163]", "SCUS-94163"},
            {"slus_209.46.chd", "SLUS-20946"}, {"slus-20946.chd", "SLUS-20946"}, {"SLUS20946.iso", "SLUS-20946"},
            {"GTA San Andreas [SLUS-20946].chd", "SLUS-20946"}, {"Final Fantasy VII (Disc 2) [SCUS-94163].cue", "SCUS-94163"},
            {"Final Fantasy VII (Disc 2) (SCUS-94164)", "SCUS-94164"}, {"Gran Turismo (Europe) (SCES-00984).bin", "SCES-00984"},
            {"Tekken 3 (J) SLPS 00700.iso", "SLPS-00700"}, {"SLES-52345", "SLES-52345"}, {"SLUS-20946", "SLUS-20946"},
            // no serial
            {"", ""}, {"Crash Bandicoot.cue", ""}, {"Resident Evil 2 (Disc 1).bin", ""}, {"disc 2.iso", ""}, {"SLUS", ""},
            {"SLUS-1234.iso", ""}, {"ABCDE-12345.iso", ""},
            // too many digits: not a serial
            {"SLUS-209461.iso", ""},
            // letters must start a token: "xSLUS-20946" has no boundary before SLUS
            {"xSLUS-20946", ""}, {"9SLUS-20946", ""},
        };
        for (auto& c : t) CHECK_EQ(serial_from_filename(c.in), c.out);
        // first serial wins
        CHECK_EQ(serial_from_filename("A (SLUS-00001) B (SLES-00002)"), "SLUS-00001");
        // serial found after a non-matching 4-letter word
        CHECK_EQ(serial_from_filename("Disc 1 of 2 [SLUS-20946]"), "SLUS-20946");
        // result is always normalised (idempotent)
        std::string r = serial_from_filename("[slus_209.46]");
        CHECK_EQ(normalize_serial(r), r);
    }
    {   // parse_system_cnf
        struct { const char* text; Cnf kind; const char* serial; } t[] = {
            {"BOOT2 = cdrom0:\\SLUS_209.46;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n", Cnf::Ps2, "SLUS-20946"},
            {"BOOT = cdrom:\\SCUS_941.63;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFF00\r\n", Cnf::Ps1, "SCUS-94163"},
            {"BOOT=cdrom:\\SLES_012.34;1", Cnf::Ps1, "SLES-01234"},
            {"boot2 = cdrom0:\\slus_209.46;1", Cnf::Ps2, "SLUS-20946"},
            {"Boot = CDROM:\\SCUS_941.63;1", Cnf::Ps1, "SCUS-94163"},
            {"BOOT2=cdrom0:SLUS_209.46;1", Cnf::Ps2, "SLUS-20946"},
            {"BOOT = cdrom:/SLUS_007.07;1", Cnf::Ps1, "SLUS-00707"},
            {"BOOT2 = cdrom0:\\SLES_523.45", Cnf::Ps2, "SLES-52345"},               // no ;1
            {"  BOOT2   =   cdrom0:\\SLUS_209.46;1   \r\n", Cnf::Ps2, "SLUS-20946"}, // extra spaces
            {"BOOT2 = cdrom0:\\SLUS_209.46;1\nVER = 1.0\n", Cnf::Ps2, "SLUS-20946"},  // LF only
            {"BOOT2 = cdrom0:\\SLUS_209.46;1\rVER = 1.0\r", Cnf::Ps2, "SLUS-20946"},  // CR only
            {"VER = 1.0\r\nBOOT2 = cdrom0:\\SLUS_209.46;1", Cnf::Ps2, "SLUS-20946"}, // not first line
            {"BOOT2 = cdrom0:\\SLUS_209.46;1\r\nBOOT = cdrom:\\SCUS_941.63;1", Cnf::Ps2, "SLUS-20946"},  // BOOT2 first wins at once
            {"BOOT = cdrom:\\SCUS_941.63;1\r\nBOOT2 = cdrom0:\\SLUS_209.46;1", Cnf::Ps2, "SLUS-20946"},  // BOOT2 overrides BOOT
            {"BOOT2 = cdrom0:\\PS2\\SLUS_209.46;1", Cnf::Ps2, "SLUS-20946"},           // subfolder
            {"BOOT2 = cdrom0:\\SLUS_209.46;1\r\nBOOT2 = cdrom0:\\SLES_523.45;1", Cnf::Ps2, "SLUS-20946"},
            // not a boot line
            {"", Cnf::None, ""}, {"\r\n\r\n", Cnf::None, ""}, {"VER = 1.0\r\nVMODE = PAL\r\n", Cnf::None, ""},
            {"BOOTX = cdrom:\\SCUS_941.63;1", Cnf::None, ""}, {"BOOT cdrom:\\SCUS_941.63;1", Cnf::None, ""},
            {"; BOOT = cdrom:\\SCUS_941.63;1", Cnf::None, ""},
            // boot line present but file is not a serial: kind is reported, serial empty
            {"BOOT2 = cdrom0:\\PS2OS.ELF;1", Cnf::Ps2, ""},
            {"BOOT = cdrom:\\GAME.EXE;1", Cnf::Ps1, ""},
            {"BOOT2 =", Cnf::Ps2, ""},
        };
        for (auto& c : t) {
            std::string s = "unset";
            Cnf k = parse_system_cnf(c.text, &s);
            CHECK(k == c.kind);
            if (c.kind != Cnf::None) CHECK_EQ(s, c.serial);
            else CHECK_EQ(s, "unset");                                  // serial untouched when nothing found
            CHECK(parse_system_cnf(c.text, nullptr) == c.kind);        // null serial pointer is fine
        }
    }
    {   // SYSTEM.CNF text round-trips through the file-name path
        std::string s;
        parse_system_cnf("BOOT2 = cdrom0:\\SLUS_209.46;1", &s);
        CHECK_EQ(serial_from_filename("x [" + s + "].iso"), s);
    }
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("hub_disc_id_test: all passed\n");
    return 0;
}
