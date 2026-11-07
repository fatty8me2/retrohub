// Pure-logic tests for GamePack (gamepack.h): Drive listing HTML parse, entity decode, safe names. No network, no files.
#include "gamepack.h"

#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

using namespace hubgp;

int main()
{
    /* decode_entities */
    {   // the closest loose match wins when a shorter owned title also fits (Master Quest vs Ocarina of Time)
        auto K2 = [](std::initializer_list<const char*> l) { std::vector<std::string> v; for (const char* x : l) v.push_back(pack_key(x)); return v; };
        std::vector<std::string> own = K2({"The Legend of Zelda: Ocarina of Time", "Zelda: Ocarina of Time Master Quest", "Mario Kart 64"});
        CHECK(pack_pick(own, pack_key("The Legend of Zelda Ocarina of Time Master Quest")) == 1);
        CHECK(pack_pick(own, pack_key("Legend of Zelda, The - Ocarina of Time (USA)")) == 0);
        std::vector<std::string> tie = K2({"Zelda Ocarina Time", "Ocarina of Time Zelda Link"});
        CHECK(pack_pick(tie, pack_key("Legend of Zelda Ocarina of Time Link Zelda Time")) != -2);   // does not crash; ties are decided by overlap only
        // and the pack ghost of a game I own under the shorter spelling is not shown
        std::vector<hubui::fg::Owned> mine = {{"n64", "n64:oot", "oot", "The Legend of Zelda: Ocarina of Time"}, {"n64", "n64:mq", "ootmq", "Zelda: Ocarina of Time Master Quest"}};
        PackGame pg1{"n64", "The Legend of Zelda Ocarina of Time Master Quest", pack_key("The Legend of Zelda Ocarina of Time Master Quest"), "The Legend of Zelda Ocarina of Time Master Quest"};
        CHECK(pack_only_ghosts(mine, {}, {pg1}, 10).empty());
    }
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
    CHECK(console_alias("Switch") == "switch");
    CHECK(console_alias("Nintendo Switch") == "switch");
    CHECK(console_alias("Switch 2") == "switch");
    CHECK(console_alias("NX") == "switch");
    CHECK(console_alias("NX2") == "switch");
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

    /* Hotfix: a friend's DS copy titled by its ROM header ("Zelda_DS:PH") and the pack's "The Legend of Zelda - Phantom Hourglass"
     * are one game; the known No-Intro name for the key joins them, so the tile shows once (friend ghost, no pack twin). Same for an owned copy. */
    {
        using hubui::fg::Owned; using hubnet::LibGame;
        std::vector<LibGame> fl = {{"nds", "nds:azee-004d56", "", "Zelda_DS:PH"}};
        auto nm = [](const std::string&, const std::string& k) { return k == "nds:azee-004d56" ? std::string("Legend of Zelda, The - Phantom Hourglass (USA) (En,Fr,Es)") : std::string(); };
        std::vector<PackGame> pk = {PackGame{"nds", "The Legend of Zelda - Phantom Hourglass", pack_key("The Legend of Zelda - Phantom Hourglass")}};
        auto before = hubui::fg::build_ghosts({}, {{"u1", &fl}});                // without the name: the bogus header title
        CHECK(before.size() == 1 && pack_only_ghosts({}, before, pk, 10).size() == 1);   // the bug: two tiles
        auto gh = hubui::fg::build_ghosts({}, {{"u1", &fl}}, nm);
        CHECK(gh.size() == 1 && gh[0].title == "The Legend of Zelda - Phantom Hourglass");
        CHECK(pack_only_ghosts({}, gh, pk, 10).empty());                         // one tile
        std::vector<Owned> mine = {{"nds", "nds:azee-004d56", "", "Zelda_DS:PH"}, {"nds", "nds:azee-004d56", "", hubui::fg::clean_dat_name(nm("", "nds:azee-004d56"))}};
        CHECK(pack_only_ghosts(mine, {}, pk, 10).empty());                       // owned: no pack ghost
        CHECK(hubui::fg::build_ghosts(mine, {{"u1", &fl}}, nm).empty());         // owned: no friend ghost
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

    {   /* install UX helpers: Drive's big-file page, ranges, plain messages, sizes / speed / time */
        const char* page = "<html><body><form id=\"download-form\" action=\"https://drive.usercontent.google.com/download\" method=\"get\">"
                           "<input type=\"hidden\" name=\"id\" value=\"1AbC-d_e\"><input type=\"hidden\" name=\"export\" value=\"download\">"
                           "<input type=\"hidden\" name=\"confirm\" value=\"t\"><input type=\"hidden\" name=\"uuid\" value=\"a1b2&amp;c3\">"
                           "<input type=\"submit\" value=\"Download anyway\"></form><p>Google Drive can't scan this file for viruses.</p></body></html>";
        std::string pq;
        CHECK(parse_confirm_form(page, &pq));
        CHECK(pq == "/download?id=1AbC-d_e&export=download&confirm=t&uuid=a1b2%26c3");
        CHECK(drive_html_kind(page) == "confirm");
        CHECK(!parse_confirm_form("<html>no form</html>", &pq));
        CHECK(!parse_confirm_form("<form action=\"/x\"><input name=a value=b></form>", &pq));
        CHECK(drive_html_kind("<html><title>Quota exceeded</title>Sorry, you can't view or download this file at this time. Too many users have viewed or downloaded this file recently.</html>") == "quota");
        CHECK(drive_html_kind("<a href=\"https://accounts.google.com/ServiceLogin?x\">Sign in</a>") == "signin");
        CHECK(drive_html_kind("<html>hello</html>").empty());
        uint64_t a = 0, b = 0, t = 0;
        CHECK(parse_content_range("bytes 100-199/1000", &a, &b, &t) && a == 100 && b == 199 && t == 1000);
        CHECK(parse_content_range("bytes 0-9/*", &a, &b, &t) && t == 0 && b == 9);
        CHECK(!parse_content_range("garbage", &a, &b, &t));
        CHECK(!parse_content_range("bytes 9-1/5", &a, &b, &t));
        CHECK(friendly_error("download interrupted").find("connection") != std::string::npos);
        CHECK(friendly_error("server answered 404").find("removed") != std::string::npos);
        CHECK(friendly_error("server answered 503").find("trouble") != std::string::npos);
        CHECK(friendly_error("quota").find("busy") != std::string::npos);
        CHECK(friendly_error("download interrupted").find("Google") == std::string::npos && friendly_error("could not reach Google Drive").find("Google") == std::string::npos);
        CHECK(friendly_error("cancelled") == "Cancelled.");
        CHECK(friendly_error("Not enough free disk space: this game needs about 2.0 GB and the drive has 1.0 GB free.").rfind("Not enough", 0) == 0);
        CHECK(friendly_error("weird thing").find("weird thing") != std::string::npos);
        CHECK(friendly_error("").size() > 5);
        CHECK(fmt_size(0) == "0 KB" && fmt_size(2048) == "2 KB" && fmt_size(5u << 20) == "5.0 MB" && fmt_size(300ull << 20) == "300 MB" && fmt_size(3ull << 29) == "1.5 GB");
        CHECK(fmt_rate(0).empty() && fmt_rate(2048) == "2 KB/s" && fmt_rate(5.5 * 1048576) == "5.5 MB/s");
        CHECK(fmt_eta(0).empty() && fmt_eta(2) == "almost done" && fmt_eta(40) == "about 40 sec left" && fmt_eta(600) == "about 10 min left" && fmt_eta(3900) == "1 h 5 min left");
        SpeedMeter m;
        CHECK(m.rate() == 0);
        m.add(0.0, 0); m.add(1.0, 1000000); m.add(2.0, 2000000);
        CHECK(m.rate() > 900000 && m.rate() < 1100000);
        m.add(2.1, 2100000);   // too soon: ignored
        CHECK(m.rate() > 900000 && m.rate() < 1100000);
        m.add(3.0, 0);         // counter went back (new file): restarts
        CHECK(m.rate() == 0);
        CHECK(install_dir("gc", "Foo: Bar").find("gamepack") != std::string::npos);
        {   // a game filed under another console than its pack folder keeps its folder identity
            const std::string root = "C:/data/gamepack";
            CHECK(installed_folder_console("C:/data/gamepack/gc/Gauntlet/disc.7z", root) == "gc");
            CHECK(installed_folder_console("c:\\DATA\\gamepack\\ps2\\Foo\\a.iso", root) == "ps2");
            CHECK(installed_folder_console("C:/elsewhere/x.iso", root).empty());
            CHECK(installed_folder_console("C:/data/gamepack/gc/loose.iso", root).empty());
            hub::GameEntry e;
            e.system = hub::System::PS2; e.key = "ps2:slus-20047"; e.title = "Gauntlet - Dark Legacy"; e.path = "C:/data/gamepack/gc/Gauntlet - Dark Legacy/x.7z";
            auto al = pack_owned_aliases(e, root);
            CHECK(al.size() == 1 && al[0].sys == "gc" && al[0].title == "Gauntlet - Dark Legacy" && al[0].key == e.key);
            e.path = "C:/games/x.7z";
            CHECK(pack_owned_aliases(e, root).empty());
            // that alias is what clears the greyed gc entry when the game was identified as PS2
            std::vector<hubui::fg::Owned> mine = {{"ps2", e.key, "", "Gauntlet - Dark Legacy"}};
            std::vector<PackGame> pk = {{"gc", "Gauntlet - Dark Legacy", pack_key("Gauntlet - Dark Legacy"), ""}};
            CHECK(pack_only_ghosts(mine, {}, pk, 10).size() == 1);   // without the alias the gc ghost would stay
            mine.push_back(al.empty() ? hubui::fg::Owned{"gc", e.key, "", "Gauntlet - Dark Legacy"} : al[0]);
            CHECK(pack_only_ghosts(mine, {}, pk, 10).empty());
        }
        CHECK(staging_root().find("gamepack-tmp") != std::string::npos && staging_root().find("gamepack\\") == std::string::npos);
    }

    if (fails) { std::printf("%d failure(s)\n", fails); return 1; }
    std::printf("gamepack: ok\n");
    return 0;
}
