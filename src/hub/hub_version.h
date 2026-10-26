#pragma once
// Release version (the VERSION file) lives in the CMake-generated hub_version.cpp (library hubversion), not in a
// compile definition: a definition on RetroHub/hubgameinfo_net would recompile every source on each version bump.
extern const char kHubVersion[];
#define HUB_VERSION_STR kHubVersion
