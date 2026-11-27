#pragma once
/* HTTP(S) and WebSocket client for macOS (docs/MACOS.md), on NSURLSession (system TLS, proxies and certificate store).
 * Windows callers keep their existing WinHTTP code under `#if RH_WIN`; macOS branches call this instead. Part of hubbase on
 * macOS only (plat_http_mac.mm). All calls are blocking and thread-safe; never call them from the UI thread. */

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace plat {
namespace http {

struct Request {
    std::string method = "GET";
    std::string url;                                           // http:// or https://
    std::vector<std::pair<std::string, std::string>> headers;  // sent as given (User-Agent too, if set)
    std::string body;                                          // POST / PUT payload
    int connect_timeout_ms = 15000;                            // resolve + connect + TLS
    int timeout_ms = 60000;                                    // no bytes received for this long = timeout
    bool follow_redirects = true;
    /* Optional streaming: called with each chunk of the response body as it arrives (then Response::body stays empty).
     * Return false to cancel; perform() then returns false with error "cancelled". */
    std::function<bool(const char* data, size_t len)> on_data;
    /* Optional progress: bytes of body received so far, total from Content-Length (0 when unknown). */
    std::function<void(uint64_t done, uint64_t total)> on_progress;
};

struct Response {
    int status = 0;                              // HTTP status; 0 when no response arrived
    std::map<std::string, std::string> headers;  // names lowercased
    std::string body;                            // empty when Request::on_data was set
    uint64_t content_length = 0;                 // 0 when unknown
    std::string error;                           // empty on success; "timeout", "cancelled", "tls", "dns", "connect" or a message
};

/* true when a response (any status) arrived and the whole body was read. */
bool perform(const Request& req, Response* out);

}  // namespace http

namespace ws {

enum class Kind { Text, Binary, Closed, Error, Timeout };

struct Message {
    Kind kind = Kind::Error;
    std::string data;   // payload for Text / Binary; error text for Error
    int close_code = 0; // for Closed
};

/* One WebSocket connection. connect() blocks until open or failed. recv() blocks up to timeout_ms. send_*() may be called
 * from another thread than recv(). close() sends a close frame; abort() drops the connection without one. */
class Socket {
  public:
    Socket();
    ~Socket();   // abort()s if still open
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool connect(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers = {},
                 int timeout_ms = 15000, std::string* error = nullptr);   // ws:// or wss://
    bool send_text(const std::string& s);
    bool send_binary(const void* data, size_t len);
    Message recv(int timeout_ms);
    void close(int code = 1000, const std::string& reason = std::string());
    void abort();
    bool is_open() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace ws
}  // namespace plat
