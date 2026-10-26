#pragma once
/* Network side of game info (WinHTTP). Kept apart from the pure logic in common/gameinfo.* so tests never need a socket.
 *  - http_get(): descriptive User-Agent per Wikimedia etiquette, short timeouts, ~1 request/second spacing.
 *  - fixtures: when a fixture directory is set (HUB_GAMEINFO_FIXTURES or set_fixture_dir), requests are answered ONLY from
 *    <dir>/<fnv1a64(url)>.json (or <hash>.404 for "not found"); a missing fixture is a transport failure. No live network then. */

#include <string>

namespace gameinfo_net {

void set_fixture_dir(const std::string& dir);   // "" = live network
std::string fixture_dir();                      // env HUB_GAMEINFO_FIXTURES unless set_fixture_dir() was called
std::string fixture_name(const std::string& url);   // "<16 hex>"

/* Same contract as gameinfo::Fetcher. Thread-safe (serialized, rate limited). */
bool fetch(const std::string& url, std::string* body, int* status);
/* Live request only (no fixtures); used by the recorder tool. */
bool http_get(const std::string& url, std::string* body, int* status);

std::string user_agent();   // "RetroHub/<version> (https://github.com/fatty8me2/retrohub)"

} // namespace gameinfo_net
