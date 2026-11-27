#include "plat/plat.h"
#include "net_internal.h"
#include "log.h"

#include <cstring>

#if RH_WIN
#include "winutil.h"
#include <windows.h>
#include <winhttp.h>

namespace hubnet {
namespace detail {

struct WsClient::Handles {
    HINTERNET session = nullptr, connect = nullptr, request = nullptr, socket = nullptr;
};

static std::wstring widen(const std::string& s) { return winutil::to_wide(s); }

WsClient::WsClient() : h_(new Handles) {}
WsClient::~WsClient() { close(); }

std::string WsClient::error() const
{
    std::lock_guard<std::mutex> l(mu_);
    return error_;
}

void WsClient::fail(const std::string& why)
{
    {
        std::lock_guard<std::mutex> l(mu_);
        error_ = why;
    }
    state_ = WsState::Failed;
    net_log("WS: %s", why.c_str());
}

void WsClient::close_handles()
{
    std::lock_guard<std::mutex> l(mu_);
    if (h_->socket) { WinHttpCloseHandle(h_->socket); h_->socket = nullptr; }
    if (h_->request) { WinHttpCloseHandle(h_->request); h_->request = nullptr; }
    if (h_->connect) { WinHttpCloseHandle(h_->connect); h_->connect = nullptr; }
    if (h_->session) { WinHttpCloseHandle(h_->session); h_->session = nullptr; }
}

bool WsClient::connect(const std::string& url)
{
    close();
    stop_ = false;
    close_code_ = 0;
    {
        std::lock_guard<std::mutex> l(mu_);
        error_.clear();
        text_.clear();
        bin_.clear();
    }
    state_ = WsState::Connecting;
    thread_ = std::thread(&WsClient::run, this, url);
    return true;
}

void WsClient::close()
{
    stop_ = true;
    /* Send our close frame but never wait for the server's answer: WinHttpWebSocketClose blocks (10 s+, on the UI thread) when the
     * server is silent or already closed and the receive thread consumed its frame (the v1.29.2 hub hangs). Shutdown runs outside mu_
     * (it is a network send); only close() ever closes the handles and it runs on the owner thread, so the handle stays valid here.
     * Closing the handles then aborts the pending receive, so the join below is immediate. */
    HINTERNET sock = nullptr;
    {
        std::lock_guard<std::mutex> l(mu_);
        sock = h_->socket;
    }
    if (sock && state_ == WsState::Open) WinHttpWebSocketShutdown(sock, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    close_handles();
    if (thread_.joinable()) thread_.join();
    state_ = WsState::Closed;
}

void WsClient::drop_abruptly()
{
    close_handles();   // no WebSocket close frame: the receive thread sees an error and the state becomes Failed
}

void WsClient::run(std::string url)
{
    bool secure = false;
    std::string rest;
    if (url.rfind("wss://", 0) == 0) { secure = true; rest = url.substr(6); }
    else if (url.rfind("ws://", 0) == 0) rest = url.substr(5);
    else { fail("server URL must start with ws:// or wss://"); return; }
    size_t slash = rest.find('/');
    std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    std::string path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    std::string host = hostport;
    INTERNET_PORT port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        port = (INTERNET_PORT)atoi(hostport.substr(colon + 1).c_str());
    }

    HINTERNET session = WinHttpOpen(L"RetroHub/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { fail("WinHttpOpen failed"); return; }
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 0);
    HINTERNET connect = WinHttpConnect(session, widen(host).c_str(), port, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", widen(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    {
        /* stop_ is checked under mu_: close() sets it before close_handles() takes mu_, so either we see it here and never start the
         * (blocking) connect, or the handles are published first and close_handles() aborts the connect below. */
        std::lock_guard<std::mutex> l(mu_);
        if (stop_) {
            if (request) WinHttpCloseHandle(request);
            if (connect) WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return;
        }
        h_->session = session;
        h_->connect = connect;
        h_->request = request;
    }
    if (!connect || !request) { fail("couldn't reach " + host); return; }

    if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
        !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        if (!stop_) fail("couldn't connect to " + host + " (error " + std::to_string(GetLastError()) + ")");
        return;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr);
    if (status != 101) { fail("server answered HTTP " + std::to_string(status)); return; }
    HINTERNET socket = WinHttpWebSocketCompleteUpgrade(request, 0);
    if (!socket) { fail("WebSocket upgrade failed"); return; }
    {
        std::lock_guard<std::mutex> l(mu_);
        WinHttpCloseHandle(h_->request);
        h_->request = nullptr;
        h_->socket = socket;
    }
    state_ = WsState::Open;

