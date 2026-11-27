// plat_http on NSURLSession (docs/MACOS.md). Objective-C++, ARC on.
#include "plat_http.h"

#import <Foundation/Foundation.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace {

using Clock = std::chrono::steady_clock;

NSString* ns(const std::string& s) {
    NSString* r = [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding];
    return r ? r : @"";
}

std::string lowered(NSString* s) {
    NSString* l = [s lowercaseString];
    const char* u = l.UTF8String;
    return u ? u : "";
}

std::string map_error(NSError* e) {
    if (!e) return std::string();
    if ([e.domain isEqualToString:NSURLErrorDomain]) {
        switch (e.code) {
        case NSURLErrorTimedOut: return "timeout";
        case NSURLErrorCancelled: return "cancelled";
        case NSURLErrorSecureConnectionFailed:
        case NSURLErrorServerCertificateHasBadDate:
        case NSURLErrorServerCertificateUntrusted:
        case NSURLErrorServerCertificateHasUnknownRoot:
        case NSURLErrorServerCertificateNotYetValid:
        case NSURLErrorClientCertificateRejected:
        case NSURLErrorClientCertificateRequired: return "tls";
        case NSURLErrorCannotFindHost:
        case NSURLErrorDNSLookupFailed: return "dns";
        case NSURLErrorCannotConnectToHost:
        case NSURLErrorNetworkConnectionLost:
        case NSURLErrorNotConnectedToInternet: return "connect";
        default: break;
        }
    }
    const char* u = e.localizedDescription.UTF8String;
    return (u && *u) ? u : "error";
}

NSMutableURLRequest* make_request(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                                  int timeout_ms) {
    NSURL* u = [NSURL URLWithString:ns(url)];
    if (!u) return nil;
    NSMutableURLRequest* r = [NSMutableURLRequest requestWithURL:u];
    r.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    r.timeoutInterval = std::max(1, timeout_ms) / 1000.0;
    for (auto& h : headers) [r setValue:ns(h.second) forHTTPHeaderField:ns(h.first)];
    return r;
}

NSURLSessionConfiguration* make_config() {
    NSURLSessionConfiguration* c = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    c.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    c.URLCache = nil;
    c.waitsForConnectivity = NO;
    return c;
}

// ---------------------------------------------------------------- HTTP

struct HttpState {
    std::mutex m;
    std::condition_variable cv;
    const plat::http::Request* req = nullptr;
    plat::http::Response* out = nullptr;
    bool got_response = false;
    bool done = false;
    bool cb_cancel = false;
    bool watchdog_timeout = false;
    std::string error;
    uint64_t received = 0;
    Clock::time_point last_activity = Clock::now();
};

}  // namespace

@interface HttpBox : NSObject
@property(nonatomic) std::shared_ptr<HttpState> st;
@end
@implementation HttpBox
@end

@interface HttpDelegate : NSObject <NSURLSessionDataDelegate>
- (void)add:(NSURLSessionTask*)t state:(std::shared_ptr<HttpState>)s;
@end

@implementation HttpDelegate {
    NSMutableDictionary<NSNumber*, HttpBox*>* _map;
}
- (instancetype)init {
    if ((self = [super init])) _map = [NSMutableDictionary dictionary];
    return self;
}
- (void)add:(NSURLSessionTask*)t state:(std::shared_ptr<HttpState>)s {
    HttpBox* b = [HttpBox new];
    b.st = s;
    @synchronized(self) { _map[@(t.taskIdentifier)] = b; }
}
- (std::shared_ptr<HttpState>)stateFor:(NSURLSessionTask*)t {
    @synchronized(self) { return _map[@(t.taskIdentifier)].st; }
}

- (void)URLSession:(NSURLSession*)session
              task:(NSURLSessionTask*)task
willPerformHTTPRedirection:(NSHTTPURLResponse*)response
        newRequest:(NSURLRequest*)request
 completionHandler:(void (^)(NSURLRequest*))completionHandler {
    auto s = [self stateFor:task];
    completionHandler((s && s->req && s->req->follow_redirects) ? request : nil);
}

- (void)URLSession:(NSURLSession*)session
          dataTask:(NSURLSessionDataTask*)task
