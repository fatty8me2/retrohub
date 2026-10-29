// Pure-logic tests for GamePack (gamepack.h): Drive listing HTML parse, entity decode, safe names. No network, no files.
#include "gamepack.h"

#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

using namespace hubgp;

int main()
{
    /* decode_entities */
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
