# Smash Remix 2.0.0 in RetroHub: fighter/stage data, RAM layout, and a direct-VS injection plan

Research only. No launcher code is wired yet. Data tables are in `src/common/remix_data.h` (generated from the Remix source; see the header).

## 0. Versions and sources

| Item | Value |
|---|---|
| Target | Smash Remix **2.0.0** (user ROM MD5 `43571020FA5DDB138BBD7F64A8C9C0D4`) |
| Remix tag `2.0.0` | commit `0eb77ccd88e3a1228da8bb5a85a45eecc43cc2ed` (2025-06-25) |
| Remix `master` (2.0.1) | commit `5e04fe7fcd023cd43c71f25f89bb6e810d254d55` (2026-03-08) |
| Remix repo | https://github.com/JSsixtyfour/smashremix |
| RetroHub entry | `src/common/games.cpp` (`"remix"`): "MD5 of the 2.0.0 release .z64", so the user's ROM is the 2.0.0 build |
| Base ROM | USA Super Smash Bros. (Remix ships as an xdelta patch of it; `original.xdelta`) |
| US decomp | `BattleShip/decomp` (PC port; addresses are the N64 US addresses from comments in `src/sc/scmanager.c`) |

Remix 2.0.0 vs 2.0.1, checked by diffing the two tags:
- `src/Global.asm` is **byte-identical** in both. The RAM map below is the same for both.
- Fighter IDs (`Character.asm` define order and `scope id`) are identical in both.
- Stage IDs `0x00..0xD6` are identical in both. 2.0.1 adds `htp_fall` ("Autumn How to Play", `0xD7`). Stage display order and some toggle/legality flags changed.
- The CSS layout (30 panels) is identical in both.

So the tables in `remix_data.h` are valid for 2.0.0 and 2.0.1 except for stage `0xD7`, which is 2.0.1 only. The generator (`scratchpad/gen_remix_data.py`, not committed) parses the 2.0.0 tag.

## 1. Fighters (IDs 0x00..0x60)

`fkind` is one byte everywhere (decomp `SCPlayerData.fkind`, Remix `Global.vs.p*` + 3). All 97 slots (0x00..0x60) fit.

- `0x00..0x1A`: vanilla. Playable vanilla are `0x00..0x0B` (Mario, Fox, DK, Samus, Luigi, Link, Yoshi, Captain Falcon, Kirby, Pikachu, Jigglypuff, Ness). `0x0C` Master Hand (boss), `0x0D` Metal Mario, `0x0E..0x1A` polygon variants and Giant DK.
- `0x1B` Random (`define_character` at Character.asm, "0x1B - RANDOM").
- `0x1C` unusable. Remix places a Mario clone here and it is not selectable.
- `0x1D..0x60`: 68 Remix-added fighters (`ADD_CHARACTERS(68)`), in define order: Falco 0x1D, Ganondorf 0x1E, Young Link 0x1F, Dr. Mario 0x20, Wario 0x21, Dark Samus 0x22, ... Banjo 0x44, Crash 0x48, Peach 0x49, ... Polygon Crash 0x60.
- Display names and the highest valid base costume index come from `src/Costumes.asm` `num_costumes` (97 rows, row index = fighter ID). The value is **0-based max** ("number of original costumes char has (0-based)"), so `4` means costumes 0..4. Extra costumes come from a separate `extra_costumes_table` and are not in the header.

**Fixed CSS panels** (`src/CharacterSelect.asm`, `scope layout`, `NUM_SLOTS(30)`), 30 panels, 12 vanilla playable and 18 non-vanilla:
Mario, Fox, DK, Samus, Luigi, Link, Yoshi, Captain Falcon, Kirby, Pikachu, Jigglypuff, Ness (vanilla) + Falco, Ganondorf, Young Link, Dr. Mario, Wario, Bowser, Wolf, Conker, Mewtwo, Marth, Sonic, Sheik, Marina, Dedede, Goemon, Banjo, Crash, Peach (new). Note: the readme says the CSS holds "16 custom characters"; the code has 18 non-vanilla panels. Trust the code.