didReceiveResponse:(NSURLResponse*)response
 completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
    auto s = [self stateFor:task];
    if (s) {
        std::lock_guard<std::mutex> lk(s->m);
        if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
            NSHTTPURLResponse* h = (NSHTTPURLResponse*)response;
            s->out->status = (int)h.statusCode;
            [h.allHeaderFields enumerateKeysAndObjectsUsingBlock:^(id k, id v, BOOL*) {
                const char* vu = [[v description] UTF8String];
                s->out->headers[lowered([k description])] = vu ? vu : "";
            }];
        }
        s->out->content_length = response.expectedContentLength > 0 ? (uint64_t)response.expectedContentLength : 0;
        s->got_response = true;
        s->last_activity = Clock::now();
        s->cv.notify_all();
    }
    completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveData:(NSData*)data {
    auto s = [self stateFor:task];
    if (!s) return;
    __block bool keep = true;
    [data enumerateByteRangesUsingBlock:^(const void* bytes, NSRange r, BOOL* stop) {
        if (s->req->on_data) {
            if (!s->req->on_data((const char*)bytes, r.length)) { keep = false; *stop = YES; }
        } else {
            std::lock_guard<std::mutex> lk(s->m);
            s->out->body.append((const char*)bytes, r.length);
        }
    }];
    uint64_t total;
    {
        std::lock_guard<std::mutex> lk(s->m);
        s->received += data.length;
        s->last_activity = Clock::now();
        total = s->out->content_length;
        if (!keep) s->cb_cancel = true;
    }
    if (!keep) { [task cancel]; return; }
    if (s->req->on_progress) s->req->on_progress(s->received, total);
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    auto s = [self stateFor:task];
    if (!s) return;
    {
        std::lock_guard<std::mutex> lk(s->m);
        if (s->cb_cancel) s->error = "cancelled";
        else if (s->watchdog_timeout) s->error = "timeout";
        else s->error = map_error(error);
        s->done = true;
        s->cv.notify_all();
    }
    @synchronized(self) { [_map removeObjectForKey:@(task.taskIdentifier)]; }
}
@end

namespace {
NSURLSession* http_session(HttpDelegate** del) {
    static HttpDelegate* d = [HttpDelegate new];
    static NSURLSession* sess = [NSURLSession sessionWithConfiguration:make_config() delegate:d delegateQueue:nil];
    *del = d;
    return sess;
}
}  // namespace

namespace plat {
namespace http {

bool perform(const Request& req, Response* out) {
    Response dummy;
    if (!out) out = &dummy;
    *out = Response();
    @autoreleasepool {
        NSMutableURLRequest* r = make_request(req.url, req.headers, req.timeout_ms);
        if (!r) { out->error = "bad url"; return false; }
        r.HTTPMethod = ns(req.method);
        if (!req.body.empty()) r.HTTPBody = [NSData dataWithBytes:req.body.data() length:req.body.size()];

        HttpDelegate* del = nil;
        NSURLSession* sess = http_session(&del);
        auto st = std::make_shared<HttpState>();
        st->req = &req;
        st->out = out;
        NSURLSessionDataTask* task = [sess dataTaskWithRequest:r];
        [del add:task state:st];
        auto start = Clock::now();
        st->last_activity = start;
        [task resume];

        const auto idle = std::chrono::milliseconds(std::max(1, req.timeout_ms));
        const auto conn = std::chrono::milliseconds(std::max(1, req.connect_timeout_ms));
        std::unique_lock<std::mutex> lk(st->m);
        bool cancelled = false;
        while (!st->done) {
            if (cancelled) { st->cv.wait(lk); continue; }
            auto deadline = st->last_activity + idle;
            if (!st->got_response) deadline = std::min(deadline, start + conn);
            if (st->cv.wait_until(lk, deadline) == std::cv_status::timeout && !st->done && Clock::now() >= deadline) {
                // Re-check against the (possibly refreshed) activity time before declaring a timeout.
                auto d2 = st->last_activity + idle;
                if (!st->got_response) d2 = std::min(d2, start + conn);
                if (Clock::now() >= d2) {
                    st->watchdog_timeout = true;
                    cancelled = true;
                    lk.unlock();
                    [task cancel];
                    lk.lock();
                }
            }
        }
        out->error = st->error;
        bool ok = st->error.empty();
        if (!ok && !st->got_response) out->status = 0;
        return ok;
    }
}

}  // namespace http

// ---------------------------------------------------------------- WebSocket

namespace ws {
namespace {
struct WsState {
    std::mutex m;
    std::condition_variable cv;
    bool opened = false;
    bool local_closed = false;
    bool has_terminal = false;
    Message terminal;
    std::deque<Message> q;

