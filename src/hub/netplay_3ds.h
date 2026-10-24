#pragma once
/* 3DS local-wireless over RetroHub rooms (np_3ds).
 *
 * Azahar emulates 3DS local wireless (UDS) through its own "Multiplayer" rooms: an ENet/UDP room server
 * (azahar-room.exe, shipped next to azahar.exe) that every emulator joins. RetroHub never asks anybody to port
 * forward: the host runs azahar-room on 127.0.0.1-reachable UDP, and every guest runs a tiny UDP forwarder on
 * 127.0.0.1 that carries the datagrams through the room's hubnet::Session (hole punch, relay fallback).
 *
 *   guest Azahar --UDP--> guest forwarder --Session--> host forwarder --UDP--> azahar-room <--UDP-- host Azahar
 *
 * The host forwarder opens one UDP socket per guest slot, so the room sees each guest as its own peer.
 * This header: pure, testable pieces (launch-spec key, command lines, nicknames, datagram fragmentation).
 * Runtime (sockets, processes, Session): netplay_3ds_run.cpp. */

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "logic.h"

namespace hubnp3ds {

constexpr int kDefaultRoomPort = 24872;    // Azahar's default room port
constexpr int kMaxMembers = 8;             // UDS networks hold up to 8 consoles; we cap what the catalog advertises separately

/* The "np_3ds" object carried in the host's launch message. */
struct Plan {
    int port = kDefaultRoomPort;   // host's azahar-room UDP port (host loopback)
    std::string password;          // random per launch; passed to Azahar with -m
    int expected = 2;              // number of PCs in the room (Session.expected)
};

/* ",\"np_3ds\":{...}" (leading comma) ready to append to the launch message body. */
std::string plan_json_fragment(const Plan& p);
bool parse_plan(const hubui::Json& launch_msg, Plan* out);   // false when the key is absent or malformed
std::string random_password(unsigned seed);                    // 12 alphanumeric chars

/* Azahar nickname rules: 4..20 chars of [A-Za-z0-9 ._-]. Made unique per room by the slot suffix. */
std::string nickname(const std::string& player_name, int slot);

/* Command lines. */
std::string room_args(int port, const std::string& password, int max_members, const std::string& log_file,
                      const std::string& ban_file);
/* Azahar "-m nick:password@host:port" (password optional) */
std::string multiplayer_arg(const std::string& nick, const std::string& password, const std::string& host, int port);
/* Session room id: "<lobby code>-<launch no>-3ds" */
std::string session_room(const std::string& lobby_code, int launch_no);

/* Qt-config patch so Azahar's Direct Connect dialog is prefilled (ip/port/nickname). Section "UI" holds the keys. */
std::string config_patch(const std::string& ini, const std::string& host, int port, const std::string& nick);

/* The 3DS username (= the nickname Azahar joins a room with; rooms reject duplicates) is stored in the NAND config savegame, NOT in qt-config.
 * console_username: unique per slot, 4..10 chars. set_console_username: patch block 0x000A0000 of that file image. */
std::string console_username(const std::string& player, int slot);
bool set_console_username(std::vector<uint8_t>* cfg, const std::string& name);
std::string console_config_path(const std::string& install_dir);
bool set_console_username_file(const std::string& install_dir, const std::string& name);   // false when the install has not run a game yet
/* QA only: muted, no prompts. */
std::string mute_config(const std::string& ini);

/* ---- datagram fragmentation (hubnet payloads are <= 1100 bytes, ENet datagrams are up to ~1400) ---- */
constexpr int kFragHeader = 5;   // magic 'F', msg id u16, index u8, count u8
constexpr int kFragPayload = 1100 - kFragHeader;
std::vector<std::string> fragment(uint16_t msg_id, const uint8_t* data, size_t len);

class Reassembler {
  public:
    /* Feed one fragment; returns true with the whole datagram once all pieces arrived. Stale partials are dropped. */
    bool add(int from, const uint8_t* frag, size_t len, uint32_t now_ms, std::string* whole);
    size_t pending() const { return parts_.size(); }
  private:
    struct Part { uint32_t t = 0; int count = 0, have = 0; std::vector<std::string> piece; };
    std::map<uint64_t, Part> parts_;
};

}  // namespace hubnp3ds
