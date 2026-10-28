// Pure-logic tests for GamePack (gamepack.h): Drive listing HTML parse, entity decode, safe names. No network, no files.
#include "gamepack.h"

#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

using namespace hubgp;

int main()
{
    /* decode_entities */
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

    /* console_code: the pack subfolder names */
    CHECK(console_code("n64") && console_code("GBC") && console_code("3ds"));
    CHECK(!console_code("gbx") && !console_code("") && !console_code("nintendo 64"));

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
