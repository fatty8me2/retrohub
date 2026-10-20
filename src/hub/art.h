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

#include <string>

struct SDL_Renderer;
struct SDL_Texture;

namespace hub { struct GameEntry; }

namespace hubart {

enum class Kind { Boxart, Title, Snap };

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

bool enabled();             // user setting "Download cover art" (default on)
void set_enabled(bool on);

} // namespace hubart