    std::vector<uint8_t> message;
    unsigned char buffer[4096];
    while (!stop_) {
        /* re-read the handle each time: close() nulls it (under mu_) before closing it, and sets stop_ first, so a receive either
         * sees null / stop_ and leaves, or is pending on a live handle and gets aborted by the close. A new handle can only be
         * created by connect(), which joins this thread first. */
        {
            std::lock_guard<std::mutex> l(mu_);
            socket = h_->socket;
        }
        if (!socket || stop_) break;
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        DWORD err = WinHttpWebSocketReceive(socket, buffer, sizeof(buffer), &read, &type);
        if (err != ERROR_SUCCESS) {
            if (!stop_) fail("connection to the server was lost");
            break;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            if (!stop_) {
                USHORT code = 0;
                char reason[128] = {};
                DWORD rl = 0;
                WinHttpWebSocketQueryCloseStatus(socket, &code, reason, sizeof(reason) - 1, &rl);
                close_code_ = code;
                fail(std::string("server closed the connection") + (rl ? std::string(": ") + reason : ""));
            }
            break;
        }
        message.insert(message.end(), buffer, buffer + read);
        if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)
            continue;
        {
            std::lock_guard<std::mutex> l(mu_);
            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
                text_.emplace_back(message.begin(), message.end());
            else if (bin_.size() < 8192)
                bin_.push_back(message);
        }
        if (void* e = wake_.load()) SetEvent((HANDLE)e);
        message.clear();
    }
}

bool WsClient::send_text(const std::string& text)
{
    if (state_ != WsState::Open) return false;
    std::lock_guard<std::mutex> l(mu_);
    if (!h_->socket) return false;
    return WinHttpWebSocketSend(h_->socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (void*)text.data(),
                                (DWORD)text.size()) == ERROR_SUCCESS;
}

bool WsClient::send_binary(const void* data, int size)
{
    if (state_ != WsState::Open) return false;
    std::lock_guard<std::mutex> l(mu_);
    if (!h_->socket) return false;
    DWORD e = WinHttpWebSocketSend(h_->socket, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, (void*)data, (DWORD)size);
    if (e != ERROR_SUCCESS) net_log("WS send error %lu", e);
    return e == ERROR_SUCCESS;
}

bool WsClient::poll_text(std::string* out)
{
    std::lock_guard<std::mutex> l(mu_);
    if (text_.empty()) return false;
    *out = std::move(text_.front());
    text_.pop_front();
    return true;
}

bool WsClient::poll_binary(std::vector<uint8_t>* out)
{
    std::lock_guard<std::mutex> l(mu_);
    if (bin_.empty()) return false;
    *out = std::move(bin_.front());
    bin_.pop_front();
    return true;
}


} // namespace detail
} // namespace hubnet

#else   // ---------------------------------------------------------------- macOS: plat::ws::Socket (NSURLSession)

#include "plat/plat_http.h"
#include "plat/plat_wake.h"

#include <chrono>

namespace hubnet {
namespace detail {

/* plat::http error tag -> the stable text prefix classify_ws_error() (ws_error_text.h) recognises, so the player sees the same
 * message as on Windows for the same situation:
 *   before the socket opened:
 *     "timeout"              -> "couldn't connect to <host> (timed out)"            Unreachable  (Windows: connect/send times out)
 *     "dns"                  -> "couldn't reach <host>"                              Unreachable  (Windows: host unresolvable)
 *     "connect"              -> "couldn't connect to <host> (connection refused)"    Unreachable  (Windows: connect fails)
 *     "tls"                  -> "couldn't connect to <host> (TLS error)"             Unreachable  (Windows: certificate failure)
 *     anything else          -> "WebSocket upgrade failed: <text>"                   Blocked      (non-101 answer / proxy; NSURLSession
 *                                                                                                  hides the HTTP status, so a 429/5xx
 *                                                                                                  that Windows reports as ServerBusy is Blocked here)
 *   after it opened:
 *     transport error        -> "connection to the server was lost"                  Dropped
 *     server close frame     -> "server closed the connection"                       Dropped (+ close_code(); the reason text is not
 *                                                                                                  exposed by plat::ws, so none is appended) */
static std::string connect_error_text(const std::string& host, const std::string& tag)
{
    if (tag == "timeout") return "couldn't connect to " + host + " (timed out)";
    if (tag == "dns") return "couldn't reach " + host;
    if (tag == "connect") return "couldn't connect to " + host + " (connection refused)";
    if (tag == "tls") return "couldn't connect to " + host + " (TLS error)";
    return "WebSocket upgrade failed: " + tag;
}

struct WsClient::Handles {
    std::shared_ptr<plat::ws::Socket> sock;   // guarded by mu_ (replaced only by connect(), after the old thread is joined)
    std::atomic<bool> finished{true};         // run() has returned
    std::atomic<bool> dropped{false};         // drop_abruptly() was called: report it as a lost connection, not a local close
};

WsClient::WsClient() : h_(new Handles) {}
WsClient::~WsClient() { close(); }

std::string WsClient::error() const
{
    std::lock_guard<std::mutex> l(mu_);
    return error_;
}

void WsClient::fail(const std::string& why)
{
    {
        std::lock_guard<std::mutex> l(mu_);
        error_ = why;
    }
    state_ = WsState::Failed;
    net_log("WS: %s", why.c_str());
}

void WsClient::close_handles()
{
    std::shared_ptr<plat::ws::Socket> s;
    {
        std::lock_guard<std::mutex> l(mu_);
        s = h_->sock;
    }
    if (s) s->abort();
}

bool WsClient::connect(const std::string& url)
{
    close();
    stop_ = false;
    close_code_ = 0;
    {
        std::lock_guard<std::mutex> l(mu_);
        error_.clear();
        text_.clear();
        bin_.clear();
        h_->sock = std::make_shared<plat::ws::Socket>();
    }
    h_->dropped = false;
    h_->finished = false;
    state_ = WsState::Connecting;
    thread_ = std::thread(&WsClient::run, this, url);
    return true;
}

void WsClient::close()
{
    stop_ = true;
    std::shared_ptr<plat::ws::Socket> s;
    {
        std::lock_guard<std::mutex> l(mu_);
        s = h_->sock;
    }
    /* Send our close frame but never wait for the server's answer: the receive thread lingers a few hundred ms so the frame can go
     * out, then the socket is aborted (repeatedly, in case the connect had not created its task yet) and the thread joined. */
    if (s && state_ == WsState::Open) s->close(1000);
    auto t0 = std::chrono::steady_clock::now();
    if (thread_.joinable()) {
        while (!h_->finished) {
            bool grace = state_ == WsState::Open &&
                         std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(300);
            if (!grace && s) s->abort();
            plat::sleep_ms(5);
        }
        thread_.join();
    }
    if (s) s->abort();
    state_ = WsState::Closed;
}

void WsClient::drop_abruptly()
{
    h_->dropped = true;
    close_handles();   // no WebSocket close frame: the receive thread sees the end and the state becomes Failed
}

void WsClient::run(std::string url)
{
    struct Done { std::atomic<bool>& f; ~Done() { f = true; } } done{h_->finished};

    bool secure = false;
    std::string rest;
    if (url.rfind("wss://", 0) == 0) { secure = true; rest = url.substr(6); }
    else if (url.rfind("ws://", 0) == 0) rest = url.substr(5);
    else { fail("server URL must start with ws:// or wss://"); return; }
    (void)secure;
    size_t slash = rest.find('/');
    std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    std::string host = hostport;
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) host = hostport.substr(0, colon);