Only the 30 panels are on the CSS. Other IDs (variants, J/E regionals, bonus fighters like Dark Samus, Lucas, Mad Piano) are selectable through other paths (d-pad up, bonus CSS, the `?` slot, Remix settings). `remix_data.h` marks `on_css` only. Selectability beyond the CSS panels is not parsed.

## 2. Stages (gkind byte, 0x00..0xD7)

`Stages.asm` `scope id` gives stage IDs. `add_stage(...)` rows give display name, legality, and class. New stage IDs run `0x29..0xD6` (sequential in `add_stage` order). The header has 183 rows:
- 9 vanilla VS stages `0x00..0x08`: Peach's Castle, Sector Z, Congo Jungle, Planet Zebes, Hyrule Castle, Yoshi's Island, Dream Land, Saffron City, Mushroom Kingdom.
- 127 Remix `class.BATTLE` stages (VS-battle capable; `can_toggle` marks the random-stage toggle list).
- 47 non-VS Remix stages (`BTT`/`BTP` bonus boards, `RTTF`, `SSS_PREVIEW`).

The `tournament_legal`, `netplay_legal` and `can_toggle` flags are carried in the header. Vanilla rows are `Flag::unknown` because they are not in `add_stage`.

The VS stage byte is set from `gSCManagerSceneData.gkind` (decomp `scVSBattleStartScene`: `gSCManagerBattleState->gkind = gSCManagerSceneData.gkind`). It is also `Global.vs.stage`.

## 3. Vanilla RAM and struct layout

Addresses are N64 virtual. **Global.asm of Remix uses the same addresses**, so these are confirmed by Remix as well as by the decomp comments.

### Scene/screen
| Address | Symbol | Type | Meaning |
|---|---|---|---|
| `0x800A4AD0` | `gSCManagerSceneData.scene_curr` = Remix `Global.current_screen` | u8 | Current scene (screen) |
| `0x800A4AD1` | `scene_prev` = Remix `previous_screen` | u8 | Previous scene |
| `0x800A4ADF` | `SCCommonData.gkind` (offset 0x0F) | u8 | Stage for the next battle |
| `0x800465D0` | Remix `Global.screen_interrupt` | u32 | Write `1` to load `current_screen` |
| `0x8003CB30` | Remix `Global.frame_counter` | u32 | Never resets; use as the frame clock |

The `SCCommonData.gkind` offset is derived from the decomp struct. `SCCommonData` is 0x48 bytes (the gap from `0x800A4AD0` to `0x800A4B18`). Offsets: `scene_curr` 0, `scene_prev` 1, `unlock_messages[7]` 2..8 (`nLBBackupUnlockEnumCount` = 7), `challenger_fkind` 9, `demo_mask_prev` 10 (u16), `demo_first_fkind` 12, `demo_fkind[2]` 13..14, `gkind` **15**. It is not in Remix's `Global.asm`, so verify it in-emulator.

### Scene change mechanism
- `scManagerRunLoop` (decomp `src/sc/scmanager.c`) is a loop that dispatches on `scene_curr` (overlays are loaded per scene in a `switch`, e.g. `nSCKindVSBattle` loads overlays 2,3,4).
- Remix's own way to change screens is `Menu.change_screen_` (Menu.asm): it writes `previous = current`, writes `current = next`, then writes `1` to `screen_interrupt`. The running scene ends at the next frame boundary (decomp comment near `scmanager.c` 1869: "manager to end the current scene at the frame boundary").
- Screen IDs match decomp `SCKind` (confirmed for 0x01, 0x07, 0x09, 0x10, 0x15, 0x16, 0x18 by `Global.screen` in Remix and `scdef.h`).

### VS settings block (`gSCManagerTransferBattleState` = Remix `Global.vs`)
Base `0x800A4D08`. Remix's `Global.vs` scope confirms offsets 0x01, 0x02, 0x03, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x11, 0x14, 0x18, 0x1C and the player blocks.

