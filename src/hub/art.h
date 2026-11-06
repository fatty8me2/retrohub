#pragma once
/* Cover art for the library: box art / title screens fetched on demand from the
 * libretro-thumbnails project (the same source RetroArch uses), cached on disk,
 * uploaded to SDL textures. Nothing is shipped with the app.
 *
 * Usage from the UI thread:
 *   hubart::init(renderer, hub::data_dir() + "art\\");
 *   every frame: hubart::pump();
 *   when drawing a card: if (SDL_Texture* t = hubart::get(entry, hubart::Kind::Boxart, &w, &h)) draw it,
 *                        else draw the gradient placeholder (a fetch is queued automatically). */

#include <cstdint>
#include <string>

struct SDL_Renderer;
struct SDL_Texture;

namespace hub { struct GameEntry; }

namespace hubart {

/* Boxart: the front cover (libretro-thumbnails Named_Boxarts; for Switch, the game's own icon from its .nup / an online cover).
 * Title / Snap: a title screen / in-game screenshot (libretro Named_Titles / Named_Snaps).
 * FullCover: a scan of the whole cover sheet laid flat, left to right BACK | SPINE | FRONT (GameTDB "coverfull"); only GameCube,
 * Wii U, DS and 3DS games have one, every other game gets nullptr (remembered as a miss like the other kinds). The spine strip is
 * the part between two equal-width covers: see fullcover_split() below. */
enum class Kind { Boxart, Title, Snap, FullCover };

/* Splits a FullCover image of w x h pixels into its three parts, given the front cover's expected width / height ratio (the case's
 * cover aspect). Outputs x ranges in pixels: back [0, back_x1), spine [back_x1, front_x0), front [front_x0, w). Uses equal-width back
 * and front; when the computed spine is implausible (< 2% or > 14% of w) falls back to a 5% spine. Pure, no texture needed. */
void fullcover_split(int w, int h, float front_aspect, int* back_x1, int* front_x0);

void init(SDL_Renderer* renderer, const std::string& cache_dir);
void shutdown();

/* Upload finished downloads to textures; cheap when idle. Call once per frame. */
void pump();

/* Texture if available; otherwise nullptr and a background fetch is queued
 * (at most once per game+kind per session; misses are remembered on disk so
 * a game without art isn't re-requested every launch). */
SDL_Texture* get(const hub::GameEntry& game, Kind kind, int* w, int* h);

/* Same, for catalog entries the player doesn't own (no GameEntry): by system + No-Intro name. */
SDL_Texture* get_by_name(const std::string& system_short, const std::string& nointro_name, Kind kind, int* w, int* h);

/* Cover for a friend's game you do not own: same name resolution as an owned copy (catalog, serial/crc from the key,
 * DAT title, fuzzy title) so it gets the same art; loads that system's DAT on demand even if you own nothing there. */
SDL_Texture* get_ghost(const std::string& system_short, const std::string& key, const std::string& catalog_id,
                       const std::string& title, Kind kind, int* w, int* h, const std::string& hint = "");

/* The No-Intro name already cached for a game key (names.json), "" if none. Cheap: never loads a DAT or touches the network. */
std::string cached_name(const std::string& key);

/* Average saturated colour of a texture returned by get() (packed r|g<<8|b<<16|255<<24), 0 if unknown / greyscale. */
uint32_t average_color(SDL_Texture* tex);

bool enabled();             // user setting "Download cover art" (default on)
void set_enabled(bool on);

/* ---- internal, exposed for tests / headless tools (no renderer, no worker thread) ---- */
namespace detail {
void init_headless(const std::string& cache_dir);
/* Name resolution as the worker does it (names.json -> catalog nointro -> DAT). how: cache/catalog/md5/crc/serial/title. */
bool resolve_name(const hub::GameEntry& game, std::string* name, std::string* how);
/* Same for a ghost (friend / pack-only game): hint = the pack entry name without extension, "" if none. */
bool resolve_ghost(const std::string& system_short, const std::string& key, const std::string& catalog_id,
                   const std::string& title, const std::string& hint, std::string* name, std::string* how);
/* Download (or read cache) + decode + downscale; returns dimensions. On failure *err says why
 * (a 404 is recorded in misses.json). png_path gets the cache file path. */
bool fetch_decode(const std::string& system_short, const std::string& nointro_name, Kind kind,
                  int* w, int* h, std::string* err, std::string* png_path);
} // namespace detail

} // namespace hubart
