# Accounts and usernames

A username is the account (see FRIENDS.md "Usernames" for the protocol and UX). This page is the naming spec. It is implemented twice and must stay identical:
- client: `src/net/username.h` (tests: `src/hub/test_username.cpp`)
- server: `BattleShip/server/cloudflare/src/usernames.js` (`normalizeUsername`, `foldKey`, `lookupKey`; tests: `test_usernames.mjs`)

## Rules (since 1.29.5)
- **Length:** 3-20 characters (Unicode code points; the server counts after NFKC, the client counts UTF-8 code points).
- **Allowed:** printable ASCII (letters, digits, space, symbols such as `! ? ' & + * ~ # @ ( ) [ ] : , = ^ $ % _ . - { } | ;`) and the Latin-1 letters U+00C0-U+00FF except `x` (U+00D7) and `/` (U+00F7). The hub font only has Basic Latin + Latin-1, so nothing else is accepted: no emoji, no other scripts (this also blocks Cyrillic/Greek look-alikes), no control, zero-width or bidi characters, no tabs/newlines.
- **Excluded ASCII:** `"` and `\` (break JSON and command lines), `<` `>` and `` ` `` (markup/shell), `/` (paths).
- **Spaces:** leading/trailing whitespace is trimmed and runs of spaces collapse to one. Only the plain space is allowed inside.
- **Shape:** no edge/doubling rule any more. Instead the uniqueness key must contain at least 3 letters/digits (`a!!!!` is rejected, `a_b-c` is fine).
- **Not allowed:** friend-code shapes (`RH-XXXX-XXXX`, also with spaces), reserved words and profanity (matched on the strong fold of the key, so `Adm1n!`, `ad min`, `Ädmin` are reserved), `qa-`/`qa_`/`qa.`/`qa ` prefixes (test accounts only).

## Uniqueness key (`foldKey`)
1. lower case
2. Latin-1 accents to base letters: `àáâãäå`->a, `æ`->ae, `ç`->c, `èéêë`->e, `ìíîï`->i, `ð`->d, `ñ`->n, `òóôõöø`->o, `ùúûü`->u, `ýÿ`->y, `þ`->th, `ß`->ss
3. remove every character that is not `[a-z0-9]`
4. digit look-alikes: `0`->o, `1`->l, `5`->s

`Kai Smith`, `kai.smith` and `Käi_Smith!` are the same name. **Key stability:** names valid under the old rules (`[A-Za-z0-9_.-]`) only ever lost `_ . -`, so steps 2-3 change nothing else for them and their key is byte-identical to the old one; existing registrations never move. Both test suites check this against a copy of the old fold (random names plus every old-alphabet character).

## Where names go (consumers)
- JSON: always escaped (`json::escape`); `"` and `\` are excluded anyway.
- Command lines: quoted with `winutil::quote_arg` (`retrorun --name`).
- Dolphin NetPlay nickname: written to `Dolphin.ini` as-is minus control characters.
- Azahar (3DS): `hubnp3ds::nickname` / `console_username` fold to ASCII (`username_ascii`), spaces become `_`, other symbols are dropped, limits 20 / 8 characters.
- OSD toasts use an ASCII-only 5x7 font: `osd::push` folds accents with `username_ascii`.
- Invite link `?h=`: filtered to letters/digits/space/`._-` and percent-encoded.
- Avatars: `username_initial` (first letter or digit, accent folded).
- No file or folder name is built from a username (data is keyed by uid).

## Old clients
They keep working: they only display names. An old client cannot *choose* a new-style name (its own validator rejects it), and its avatar initial may show a raw byte for an accented first letter. The server never sends names in a way that crashes them.
