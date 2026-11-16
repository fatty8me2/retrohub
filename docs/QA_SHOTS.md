# Headless screenshot hooks (QA)

`HUB_SCREENSHOT=<file.bmp>` makes RetroHub render one frame with SDL's dummy video driver and a software renderer, write the BMP and exit. No window is ever shown.
Every `HUB_SHOT_*` variable below is read **only in that mode** (`g_headless`): a normal run behaves the same whatever is in the environment, fake data is never written to a real vault, never sent to a server and never launches anything.

Rules for QA runs: run a COPY of the build output (no `userdata` folder) in a sandbox, set `HUB_SHOT_VAULT=<dir inside it>`, `RETROHUB_QA=1`, `SSB64_MUTE=1`, and never start while the owner is gaming (see `qa\_gamecheck.ps1`).
`qa\shot_matrix.ps1` does all of this and captures the standard matrix (see the end of this file).

Exit codes: 0 = shot saved. **2 = a requested aid could not be honoured (unknown page / Settings section / game key / state): a message on stderr, no screenshot.**

## Core

| Variable | Values | What it shows |
| --- | --- | --- |
| `HUB_SCREENSHOT` | file path | turns headless mode on, writes the frame there |
| `HUB_SHOT_SIZE` | `WxH` | window size, e.g. `1280x720` |
| `HUB_SHOT_SECS` | seconds (default 4) | run this long first |
| `HUB_SCALE`, `HUB_UI_SCALE` | 1, 1.5, 2 / percent | DPI scale / UI size |
| `HUB_SHOT_VAULT` | dir | the (scratch) save vault; default `%TEMP%\retrohub-shot-vault` |
| `HUB_SHOT_NOWAIT` | any | do not wait for the library scan |
| `HUB_SHOT_TRACE` | file | append trace lines (pad presses, captured page, ...) |
| `HUB_START_PAGE` | 0 Library, 1 Online, 2 Saves, 3 Controllers, 4 Settings, 5/6 Updates, 7 Discover | start page (older numbering) |
| `HUB_SHOT_PAGE` | `library`, `discover[:console]`, `online`, `saves`, `controllers`, `settings[:section]`, `about` (= `updates`) | open a page. `settings:<section>`: index `0-4` or name `general`, `online`, `video` (`games`), `account`, `help`. An unknown page or section exits 2 (no clamping) |
| `HUB_SHOT_SETTINGS_TAB` | 0-4 | (page_about.cpp) Settings section when the Updates page is used as the start; prefer `HUB_SHOT_PAGE=settings:<name>` |
| `HUB_SHOT_KEYS` | `right,down,enter,esc,f2,i,f,o,j,pgdn,...,a-z` | keyboard presses at 0.3 s steps |
| `HUB_SHOT_PAD` | `1`/`0` or `down,a,lb,shot:<name>,...` | controller glyphs / pad presses; `shot:<name>` saves `<stem>-<name>.bmp` |
| `HUB_SHOT_CLICK` | `x,y;rx,y;...` | mouse clicks (`r` = right click), window pixels |
| `HUB_SHOT_WHEEL` | `x,y,notches` | mouse wheel |
| **`HUB_SHOT_HOVER`** | `x,y` (window pixels) | **parks the virtual mouse there and waits 1.2 s so the tooltip of that spot shows.** Any other text is the Friends panel's own "hovered friend uid" aid |

## Library and games

