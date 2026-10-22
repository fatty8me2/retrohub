// GENERATED from Smash Remix (JSSixtyfour/smashremix) tag 2.0.0,
// commit 0eb77ccd88e3a1228da8bb5a85a45eecc43cc2ed. Do not hand-edit. Source of truth is the Remix tag above.
//
// Sources (Remix repo paths):
//   src/Character.asm      scope id (vanilla ids), define_character order (new ids 0x1B..0x60)
//   src/Costumes.asm       num_costumes table (costume max index, 0-based, per fighter id)
//   src/CharacterSelect.asm  layout scope: 30 CSS slots
//   src/Stages.asm         scope id (stage ids), add_stage(...) rows (class, legality, toggle)
//   src/Global.asm         RAM addresses for screens, VS struct, screen_interrupt, frame counter
//   Layout cross-checked against src/Global.asm `vs` scope and the BattleShip decomp
//   (src/sc/sctypes.h SCBattleState/SCPlayerData/SCCommonData, src/sc/scdef.h SCKind).
#pragma once

#include <cstddef>
#include <cstdint>

namespace remix {

// ---- RAM (N64 virtual addresses, big-endian in guest memory) ----
// Screen/scene byte. Same byte as the decomp's gSCManagerSceneData.scene_curr.
constexpr std::uint32_t kCurrentScreen      = 0x800A4AD0;  // u8
constexpr std::uint32_t kPreviousScreen     = 0x800A4AD1;  // u8
// SCCommonData.gkind (decomp offset 0x0F from 0x800A4AD0). Copied into the battle state at VS start.
constexpr std::uint32_t kSceneGkind         = 0x800A4ADF;  // u8
// Write 1 (u32) here after setting kCurrentScreen to load that screen (Menu.change_screen_).
constexpr std::uint32_t kScreenInterrupt    = 0x800465D0;  // u32
constexpr std::uint32_t kFrameCounter       = 0x8003CB30;  // u32, never resets
// Pointer to the active match settings (VS: points at kVsBase).
constexpr std::uint32_t kMatchInfoPtr       = 0x800A50E8;  // u32 pointer
// gSCManagerTransferBattleState: the VS settings block the CSS writes and the battle reads.
constexpr std::uint32_t kVsBase             = 0x800A4D08;
constexpr std::uint32_t kVsPlayer1          = 0x800A4D28;  // = kVsBase + 0x20
constexpr std::uint32_t kVsPlayerStride     = 0x74;
constexpr std::size_t   kVsPlayerCount      = 4;

// Offsets inside the VS block (add to kVsBase). Remix names in comments.
namespace vs {
constexpr std::uint32_t kGameType       = 0x00;  // u8, 1 = Royal (VS). Remix GAMEMODE::VS
constexpr std::uint32_t kStage          = 0x01;  // u8, stage id (Remix vs.stage)
constexpr std::uint32_t kTeams          = 0x02;  // u8 bool (Remix vs.teams)
constexpr std::uint32_t kGameRules      = 0x03;  // u8 bits: 1 = time, 2 = stock, 3 = both (Remix vs.game_mode)
constexpr std::uint32_t kPlCount        = 0x04;  // u8 humans registered
constexpr std::uint32_t kCpCount        = 0x05;  // u8 CPUs registered
constexpr std::uint32_t kTime           = 0x06;  // u8 minutes, 100 = infinite (Remix vs.time)
constexpr std::uint32_t kStocks         = 0x07;  // u8; Remix: 0 = 1 stock. Encoding unverified, see docs/REMIX.md
constexpr std::uint32_t kHandicap       = 0x08;  // u8 0 off, 1 on, 2 auto (Remix vs.handicap)
constexpr std::uint32_t kTeamAttack     = 0x09;  // u8 bool
constexpr std::uint32_t kStageSelect    = 0x0A;  // u8 bool
constexpr std::uint32_t kDamageRatio    = 0x0B;  // u8, 50..200 percent
constexpr std::uint32_t kItemToggles    = 0x0C;  // u32 item on/off bits
constexpr std::uint32_t kIsResetPlayers = 0x10;  // u8 bool
constexpr std::uint32_t kGameStatus     = 0x11;  // u8 (0 wait, 1 ongoing, ...) (Remix vs.game_status)
constexpr std::uint32_t kTimeRemain     = 0x14;  // u32 timer, 60 per second (Remix vs.timer)
constexpr std::uint32_t kTimePassed     = 0x18;  // u32 elapsed, 60 per second (Remix vs.elapsed)
constexpr std::uint32_t kItemFrequency  = 0x1C;  // u8, 0 none .. 5 high (Remix vs.item_frequency)
}  // namespace vs

// Offsets inside one player block (add to kVsPlayer1 + n * kVsPlayerStride).
namespace player {
constexpr std::uint32_t kLevel          = 0x00;  // u8 CPU level, begins at 1
constexpr std::uint32_t kHandicapLevel  = 0x01;  // u8 per-player handicap
constexpr std::uint32_t kPkind          = 0x02;  // u8, see PKind
constexpr std::uint32_t kFkind          = 0x03;  // u8 fighter id (see kFighters)
constexpr std::uint32_t kTeam           = 0x04;  // u8
constexpr std::uint32_t kPlayer         = 0x05;  // u8 controller port / slot
constexpr std::uint32_t kCostume        = 0x06;  // u8 costume index
constexpr std::uint32_t kShade          = 0x07;  // u8 shade for duplicate costumes
constexpr std::uint32_t kColor          = 0x08;  // u8 (emblem / tag colour)
constexpr std::uint32_t kSingleStockIcon= 0x09;  // u8 bool
constexpr std::uint32_t kTag            = 0x0A;  // u8 tag sprite index, 4 = none
constexpr std::uint32_t kStockCount     = 0x0B;  // s8, -1 = no stocks
constexpr std::uint32_t kFighterGObj    = 0x58;  // pointer, runtime only
}  // namespace player

enum PKind : std::uint8_t { kPkindHuman = 0, kPkindCpu = 1, kPkindNone = 2 };

// Scene/screen ids (Global.screen in Remix == decomp nSCKind*). Same numbering in 2.0.0 and 2.0.1.
namespace screen {
constexpr std::uint8_t kTitle        = 0x01;
constexpr std::uint8_t kModeSelect   = 0x07;
constexpr std::uint8_t kVsGameModeMenu = 0x09;
constexpr std::uint8_t kVsCss        = 0x10;  // nSCKindPlayersVS
constexpr std::uint8_t kStageSelect  = 0x15;  // nSCKindMaps
constexpr std::uint8_t kVsBattle     = 0x16;  // nSCKindVSBattle
constexpr std::uint8_t kVsResults    = 0x18;  // nSCKindVSResults
}  // namespace screen

// ---- Fighters ----
// id 0x00..0x60 inclusive (kFighterIdCount slots). 0x1C is unusable. max_costume = highest valid
// 0-based costume index for the base costume set, from the Costumes.asm num_costumes table;
// extra costumes come from a separate table and are not counted here.
struct Fighter {
    std::uint8_t id;
    const char* name;
    std::uint8_t max_costume;
    bool on_css;          // one of the 30 fixed CSS panels (CharacterSelect.asm layout)
};

constexpr std::uint8_t kFighterIdCount = 0x61;
constexpr Fighter kFighters[] = {
    {0x00, "Mario", 4, true},
    {0x01, "Fox", 3, true},
    {0x02, "Donkey Kong", 4, true},
    {0x03, "Samus", 4, true},
    {0x04, "Luigi", 3, true},
    {0x05, "Link", 3, true},
    {0x06, "Yoshi", 5, true},
    {0x07, "Captain Falcon", 5, true},
    {0x08, "Kirby", 4, true},
    {0x09, "Pikachu", 3, true},
    {0x0A, "Jigglypuff", 4, true},
    {0x0B, "Ness", 3, true},
    {0x0C, "Master Hand", 0, false},
    {0x0D, "Metal Mario", 5, false},
    {0x0E, "Polygon Mario", 5, false},
    {0x0F, "Polygon Fox", 5, false},
    {0x10, "Polygon Donkey Kong", 5, false},
    {0x11, "Polygon Samus", 5, false},
    {0x12, "Polygon Luigi", 5, false},
    {0x13, "Polygon Link", 5, false},
    {0x14, "Polygon Yoshi", 5, false},
    {0x15, "Polygon Captain Falcon", 5, false},
    {0x16, "Polygon Kirby", 5, false},
    {0x17, "Polygon Pikachu", 5, false},
    {0x18, "Polygon Jigglypuff", 5, false},
    {0x19, "Polygon Ness", 5, false},
    {0x1A, "Giant Donkey Kong", 4, false},
    {0x1B, "Random", 0, false},
    {0x1C, "Unused (Mario clone slot)", 0, false},
    {0x1D, "Falco", 5, true},
    {0x1E, "Ganondorf", 6, true},
    {0x1F, "Young Link", 5, true},
    {0x20, "Dr. Mario", 5, true},
    {0x21, "Wario", 5, true},
    {0x22, "Dark Samus", 7, false},
    {0x23, "E Link", 3, false},
    {0x24, "J Samus", 4, false},
    {0x25, "J Ness", 3, false},
    {0x26, "Lucas", 6, false},
    {0x27, "J Link", 3, false},
    {0x28, "J Falcon", 5, false},
    {0x29, "J Fox", 3, false},
    {0x2A, "J Mario", 4, false},
    {0x2B, "J Luigi", 3, false},
    {0x2C, "J Donkey Kong", 4, false},
    {0x2D, "E Pikachu", 3, false},
    {0x2E, "J Jigglypuff", 4, false},
    {0x2F, "E Jigglypuff", 4, false},
    {0x30, "J Kirby", 4, false},
    {0x31, "J Yoshi", 5, false},
    {0x32, "J Pikachu", 3, false},
    {0x33, "E Samus", 4, false},
    {0x34, "Bowser", 5, true},
    {0x35, "Giga Bowser", 5, false},
    {0x36, "Piano", 5, false},
    {0x37, "Wolf", 5, true},
    {0x38, "Conker", 5, true},
    {0x39, "Mewtwo", 5, true},
    {0x3A, "Marth", 5, true},
    {0x3B, "Sonic", 5, true},
    {0x3C, "Sandbag", 5, false},
    {0x3D, "Super Sonic", 5, false},
    {0x3E, "Sheik", 5, true},
    {0x3F, "Marina", 5, true},
    {0x40, "Dedede", 5, true},
    {0x41, "Goemon", 5, true},
    {0x42, "Peppy", 5, false},
    {0x43, "Slippy", 5, false},
    {0x44, "Banjo", 6, true},
    {0x45, "Metal Luigi", 5, false},
    {0x46, "Ebisumaru", 6, false},
    {0x47, "Dragon King", 5, false},
    {0x48, "Crash Bandicoot", 6, true},
    {0x49, "Princess Peach", 6, true},
    {0x4A, "Roy", 5, false},
    {0x4B, "Dr. Luigi", 7, false},
    {0x4C, "Lanky Kong", 7, false},
    {0x4D, "Polygon Wario", 5, false},
    {0x4E, "Polygon Lucas", 5, false},
    {0x4F, "Polygon Bowser", 5, false},
    {0x50, "Polygon Wolf", 5, false},
    {0x51, "Polygon Dr. Mario", 5, false},
    {0x52, "Polygon Sonic", 5, false},
    {0x53, "Polygon Sheik", 5, false},
    {0x54, "Polygon Marina", 5, false},
    {0x55, "Polygon Falco", 5, false},
    {0x56, "Polygon Ganondorf", 5, false},
    {0x57, "Polygon Dark Samus", 5, false},
    {0x58, "Polygon Marth", 5, false},
    {0x59, "Polygon Mewtwo", 5, false},
    {0x5A, "Polygon Dedede", 5, false},
    {0x5B, "Polygon Young Link", 5, false},
    {0x5C, "Polygon Goemon", 5, false},
    {0x5D, "Polygon Conker", 5, false},
    {0x5E, "Polygon Banjo", 5, false},
    {0x5F, "Polygon Peach", 5, false},
    {0x60, "Polygon Crash Bandicoot", 5, false},
};

// ---- Stages ----
// id is the value stored in gkind / vs.stage. class: vanilla_vs (0x00..0x08), remix_vs (add_stage
// class BATTLE, the VS-battle-capable Remix stages), remix_other (break-the-targets, board-the-platforms,
// RTTF, preview). Flag::unknown = not parsed for that stage (vanilla rows are not in add_stage).
enum class StageClass : std::uint8_t { vanilla_vs, remix_vs, remix_other };
enum class Flag : std::uint8_t { unknown, no, yes };

struct Stage {
    std::uint8_t id;
    const char* name;
    StageClass cls;
    Flag tournament_legal;
    Flag netplay_legal;
    Flag can_toggle;      // appears in the random-stage toggle list
};

constexpr Stage kStages[] = {
    {0x00, "Peach's Castle", StageClass::vanilla_vs, Flag::unknown, Flag::unknown, Flag::unknown},
    {0x01, "Sector Z", StageClass::vanilla_vs, Flag::unknown, Flag::unknown, Flag::unknown},
    {0x02, "Congo Jungle", StageClass::vanilla_vs, Flag::unknown, Flag::unknown, Flag::unknown},
    {0x03, "Planet Zebes", StageClass::vanilla_vs, Flag::unknown, Flag::unknown, Flag::unknown},
    {0x04, "Hyrule Castle", StageClass::vanilla_vs, Flag::unknown, Flag::unknown, Flag::unknown},
    {0x05, "Yoshi's Island", StageClass::vanilla_vs, Flag::unknown, Flag::unknown, Flag::unknown},
    {0x06, "Dream Land", StageClass::vanilla_vs, Flag::unknown, Flag::unknown, Flag::unknown},
    {0x07, "Saffron City", StageClass::vanilla_vs, Flag::unknown, Flag::unknown, Flag::unknown},
    {0x08, "Mushroom Kingdom", StageClass::vanilla_vs, Flag::unknown, Flag::unknown, Flag::unknown},
    {0x29, "Deku Tree", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x2A, "First Destination", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x2B, "Ganon's Tower", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x2C, "Gym Leader Castle", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x2D, "Pokemon Stadium", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x2E, "Tal Tal Heights", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x2F, "Glacial River", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x30, "WarioWare", StageClass::remix_other, Flag::unknown, Flag::unknown, Flag::yes},
    {0x31, "Battlefield", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x32, "Flat Zone", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x33, "Dr. Mario", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x34, "Cool Cool Mountain", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x35, "Dragon King", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0x36, "Great Bay", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x37, "Fray's Stage", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x38, "Tower of Heaven", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0x39, "Fountain of Dreams", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x3A, "Muda Kingdom", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0x3B, "Mementos", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x3C, "Showdown", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x3D, "Spiral Mountain", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x3E, "N64", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x3F, "Mute City DL", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x40, "Mad Monster Mansion", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0x41, "Mushroom Kingdom DL", StageClass::remix_vs, Flag::yes, Flag::no, Flag::yes},
    {0x42, "Mushroom Kingdom ~", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x43, "Bowser's Stadium", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0x44, "Peach's Castle II", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x45, "Delfino Plaza", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x46, "Corneria", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0x47, "Kitchen Island", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0x48, "Big Blue", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x49, "Onett", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x4A, "Crateria", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x4B, "Frosty Village", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x4C, "Smashville", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x4D, "Break the Targets", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x4E, "Break the Targets", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x4F, "Break the Targets", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x50, "Battlefield DL", StageClass::remix_vs, Flag::yes, Flag::no, Flag::yes},
    {0x51, "Break the Targets", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x52, "Break the Targets", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x53, "Break the Targets", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x54, "Break the Targets", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x55, "Hyrule Temple", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x56, "Break the Targets", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x57, "Board the Platforms", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x58, "New Pork City", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x59, "Board the Platforms", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x5A, "Smashketball", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x5B, "Board the Platforms", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x5C, "Norfair", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0x5D, "Raid Blue", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0x5E, "Congo Falls", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0x5F, "Pigmask Fortress", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x60, "Yoshi's Story", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x61, "World 1-1", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x62, "Flat Zone II", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x63, "Gerudo Valley", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x64, "Board the Platforms", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x65, "Board the Platforms", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x66, "Board the Platforms", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x67, "Hyrule Castle DL", StageClass::remix_vs, Flag::yes, Flag::no, Flag::yes},
    {0x68, "Hyrule Castle ~", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x69, "Congo Jungle DL", StageClass::remix_vs, Flag::yes, Flag::no, Flag::yes},
    {0x6A, "Congo Jungle ~", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x6B, "Peach's Castle DL", StageClass::remix_vs, Flag::yes, Flag::no, Flag::yes},
    {0x6C, "Peach's Castle ~", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x6D, "Board the Platforms", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x6E, "Fray's Stage - Night", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x6F, "Goomba Road", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x70, "Board the Platforms", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x71, "Sector Z DL", StageClass::remix_vs, Flag::yes, Flag::no, Flag::yes},
    {0x72, "Saffron City DL", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x73, "Yoshi's Island DL", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x74, "Planet Zebes DL", StageClass::remix_vs, Flag::yes, Flag::no, Flag::yes},
    {0x75, "Sector Z ~", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x76, "Saffron City ~", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x77, "Yoshi's Island ~", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x78, "Dream Land ~", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x79, "Planet Zebes ~", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x7A, "Break the Targets", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x7B, "Board the Platforms", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x7C, "Bowser's Keep", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x7D, "Rith Essa", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x7E, "Venom", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x7F, "Break the Targets", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x80, "Board the Platforms", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x81, "Break the Targets", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x82, "Board the Platforms", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x83, "Windy", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0x84, "dataDyne Central", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x85, "Planet Clancer", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x86, "Jungle Japes", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x87, "Break the Targets", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x88, "Game Boy Land", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x89, "Break the Targets", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x8A, "Board the Platforms", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x8B, "Allstar Rest Area", StageClass::remix_vs, Flag::no, Flag::no, Flag::no},
    {0x8C, "Board the Platforms", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x8D, "Castle Siege", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x8E, "Yoshi's Island II", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x8F, "Final Destination DL", StageClass::remix_vs, Flag::yes, Flag::no, Flag::yes},
    {0x90, "Final Tentination", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x91, "Cool Cool Mountain SR", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0x92, "Duel Zone DL", StageClass::remix_vs, Flag::yes, Flag::no, Flag::yes},
    {0x93, "Cool Cool Mountain DL", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0x94, "Meta Crystal DL", StageClass::remix_vs, Flag::yes, Flag::no, Flag::yes},
    {0x95, "Dream Greens", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x96, "Peach's Castle Beta", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x97, "Hyrule Castle SR", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x98, "Sector Z SR", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0x99, "Mute City", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0x9A, "Home Run Contest", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0x9B, "Mushroom Kingdom SR", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0x9C, "Green Hill Zone", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0x9D, "Subcon", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0x9E, "Pirate Land", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0x9F, "Casino Night Zone", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0xA0, "Break the Targets", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0xA1, "Board the Platforms", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0xA2, "Metallic Madness", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0xA3, "Rainbow Road", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0xA4, "Pokemon Stadium 2", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0xA5, "Norfair Remix", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0xA6, "Toad's Turnpike", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0xA7, "Tal Tal Heights Remix", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0xA8, "Board the Platforms", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0xA9, "Winter Dream Land", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0xAA, "Break the Targets", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0xAB, "Glacial River Remix", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0xAC, "Break the Targets", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0xAD, "Dragon King Remix", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0xAE, "Board the Platforms", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0xAF, "Break the Targets", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0xB0, "Dracula's Castle", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0xB1, "Reverse Castle", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0xB2, "Board the Platforms", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0xB3, "Mt. Dedede", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0xB4, "Edo Town", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0xB5, "Deku Tree DL", StageClass::remix_vs, Flag::yes, Flag::no, Flag::yes},
    {0xB6, "Crateria DL", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0xB7, "Break the Targets", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0xB8, "First Destination Remix", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0xB9, "Board the Platforms", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0xBA, "Twilight City", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0xBB, "Melrode", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0xBC, "Meta Crystent", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0xBD, "Remix 1p Race to the Finish", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0xBE, "Grim Reaper's Cavern", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0xBF, "Scuttle Town", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0xC0, "Big Boo's Haunt", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0xC1, "Dinosaur Land", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0xC2, "Break the Targets", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0xC3, "Spawned Fear", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0xC4, "Smashville Remix", StageClass::remix_vs, Flag::yes, Flag::yes, Flag::yes},
    {0xC5, "Board the Platforms", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0xC6, "Poke Floats", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0xC7, "Big Snowman", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0xC8, "Dream Land Beta DL", StageClass::remix_vs, Flag::no, Flag::yes, Flag::yes},
    {0xC9, "LMAO Castle", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0xCA, "Discovery Falls", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0xCB, "Break the Targets", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0xCC, "Discovery Falls Remix", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0xCD, "N64 Remix", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0xCE, "Board The Platforms", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0xCF, "Break the Targets", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0xD0, "Board The Platforms", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0xD1, "Soccer", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0xD2, "Time Twister", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0xD3, "Time Twister", StageClass::remix_other, Flag::no, Flag::no, Flag::no},
    {0xD4, "N. Sanity Beach", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0xD5, "Snow Go", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
    {0xD6, "Future Frenzy", StageClass::remix_vs, Flag::no, Flag::no, Flag::yes},
};

// ---- Compile-time sanity (pure data checks) ----
static_assert(vs::kTime == 0x06 && vs::kGameStatus == 0x11 && vs::kItemFrequency == 0x1C, "VS offsets");
static_assert(kVsPlayer1 == kVsBase + 0x20, "player block starts at +0x20");
static_assert(kVsPlayerStride == 0x74, "SCPlayerData is 0x74 bytes");
static_assert(kSceneGkind == kCurrentScreen + 0x0F, "SCCommonData.gkind offset");
static_assert(sizeof(kFighters) / sizeof(kFighters[0]) == kFighterIdCount, "one row per fighter id");

}  // namespace remix