    void set_terminal(Kind k, int code, const std::string& text) {   // caller holds m
        if (has_terminal) return;
        has_terminal = true;
        terminal.kind = k;
        terminal.close_code = code;
        terminal.data = text;
        cv.notify_all();
    }
};
}  // namespace
}  // namespace ws
}  // namespace plat

@interface WsDelegate : NSObject <NSURLSessionWebSocketDelegate>
@property(nonatomic) std::shared_ptr<plat::ws::WsState> st;
@end

static void ws_arm(std::shared_ptr<plat::ws::WsState> s, NSURLSessionWebSocketTask* task) {
    [task receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage* msg, NSError* err) {
        if (err) {
            std::lock_guard<std::mutex> lk(s->m);
            NSInteger cc = task.closeCode;
            if (cc != NSURLSessionWebSocketCloseCodeInvalid) {
                s->set_terminal(plat::ws::Kind::Closed, (int)cc, "");
            } else if (s->local_closed) {
                s->set_terminal(plat::ws::Kind::Closed, 1000, "");
            } else {
                s->set_terminal(plat::ws::Kind::Error, 0, map_error(err));
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lk(s->m);
            plat::ws::Message m;
            if (msg.type == NSURLSessionWebSocketMessageTypeString) {
                m.kind = plat::ws::Kind::Text;
                const char* u = msg.string.UTF8String;
                m.data = u ? u : "";
            } else {
                m.kind = plat::ws::Kind::Binary;
                m.data.assign((const char*)msg.data.bytes, msg.data.length);
            }
            s->q.push_back(std::move(m));
            s->cv.notify_all();
        }
        ws_arm(s, task);
    }];
}

@implementation WsDelegate
- (void)URLSession:(NSURLSession*)session
          webSocketTask:(NSURLSessionWebSocketTask*)task
    didOpenWithProtocol:(NSString*)protocol {
    auto s = self.st;
    {
        std::lock_guard<std::mutex> lk(s->m);
        s->opened = true;
        s->cv.notify_all();
    }
    ws_arm(s, task);
}
- (void)URLSession:(NSURLSession*)session
       webSocketTask:(NSURLSessionWebSocketTask*)task
    didCloseWithCode:(NSURLSessionWebSocketCloseCode)code
              reason:(NSData*)reason {
    auto s = self.st;
    std::lock_guard<std::mutex> lk(s->m);
    s->set_terminal(plat::ws::Kind::Closed, (int)code, "");
}
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
    auto s = self.st;
    std::lock_guard<std::mutex> lk(s->m);
    if (error) {
        if (s->local_closed && error.code == NSURLErrorCancelled) s->set_terminal(plat::ws::Kind::Closed, 1000, "");
        else s->set_terminal(plat::ws::Kind::Error, 0, map_error(error));
    } else {
        s->set_terminal(plat::ws::Kind::Closed, (int)((NSURLSessionWebSocketTask*)task).closeCode, "");
    }
}
@end

struct plat::ws::Socket::Impl {
    std::shared_ptr<WsState> st = std::make_shared<WsState>();
    NSURLSession* session = nil;
    NSURLSessionWebSocketTask* task = nil;
    std::mutex om;   // guards session / task
};

