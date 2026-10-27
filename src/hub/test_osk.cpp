#include <fstream>
// Console test: on-screen keyboard layout, edit/session logic and the folder browser (scratch dirs only).
#include "osk_logic.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
using namespace osk;
namespace fs = std::filesystem;

static void press_char(Session& s, const char* ch) {
    Key k; k.act = KeyAct::Char; k.label = k.ch = ch;
    s.press(k);
}

int main() {
    // chat-leak guard: a running game or an unfocused hub blocks all UI input; empty / blocked chat is never sent
    CHECK(input_blocked(true, true) && input_blocked(false, false) && input_blocked(true, false) && !input_blocked(false, true));
    for (unsigned m : {1u, 0xFFFu, 1u << 4, 1u << 8}) CHECK(gate_pad_mask(m, true) == 0 && gate_pad_mask(m, false) == m);
    CHECK(!chat_may_send("", false) && !chat_may_send("   	", false) && !chat_may_send("hi", true) && chat_may_send("hi", false));
    {   // injected pad presses while blocked never type into a keyboard session
        Session s; s.start(Kind::Text, "", 100, false, false);
        for (int i = 0; i < 20; ++i) if (gate_pad_mask(1u << 4, input_blocked(true, true))) press_char(s, "1");
        CHECK(s.ed.text.empty());
    }
    // layouts: every page non-empty, valid positions, vertical/horizontal moves stay in range and wrap
    for (Page p : {Page::Lower, Page::Upper, Page::Symbols, Page::Code}) {
        const auto& L = layout(p);
        CHECK(L.size() >= 4);
        for (int r = 0; r < (int)L.size(); ++r)
            for (int c = 0; c < (int)L[r].size(); ++c) {
                for (int d = 0; d < 4; ++d) {
                    Pos q = move(p, Pos{r, c}, d == 0 ? -1 : d == 1 ? 1 : 0, d == 2 ? -1 : d == 3 ? 1 : 0);
                    CHECK(q.r >= 0 && q.r < (int)L.size() && q.c >= 0 && q.c < (int)L[q.r].size());
                }
            }
    }
    // every Latin letter + digit reachable on Lower; Done/Back/Space present
    {
        std::set<std::string> have;
        for (auto& row : layout(Page::Lower)) for (auto& k : row) have.insert(k.label);
        for (char ch = 'a'; ch <= 'z'; ++ch) CHECK(have.count(std::string(1, ch)));
        for (char ch = '0'; ch <= '9'; ++ch) CHECK(have.count(std::string(1, ch)));
        CHECK(have.count("Done") && have.count("Del") && have.count("Space"));
    }
    // code page = exactly the room alphabet (32 keys, no I O 0 1)
    {
        std::string all;
        for (auto& row : layout(Page::Code)) for (auto& k : row) if (k.act == KeyAct::Char) all += k.ch;
        CHECK(all == room_alphabet());
        CHECK(all.size() == 32 && all.find_first_of("IO01") == std::string::npos);
    }
    // vertical move keeps the x position: wide Space key is reached from the middle of the letters
    {
        const auto& L = layout(Page::Lower);
        int last = (int)L.size() - 1;
        Pos q = move(Page::Lower, Pos{last - 1, 4}, 0, 1);
        CHECK(q.r == last && L[last][q.c].act == KeyAct::Space);
        CHECK(move(Page::Lower, Pos{0, 0}, -1, 0).c == (int)L[0].size() - 1);   // wraps
        CHECK(move(Page::Lower, Pos{0, 3}, 0, -1).r == last);                    // wraps vertically
    }
    // edit: insert / cursor / backspace / maxlen
    {
        Edit e; e.maxlen = 5;
        CHECK(e.insert("abcdefg") == 5 && e.text == "abcde" && e.cursor == 5 && e.full());
        e.move_cursor(-2); e.backspace();
        CHECK(e.text == "abde" && e.cursor == 2);
        CHECK(e.insert("X") == 1 && e.text == "abXde");
        e.del(); CHECK(e.text == "abXe");
        e.move_cursor(-10); CHECK(e.cursor == 0);
        e.backspace(); CHECK(e.text == "abXe");
        e.move_cursor(10); CHECK(e.cursor == 4);
        e.clear(); CHECK(e.text.empty() && e.cursor == 0);
        Edit u; u.upper = true; u.no_blank = true;
        u.insert("a b\tc"); CHECK(u.text == "ABC");
        Edit g; g.maxlen = 10;
        g.insert("h\xC3\xA9llo");   // h e-acute l l o
        g.move_cursor(-3); g.backspace();   // removes the whole e-acute
        CHECK(g.text == "hllo");
    }
    // room kind: 6-char quick entry, auto done, paste normalisation
    {
        CHECK(normalize_room_paste(" ab-c d2e3x ", 6) == "ABCD2E");
        CHECK(normalize_room_paste("io01", 6).empty());
        Session s;
        s.start(Kind::Room, "", 6, true, true);
        CHECK(s.page == Page::Code && s.auto_done == 6);
        s.type_chars("abc23");
        CHECK(!s.done && s.ed.text == "ABC23");
        s.type_chars("k");
        CHECK(s.done && s.ed.text == "ABC23K");
        Session p;
        p.start(Kind::Room, "", 6, true, true);
        p.clipboard = "code: kp7-m2q";
        Key pk; pk.act = KeyAct::Paste;
        p.press(pk);
        CHECK(p.ed.text == "CODEKP" || p.ed.text.size() == 6);
        CHECK(p.done);
        Session b;
        b.start(Kind::Room, "ab", 6, true, true);
        CHECK(b.ed.text == "AB");
        b.type_chars("0 1 o i");   // invalid characters are dropped
        CHECK(b.ed.text == "AB");
    }
    // text session: shift is one-shot, Y cycles pages, symbols toggle
    {
        Session s;
        s.start(Kind::Text, "", 20, false, false);
        CHECK(s.page == Page::Lower);
        Key sh; sh.act = KeyAct::Shift;
        s.press(sh); CHECK(s.page == Page::Upper);
        press_char(s, "H"); CHECK(s.page == Page::Lower);
        press_char(s, "i");
        CHECK(s.ed.text == "Hi");
        s.shift_cycle(); CHECK(s.page == Page::Upper);
        s.shift_cycle(); CHECK(s.page == Page::Symbols);
        s.shift_cycle(); CHECK(s.page == Page::Lower);
        Key sy; sy.act = KeyAct::Symbols;
        s.press(sy); CHECK(s.page == Page::Symbols);
        s.press(sy); CHECK(s.page == Page::Lower);
        Key sp; sp.act = KeyAct::Space;
        s.press(sp); CHECK(s.ed.text == "Hi ");
        Key dn; dn.act = KeyAct::Done;
        s.press(dn); CHECK(s.done && !s.cancelled);
        Session c;
        c.start(Kind::Code, "rh-", 12, true, false);
        CHECK(c.ed.text == "RH-" && c.page == Page::Upper);
        c.type_chars("ab"); CHECK(c.ed.text == "RH-AB" && !c.done);
    }
    // key_under always valid after page switches
    {
        Session s;
        s.start(Kind::Text, "", 8, false, false);
        s.pos = Pos{4, 5};
        s.set_page(Page::Symbols);
        CHECK(s.key_under().w > 0);
    }
    // parent_of
    CHECK(parent_of("C:\\Users\\bob") == "C:\\Users");
    CHECK(parent_of("C:\\Users") == "C:\\");
    CHECK(parent_of("C:\\").empty());
    CHECK(parent_of("C:\\Users\\bob\\").size() == 8);
    CHECK(parent_of("").empty());
    // should_open
    CHECK(should_open(true, true, false));
    CHECK(should_open(false, false, false));
    CHECK(!should_open(false, true, false));
    CHECK(!should_open(true, true, true));
    CHECK(!should_open(false, false, true));
    // folder browser on a scratch tree
    {
        fs::path root = fs::temp_directory_path() / "osk_test_tree";
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / "Bravo" / "inner", ec);
        fs::create_directories(root / "alpha", ec);
        fs::create_directories(root / ".hidden", ec);
        FolderBrowser fb;
        fb.open(root.string());
        CHECK(fb.cwd == root.string());
        CHECK(fb.entries.size() == 4);   // select, up, alpha, Bravo
        CHECK(fb.entries[0].type == FolderEntry::Select && fb.entries[1].type == FolderEntry::Up);
        CHECK(fb.entries[2].name == "alpha" && fb.entries[3].name == "Bravo");
        fb.move(3);   // Bravo
        CHECK(fb.sel == 3);
        fb.activate();
        CHECK(fb.cwd.find("Bravo") != std::string::npos && fb.entries.size() == 3);
        fb.sel = 0; fb.activate();   // select this folder
        CHECK(fb.done && fb.result.find("Bravo") != std::string::npos);
        FolderBrowser f2;
        f2.open(root.string());
        f2.up();
        CHECK(f2.cwd == parent_of(root.string()));
        f2.open("");   // drive list
        CHECK(f2.cwd.empty() && !f2.entries.empty());
        f2.up(); CHECK(f2.cancelled);
        FolderBrowser f3;
        f3.open((root / "alpha").string());
        f3.choose_here();
        CHECK(f3.done && f3.result == (root / "alpha").string());
        FolderBrowser f4;
        f4.open("Z:\\definitely\\missing");
        CHECK(f4.cwd.empty());
        // file mode
        { std::ofstream(root / "acct.JSON") << "{}"; std::ofstream(root / "b.zip") << "z"; std::ofstream(root / "c.json") << "{}"; }
        FolderBrowser f5;
        f5.pick_files = true;
        f5.filters = {".json"};
        f5.open(root.string());
        CHECK(f5.entries[0].type == FolderEntry::Up);   // no "Select this folder"
        int nfile = 0;
        for (auto& e : f5.entries) if (e.type == FolderEntry::File) ++nfile;
        CHECK(nfile == 2);                              // acct.JSON + c.json, not b.zip
        CHECK(f5.entries.back().type == FolderEntry::File && f5.entries[f5.entries.size() - 3].type == FolderEntry::Dir);   // dirs first
        f5.choose_here(); CHECK(!f5.done);
        f5.sel = (int)f5.entries.size() - 1; f5.activate();
        CHECK(f5.done && f5.result.find("c.json") != std::string::npos);
        fs::remove_all(root, ec);
    }
    if (fails) { std::printf("%d failure(s)\n", fails); return 1; }
    std::printf("osk tests passed\n");
    return 0;
}
