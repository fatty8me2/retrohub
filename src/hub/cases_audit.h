#pragma once
/* Cover-art coverage audit for the Cases view (art lane "artcov").
 *
 * HUB_CASES_AUDIT=<out.tsv> (headless runs only, i.e. with HUB_SCREENSHOT set): once art loading has settled the Cases view writes one row
 * per entry it shows (the owned games, then the friend / pack ghosts) and quits. The row building and the TSV format are pure (tested by
 * hub_art_test); Auditor drives hubart for every entry exactly like the carousel does (front cover, full cover scan) and reads the result.
 *
 * Columns: index section sys title key catalog_id front full spine back art_name note
 *   section  own | friend | pack
 *   front    scan (taken from the full cover scan) | GameTDB | libretro | icon (Switch package icon) | GENERATED | PENDING
 *   full     yes | no | n/a (the console has no scan source)
 *   spine / back   real (cut from the scan) | generated
 *
 * HUB_CASES_AUDIT_FRIENDS=<friend_libs.json> (headless only) feeds the ghost builder from a copy of a friends cache instead of the network. */

#include "art.h"
#include "friend_games.h"
#include "library.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace hubui {
namespace cases_audit {

struct Item {
    std::string section;   // own | friend | pack
    std::string sys, title, key, cid, hint;
    bool ghost = false;
    hub::GameEntry entry;  // !ghost
};

struct Row {
    int index = 0;
    std::string section, sys, title, key, cid;
    std::string front = "PENDING", full = "no", spine = "generated", back = "generated", art_name, note;
};

/* "gametdb:ALZE01:US" -> GameTDB, "nupicon:<id>" -> icon, anything else (a No-Intro / Redump name) -> libretro, "" -> "". */
std::string source_of_art_name(const std::string& art_name);
/* Whether a console has a full cover scan source (GameTDB: GameCube, Wii U, DS, 3DS). */
bool console_has_scan(const std::string& sys_short);
/* One row from what the front / full slots hold. A full cover that loaded gives a real front (scan), spine and back. */
Row make_row(int index, const Item& it, const hubart::SlotInfo& front, const hubart::SlotInfo& full);
std::string tsv_header();
std::string tsv_row(const Row& r);   // tabs / newlines inside fields are folded to spaces
/* Totals line: "total=87 own=70 friend=16 pack=1 front: scan=.. GameTDB=.. libretro=.. icon=.. GENERATED=.. PENDING=.. | full yes=.. no=.. | spine real=.. | back real=.." */
std::string summary(const std::vector<Row>& rows);

/* The Item list for the Cases view: A.lib (hacks included), then the ghosts (friend first, pack last: uids empty). */
std::vector<Item> items_for(const std::vector<hub::GameEntry>& lib, const std::vector<fg::Ghost>& ghosts);

/* Friend libraries from a friends cache file ({"libs":{"<uid>":{"ver":N,"games":[{"s","k","c","t"}]}}}), friend order = file order. */
std::vector<std::pair<std::string, std::vector<hubnet::LibGame>>> parse_friend_libs(const std::string& json_text);
/* One known pack game (hubgp::PackGame without the dependency), as a tab separated line: sys title key hint. */
struct PackRow { std::string sys, title, key, hint; };
std::vector<PackRow> parse_pack_tsv(const std::string& text);
std::string pack_tsv(const std::vector<PackRow>& rows);
/* Headless injection of the pack index for rebuild_ghosts(): the rows of HUB_CASES_AUDIT_PACK=<tsv> (empty when unset / not headless). */
const std::vector<PackRow>& injected_pack();
/* Headless injection for rebuild_ghosts(): the libs of HUB_CASES_AUDIT_FRIENDS (empty when unset / not headless). Stable pointers. */
const std::vector<std::pair<std::string, const std::vector<hubnet::LibGame>*>>& injected_libs();

/* Called every frame from draw_cases() in headless runs: does nothing unless HUB_CASES_AUDIT is set. */
void tick(const std::vector<hub::GameEntry>& lib, const std::vector<fg::Ghost>& ghosts, bool scanning = false);

} // namespace cases_audit
} // namespace hubui