namespace plat {
namespace ws {

Socket::Socket() : p_(new Impl) {}
Socket::~Socket() { abort(); }

bool Socket::connect(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers, int timeout_ms,
                     std::string* error) {
    auto fail = [&](const std::string& e) { if (error) *error = e; return false; };
    @autoreleasepool {
        {
            std::lock_guard<std::mutex> lk(p_->om);
            if (p_->task) return fail("already connected");
        }
        NSMutableURLRequest* r = make_request(url, headers, timeout_ms);
        if (!r) return fail("bad url");
        WsDelegate* d = [WsDelegate new];
        d.st = p_->st;
        NSURLSession* sess = [NSURLSession sessionWithConfiguration:make_config() delegate:d delegateQueue:nil];
        NSURLSessionWebSocketTask* t = [sess webSocketTaskWithRequest:r];
        t.maximumMessageSize = 16 * 1024 * 1024;
        {
            std::lock_guard<std::mutex> lk(p_->om);
            p_->session = sess;
            p_->task = t;
        }
        [t resume];
        auto st = p_->st;
        std::unique_lock<std::mutex> lk(st->m);
        bool ok = st->cv.wait_for(lk, std::chrono::milliseconds(std::max(1, timeout_ms)),
                                  [&] { return st->opened || st->has_terminal; });
        if (ok && st->opened) return true;
        std::string e = (ok && st->has_terminal) ? st->terminal.data : std::string("timeout");
        if (ok && st->has_terminal && e.empty()) e = "closed";
        lk.unlock();
        abort();
        return fail(e);
    }
}

bool Socket::send_text(const std::string& s) {
    NSURLSessionWebSocketTask* t;
    { std::lock_guard<std::mutex> lk(p_->om); t = p_->task; }
    if (!t || !is_open()) return false;
    @autoreleasepool {
        auto done = std::make_shared<std::pair<std::mutex, std::condition_variable>>();
        auto res = std::make_shared<int>(-1);
        NSURLSessionWebSocketMessage* m = [[NSURLSessionWebSocketMessage alloc] initWithString:ns(s)];
        [t sendMessage:m completionHandler:^(NSError* e) {
            std::lock_guard<std::mutex> lk(done->first);
            *res = e ? 0 : 1;
            done->second.notify_all();
        }];
        std::unique_lock<std::mutex> lk(done->first);
        done->second.wait(lk, [&] { return *res >= 0; });
        return *res == 1;
    }
}

bool Socket::send_binary(const void* data, size_t len) {
    NSURLSessionWebSocketTask* t;
    { std::lock_guard<std::mutex> lk(p_->om); t = p_->task; }
    if (!t || !is_open()) return false;
    @autoreleasepool {
        auto done = std::make_shared<std::pair<std::mutex, std::condition_variable>>();
        auto res = std::make_shared<int>(-1);
        NSURLSessionWebSocketMessage* m = [[NSURLSessionWebSocketMessage alloc] initWithData:[NSData dataWithBytes:data length:len]];
        [t sendMessage:m completionHandler:^(NSError* e) {
            std::lock_guard<std::mutex> lk(done->first);
            *res = e ? 0 : 1;
            done->second.notify_all();
        }];
        std::unique_lock<std::mutex> lk(done->first);
        done->second.wait(lk, [&] { return *res >= 0; });
        return *res == 1;
    }
}

Message Socket::recv(int timeout_ms) {
    auto st = p_->st;
    std::unique_lock<std::mutex> lk(st->m);
    st->cv.wait_for(lk, std::chrono::milliseconds(std::max(0, timeout_ms)), [&] { return !st->q.empty() || st->has_terminal; });
    if (!st->q.empty()) {
        Message m = std::move(st->q.front());
        st->q.pop_front();
        return m;
    }
    if (st->has_terminal) return st->terminal;
    Message m;
    m.kind = Kind::Timeout;
    return m;
}

void Socket::close(int code, const std::string& reason) {
    NSURLSessionWebSocketTask* t;
    { std::lock_guard<std::mutex> lk(p_->om); t = p_->task; }
    if (!t) return;
    { std::lock_guard<std::mutex> lk(p_->st->m); p_->st->local_closed = true; }
    @autoreleasepool {
        NSData* rd = reason.empty() ? nil : [NSData dataWithBytes:reason.data() length:reason.size()];
        [t cancelWithCloseCode:(NSURLSessionWebSocketCloseCode)code reason:rd];
    }
}

void Socket::abort() {
    NSURLSession* s;
    NSURLSessionWebSocketTask* t;
    {
        std::lock_guard<std::mutex> lk(p_->om);
        s = p_->session;
        t = p_->task;
    }
    if (!t) return;
    { std::lock_guard<std::mutex> lk(p_->st->m); p_->st->local_closed = true; }
    [t cancel];
    [s invalidateAndCancel];
}

bool Socket::is_open() const {
    std::lock_guard<std::mutex> lk(p_->st->m);
    return p_->st->opened && !p_->st->has_terminal && !p_->st->local_closed;
}

}  // namespace ws
}  // namespace plat
