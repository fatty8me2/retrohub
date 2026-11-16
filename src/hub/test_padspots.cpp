// Console test: controller paths onto mouse-only controls (padspots_logic.h): which toast Y presses, the Sort chip arrow and reverse keys.
#include "padspots_logic.h"

#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
using namespace hubui;
using namespace hubui::padspots;

static ToastPadIn T(bool act, bool pill, double age) { ToastPadIn t; t.has_action = act; t.pill_shown = pill; t.age = age; return t; }

int main() {
    ToastPadEnv env; env.pad_mode = true;
    {   // one action toast: the glyph (and Y) only once it has been on screen for the delay, and only while its pill shows
        CHECK(toast_pad_pick({T(true, true, kToastPadDelay)}, env) == 0);
        CHECK(toast_pad_pick({T(true, true, 3.0)}, env) == 0);
        CHECK(toast_pad_pick({T(true, true, kToastPadDelay - 0.01)}, env) == -1);   // too young: a press meant for the page is not taken
        CHECK(toast_pad_pick({T(true, false, 3.0)}, env) == -1);                    // fading out / not drawn: the mouse cannot click it either
        CHECK(toast_pad_pick({T(false, true, 3.0)}, env) == -1);                    // a plain toast has nothing to press
        CHECK(toast_pad_pick({}, env) == -1);
    }
    {   // several toasts: the newest action toast decides; an older one is never pressed underneath a newer, younger one
        CHECK(toast_pad_pick({T(true, true, 5.0), T(false, true, 0.1)}, env) == 0);   // a newer plain toast does not hide the action
        CHECK(toast_pad_pick({T(true, true, 5.0), T(true, true, 2.0)}, env) == 1);
        CHECK(toast_pad_pick({T(true, true, 5.0), T(true, true, 0.2)}, env) == -1);
        CHECK(toast_pad_pick({T(true, true, 5.0), T(true, false, 0.1)}, env) == -1);   // the newer one is still fading in: not the older one either
        CHECK(toast_pad_pick({T(true, true, 5.0), T(true, false, 9.0)}, env) == 0);   // the newer one is fading out: the older still shows its pill
    }
    {   // anything else owning the pad turns it off
        const std::vector<ToastPadIn> one = {T(true, true, 2.0)};
        ToastPadEnv e = env; e.pad_mode = false;      CHECK(toast_pad_pick(one, e) == -1);   // mouse / keyboard: no glyph, no Y
        e = env; e.input_blocked = true;              CHECK(toast_pad_pick(one, e) == -1);
        e = env; e.game_running = true;               CHECK(toast_pad_pick(one, e) == -1);
        e = env; e.popup_open = true;                 CHECK(toast_pad_pick(one, e) == -1);
        e = env; e.text_input = true;                 CHECK(toast_pad_pick(one, e) == -1);
        e = env; e.wizard = true;                     CHECK(toast_pad_pick(one, e) == -1);
        CHECK(toast_pad_env_ok(env));
    }
    {   // Sort chip arrow: same rule as the List view column arrows (ascending up, descending down); Reverse flips it
        CHECK(!sort_arrow_down(SortSpec(SortMode::Title)) && sort_arrow_down(SortSpec(SortMode::Title, true)));
        CHECK(sort_arrow_down(SortSpec(SortMode::LastPlayed)) && !sort_arrow_down(SortSpec(SortMode::LastPlayed, true)));
        CHECK(sort_arrow_down(SortSpec(SortMode::MostPlayed)) && !sort_arrow_down(SortSpec(SortMode::MostPlayed, true)));
        CHECK(sort_arrow_down(SortSpec(SortMode::RecentlyAdded)) && !sort_arrow_down(SortSpec(SortMode::RecentlyAdded, true)));
        CHECK(!sort_arrow_down(SortSpec(SortMode::ReleaseYear)) && sort_arrow_down(SortSpec(SortMode::ReleaseYear, true)));   // "Year (oldest)" first
        CHECK(!sort_arrow_down(SortSpec(SortMode::Console)) && sort_arrow_down(SortSpec(SortMode::Console, true)));
        CHECK(sort_arrow_down(SortSpec(SortMode::Title, true, true)) == sort_arrow_down(SortSpec(SortMode::Title, true)));   // favourites first does not change it
        for (int m = 0; m < kSortModes; ++m)   // the label words and the arrow always change together
            CHECK(sort_arrow_down(SortSpec((SortMode)m)) != sort_arrow_down(SortSpec((SortMode)m, true)) &&
                  sort_label(SortSpec((SortMode)m)) != sort_label(SortSpec((SortMode)m, true)));
    }
    {   // room for the arrow
        CHECK(arrow_pad_spaces(4.f, 14.f) == 4 && arrow_pad_spaces(4.f, 16.f) == 4 && arrow_pad_spaces(5.f, 1.f) == 1);
        CHECK(arrow_pad_spaces(0.f, 14.f) == 3 && arrow_pad_spaces(-1.f, 14.f) == 3);
        CHECK(arrow_pad_spaces(4.f, 14.f) * 4.f >= 14.f);
    }
    {   // reverse keys: only with the chip focused and its ring showing, never while typing
        SortReverseIn in; in.focused = true; in.ring_visible = true;
        CHECK(!sort_reverse_press(in));                                    // no key
        in.pad_x = true;      CHECK(sort_reverse_press(in));
        in.pad_x = false; in.menu_key = true;  CHECK(sort_reverse_press(in));
        in.menu_key = false; in.shift_f10 = true; CHECK(sort_reverse_press(in));
        SortReverseIn n = in; n.focused = false;      CHECK(!sort_reverse_press(n));   // X elsewhere stays Favorite
        n = in; n.ring_visible = false;               CHECK(!sort_reverse_press(n));   // focus left by a mouse click: no surprise
        n = in; n.text_input = true;                  CHECK(!sort_reverse_press(n));
    }
    std::printf("hub_padspots_test: %s (%d failure%s)\n", fails ? "FAILED" : "ok", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