    std::shared_ptr<plat::ws::Socket> sock;
    {
        std::lock_guard<std::mutex> l(mu_);
        sock = h_->sock;
    }
    if (stop_) return;
    std::string err;
    bool ok = sock->connect(url, {{"User-Agent", "RetroHub/1.0"}}, 5000, &err);
    if (stop_) return;
    if (!ok) { fail(connect_error_text(host, err)); return; }
    state_ = WsState::Open;

    auto stop_at = std::chrono::steady_clock::time_point::max();
    while (true) {
        if (stop_ && stop_at == std::chrono::steady_clock::time_point::max())
            stop_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);   // let our close frame flush
        if (std::chrono::steady_clock::now() >= stop_at) break;
        plat::ws::Message m = sock->recv(stop_ ? 20 : 100);
        if (m.kind == plat::ws::Kind::Timeout) continue;
        if (m.kind == plat::ws::Kind::Closed || m.kind == plat::ws::Kind::Error) {
            if (stop_) break;
            if (m.kind == plat::ws::Kind::Closed && !h_->dropped) {
                close_code_ = m.close_code;
                fail("server closed the connection");
            } else {
                fail("connection to the server was lost");
            }
            break;
        }
        if (stop_) continue;
        {
            std::lock_guard<std::mutex> l(mu_);
            if (m.kind == plat::ws::Kind::Text)
                text_.push_back(std::move(m.data));
            else if (bin_.size() < 8192)
                bin_.emplace_back(m.data.begin(), m.data.end());
        }
        if (void* w = wake_.load()) static_cast<plat::Waker*>(w)->signal();
    }
}

bool WsClient::send_text(const std::string& text)
{
    if (state_ != WsState::Open) return false;
    std::shared_ptr<plat::ws::Socket> s;
    {
        std::lock_guard<std::mutex> l(mu_);
        s = h_->sock;
    }
    return s && s->send_text(text);
}

bool WsClient::send_binary(const void* data, int size)
{
    if (state_ != WsState::Open) return false;
    std::shared_ptr<plat::ws::Socket> s;
    {
        std::lock_guard<std::mutex> l(mu_);
        s = h_->sock;
    }
    if (!s) return false;
    bool ok = s->send_binary(data, (size_t)size);
    if (!ok) net_log("WS send error");
    return ok;
}

bool WsClient::poll_text(std::string* out)
{
    std::lock_guard<std::mutex> l(mu_);
    if (text_.empty()) return false;
    *out = std::move(text_.front());
    text_.pop_front();
    return true;
}

bool WsClient::poll_binary(std::vector<uint8_t>* out)
{
    std::lock_guard<std::mutex> l(mu_);
    if (bin_.empty()) return false;
    *out = std::move(bin_.front());
    bin_.pop_front();
    return true;
}

} // namespace detail
} // namespace hubnet

#endif
