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
};

LobbyClient::LobbyClient() : impl_(new Impl) {}
LobbyClient::~LobbyClient() { stop(); }

bool LobbyClient::start(const std::string& server, const std::string& code, bool role_host, const std::string& name)
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
    std::string url = hub_url(server.empty() ? default_server() : server, code, role_host, name, 0);
    url += "&caps=hostmig";   // we understand host_changed: keep the room alive when the host leaves
    net_log("Lobby: connecting %s", url.c_str());
    return d.ws.connect(url);
}

void LobbyClient::stop()
{
    Impl& d = *impl_;
    d.ws.close();
    d.started = d.welcomed = false;
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
            d.slot = (int)j.i64("slot", -1);
            d.status = "connected";
            net_log("Lobby: welcome slot %d", d.slot);
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

    if (d.ws.state() == WsState::Failed) {
        d.failed = true;
        int code = d.ws.close_code();
        d.status = (code == 4001) ? friendly("host-left") : d.ws.error();
        net_log("Lobby: failed: %s", d.status.c_str());
        return;
    }
    uint32_t now = now_ms();
    if (d.welcomed && now - d.last_keepalive >= 20000) {
        d.last_keepalive = now;
        d.ws.send_text("{\"type\":\"keepalive\"}");
    }
}

} // namespace hubnet
