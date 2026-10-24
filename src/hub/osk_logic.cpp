#include "osk_logic.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace osk {
namespace fs = std::filesystem;

static Key C(const char* s, float w = 1.f) { Key k; k.act = KeyAct::Char; k.label = s; k.ch = s; k.w = w; return k; }
static Key A(KeyAct a, const char* label, float w) { Key k; k.act = a; k.label = label; k.w = w; return k; }
static Row chars(const char* s) {
    Row r;
    for (const char* p = s; *p; ++p) { char b[2] = {*p, 0}; r.push_back(C(b)); }
    return r;
}
static Row cat(Row a, const Row& b) { a.insert(a.end(), b.begin(), b.end()); return a; }
static Row one(Key k) { return Row{k}; }

static std::vector<Row> build(Page p) {
    std::vector<Row> L;
    if (p == Page::Code) {
        const char* al = room_alphabet();   // 32 characters, 4 rows of 8
        std::string s = al;
        for (int r = 0; r < 4; ++r) L.push_back(chars(s.substr(r * 8, 8).c_str()));
        L.push_back(Row{A(KeyAct::Back, "Del", 2), A(KeyAct::Clear, "Clear", 2), A(KeyAct::Paste, "Paste", 2), A(KeyAct::Done, "Done", 2)});
        return L;
    }
    const char* nums = "1234567890";
    L.push_back(cat(chars(nums), one(A(KeyAct::Back, "Del", 1.6f))));
    if (p == Page::Symbols) {
        L.push_back(cat(chars("!@#$%^&*()"), one(C("~", 1.f))));
        L.push_back(cat(chars("-_=+[]{}\\|"), one(C("`", 1.f))));
        L.push_back(cat(one(A(KeyAct::Symbols, "abc", 1.6f)), chars(";:'\"<>,.?/")));
    } else {
        const bool up = p == Page::Upper;
        auto lc = [&](const char* s) { std::string t = s; if (up) for (auto& ch : t) ch = (char)std::toupper((unsigned char)ch); return chars(t.c_str()); };
        L.push_back(cat(lc("qwertyuiop"), one(C("-"))));
        L.push_back(cat(lc("asdfghjkl"), cat(one(C(".")), one(C("@")))));
        L.push_back(cat(one(A(KeyAct::Shift, up ? "SHIFT" : "Shift", 1.6f)), cat(lc("zxcvbnm"), cat(one(C("_")), cat(one(C("/")), one(C(":")))))));
    }
    L.push_back(Row{A(KeyAct::Symbols, p == Page::Symbols ? "ABC" : "#+=", 1.6f), A(KeyAct::Left, "<", 1.f), A(KeyAct::Space, "Space", 4.f),
                    A(KeyAct::Right, ">", 1.f), A(KeyAct::Paste, "Paste", 1.6f), A(KeyAct::Done, "Done", 1.8f)});
    return L;
}

