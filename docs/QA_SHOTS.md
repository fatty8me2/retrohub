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
| `HUB_SHOT_DETAILS` | game key, e.g. `fake:2`, `switch:0400486024dec000` | open that game's page; a key not in the library exits 2 |
| `HUB_SHOT_TAB` | 0-3 | tab of the game page (0 About, 1 Saves, ...) |
| `HUB_SHOT_SEARCH`, `_LIBFILTER` (`fav;group;genre=X`), `_LIBVIEW` (`grid\|list\|compact\|showcase`), `_LIBCOL`, `_SORT`, `_CASES*`, `HUB_START_VIEW=all`, `HUB_START_SYS` | | library views and filters |
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
| `HUB_SHOT_CLOUD_DIALOG` | `conflict`, `lease`, `checking`, `pulled`, `offline` | launch-time cloud dialogs / notices |
| `HUB_SHOT_CLOUD_CODE` | any | reveal the recovery code |
| `HUB_SHOT_HISTORY` | `<system>/<key>` | save history of a game on the Saves page |

## Online, friends, notifications

| Variable | Values | What it shows |
| --- | --- | --- |
| **`HUB_SHOT_OFFLINE`** | `home` (the Play Online page with the friends panel "offline") or `room` (a room that is still "Connecting...") | Play Online with no network (headless never connects anyway) |
| `HUB_SHOT_FRIENDS` | any | friends panel open |
| `HUB_SHOT_FAKE_FRIENDS` (+ `_CHAT`, `_BELL`, `_PROFILES`, `_LONGNAMES`, ...) | | fake friends, chat, notifications |
| **`HUB_SHOT_UPDATE_NOTES`** | comma list of `ready`, `failed`, `postponed`, `rollback`, `interrupted` | adds those update notes **and opens the Notifications list (bell panel) with the friends panel** so the notes are readable. `HUB_SHOT_BELL=0` keeps it closed (badge only) |
| `HUB_SHOT_BELL` | any | open the Notifications list (with `HUB_SHOT_FAKE_FRIENDS`) |
| `HUB_SHOT_UPDATE` | any | also draw the update prompt |
| `HUB_SHOT_WELCOME` (`_FROM`, `_TO`, `_T`, `_OWNER`) | | the "What's new" welcome card |
| `HUB_SHOT_FAKE_ROOM` (+ `_ROOM_GAME`, `_READY`, `_PICKER`, `_PICKER_Q`, `_HOSTCHANGE`, `_PLAY_AGAIN`, `_LOCALS2`, `_PICKS`) | | a fake room |
| `HUB_SHOT_REJOIN` | text | the disconnected Rejoin strip |
| `HUB_SHOT_ACCESS` | `pending`, `unavailable`, `setup`, `setup-offline` | whole-app lock screens |
| `HUB_SHOT_ADMIN` | `1`, `pending`, `banned`, `detail`, `confirm`, `settings` | owner dashboard (fake data) |
| `HUB_SHOT_NET` | `1` | allows the friends client, but only against a loopback server |
| `HUB_SHOT_PARTY`, `_BRACKET`, `_KT`, `_CHALLENGE`, `_WRAPPED`, `_USERNAME*`, `_WIZARD`, `_SURPRISE` | | Plan 4 / account / first-run aids (grep the name) |

## Emulator dialogs

| Variable | Values | What it shows |
| --- | --- | --- |
| `HUB_SHOT_EMU` | `azahar`, `cemu` (download prompt), `lichiiupdate`, `fritchii`, `failed`, `unzip`, `ps2bios`, `switchkeys`, `hookshotav1`, `missingrom`, `hidden`, `ps1bios`, `resume` | emulator dialogs |
| **`HUB_SHOT_EMU=working[:<emu>[:<percent>]]`** | `<emu>` = `azahar`, `cemu`, `dolphin`, `pcsx2`, `ppsspp`, `lichii`, `fritchii` (default `azahar`), percent 0-100 (default 42) | **the install-in-progress dialog frozen at a fixed percentage** (no download runs). Unknown emulator exits 2 |
| `HUB_SHOT_FRITCHII_PIN` | any | a fake Fritchii pin (96 MB) |
| `HUB_SHOT_QM*` | | quick menu aids |
| `HUB_SHOT_CTL_*`, `HUB_SHOT_FAKE_PADS=n` | | Controllers page aids |

## Matrix

`powershell -NoProfile -ExecutionPolicy Bypass -File qa\shot_matrix.ps1 [-Out dir] [-Only <name-regex>] [-Sizes 1280x720,1920x1080] [-NoKey]`
captures every page/state above at 1280x720 and 1920x1080 (key pages also at 1024x600), into `build\qa_matrix_out` by default: PNGs, `contact-<size>-<n>.png` sheets, `index.txt` (page | env vars | size | file | status) and `summary.txt` (shots, failures). It refuses to start (exit 3) while a game is running (and pauses if one starts mid-run), resets the sandbox `userdata` and vault before every shot, runs one hub at a time and reports non-zero exits, missing files and all-black images (exit 1).
