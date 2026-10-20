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
    struct Msg { int from; std::string body; };
    std::deque<Msg> msgs;
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
    d.msgs.clear();
    d.status = "connecting";
    d.last_keepalive = now_ms();
    std::string url = hub_url(server.empty() ? default_server() : server, code, role_host, name, 0);
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

bool LobbyClient::poll_msg(int* from_slot, std::string* body_json)
{
    Impl& d = *impl_;
    if (d.msgs.empty()) return false;
    *from_slot = d.msgs.front().from;
    *body_json = std::move(d.msgs.front().body);
    d.msgs.pop_front();
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
    while (d.ws.poll_text(&text)) {
        Json j;
        if (!json_parse(text, &j) || j.type != Json::Object) continue;
        std::string type = j.s("type");
        if (type == "welcome") {
            d.welcomed = true;
            d.slot = j.i("slot", -1);
            d.status = "connected";
            net_log("Lobby: welcome slot %d", d.slot);
        } else if (type == "members") {
            d.members.clear();
            if (const Json* a = j.get("members"))
                for (const Json& m : a->arr) d.members.push_back({m.i("slot", -1), m.s("name")});
        } else if (type == "msg") {
            if (const Json* b = j.get("body")) d.msgs.push_back({j.i("from", -1), b->raw});
        } else if (type == "error") {
            d.failed = true;
            d.status = friendly(j.s("reason", "error"));
            net_log("Lobby: error %s", d.status.c_str());
            return;
        }
    }

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