| Variable | Values | What it shows |
| --- | --- | --- |
| **`HUB_SHOT_FAKE_LIB`** | **`0` or `empty` = a truly EMPTY library (first-run state, no bundled game either)**; `native` = only the bundled Smash 64 entry; `n` (1+) = n made-up games plus Crash Bash and Super Mario 3D World (`HUB_SHOT_NO_WIIU` drops the latter); `all` = every console + Smash Classic / Remix / Kanto; `tpak` = Pokemon Stadium 2 + Game Boy games | the library (no scan, no art) |
| `HUB_SHOT_FAKE_TITLES`, `_FAKE_AZ`, `_FAKE_SHELF[=<system>]`, `_SWITCH=1\|2`, `_PSP`, `_FAKE_DUPES`, `_HACKS`, `_PALPARK` | flags | title sets, A-Z spread, shelf mix, Switch / Switch 2 / PSP games, duplicate copies, ROM hacks, Pal Park (lib_state.cpp, hack_ui.cpp) |
| `HUB_SHOT_DETAILS` | game key, e.g. `fake:2`, `switch:0400486024dec000` | open that game's page; a key not in the library exits 2. Traditional: the full-window page (an Overlay sheet, ImGui window `details##uisheet`). Library desk with its Library page showing: that game is selected in the list and its page is in the right-hand pane with the focus on Play (otherwise the full-window page). Either way the `captured` line says `details=yes game=<key>` |
| `HUB_SHOT_TAB` | 0-3 | tab of the game page (0 About, 1 Saves, 2 Screenshots, 3 Settings; a tab the game lacks clamps to the nearest). The desk pane keeps the shown tab when the list moves to a game that has it |
| `HUB_SHOT_ACT` | `play`, `online`, `favorite`, `savefolder`, `cheats`, `controls`, `desktop` | presses that game page action once (the secondary ones live in the page's More menu); every action writes `act <key> <name> <state>` trace lines |
| `HUB_SHOT_SEARCH`, `_LIBFILTER` (`fav;group;genre=X`), `_LIBVIEW` (`grid\|list\|compact\|showcase`), `_LIBCOL`, `_SORT`, `_CASES*`, `HUB_START_VIEW=all`, `HUB_START_SYS` | | library views and filters |
| **`HUB_SHOT_LAYOUT`** | `desk`, `traditional` | **the Settings > General > Layout choice for this run** (`Settings::ui_layout`): `desk` = the Library desk (game list on the left, the selected game's page in the pane on the right, icon sidebar). An unknown value exits 2. `HUB_SHOT_SEARCH` fills its search box, `HUB_SHOT_LIBFILTER` / `HUB_SHOT_SORT` set the library filters / Sort order it shares with Traditional; pad walks (`HUB_SHOT_PAD=down,...,right,rb,down,b,left`) move the list cursor, enter the pane (Play takes the focus), switch its tabs, scroll its body and come back; `lt,lt,lt,lt,up` reaches the toolbar (Sort chip, then Filters, Surprise me, More with Right). Letter keys (`HUB_SHOT_KEYS=m,z`) jump to titles. `HUB_SHOT_TRACE` logs `desk-model` (games, matches, friends' games, sort), `desk-cursor` (row, group, key, selection row), `desk-group(s)`, `desk-focus`, `desk-page` (a page request selected that game), `desk-play` (A / Enter: the game page's Play path; nothing launches headless), `desk-jump <prefix> row=`, `desk-filter`, `sort`, `desk-coverflow open/closed <key>`, `desk-ghost`, `desk-drag`, `desk-restore`, `desk-persist`, `desk-card shown/try/not-now`, `desk-note`. `qa\desk_shots.ps1` captures the set (and `-Bin <other build> -Tag <name>` the Traditional comparison shots); it pre-writes the sandbox `userdata\hub.json` for shots that need a state (setup finished, the Try card answered, `desk_list_w`, `desk_sel` / `desk_scroll`) |
| `HUB_SHOT_DESK_DRAG` | pixels (+ wider, - narrower) | the virtual mouse drags the Library desk's divider that far through the real drag code (trace `desk-drag w=<units>`) |
| `HUB_SHOT_DESK_TYPE` | characters | types them (one per 0.4 s from 2 s on) as text input: letters / digits jump in the list, `/` starts a search |
| `HUB_CASES_AUDIT_FRIENDS` | JSON file `{"libs": {"<uid>": {"games": [{"s": "n64", "k": "...", "t": "Title"}]}}}` | friends' libraries for a screenshot run: their games you do not own show greyed (Traditional shelves, Library desk list) |
| **Game page** | | **`qa\gamepage_shots.ps1`** captures the page in both layouts (1024x600, 1280x720, 1920x1080, UI size 150 %, Light; About / Saves / Screenshots / Settings, play time, save states, Ocarina of Time), pad walks (tabs with RB, the Settings rows until the body scrolls, B), the More menu, every popup the page opens (Customize, Add to shelf, screenshot viewer, save states, history, hack remove, Find Smash 64, the Hookshot card's list) and `HUB_SHOT_ACT` for each action, from a sandbox copy. Trace lines: `gamepage-open <key>` (the full-window page opened), `gamepage-more`, `gamepage-action <id> <key>` (a More menu item), `gamepage-tab <tab> <key>`, `gamepage-focus play <key>` (entering the desk pane), `shots-folder` (Open folder: headless runs only trace it) |
| **Game page Settings tab** | | compact one-line rows; the focused (or hovered) row's help is in a pinned strip (the page footer in Traditional, beside the hints; a slim strip at the bottom of the desk pane); on a wide body (1920x1080 at 100 %, 2560x1440) the sections flow into two columns. `qa\gamepage_shots.ps1 -Set dense` captures Ocarina of Time (idle / done_fritchii), a fake N64 and PS1 game, About and Saves at 1024x600, 1280x720, 1920x1080, 2560x1440, 125 % / 150 % and Light in both hosts (also runs on an older build with `-Bin`); `-Set densewalk` the pad walks (down both columns, across, a dropdown on the right, a section closed, RT / LT, the strip following the focus), a hover, the keyboard hints, the hack remove / add and the Uninstall confirm over the page. Trace: `gs-visible <key> <rows>` each time the number of whole option rows in view (section headers count) changes |
| **`HUB_SHOT_PACKGAME`** | game key, e.g. `fake:3` | that game's About tab shows it as installed from a shared pack (the folder line and Uninstall), so the Uninstall confirm can be captured. Its path is not in the pack folder: a confirmed Uninstall finds nothing to delete |
| `HUB_SHOT_CONTINUE` | `1` or `playing` | play history for the Continue hero and Recently played; **assigned, never added, so the fake hours are the same on every run** |
| `HUB_SHOT_STATES` | any | fake save states with thumbnails for `fake:1` and `fake:4` (same fixed history) |
| `HUB_SHOT_PLAYTIME` | any | fixed play-time cases for `fake:0..4` and generated sessions |
| **`HUB_SHOT_FRESH`** | `1` | wipes the sandbox play history of the `fake:*` games before seeding (done automatically with `HUB_SHOT_FAKE_LIB` / `HUB_SHOT_HOOKSHOT`; real games are never touched) |
| `HUB_SHOT_HOOKSHOT` | `idle`, `problems`, `building`, `paused`, `done` (Play uses Lichii), `done_on` (Hookshot), `done_fritchii`, `failed`; unset/empty/`0` = off | **the Hookshot / Fritchii card on Ocarina of Time's About tab.** Use `HUB_SHOT_FAKE_LIB=1 HUB_SHOT_HOOKSHOT=<mode>` (the fake library always gets the Ocarina entry `switch:0400486024dec000` and the page opens by itself; `HUB_SHOT_FAKE_LIB` may be omitted, or `0`/`native`) |
| `HUB_SHOT_GBALINK`, `_VERIFY`, `_TPAK`, `_RESUME`, `_HIDDEN`, `_FINDGAMES`, `_ADDGAMES`, `_SAVEIMPORT`, `_CHEATS`, `_CUSTOM`, `_PICKER*`, `_PALETTE`, `_INFO` | see source | feature-specific aids (grep the name) |

## Saves and cloud

| Variable | Values | What it shows |
| --- | --- | --- |
| **`HUB_SHOT_CLOUD2`** | `checking` (the real engine's "Checking your cloud saves..."), `ok`, `mixed` (syncing / waiting / conflict / playing elsewhere / local only), `over` (quota full, games stay local), `attention` (needs sign-in), `offline`, `paused` (sync switched off), `fresh` (signed in, no games). Unknown name exits 2 | the Saves page cloud box + games list with a fake account (`cloud_shot_fake` in cloud_engine.cpp; use with `HUB_SHOT_PAGE=saves`) |
| `HUB_SHOT_CLOUD_OPEN` | `<system>/<key>` | expand that game's version history |
| `HUB_SHOT_CLOUD_DIALOG` | `conflict`, `conflictmany` (8 files: the middle scrolls), `lease`, `checking`, `waiting` (the strip with its Cancel control; B / Esc / A on it write the trace line `cloud-wait-cancel`), `pulled`, `offline` | launch-time cloud dialogs / notices |
| `HUB_SHOT_CLOUD_CODE` | any | reveal the recovery code |
| `HUB_SHOT_HISTORY` | `<system>/<key>` | save history of a game on the Saves page |

## Online, friends, notifications

| Variable | Values | What it shows |
| --- | --- | --- |
| **`HUB_SHOT_OFFLINE`** | `home` (the Play Online page with the friends panel "offline") or `room` (a room that is still "Connecting...") | Play Online with no network (headless never connects anyway) |
| `HUB_SHOT_FRIENDS` | any | friends panel open |
| `HUB_SHOT_FAKE_FRIENDS` (+ `_CHAT`, `_BELL`, `_PROFILES`, `_LONGNAMES`, ...) | | fake friends, chat, notifications |
| `HUB_SHOT_FAKE_ROOMS` | `n` (0-40) | n made-up friends in open rooms on Play Online's "Friends' rooms" list (page_online.cpp; the list only, never the friends data). `qa\scroll_shots.ps1` uses it |
| **`HUB_SHOT_UPDATE_NOTES`** | comma list of `ready`, `failed`, `postponed`, `rollback`, `interrupted` | adds those update notes **and opens the Notifications list (bell panel) with the friends panel** so the notes are readable. `HUB_SHOT_BELL=0` keeps it closed (badge only) |
| `HUB_SHOT_BELL` | any | open the Notifications list (with `HUB_SHOT_FAKE_FRIENDS`) |
| `HUB_SHOT_UPDATE` | any | also draw the update prompt |
| `HUB_SHOT_WELCOME` (`_FROM`, `_TO`, `_T`, `_OWNER`) | | the "What's new" welcome card |
| `HUB_SHOT_FAKE_ROOM` (+ `_ROOM_GAME`, `_READY`, `_PICKER`, `_PICKER_Q`, `_HOSTCHANGE`, `_PLAY_AGAIN`, `_LOCALS2`, `_PICKS`) | | a fake room |
| `HUB_SHOT_REJOIN` | text | the disconnected Rejoin strip |
| `HUB_SHOT_ACCESS` | `pending`, `unavailable`, `setup`, `setup-offline` | whole-app lock screens |
| `HUB_SHOT_ADMIN` | `1`, `pending`, `banned`, `detail`, `confirm`, `settings` | owner dashboard (fake data) |
| `HUB_SHOT_NET` | `1` | allows the friends client, but only against a loopback server |
| `HUB_SHOT_PARTY`, `_BRACKET`, `_KT`, `_CHALLENGE`, `_WRAPPED`, `_USERNAME*`, `_WIZARD`, `_SURPRISE` | | Plan 4 / account / first-run aids (grep the name). `HUB_SHOT_PARTY=couchpick`: the couch party setup on "I'll pick" (with `HUB_SHOT_FAKE_LIB=30`: its 24 game chips) |
| `HUB_SHOT_FRCONF` | `remove`, `block` | with `HUB_SHOT_FAKE_FRIENDS` + `HUB_SHOT_FRIENDS`: the "Are you sure?" question for Mia (nothing is sent) |
| `HUB_SHOT_PLAN_NIGHT` | any | with `HUB_SHOT_FAKE_FRIENDS` + `HUB_SHOT_FRIENDS`: Mia's chat with the "Plan a session" (game night) dialog open |
| `HUB_SHOT_FAKE_NIGHT` | any | with `HUB_SHOT_FAKE_FRIENDS`: a game night starting now (its card, and its row at the top of the Friends list). Trace lines: `gn-card-focus <id>` (the D-pad is on the row), `gn-card-open <id>` (A / click opened its dialog), `gn-card-act <Host\|Join> <id>`, `gn-card-dismiss <id>` |

## Emulator dialogs

| Variable | Values | What it shows |
| --- | --- | --- |
| `HUB_SHOT_EMU` | `azahar`, `cemu` (download prompt), `lichiiupdate`, `fritchii`, `failed`, `unzip`, `ps2bios`, `switchkeys`, `hookshotav1`, `hookshotmissing`, `missingrom`, `hidden` (with `HUB_SHOT_HIDDEN=1`, or `=many` for 18 rows), `ps1bios`, `resume` | emulator dialogs |
| `HUB_SHOT_NXPREP` | any | the "Getting Fritchii ready" copy dialog frozen half-way (no copy runs) |
| `HUB_SHOT_HACKS` | `add`, `choose` (18 candidate games), `fail`, `remove` (nothing is deleted), `versions`, `tile`; any value also lets the made-up games offer "Add hack or translation" (its pad file browser; a screenshot run never opens the Windows file dialog) | ROM hack dialogs and views (hack_ui.cpp) |
| **`HUB_SHOT_EMU=working[:<emu>[:<percent>]]`** | `<emu>` = `azahar`, `cemu`, `dolphin`, `pcsx2`, `ppsspp`, `lichii`, `fritchii` (default `azahar`), percent 0-100 (default 42) | **the install-in-progress dialog frozen at a fixed percentage** (no download runs). Unknown emulator exits 2 |
| `HUB_SHOT_FRITCHII_PIN` | any | a fake Fritchii pin (96 MB) |
| `HUB_SHOT_QM*` | | quick menu aids |
| `HUB_SHOT_CTL_*`, `HUB_SHOT_FAKE_PADS=n` | | Controllers page aids |

## UI kit

| Variable | Values | What it shows |
| --- | --- | --- |
| **`HUB_SHOT_KITDEMO`** | `sheet` (a game-page-like page sheet over the whole window), `pane` (the "Library desk": a game list on the left, the same page in a pane on the right), `dialog` (the sheet with a scrolling dialog on top). Unknown value exits 2 | the UI kit (`ui_scroll.h`, `ui_sheet.h`, `ui_controls.h`) with fake content: tab bar, three sections (Advanced closed), 17 option rows with long labels and values. `HUB_SHOT_KITTAB=0..3` picks the tab (default 3, Settings). Add `HUB_SHOT_PAD=down,down,...,shot:<name>` to watch the focus follow and LT / RT (`lt`, `rt`) page the body. `qa\uikit_shots.ps1` captures the whole set (sizes, 150 %, Light, pad runs) from a sandbox copy |

`qa\dialogs_shots.ps1 [-Bin <build output>] [-Tag before|after] [-Only regex]` captures the dialogs converted to `ui::dialog` (batch 1: emulator
prompts, Add games, Find my games, ROM hacks, cloud saves) at 1024x600, 1280x720, 1920x1080 and 1024x600 at 150 %, plus pad walks (default focus,
B, a long middle scrolling under pinned buttons), from a sandbox copy. Output and a `summary.txt` with the pad checks go to `build\qa_dialogs_out\<tag>`.
`qa\dialogs2b_shots.ps1` (same options) does the same for batch 2b: couch party setup (and its wrapping game chips), Party night / The plan,
standings, your profile editor, the remove / block question, Plan a session, the three Kanto Together dialogs, the admin question, and the
game night card's row in the Friends list (focus, A opens its dialog, B fires nothing, Dismiss). Output: `build\qa_dialogs2b_out\<tag>`.
With the focus in a dialog's scrolling middle the trace's `win=` is the middle (`<popup name>/##dlg_body_...`): match it with `-like '<name>*'`.
`qa\dialogs2a_shots.ps1` does the same for batch 2a (Customize and the shelf dialogs, a friend's game, the pack Uninstall confirm, the username prompt, the account confirms, Find my games) in both layouts. Aids it adds: `HUB_SHOT_CUSTOM_SHELVES=<n>` (made-up shelves, screenshot vault only), `HUB_SHOT_GP_UNINSTALL=<key>[:pad]` (that game shows as installed from a pack; the confirm opens by itself, or by pad with `:pad`; nothing is deleted), `HUB_SHOT_GHOST_OPEN=<title>` (as well as an index). **`HUB_SHOT_FINDGAMES` never scans this PC:** without `HUB_FINDGAMES_HOME` it shows made-up folders.

## Matrix

`powershell -NoProfile -ExecutionPolicy Bypass -File qa\shot_matrix.ps1 [-Out dir] [-Only <name-regex>] [-Sizes 1280x720,1920x1080] [-NoKey]`
captures every page/state above at 1280x720 and 1920x1080 (key pages also at 1024x600), into `build\qa_matrix_out` by default: PNGs, `contact-<size>-<n>.png` sheets, `index.txt` (page | env vars | size | file | status) and `summary.txt` (shots, failures). It refuses to start (exit 3) while a game is running (and pauses if one starts mid-run), resets the sandbox `userdata` and vault before every shot, runs one hub at a time and reports non-zero exits, missing files and all-black images (exit 1).
