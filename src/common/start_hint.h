#pragma once
/* "Controller tips" hint shown for a few seconds when a game starts: a small semi-transparent card in the bottom-left
 * corner. This header is the shared part: timing (fade in / hold / fade out), the text of the rows, and a software
 * renderer that produces an ARGB bitmap (Xbox-style button glyphs, matching the hub's footer hints). The consumers are
 *   - retrorun: draws the bitmap into its own SDL frame (and into headless screenshots),
 *   - hint_popup.h: a click-through topmost popup window for games RetroHub does not draw into (Dolphin, PCSX2, Azahar,
 *     Cemu, mGBA, BattleShip, n64run's GL window).
 * The shortcuts described here must match the implemented ones: Back(View)+Y opens the quick menu (hub: cheats_pad_watch),
 * Back(View)+Start held for kExitHoldMs leaves the game (src/common/exit_combo.h). */

#include <cstdint>
#include <string>
#include <vector>

namespace starthint {

/* ---- timing (pure) ---- */
constexpr int kFadeInMs = 400;
constexpr int kHoldMs = 4200;      // fully visible
constexpr int kFadeOutMs = 600;
constexpr int kTotalMs = kFadeInMs + kHoldMs + kFadeOutMs;   // 5.2 s on screen
constexpr float kMaxAlpha = 0.92f;       // the card is semi-transparent even at its most visible
constexpr int kGiveUpWaitMs = 60000;     // external emulators: no usable game window after this long -> skip silently

/* Opacity multiplier 0..1 for `elapsed_ms` since the hint started (negative = not started). */
float alpha_at(long long elapsed_ms);
inline bool finished(long long elapsed_ms) { return elapsed_ms >= kTotalMs; }
inline bool visible(long long elapsed_ms) { return elapsed_ms >= 0 && elapsed_ms < kTotalMs && alpha_at(elapsed_ms) > 0.f; }

/* ---- content (pure) ---- */
enum class Mode { Pad, Keyboard };
enum class TokKind { Text, Label, Plus, Pad, Key };   // Pad: Xbox glyph ("View", "Menu", "Y"); Key: key cap ("Esc")
struct Tok { TokKind kind; std::string text; };
using Row = std::vector<Tok>;

/* Rows of the card. Pad: "Hold View + Y  game menu" / "Hold View + Menu  back to RetroHub". Keyboard (only meaningful for
 * the built-in runners, which have keys for this): "Esc  back to RetroHub" / "F12  screenshot". */
std::vector<Row> rows(Mode m);
/* ASCII rendering of a row for tests and logs: "Hold View + Y - game menu". */
std::string plain(const Row& r);

/* Which mode to show: the last input the player used. External emulators have no keyboard equivalents, so they pass
 * runner=false and always get the controller wording. */
Mode pick_mode(bool last_input_keyboard, bool runner);

/* UI scale for a game image / window of this height (1.0 at <=540 px). */
float scale_for_height(int frame_h);

/* ---- rendering ---- */
struct Bitmap {
    int w = 0, h = 0;
    std::vector<uint32_t> px;   // 0xAARRGGBB, straight (non-premultiplied) alpha
};
Bitmap render(Mode m, float scale);   // Windows: text via GDI (Segoe UI), shapes anti-aliased in software

/* Top-left of the card inside a w x h area (bottom-left corner with a margin). */
void placement(int area_w, int area_h, const Bitmap& b, int* x, int* y);
/* Alpha-composites the card (times `alpha`) onto an ARGB/XRGB frame (row pitch in pixels). */
void composite(uint32_t* dst, int w, int h, int pitch_px, const Bitmap& b, float alpha);

}  // namespace starthint
