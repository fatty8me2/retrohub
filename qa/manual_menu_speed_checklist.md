# Game menu + speed: 2-minute manual check (real desktop, real pad)

The automated tests cover the menu logic (hub_qm_nav_test, hub_qm_e2e), the built-in runners' speeds (runner_speed_headless) and
Dolphin's speed via the DSU pad on a hidden desktop (hub_dolphin_speed_live). They cannot cover what Windows does on your real screen:
foreground/z-order with a full-screen game, your physical controller, and a real GameCube game. Check those here.
If anything fails, send `userdata\logs\quickmenu.log` (every open, close, press and speed change is logged there).

## A. Built-in emulator (any GBA/SNES/N64 game), full screen
1. Start the game. Press **View+Y**. The Game menu appears **on top of the game** and stays open. Keep holding View+Y for 2 s: nothing happens.
2. Release. Press **D-pad down** once: the Speed row is highlighted. Press **right** twice: the game behind runs at **2x** right away.
3. Press **left** until it shows **1x**. The game runs at normal speed.
4. Press **B**: the menu closes, the game is in front and takes your pad again.
5. Hold **View+RT** in the game: fast-forward while held; release: normal speed. Repeat with the menu speed set to 0.5x: release returns to 0.5x.
6. Press View+Y, then **click another window** (or Alt+Tab) while the menu is open. Press D-pad / B: the menu still reacts to the pad.
7. After B, the game is **in front of** RetroHub (RetroHub must not stay on top).
8. Only if you host an online room: View+Y, go to **Invite friends**, press A. The picker opens and the d-pad moves inside it; B closes
   just the picker (the Game menu stays open); a second B closes the menu.

## B. GameCube (Dolphin), full screen
1. Start a game. View+Y: menu on top. Speed row: **right** to **2x**. The game speeds up **while the menu is still open**
   (the row shows "Changing Dolphin's speed..." for about a second, then nothing).
2. **Left** back to **1x**: the game returns to normal speed. Resume with B.
3. While playing, click another app and press **Esc** and **F10** there: Dolphin must **not** stop or pause.
4. Set 0.5x, Resume, play 5 s, open the menu: it still says 0.5x and the game runs at half speed. Set 1x again.
5. Emergency: hold **View+LB** for 1 s: a toast says "Resetting Dolphin to 1x..." and the game returns to normal speed within about 10 s (it steps down to 0.1x first, then up to 1x).

## Known limits (by design)
- Dolphin exposes no readable speed: RetroHub tracks the speed it set. After the emergency reset it re-syncs by stepping down to
  Dolphin's 0.1x floor and back up, which takes about 8 s of presses.
- Speed is off in online games (everyone has to stay in step).
