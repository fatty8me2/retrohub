#pragma once
/* Signed HTTP client used by the v2 cloud engine (WinHTTP). One instance = one connection session; not thread-safe. */
#include "cloud.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace hub::saves {

struct HttpReply {
    int status = 0;                                   // 0 = network failure / timeout (error says why)
    std::string error;
    std::string body;                                 // only when no sink consumed it (non-2xx bodies are always here)
    std::map<std::string, std::string> headers;       // lowercase names: x-rh-sha x-rh-plain-sha x-rh-enc x-rh-size retry-after content-range etag
};

class CloudHttp {
public:
    /* connect_ms bounds name resolution and connect; io_ms bounds send and receive of each step. */
    CloudHttp(const CloudConfig& cfg, int connect_ms, int io_ms);
    ~CloudHttp();
    CloudHttp(const CloudHttp&) = delete;
    CloudHttp& operator=(const CloudHttp&) = delete;
    bool ok() const;
    /* One attempt (the engine owns retry policy). JSON POST bodies are signed through X-RH-Sha automatically. A 401
     * stale-timestamp reply with serverTime corrects the clock once and the request is re-sent. headers are "Name: value" lines
     * (X-RH-Sha for blob PUT = sha256 of the stored bytes). sink != null streams 2xx bodies into it. */
    HttpReply call(const std::string& method, const std::string& path_and_query, const std::vector<std::string>& headers = {},
                   const std::string& body = std::string(), const std::function<bool(const char*, size_t)>& sink = nullptr,
                   const std::function<std::vector<std::string>()>& headers_fn = nullptr);
private:
    struct Impl;
    Impl* p_;
};

/* File / compression helpers shared with the engine (implemented in cloud.cpp). */
std::string sha256_hex_file(const std::string& path_utf8);
long long file_mtime_unix(const std::string& path_utf8);            // 0 when unreadable
bool set_file_mtime_unix(const std::string& path_utf8, long long t);
bool deflate_raw(const std::string& in, std::string* out);
bool inflate_raw(const std::string& in, std::string* out, size_t max_out);

} // namespace hub::saves
