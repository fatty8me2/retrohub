#pragma once
/* Pure decisions behind the dialogs of batch 2b (party, friends, game night, Kanto Together, admin) on ui::dialog. No ImGui and no app
 * state here, so hub_dialogs2b_test can check them; friends_ui.cpp, kanto_ui.cpp and party_view.cpp feed them. */

namespace hubui {
namespace dlg2b {

/* ---- Game night card: a controller entry in the Friends list ----
 * The card itself is a toast (its own window, never focused). While it is up, the Friends list shows one focusable row for it; A (or a
 * click) on that row opens a small dialog with exactly the card's two actions (Host or Join, and Dismiss). Start is not involved. */
constexpr double kNightCardLife = 120.0;   // seconds a game night card stays up as a toast

/* The card is on screen: a game night card nobody acted on, whose toast is still up and younger than its life. The row and its dialog
 * exist exactly while this holds; when it stops holding the dialog closes and nothing is done. */
inline bool night_card_up(bool is_night, bool done, bool toast, double age) {
    return is_night && !done && toast && age >= 0.0 && age <= kNightCardLife;
}

/* ---- Profile editor: the avatar grid ----
 * Cells per row: as many `cell`-wide cells with `gap` between them as fit in `avail`, at least 1, at most `max_n` (the old fixed row). */
inline int cells_per_row(float avail, float cell, float gap, int max_n) {
    if (max_n < 1) max_n = 1;
    if (!(cell > 0.f)) return max_n;
    int n = 1;
    while (n < max_n && (float)(n + 1) * cell + (float)n * gap <= avail + 0.5f) ++n;
    return n;
}

/* ---- Kanto Together: which of its three dialogs is up ----
 * The install / build progress wins, then the one-time first-session note, then How to play (the order kt_draw_modals always had). */
enum class KtDialog { None, Setup, Intro, Howto };
inline KtDialog kt_dialog(bool installing, bool intro, bool howto) {
    if (installing) return KtDialog::Setup;
    if (intro && !howto) return KtDialog::Intro;
    if (howto) return KtDialog::Howto;
    return KtDialog::None;
}

}  // namespace dlg2b
}  // namespace hubui
