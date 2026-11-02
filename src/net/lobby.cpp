#include "net_internal.h"
#include "log.h"
#include "session.h"

#include <cstdio>

namespace hubnet {

using namespace detail;

struct LobbyClient::Impl {
    WsClient ws;
    bool started = false, welcomed = false, failed = false;
    std::string status = "idle";
    int slot = -1;
    std::vector<Member> members;
    std::deque<LobbyEvent> events;   // msgs and host changes in arrival order
    uint32_t last_keepalive = 0;
    std::string server, code, name, url;   // kept so a dropped connection can be re-established with the same code
    bool role_host = false, was_welcomed = false, watch = false, dropin = false;
    int attempts = 0;
    uint32_t retry_at = 0;
    bool waiting_retry = false;
    std::string ident;   // see set_ident_msg
};
static const int kMaxReconnects = 6;

LobbyClient::LobbyClient() : impl_(new Impl) {}
LobbyClient::~LobbyClient() { stop(); }

bool LobbyClient::start(const std::string& server, const std::string& code, bool role_host, const std::string& name)
{
    return start_impl(server, code, role_host, name, false);
}

bool LobbyClient::start_watch(const std::string& server, const std::string& code, const std::string& name)
{
    return start_impl(server, code, false, name, true);
}

bool LobbyClient::start_dropin(const std::string& server, const std::string& code, const std::string& name)
{
    return start_impl(server, code, false, name, true, true);
}

bool LobbyClient::start_impl(const std::string& server, const std::string& code, bool role_host, const std::string& name, bool watch, bool dropin)
{
    stop();
    Impl& d = *impl_;
    d.started = true;
    d.welcomed = d.failed = false;
    d.slot = -1;
    d.members.clear();
    d.events.clear();
    d.status = "connecting";
    d.last_keepalive = now_ms();
    d.server = server; d.code = code; d.name = name; d.role_host = role_host; d.watch = watch; d.dropin = dropin;
    d.was_welcomed = false; d.attempts = 0; d.waiting_retry = false;
    std::string url = hub_url(server.empty() ? default_server() : server, code, role_host, name, 0, watch, dropin);
    url += watch ? "&caps=reasons" : "&caps=hostmig,reasons";   // we understand host_changed (keep the room alive when the host leaves) and the distinct error reasons
    d.url = url;
    net_log("Lobby: connecting %s", url.c_str());
    return d.ws.connect(url);
}

void LobbyClient::set_ident_msg(const std::string& json_frame) { impl_->ident = json_frame; }

void LobbyClient::stop()
{
    Impl& d = *impl_;
    d.ws.close();
    d.started = d.welcomed = false;
    d.waiting_retry = false;
    d.slot = -1;
    d.members.clear();
}

bool LobbyClient::connected() const { return impl_->welcomed && !impl_->failed; }
bool LobbyClient::failed() const { return impl_->failed; }
std::string LobbyClient::status() const { return impl_->status; }
int LobbyClient::my_slot() const { return impl_->slot; }
const std::vector<Member>& LobbyClient::members() const { return impl_->members; }

void LobbyClient::send_msg(const std::string& body_json)
{
    if (!impl_->welcomed || impl_->failed) return;
    impl_->ws.send_text("{\"type\":\"msg\",\"body\":" + body_json + "}");
}

bool LobbyClient::poll_event(LobbyEvent* out)
{
    Impl& d = *impl_;
    if (d.events.empty()) return false;
    *out = std::move(d.events.front());
    d.events.pop_front();
    /* My slot changes exactly when the host-change event is consumed, so messages queued before it are
     * still interpreted under the old numbering. */
    if (out->type == LobbyEvent::HostChange && out->from_slot >= 0 && out->from_slot == d.slot) d.slot = 0;
    return true;
}

static std::string friendly(const std::string& reason)
{
    if (reason == "room-in-use") return "That room code is already in use";
    if (reason == "no-such-room") return "No such room";
    if (reason == "room-full") return "The room is full";
    if (reason == "host-left") return "The host left the room";
    if (reason == "host-left-game") return "The host left - game over";
    if (reason == "bad-slot") return "That seat is already taken";
    if (reason == "host-reconnecting") return "The host is reconnecting";
    if (reason == "spectators-full") return "This room already has the maximum number of spectators";
    if (reason == "spectate-busy") return "Spectating is paused: the server is busy, try again later";
    if (reason == "no-free-port") return "That game has no free controller port";
    if (reason == "host-too-old") return "Your friend needs to update RetroHub";
    if (reason == "no-such-game") return "That game has ended";
    return reason;
}

void LobbyClient::update()
{
    Impl& d = *impl_;
    if (!d.started || d.failed) return;

    std::string text;
    while (d.ws.poll_text(&text))
        if (!handle_text(text)) return;
    update_tail();
}

void LobbyClient::inject_server_text(const std::string& text)
{
    Impl& d = *impl_;
    d.started = true;
    handle_text(text);
}

bool LobbyClient::handle_text(const std::string& text)
{
    Impl& d = *impl_;
    {
        Json j;
        if (!json_parse(text, &j) || j.type != Json::Object) return true;
        std::string type = j.str("type");
        if (type == "welcome") {
            d.welcomed = true;
            d.was_welcomed = true;
            d.attempts = 0;
            d.slot = (int)j.i64("slot", -1);
            d.status = "connected";
            net_log("Lobby: welcome slot %d", d.slot);
            if (!d.ident.empty()) d.ws.send_text(d.ident);
        } else if (type == "members") {
            d.members.clear();
            if (const Json* a = j.get("members"))
                for (const Json& m : a->a) d.members.push_back({(int)m.i64("slot", -1), m.str("name")});
        } else if (type == "host_changed") {
            int from = (int)j.i64("from_slot", -1);
            LobbyEvent e;
            e.type = LobbyEvent::HostChange;
            e.from_slot = from;
            e.name = j.str("name");
            d.events.push_back(std::move(e));
            net_log("Lobby: host changed: slot %d (%s) is now the host", from, j.str("name").c_str());
        } else if (type == "msg") {
            if (const Json* b = j.get("body")) {
                LobbyEvent e;
                e.type = LobbyEvent::Msg;
                e.from_slot = (int)j.i64("from", -1);
                e.body = json::stringify(*b);
                d.events.push_back(std::move(e));
            }
        } else if (type == "error") {
            std::string why = j.str("reason", "error");
            if (d.attempts > 0 && d.attempts < kMaxReconnects && (why == "room-in-use" || why == "bad-slot")) {
                /* Our old socket has not been reaped by the server yet: try again shortly. */
                ++d.attempts;
                d.welcomed = false;
                d.status = "Connection lost - reconnecting (" + std::to_string(d.attempts) + "/" + std::to_string(kMaxReconnects) + ")...";
                d.retry_at = now_ms() + 1500;
                d.waiting_retry = true;
                return true;
            }
            d.failed = true;
            d.status = friendly(j.str("reason", "error"));
            net_log("Lobby: error %s", d.status.c_str());
            return false;
        }
    }
    return true;
}

void LobbyClient::update_tail()
{
    Impl& d = *impl_;

    uint32_t now = now_ms();
    if (d.waiting_retry) {
        if ((int32_t)(now - d.retry_at) < 0) return;
        d.waiting_retry = false;
        d.ws.close();
        d.last_keepalive = now;
        net_log("Lobby: reconnect attempt %d", d.attempts);
        if (!d.ws.connect(d.url)) { d.failed = true; d.status = "Connection lost. Press Rejoin to try again."; }
        return;
    }
    if (d.ws.state() == WsState::Failed) {
        int code = d.ws.close_code();
        net_log("Lobby: socket failed (code %d): %s", code, d.ws.error().c_str());
        if (code == 4001) { d.failed = true; d.status = friendly("host-left"); return; }
        /* Anything else (network blip, Worker restart, Wi-Fi roam): reconnect by itself with the same room code. */
        if (d.was_welcomed && d.attempts < kMaxReconnects) {
            ++d.attempts;
            d.welcomed = false;
            d.status = "Connection lost - reconnecting (" + std::to_string(d.attempts) + "/" + std::to_string(kMaxReconnects) + ")...";
            d.retry_at = now + 1000u * (uint32_t)d.attempts;
            d.waiting_retry = true;
            return;
        }
        d.failed = true;
        d.status = d.was_welcomed ? "Connection lost. Press Rejoin to try again." : d.ws.error();
        return;
    }
    if (d.welcomed && now - d.last_keepalive >= 20000) {
        d.last_keepalive = now;
        d.ws.send_text("{\"type\":\"keepalive\"}");
    }
}

} // namespace hubnet