| Offset | Address | Field (decomp) | Remix name / note |
|---|---|---|---|
| 0x00 | 0x800A4D08 | `game_type` | `GAMEMODE::VS` = 1 (Royal) |
| 0x01 | 0x800A4D09 | `gkind` | `vs.stage` |
| 0x02 | 0x800A4D0A | `is_team_battle` | `vs.teams` |
| 0x03 | 0x800A4D0B | `game_rules` | `vs.game_mode`: 1 time, 2 stock, 3 both |
| 0x04 | 0x800A4D0C | `pl_count` | (not in Remix scope; set it) |
| 0x05 | 0x800A4D0D | `cp_count` | (not in Remix scope; set it) |
| 0x06 | 0x800A4D0E | `time_limit` | `vs.time`: minutes, 100 = infinite |
| 0x07 | 0x800A4D0F | `stocks` | `vs.stocks` (see risks) |
| 0x08 | 0x800A4D10 | `handicap` | `vs.handicap`: 0 off, 1 on, 2 auto |
| 0x09 | 0x800A4D11 | `is_team_attack` | `vs.team_attack` |
| 0x0A | 0x800A4D12 | `is_stage_select` | `vs.stage_select` |
| 0x0B | 0x800A4D13 | `damage_ratio` | `vs.damage`: 50..200 |
| 0x0C | 0x800A4D14 | `item_toggles` (u32) | |
| 0x10 | 0x800A4D18 | `is_reset_players` | |
| 0x11 | 0x800A4D19 | `game_status` | `vs.game_status`: 0 wait |
| 0x14 | 0x800A4D1C | `time_remain` (u32) | `vs.timer` |
| 0x18 | 0x800A4D20 | `time_passed` (u32) | `vs.elapsed` |
| 0x1C | 0x800A4D24 | `item_appearance_rate` | `vs.item_frequency` |
| 0x20 | 0x800A4D28 | `players[0]` (P1) | `vs.p1`, stride `0x74` |

Player block (`SCPlayerData`, 0x74 bytes; the struct is 4 players, so 0x20 + 4 * 0x74 = 0x1F0 which matches `gSCManager1PGameBattleState` to `gSCManagerTransferBattleState` = 0x1F0):

| Off | Field | Off | Field |
|---|---|---|---|
| 0x00 | `level` (CPU level, from 1) | 0x0B | `stock_count` (s8, -1 = none) |
| 0x01 | `handicap` | 0x0C | `is_spgame_enemy` |
| 0x02 | `pkind` (0 human, 1 CPU, 2 none) | 0x0D | `place` |
| 0x03 | `fkind` | 0x10 | `falls` (s32) |
| 0x04 | `team` | 0x14 | `score` (s32) |
| 0x05 | `player` | 0x58 | `fighter_gobj` (runtime, do not write) |
| 0x06 | `costume` | 0x5C | `stale_id` |
| 0x07 | `shade` | 0x60 | `stale_info[5]` (u16 attack_id, u16 motion_count) |
| 0x08 | `color` | | |
| 0x09 | `is_single_stockicon` | | |
| 0x0A | `tag` (4 = none) | | |

`mnPlayersVSSetSceneData` (decomp `src/mn/mnplayers/mnplayersvs.c` ~4866) is the authoritative list of what the VS CSS writes into this block at exit: `time_limit`, `stocks`, `is_team_battle`, `game_rules`, and per player `player`, `fkind`, `pkind`, `costume`, `shade`, `color`, `tag`, `is_single_stockicon`, `level` (CPU) or `handicap` (human), plus `pl_count`/`cp_count`. Remix's CSS is its own asm, so assume it writes the same fields and verify.

### Remix does not move these
`Global.asm` (identical in 2.0.0 and 2.0.1) puts the VS block at `0x800A4D08` and `match_info` at `0x800A50E8`. Remix's `AI.asm` walks the same player blocks using `Global.vs.P_OFFSET` (0x20) and `P_DIFF` (0x74). Nothing relocates the VS struct. The Remix character table (`STRUCT_TABLE` at `0x80116E10` in Character.asm) and the extra fighter data are separate and do not change the battle struct.

## 4. Endianness and RDRAM access