const std::vector<Row>& layout(Page p) {
    static const std::vector<Row> lo = build(Page::Lower), up = build(Page::Upper), sy = build(Page::Symbols), co = build(Page::Code);
    switch (p) {
    case Page::Upper: return up;
    case Page::Symbols: return sy;
    case Page::Code: return co;
    default: return lo;
    }
}
const char* room_alphabet() { return "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; }

static float row_w(const Row& r) { float t = 0; for (auto& k : r) t += k.w; return t; }
float key_center_fraction(const Row& row, int c) {
    float t = row_w(row), x = 0;
    for (int i = 0; i < c && i < (int)row.size(); ++i) x += row[i].w;
    return t > 0 ? (x + row[std::min<int>(c, (int)row.size() - 1)].w * 0.5f) / t : 0.f;
}
int key_at_fraction(const Row& row, float f) {
    float t = row_w(row), x = 0, target = f * t;
    for (size_t i = 0; i < row.size(); ++i) {
        x += row[i].w;
        if (target < x) return (int)i;
    }
    return (int)row.size() - 1;
}
Pos clamp(Page p, Pos pos) {
    const auto& L = layout(p);
    pos.r = std::max(0, std::min(pos.r, (int)L.size() - 1));
    pos.c = std::max(0, std::min(pos.c, (int)L[pos.r].size() - 1));
    return pos;
}
Pos move(Page p, Pos from, int dx, int dy) {
    const auto& L = layout(p);
    from = clamp(p, from);
    if (dx) {
        int n = (int)L[from.r].size();
        from.c = ((from.c + dx) % n + n) % n;
    }
    if (dy) {
        float f = key_center_fraction(L[from.r], from.c);
        int nr = from.r + dy;
        if (nr < 0) nr = (int)L.size() - 1;
        if (nr >= (int)L.size()) nr = 0;
        from.r = nr;
        from.c = key_at_fraction(L[nr], f);
    }
    return from;
}

/* ---- Edit ---- */
static bool is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }
void Edit::set(const std::string& t) {
    text = t.substr(0, maxlen);
    while (!text.empty() && is_cont((unsigned char)text.back()) && text.size() < t.size()) text.pop_back();
    cursor = (int)text.size();
}
int Edit::insert(const std::string& s) {
    std::string add;
    for (unsigned char ch : s) {
        if (ch < 32 || ch == 127) continue;
        if (no_blank && ch == ' ') continue;
        if (room_only) {
            char u = (char)std::toupper(ch);
            if (ch >= 128 || !std::strchr(room_alphabet(), u)) continue;
            ch = (unsigned char)u;
        } else if (upper && ch < 128) ch = (unsigned char)std::toupper(ch);
        add.push_back((char)ch);
    }
    size_t room = maxlen > text.size() ? maxlen - text.size() : 0;
    if (add.size() > room) {
        add.resize(room);
        while (!add.empty() && is_cont((unsigned char)add.back())) add.pop_back();
        if (!add.empty() && (unsigned char)add.back() >= 0xC0) add.pop_back();
    }
    text.insert((size_t)cursor, add);
    cursor += (int)add.size();
    return (int)add.size();
}
void Edit::backspace() {
    if (cursor <= 0) return;
    int e = cursor;
    --cursor;
    while (cursor > 0 && is_cont((unsigned char)text[cursor])) --cursor;
    text.erase((size_t)cursor, (size_t)(e - cursor));
}
void Edit::del() {
    if (cursor >= (int)text.size()) return;
    int e = cursor + 1;
    while (e < (int)text.size() && is_cont((unsigned char)text[e])) ++e;
    text.erase((size_t)cursor, (size_t)(e - cursor));
}
void Edit::move_cursor(int d) {
    while (d > 0 && cursor < (int)text.size()) {
        ++cursor;
        while (cursor < (int)text.size() && is_cont((unsigned char)text[cursor])) ++cursor;
        --d;
    }
    while (d < 0 && cursor > 0) {
        --cursor;
        while (cursor > 0 && is_cont((unsigned char)text[cursor])) --cursor;
        ++d;
    }
}
void Edit::clear() { text.clear(); cursor = 0; }

std::string normalize_room_paste(const std::string& s, size_t max) {
    std::string out;
    for (unsigned char ch : s) {
        char u = (char)std::toupper(ch);
        if (ch < 128 && std::strchr(room_alphabet(), u) && u) out.push_back(u);
        if (out.size() >= max) break;
    }
    return out;
}

/* ---- Session ---- */
void Session::start(Kind k, const std::string& initial, size_t maxlen, bool upper, bool no_blank) {
    kind = k;
    ed = Edit();
    ed.maxlen = maxlen;
    ed.upper = upper || k != Kind::Text;
    ed.no_blank = no_blank || k == Kind::Room;
    ed.room_only = k == Kind::Room;
    ed.set(k == Kind::Room ? normalize_room_paste(initial, maxlen) : initial);
    if (ed.upper && k != Kind::Room) for (auto& ch : ed.text) if ((unsigned char)ch < 128) ch = (char)std::toupper((unsigned char)ch);
    letters = (k == Kind::Room) ? Page::Code : (ed.upper ? Page::Upper : Page::Lower);
    page = letters;
    pos = Pos();
    oneshot_shift = false;
    done = cancelled = false;
    auto_done = k == Kind::Room ? maxlen : 0;
}
const Key& Session::key_under() const {
    const auto& L = layout(page);
    Pos q = clamp(page, pos);
    return L[q.r][q.c];
}
void Session::set_page(Page p) {
    page = p;
    pos = clamp(page, pos);
}
void Session::type_chars(const std::string& s) {
    ed.insert(s);
    if (auto_done && ed.text.size() >= auto_done) done = true;
}
void Session::press(const Key& k) {
    switch (k.act) {
    case KeyAct::Char:
        type_chars(k.ch);
        if (oneshot_shift && page == Page::Upper && kind == Kind::Text && !ed.upper) { oneshot_shift = false; set_page(Page::Lower); }
        break;
    case KeyAct::Space: type_chars(" "); break;
    case KeyAct::Back: ed.backspace(); break;
    case KeyAct::Clear: ed.clear(); break;
    case KeyAct::Left: ed.move_cursor(-1); break;
    case KeyAct::Right: ed.move_cursor(1); break;
    case KeyAct::Done: done = true; break;
    case KeyAct::Paste:
        if (kind == Kind::Room) type_chars(normalize_room_paste(clipboard, ed.maxlen - ed.text.size()));
        else type_chars(clipboard);
        break;
    case KeyAct::Shift:
        if (page == Page::Symbols) { set_page(letters); break; }
        if (page == Page::Upper) { oneshot_shift = false; letters = Page::Lower; set_page(Page::Lower); }
        else { oneshot_shift = true; letters = Page::Upper; set_page(Page::Upper); }
        break;
    case KeyAct::Symbols:
        if (kind == Kind::Room) break;
        if (page == Page::Symbols) set_page(letters);
        else { letters = page; set_page(Page::Symbols); }
        break;
    }
}
void Session::shift_cycle() {
    if (kind == Kind::Room) return;
    if (page == Page::Lower) { oneshot_shift = false; letters = Page::Upper; set_page(Page::Upper); }
    else if (page == Page::Upper) { letters = Page::Upper; set_page(Page::Symbols); }
    else { letters = Page::Lower; set_page(Page::Lower); }
}

