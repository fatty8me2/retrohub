#pragma once
/* The emulator installer's HTTPS download (emulators.cpp http_download: WinHTTP, redirects followed, status 200 only), for
 * other downloads that need to be cancelled or paused (the Hookshot build kit, hookshot_build.h). */
#include <atomic>
#include <cstdint>
#include <string>

namespace hubemu {

/* GET https://host/path into out_file (overwritten). got / total (either may be null) count bytes; total is the Content-Length
 * when the server sends one. While *pause is true the transfer waits; once *cancel is true it stops and *err = "cancelled".
 * On failure *err says why in plain words. */
bool http_get_file(const std::string& host, const std::string& path, const std::string& out_file, std::atomic<uint64_t>* got,
                   std::atomic<uint64_t>* total, const std::atomic<bool>* cancel, const std::atomic<bool>* pause, std::string* err);

}   // namespace hubemu