- N64 is big-endian: a 32-bit value at address `A` has its most significant byte at `A`.
- mupen64plus keeps RDRAM host-endian-swapped on little-endian hosts. Byte access at physical address `A` is at host index `A ^ 3`, and halfword at `A ^ 2`. RetroHub already does this: `src/n64run/frontend.cpp:596-598` uses `ram[(d.offset + i) ^ 3u]` ("RDRAM is stored as host-endian 32-bit words; ^3 on the byte index yields N64 (big-endian) byte order").
- Virtual to physical for RDRAM: `addr & 0x7FFFFF` (8 MB). Remix needs the **8 MB expansion pak** (readme.md line 14: "Smash Remix requires the 8 MB expansion pak to be enabled"). `frontend.cpp` already hashes 8 MB.
- Get the RDRAM pointer with `DebugMemGetPointer(M64P_DBG_PTR_RDRAM)` (`frontend.cpp:563`). Write bytes with `ptr[(phys + i) ^ 3]`. For a 32-bit write, write big-endian bytes in order, each via `^3`.
- Mupen core config already forced in `frontend.cpp:401-420` (`R4300Emulator=2`, `RandomizeInterrupt=0`, `DisableExtraMem=0`). Keep these; they are needed for determinism.

## 5. Recommended injection approach

Write the VS match state directly at a fixed frame count, from the per-VI hook, using Remix's own screen change. No CSS input and no start-button simulation.

**Why not drive the CSS with input:** a Start press in the CSS depends on input timing and on the menu's state. A fixed-frame write is identical on every peer when the lockstep (`Lockstep` in n64run) gives every peer the same frame count.

**Steps (all in the per-VI hook; run identically on every peer):**
1. Wait until `frame_counter` (`0x8003CB30`) reaches `F` (a constant chosen once for the whole room, e.g. 300) **and** `current_screen` (`0x800A4AD0`) is `0x01` (title) or another known stable screen. If not, retry next frame. Do not write mid-transition.
2. Write the VS block (section 3): `game_type=1`, `gkind=stage`, `teams=0`, `game_rules`, `time_limit`, `stocks`, `handicap=0`, `team_attack=0`, `stage_select=0`, `damage_ratio=100`, `item_appearance_rate`, `item_toggles`, `game_status=0`, `pl_count`, `cp_count`.
3. For each player `i` (0..3): `pkind`, `fkind`, `costume` (within `max_costume`), `shade`, `color`, `tag`, `team`, `player=i`, `is_single_stockicon=(game_rules & 1)`, `stock_count=stocks`, `level` (CPU) or `handicap` (human). Unused slots: `pkind = 2` (none).
4. Write `SCCommonData.gkind` (`0x800A4ADF`) = stage, so `scVSBattleStartScene` copies the same value.
5. Set `previous_screen = 0x10` and `current_screen = 0x16` (`VS_BATTLE`). Write `1` to `screen_interrupt` (`0x800465D0`). This is what `Menu.change_screen_` does; the scene dispatcher then loads the battle overlays.
6. Read back the VS block and player blocks to check they stuck (hash them into the lockstep log).

**Gating test (must pass before use):** headless run (`RETRORUN_MUTE`/`N64RUN_MUTE`, `--headless`, no windows). Start a VS match with Mario vs. CPU Fox on Peach's Castle, and also one Remix-added fighter (for example Falco, `0x1D`) and one of the 18 non-vanilla CSS panels, plus a 2-human + 2-CPU case. Check: no crash, stock count and time display match the header values, and the RDRAM hash agrees across two `--net` peers for N frames.

### The fighter-file gap (main risk for step 5)
`ftManagerMakeFighter` (decomp `src/ft/ftmanager.c` ~822) reads the fighter's attribute data through `*fp->data->p_file_main`. That pointer is only set by `ftManagerSetupFilesAllKind` (ftmanager.c ~375), and the VS CSS calls it for the 12 vanilla playable fighters (`mnplayersvs.c` ~5466..5471 loop over `nFTKindPlayableStart..nFTKindPlayableEnd`). Remix's custom fighters are loaded by Remix's own CSS heap code (`CharacterSelect.asm` `get_free_heap_slot_`, `clear_first_obsolete_heap_slot_`, `get_heap_slot_for_char_id_`), not by that loop.

