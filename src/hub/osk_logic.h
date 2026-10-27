#pragma once
/* On-screen keyboard + folder browser: pure logic (no ImGui, no SDL). The widget lives in osk.cpp. */

#include <string>
#include <vector>

namespace osk {

enum class KeyAct { Char, Shift, Symbols, Space, Back, Done, Left, Right, Clear, Paste };
enum class Page { Lower, Upper, Symbols, Code };
enum class Kind { Text, Code, Room };   // Code: upper-case codes (friend / link / room). Room: 6-char room code alphabet + auto-done.

struct Key {
    KeyAct act = KeyAct::Char;
    std::string label;   // what is drawn on the key
    std::string ch;      // text inserted (Char keys)
    float w = 1.f;       // relative width
};
using Row = std::vector<Key>;

const std::vector<Row>& layout(Page p);
const char* room_alphabet();   // letters/digits valid in a room code (no I, O, 0, 1)

struct Pos { int r = 0, c = 0; };
/* Moves the highlight. Horizontal wraps within the row; vertical keeps the same x position (rows have different widths). */
Pos move(Page p, Pos from, int dx, int dy);
Pos clamp(Page p, Pos pos);
/* Index of the key in `row` that covers horizontal fraction f (0..1) of the row. */
int key_at_fraction(const Row& row, float f);
float key_center_fraction(const Row& row, int c);

/* Text being edited. Byte based; UTF-8 aware for cursor moves and backspace. */
struct Edit {
    std::string text;
    int cursor = 0;       // byte offset
    size_t maxlen = 255;
    bool upper = false;   // force upper-case
    bool no_blank = false;
    bool room_only = false;   // only room-code characters

    void set(const std::string& t);
    /* Inserts s at the cursor through the filter; returns the number of bytes inserted. */
    int insert(const std::string& s);
    void backspace();
    void del();
    void move_cursor(int d);   // in characters
    void clear();
    bool full() const { return text.size() >= maxlen; }
};

/* "ab-c d" -> normalised room code candidate: upper-case, strips blanks/dashes/invalid characters, caps at max. */
std::string normalize_room_paste(const std::string& s, size_t max);

/* One keyboard session: current page, highlight, text and the done/cancel outcome. */
struct Session {
    Edit ed;
    Page page = Page::Lower;
    Page letters = Page::Lower;   // page to return to from Symbols
    Pos pos;
    Kind kind = Kind::Text;
    bool oneshot_shift = false;   // Upper for exactly one character
    bool done = false, cancelled = false;
    size_t auto_done = 0;         // finish automatically at this length (0 = never)
    std::string clipboard;        // text for the Paste key (filled by the widget)

    void start(Kind k, const std::string& initial, size_t maxlen, bool upper, bool no_blank);
    const Key& key_under() const;
    void press(const Key& k);
    void press_current() { press(key_under()); }
    void dir(int dx, int dy) { pos = move(page, pos, dx, dy); }
    void shift_cycle();   // the Y button: lower -> upper -> symbols -> lower
    void type_chars(const std::string& s);   // physical keyboard / paste
    void set_page(Page p);
};

/* ---- pad-navigable folder browser ---- */
struct FolderEntry {
    enum Type { Select, Up, Drive, Dir, Shortcut, File } type = Dir;
    std::string name;   // shown
    std::string path;   // where it leads (Select: the folder chosen)
};
struct FolderBrowser {
    std::string cwd;   // "" = list of drives / shortcuts
    std::vector<FolderEntry> entries;
    int sel = 0;
    bool done = false, cancelled = false;
    std::string result;
    bool pick_files = false;                // file mode: list matching files, no "Select this folder"
    std::vector<std::string> filters;       // file mode: extensions incl. dot (".json"); empty = every file

    bool matches(const std::string& filename) const;
    void open(const std::string& start);   // falls back to the drive list when start is not a folder
    void refresh();
    void activate();   // A: enter the highlighted folder, or choose it (Select entry)
    void up();         // B: parent folder (drive list above a drive root); cancels from the drive list
    void choose_here();   // Start: choose the current folder
    void move(int d);
};
std::string parent_of(const std::string& path);   // "" when path is a drive root / empty

/* A pad (or no keyboard seen yet) wants the on-screen keyboard instead of ImGui's own text editing. */
bool should_open(bool pad_mode, bool keyboard_seen, bool mouse_activated);

/* The hub ignores keyboard, text and pad input for its UI while its window lacks input focus, and for kRegainGraceSecs after it
 * regains it (a pad press queued at the focus change must not type). A running game alone does not block: people Alt-Tab back. */
constexpr double kRegainGraceSecs = 0.25;
struct FocusGate {
    bool was_focused = false;
    double regained_at = -1e9;
    bool blocked(bool focused, double now) {
        if (focused && !was_focused) regained_at = now;
        was_focused = focused;
        return !focused || now - regained_at < kRegainGraceSecs;
    }
};
inline unsigned gate_pad_mask(unsigned mask, bool blocked) { return blocked ? 0u : mask; }
/* A chat line may be sent: not blocked, and not empty / whitespace only. */
bool chat_may_send(const std::string& text, bool blocked);

} // namespace osk
