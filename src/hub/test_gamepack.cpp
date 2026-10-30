// Pure-logic tests for GamePack (gamepack.h): Drive listing HTML parse, entity decode, safe names. No network, no files.
#include "gamepack.h"

#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

using namespace hubgp;

int main()
{
    /* decode_entities */
    {   // pack_pick: exact first, then a single loose match; numbers must agree
        auto K = [](std::initializer_list<const char*> l) { std::vector<std::string> v; for (const char* x : l) v.push_back(pack_key(x)); return v; };
        std::vector<std::string> n64 = K({"007 - GoldenEye", "Mario Golf 64 (English)", "Smash_Remix_2.0.0", "The Legend of Zelda Ocarina of Time Master Quest", "Mario Party 2 (U) [!]", "Mario Party 3 (U) [!]"});
        CHECK(pack_pick(n64, pack_key("GoldenEye 007")) == 0);
        CHECK(pack_pick(n64, pack_key("Mario Golf")) == 1);
        CHECK(pack_pick(n64, pack_key("Smash Remix 2.0")) == 2);
        CHECK(pack_pick(n64, pack_key("Zelda: Ocarina of Time Master Quest")) == 3);
        CHECK(pack_pick(n64, pack_key("Mario Party")) == -1);      // 2 and 3 differ by a number: no guess
        CHECK(pack_pick(n64, pack_key("Mario Party 3")) == 5);
        std::vector<std::string> nds = K({"Pokemon - Black Version (USA, Europe)", "Pokemon - Black Version 2 (USA, Europe)"});
        CHECK(pack_pick(nds, pack_key("Pokemon Black")) == 0 && pack_pick(nds, pack_key("Pokemon Black 2")) == 1);
        std::vector<std::string> gc = K({"Legend of Zelda, The - The Wind Waker (USA, Canada)", "Legend of Zelda, The - Twilight Princess (USA)"});
        CHECK(pack_pick(gc, pack_key("Zelda: The Wind Waker")) == 0);
        std::vector<std::string> gbc = K({"Pokemon - Yellow Version - Special Pikachu Edition (USA, Europe)", "Pokemon - Red Version (USA, Europe)"});
        CHECK(pack_pick(gbc, pack_key("Pokemon Yellow")) == 0 && pack_pick(gbc, pack_key("Pokemon Red")) == 1);
        CHECK(pack_pick(gbc, pack_key("Tetris")) == -1);
    }
    CHECK(console_alias("Wii u") == "wiiu");
    CHECK(console_alias("GameCube") == "gc");
    CHECK(console_alias("SNES") == "snes");
    CHECK(console_alias("NES") == "nes");
    CHECK(console_alias("PS2") == "ps2");
    CHECK(console_alias("3ds") == "3ds");
    CHECK(console_alias("NDS") == "nds");
    CHECK(console_alias("Tools") == "");
    CHECK(pack_key("The Legend of Zelda: A Link to the Past") == pack_key("Legend of Zelda, The - A Link to the Past (USA)"));
    CHECK(pack_key("Pokemon X") == pack_key("Pokemon X (USA) (En,Ja,Fr,De,Es,It,Ko)"));
    CHECK(pack_key("Pokemon Platinum") == pack_key("Pokemon - Platinum Version (USA)"));
    CHECK(pack_key("Pokemon Black 2") == pack_key("Pokemon - Black Version 2 (USA, Europe) (NDSi Enhanced)"));
    CHECK(pack_key("Pokemon Black") != pack_key("Pokemon - Black Version 2 (USA, Europe)"));
    CHECK(pack_key("Pok\xc3\xa9" "mon Pearl") == pack_key("Pokemon - Pearl Version (USA) (Rev 5)"));
    CHECK(pack_key("Super Mario 3D World") == pack_key("Super Mario 3D World (USA) (En,Fr,Es) (Rev 2)"));
    CHECK(decode_entities("plain") == "plain");
    CHECK(decode_entities("") == "");
    CHECK(decode_entities("Yoshi&#39;s Story (U) [!]") == "Yoshi's Story (U) [!]");
    CHECK(decode_entities("a &amp; b &lt;c&gt; &quot;d&quot;") == "a & b <c> \"d\"");
    CHECK(decode_entities("&#x42;&#65;") == "BA");   // hex 0x42 = 'B', decimal 65 = 'A'
    CHECK(decode_entities("&unknown; &amp x") == "&unknown; &amp x");   // unknown / unterminated: left as-is
    CHECK(decode_entities("a & b") == "a & b");

    /* parse_folder_html: the embeddedfolderview layout (folders + files, entity-encoded titles) */
    const char* html =
        "<html><body class=\"flip-embedded\"><div class=\"flip-entries\">"
        "<div class=\"flip-entry\" id=\"entry-1FolderA\" tabindex=\"0\" role=\"link\"><div class=\"flip-entry-info\">"
        "<a href=\"https://drive.google.com/drive/folders/1FolderA\" target=\"_blank\"><div class=\"flip-entry-visual\">"
        "<div class=\"flip-entry-icon\"></div></div><div class=\"flip-entry-title\">007 - GoldenEye</div></a>"
        "</div><div class=\"flip-entry-last-modified\"><div>11:28 am</div></div></div>"
        "<div class=\"flip-entry\" id=\"entry-1FileB\" tabindex=\"0\" role=\"link\"><div class=\"flip-entry-info\">"
        "<a href=\"https://drive.google.com/file/d/1FileB/view?usp=drive_web\" target=\"_blank\"><div class=\"flip-entry-visual\">"
        "<div class=\"flip-entry-icon\"></div></div><div class=\"flip-entry-title\">Yoshi&#39;s Story (U) [!].z64</div></a>"
        "</div><div class=\"flip-entry-last-modified\"><div>11:30 am</div></div></div>"
        "</div></body></html>";
    auto v = parse_folder_html(html);
    CHECK(v.size() == 2);
    if (v.size() == 2) {
        CHECK(v[0].id == "1FolderA" && v[0].name == "007 - GoldenEye" && v[0].is_folder);
        CHECK(v[1].id == "1FileB" && v[1].name == "Yoshi's Story (U) [!].z64" && !v[1].is_folder);
    }
    CHECK(parse_folder_html("").empty());
    CHECK(parse_folder_html("<div class=\"flip-entry\" id=\"entry-1X\"><div class=\"flip-entry-title\">NoLink</div></div>").empty());   // no link: unusable

    /* safe_name */
    CHECK(safe_name("Mario Kart 64 (U) [!].z64") == "Mario Kart 64 (U) [!].z64");
    CHECK(safe_name("a<b>c:d\"e/f\\g|h?i*j") == "a b c d e f g h i j");
    CHECK(safe_name("  ..name..  ") == "name");
    CHECK(safe_name("<>") == "game");
    CHECK(safe_name("NUL.gba") == "_NUL.gba");
    CHECK(safe_name("com1") == "_com1");
    CHECK(safe_name("Console.gba") == "Console.gba");

    /* parse_links: Drive URLs (folders/, old folder/, embeddedfolderview), bare ids, dedupe */
    CHECK(parse_links("").empty());
    {
        auto v = parse_links("https://drive.google.com/drive/folders/1abcDEF_-xyz098765432?usp=sharing");
        CHECK(v.size() == 1 && v[0] == "1abcDEF_-xyz098765432");
    }
    {
        auto v = parse_links("https://drive.google.com/drive/folder/1OldStyleFolderId123456\nhttps://drive.google.com/embeddedfolderview?id=1EmbeddedFolderId12345");
        CHECK(v.size() == 2 && v[0] == "1OldStyleFolderId123456" && v[1] == "1EmbeddedFolderId12345");
    }
    {   // bare ids (comma / space separated); short tokens and URL words are ignored
        auto v = parse_links("1BareFolderIdOne123456, 1BareFolderIdTwo123456 and short");
        CHECK(v.size() == 2 && v[0] == "1BareFolderIdOne123456" && v[1] == "1BareFolderIdTwo123456");
    }
    {   // the same id via URL and bare collapses to one; "embeddedfolderview" (a URL word) is not an id
        auto v = parse_links("https://drive.google.com/embeddedfolderview?id=1SameFolderIdHere123456 and 1SameFolderIdHere123456");
        CHECK(v.size() == 1 && v[0] == "1SameFolderIdHere123456");
    }

    /* stored pack_links helpers */
    {
        std::string s = "1OldCommaIdOne123456,1OldCommaIdTwo123456  1OldCommaIdOne123456";
        auto v = links_split(s);
        CHECK(v.size() == 2 && links_join(v) == "1OldCommaIdOne123456 1OldCommaIdTwo123456");
        int found = -1;
        CHECK(links_add(&s, "nothing here", &found) == 0 && found == 0);
        CHECK(links_add(&s, "https://drive.google.com/drive/folders/1OldCommaIdTwo123456?usp=sharing", &found) == 0 && found == 1);
        CHECK(links_add(&s, "https://drive.google.com/drive/folders/1NewFolderIdAAA123456 https://drive.google.com/drive/folders/1NewFolderIdBBB123456", &found) == 2 && found == 2);
        CHECK(links_split(s).size() == 4 && s == "1OldCommaIdOne123456 1OldCommaIdTwo123456 1NewFolderIdAAA123456 1NewFolderIdBBB123456");
        CHECK(links_remove(&s, "1OldCommaIdTwo123456") && links_split(s).size() == 3);
        CHECK(!links_remove(&s, "1NotThereAtAll123456"));
        std::string e; CHECK(links_split(e).empty() && links_join({}).empty());
        CHECK(link_label("short_id_12345") == "short_id_12345");
        CHECK(link_label("1NewFolderIdAAA1234567890") == "1NewFolder...567890");
    }

    /* parse_packs_json: the shared community list (data/packs.json) */
    {   // object form; names ignored, duplicates dropped
        std::vector<std::string> v;
        CHECK(parse_packs_json("{\"packs\":[{\"id\":\"1CommunityPackOne123456\",\"name\":\"Brendan\"},{\"id\":\"1CommunityPackOne123456\",\"name\":\"again\"},{\"id\":\"1CommunityPackTwo123456\"}]}", &v));
        CHECK(v.size() == 2 && v[0] == "1CommunityPackOne123456" && v[1] == "1CommunityPackTwo123456");
    }
    {   // bare array of ids; bad ids dropped, the good ones kept
        std::vector<std::string> v;
        CHECK(parse_packs_json("[\"1GoodPackIdHere123456\", \"bad id with spaces\", \"\", 7, {\"id\":\"1AnotherPackId123456\"}]", &v));
        CHECK(v.size() == 2 && v[0] == "1GoodPackIdHere123456" && v[1] == "1AnotherPackId123456");
    }
    {   // empty list parses to nothing; wrong shapes and bad JSON fail
        std::vector<std::string> v;
        CHECK(parse_packs_json("{\"packs\":[]}", &v) && v.empty());
        CHECK(!parse_packs_json("{\"other\":[1]}", &v));
        CHECK(!parse_packs_json("42", &v));
        CHECK(!parse_packs_json("{nope}", &v));
    }

    /* console_code: the pack subfolder names */
    CHECK(console_code("n64") && console_code("GBC") && console_code("3ds"));
    CHECK(!console_code("gbx") && !console_code("") && console_code("nintendo 64"));

    /* display_title */
    CHECK(display_title("Legend of Zelda, The - A Link to the Past (USA).zip") == "The Legend of Zelda - A Link to the Past");
    CHECK(display_title("Smash_Remix_2.0.0") == "Smash Remix 2.0.0");
    CHECK(display_title("Pokemon - Platinum Version (USA) [!]") == "Pokemon - Platinum Version");
    CHECK(display_title("  Mario   Golf 64 (English).z64") == "Mario Golf 64");
    CHECK(display_title("Hunter, Theodore") == "Hunter, Theodore");
    CHECK(display_title("(USA)") == "");
    /* pack_hint: only the game-file extension goes; region tags stay (they are part of the No-Intro / Redump name) */
    CHECK(pack_hint("Legend of Zelda, The - A Link to the Past (USA).zip") == "Legend of Zelda, The - A Link to the Past (USA)");
    CHECK(pack_hint("Pikmin.zip") == "Pikmin");
    CHECK(pack_hint("Smash_Remix_2.0.0") == "Smash_Remix_2.0.0");
    CHECK(pack_hint("Super Mario Sunshine (USA, Canada).7z") == "Super Mario Sunshine (USA, Canada)");
    {   // the hint rides along from the index to the ghost
        PackGame pg{"gc", "Pikmin", pack_key("Pikmin"), "Pikmin (USA)"};
        auto g = pack_only_ghosts({}, {}, {pg}, 10);
        CHECK(g.size() == 1 && g[0].hint == "Pikmin (USA)");
        index_test_set({pg});
        auto ig = index_games();
        CHECK(ig.size() == 1 && ig[0].hint == "Pikmin (USA)");
    }

    /* pack_only_ghosts: owned / friend ghost / twin / pack duplicates are not shown again */
    {
        using hubui::fg::Owned; using hubui::fg::Ghost;
        auto P = [](const char* sys, const char* t) { return PackGame{sys, t, pack_key(t)}; };
        std::vector<Owned> mine = {{"n64", "n64:a", "", "GoldenEye 007"}, {"gb", "gb:1", "", "Pokemon Red"}};
        Ghost fg; fg.id = "n64|t:mario kart 64"; fg.sys = "n64"; fg.title = "Mario Kart 64"; fg.uids = {"u1"};
        std::vector<PackGame> pack = {P("n64", "007 - GoldenEye"), P("n64", "Mario Kart 64"), P("n64", "Mario Golf 64"),
                                      P("gbc", "Pokemon - Red Version"), P("gbc", "Pokemon Gold"), P("gb", "Pokemon Gold"), P("gba", "Metroid Fusion")};
        auto g = pack_only_ghosts(mine, {fg}, pack, 100);
        CHECK(g.size() == 3);
        if (g.size() == 3) {
            CHECK(g[0].title == "Mario Golf 64" && g[0].uids.empty() && g[0].sys == "n64");
            CHECK(g[1].title == "Metroid Fusion" && g[2].title == "Pokemon Gold");
        }
        CHECK(pack_only_ghosts(mine, {fg}, pack, 2).size() == 2);   // room cap
        CHECK(pack_only_ghosts({}, {}, {}, 10).empty());
    }

    /* installed_folder_title: the pack title an installed file belongs to (the library title of "game.iso" can differ from it) */
    {
        const std::string r = "C:\\Users\\a\\RetroHub\\userdata\\gamepack\\";
        CHECK(installed_folder_title(r + "gc\\Gauntlet - Dark Legacy\\Gauntlet - Dark Legacy (USA).7z", r) == "Gauntlet - Dark Legacy");
        CHECK(installed_folder_title("c:/users/A/retrohub/USERDATA/gamepack/ps2/Foo/sub/x.iso", r) == "Foo");   // case + slash insensitive, nested file
        CHECK(installed_folder_title(r + "gc\\a.7z", r).empty());          // loose file in a console folder: not an installed game
        CHECK(installed_folder_title(r + "gc", r).empty() && installed_folder_title(r, r).empty());
        CHECK(installed_folder_title("D:\\Games\\gc\\Foo\\a.7z", r).empty());   // outside the pack folder
        CHECK(installed_folder_title(r + "gc\\Foo\\a.7z", "").empty());
        CHECK(installed_folder_title(r + "gc\\Foo\\a.7z", r.substr(0, r.size() - 1)) == "Foo");   // root without the trailing slash
        /* the title mismatch it repairs: pack title vs the inner file name / header title */
        CHECK(pack_key("Gauntlet - Dark Legacy") != pack_key("game"));
        CHECK(pack_key("Animal Crossing (USA, Canada)") == pack_key("Animal Crossing (USA, Canada)"));
    }

    /* sys_folder: the consoles in the shared pack */
    CHECK(std::string(sys_folder("n64")) == "19_2xjH7Ps7epabxUrr9sfxz69kuJGp3W");
    CHECK(std::string(sys_folder("gc")) == "17AvjquyVOjSs0ua1LnLZGRN3B2bUYQar");
    CHECK(std::string(sys_folder("gba")) == "10zAwtXq1STlpfdIpCDppwXwpKhK3gyRP");
    CHECK(std::string(sys_folder("gbc")) == "1idefowNapO3KpwE4nRv8zqrpCeEZFgpc");
    CHECK(std::string(sys_folder("nds")) == "1FVsjeQMBAcxw-_gRU4MSSRLQpIOD3ygu");
    CHECK(std::string(sys_folder("ps1")) == "19H5x-ZY6-8HHUl0g3XI0NEsW2e6ROWzr");
    CHECK(sys_folder("gb")[0] == '\0');
    CHECK(sys_folder("wiiu")[0] == '\0');

    if (fails) { std::printf("%d failure(s)\n", fails); return 1; }
    std::printf("gamepack: ok\n");
    return 0;
}