/* ---- folders ---- */
std::string parent_of(const std::string& path) {
    std::string p = path;
    while (p.size() > 3 && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    if (p.empty() || (p.size() <= 3 && p.size() >= 2 && p[1] == ':')) return "";
    size_t i = p.find_last_of("/\\");
    if (i == std::string::npos || i == 0) return "";
    std::string par = p.substr(0, i);
    if (par.size() == 2 && par[1] == ':') par += "\\";
    return par;
}

static std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
static fs::path P(const std::string& s) { return fs::u8path(s); }
static std::string S8(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }

void FolderBrowser::open(const std::string& start) {
    done = cancelled = false;
    result.clear();
    std::error_code ec;
    cwd = (!start.empty() && fs::is_directory(P(start), ec)) ? start : "";
    sel = 0;
    refresh();
}
void FolderBrowser::refresh() {
    entries.clear();
    std::error_code ec;
    if (cwd.empty()) {
#ifdef _WIN32
        const char* up = std::getenv("USERPROFILE");
        if (up) {
            for (const char* n : {"Desktop", "Documents", "Downloads"}) {
                std::string p = std::string(up) + "\\" + n;
                if (fs::is_directory(P(p), ec)) entries.push_back({FolderEntry::Shortcut, n, p});
            }
        }
        DWORD m = GetLogicalDrives();
        for (int i = 0; i < 26; ++i)
            if (m & (1u << i)) {
                std::string d = std::string(1, (char)('A' + i)) + ":\\";
                entries.push_back({FolderEntry::Drive, d, d});
            }
#endif
        if (sel >= (int)entries.size()) sel = 0;
        return;
    }
    entries.push_back({FolderEntry::Select, "Select this folder", cwd});
    entries.push_back({FolderEntry::Up, "..  (up one level)", parent_of(cwd)});
    std::vector<FolderEntry> dirs;
    for (fs::directory_iterator it(P(cwd), fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2) || e2) continue;
        std::string name = S8(it->path().filename());
        if (name.empty() || name[0] == '.' || name[0] == '$') continue;
#ifdef _WIN32
        DWORD at = GetFileAttributesW(it->path().c_str());
        if (at != INVALID_FILE_ATTRIBUTES && (at & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
#endif
        dirs.push_back({FolderEntry::Dir, name, S8(it->path())});
    }
    std::sort(dirs.begin(), dirs.end(), [](const FolderEntry& a, const FolderEntry& b) { return lower(a.name) < lower(b.name); });
    entries.insert(entries.end(), dirs.begin(), dirs.end());
    if (sel >= (int)entries.size()) sel = (int)entries.size() - 1;
    if (sel < 0) sel = 0;
}
void FolderBrowser::move(int d) {
    if (entries.empty()) return;
    int n = (int)entries.size();
    sel = ((sel + d) % n + n) % n;
}
void FolderBrowser::activate() {
    if (sel < 0 || sel >= (int)entries.size()) return;
    const FolderEntry e = entries[sel];
    if (e.type == FolderEntry::Select) { result = e.path; done = true; return; }
    cwd = e.type == FolderEntry::Up ? e.path : e.path;
    sel = cwd.empty() ? 0 : 0;
    refresh();
    if (!cwd.empty() && e.type != FolderEntry::Up) sel = 0;
}
void FolderBrowser::up() {
    if (cwd.empty()) { cancelled = true; return; }
    cwd = parent_of(cwd);
    sel = 0;
    refresh();
}
void FolderBrowser::choose_here() {
    if (cwd.empty()) return;
    result = cwd;
    done = true;
}

bool should_open(bool pad_mode, bool keyboard_seen, bool mouse_activated) {
    if (mouse_activated) return false;
    return pad_mode || !keyboard_seen;
}

} // namespace osk
