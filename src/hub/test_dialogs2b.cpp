// Console test: the pure decisions behind the batch 2b dialogs (dialogs2b_logic.h): when the game night card's Friends-list row and
// dialog exist, the profile editor's avatar grid, and which Kanto Together dialog is up.
#include "dialogs2b_logic.h"

#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

using namespace hubui::dlg2b;

static void night_card() {
    CHECK(night_card_up(true, false, true, 0.0));
    CHECK(night_card_up(true, false, true, 60.0));
    CHECK(night_card_up(true, false, true, kNightCardLife));          // the toast's last moment
    CHECK(!night_card_up(true, false, true, kNightCardLife + 0.01));  // past its life: the toast is gone, so are the row and its dialog
    CHECK(!night_card_up(false, false, true, 1.0));                    // an invite / request / ask card: no row (Start, Y and the bell cover those)
    CHECK(!night_card_up(true, true, true, 1.0));                      // acted on (Host / Join / Dismiss): gone
    CHECK(!night_card_up(true, false, false, 1.0));                    // toast already closed
    CHECK(!night_card_up(true, false, true, -1.0));                    // clock oddity: never shown "from the future"
}

static void avatar_grid() {
    // the old editor: 7 cells of 6 units with 0.75 gaps = 46.5 units
    CHECK(cells_per_row(46.5f, 6.f, 0.75f, 7) == 7);
    CHECK(cells_per_row(60.f, 6.f, 0.75f, 7) == 7);       // never more than the old row
    CHECK(cells_per_row(46.f, 6.f, 0.75f, 7) == 7);       // within half a unit (rounding) still fits
    CHECK(cells_per_row(45.f, 6.f, 0.75f, 7) == 6);       // a narrow dialog: one cell moves down
    CHECK(cells_per_row(12.75f, 6.f, 0.75f, 7) == 2);
    CHECK(cells_per_row(3.f, 6.f, 0.75f, 7) == 1);        // never zero
    CHECK(cells_per_row(100.f, 0.f, 0.75f, 7) == 7);      // degenerate cell size: the old count
    CHECK(cells_per_row(100.f, 6.f, 0.75f, 0) == 1);
}

static void kanto() {
    CHECK(kt_dialog(false, false, false) == KtDialog::None);
    CHECK(kt_dialog(true, true, true) == KtDialog::Setup);      // the install progress wins over everything
    CHECK(kt_dialog(true, false, false) == KtDialog::Setup);
    CHECK(kt_dialog(false, true, false) == KtDialog::Intro);
    CHECK(kt_dialog(false, true, true) == KtDialog::Howto);     // How to play asked for: the first-session note waits
    CHECK(kt_dialog(false, false, true) == KtDialog::Howto);
}

int main() {
    night_card();
    avatar_grid();
    kanto();
    std::printf(fails ? "hub_dialogs2b_test FAILED\n" : "hub_dialogs2b_test OK\n");
    return fails ? 1 : 0;
}