So a direct `VS_BATTLE` entry from the title could start with NULL fighter file pointers. Two ways to handle it:
- **Preferred:** before step 5, make the game load the fighters the same way the CSS does. Enter `VS_CSS` (`0x10`) with `screen_interrupt` at frame `F`, let the CSS init run (it preloads the vanilla playable set), then do steps 2-5 a fixed number of frames later. Custom fighters still need Remix's heap loader. Find that function's address from a build of the ROM (see next steps) and call it once per fkind from the hook.
- **Fallback:** test whether Remix loads custom fighters on battle start. If not, the fallback is the CSS-first path above with each custom fighter loaded by its heap routine.

Decide by experiment in the gating test, not by reading.

## 6. Open risks and unknowns

1. **Fighter file preload (section 5).** Highest risk. Crash or NULL deref at battle start if a fighter's files are not resident.
2. **Stock encoding.** Remix `Global.asm` says `vs.stocks`: "0 = 1 stock". The decomp CSS stores `sMNPlayersVSStockValue` 0-based and displays `+1`, but `scvsbattle.c:333` passes `gSCManagerBattleState->stocks` straight to `desc.stock_count`. These two are not obviously consistent. Test: write `stocks = N-1` and read `player.stock_count` (+0x0B) after start. Adjust if it does not match.
3. **Time encoding.** Remix says `2 = 2 min` and `100 = infinite`, which matches `SCBATTLE_TIMELIMIT_INFINITE = 100` in the decomp. Confirm `time` is minutes in the HUD.
4. **Settings that live outside the VS block.** Remix's options menus (Settings.asm, Remix menus, hazards, random profiles, item settings, CSS debug menu per-panel attributes such as giant/tiny or alt moveset) are not in `vs`. A direct start skips the code that applies them. Decide per setting: pin it to the same value on all peers, or write it. Also check where Remix persists these (SRAM/ save paths) so every peer uses identical settings. Otherwise the battle desyncs.
5. **Determinism.** `RandomizeInterrupt=0` is already set. Stage choice in the VS flow can use time-based RNG (decomp `mnmaps.c` uses `syUtilsRandTimeUCharRange`), but we set the stage explicitly so that path is skipped. In-battle item and CPU randomness uses the game RNG. Remix has `get_random_int_safe_` (`Global.asm`) for netplay. Confirm the battle uses it, or check RDRAM hash agreement after N frames.
6. **Costume bounds.** `max_costume` is the base set only. Extra costumes (`extra_costumes_table`) are not validated. Clamp to `max_costume` unless tested.
7. **Stage 0xD7** exists only in 2.0.1. Do not show it in a 2.0.0 lobby.
8. **Readme vs code.** Readme says "16 custom characters" on the CSS; the code has 18 non-vanilla panels. Use the code.
9. **Address provenance.** The `scene_curr`/`vs` addresses are confirmed by Remix `Global.asm` (same in both tags). The `SCCommonData.gkind` address (`0x800A4ADF`) is derived from the decomp struct, not from Remix. Verify in-emulator.
10. **Decomp is a PC port.** `BattleShip/decomp` has `#ifdef PORT` branches (e.g. extra `coop_*` fields at the end of `SCCommonData`, `port_fighter_data`). Only the non-PORT N64 layout above is valid for the Remix ROM. The `coop_*` fields do not exist in the Remix ROM.
11. **Not verified:** the Remix stage select and CSS may keep extra per-slot state (e.g. the "bonus" fighters and d-pad-up fighters). Those are not in the VS block and are not needed for a direct start.

## 7. Next steps (not done here)

1. Build the Remix 2.0.0 ROM with the repo's `bass` assembler and `original.xdelta` / `roms/original.z64` to get a symbol map. This gives addresses for the Remix CSS heap loader and `ftManagerSetupFilesAllKind` equivalents for risk 1. It needs the user's US base ROM, so confirm the base MD5 first.
2. Write a headless probe (n64run `--headless`, muted) that logs `current_screen`, `frame_counter`, and the VS block at a fixed frame.
3. Implement the gating test in `qa/` as a rerunnable script.
