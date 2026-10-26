/* DSU (cemuhook) server: up to 4 virtual pads that Cemu reads over UDP (127.0.0.1:26760) as "DSUController" players.
 * The packet builders/parsers are pure (dsu_test); DsuServer is a small Winsock thread around them.
 * Input struct mirrors stream_proto.h PadState; `buttons` uses the XInput wButtons layout (see DsuBtn). */
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace dsu {

constexpr uint16_t kProtocolVersion = 1001;
constexpr uint16_t kDefaultPort = 26760;
constexpr int kSlots = 4;
enum : uint32_t { kMsgVersion = 0x100000, kMsgInfo = 0x100001, kMsgData = 0x100002 };
constexpr size_t kHeaderSize = 16;
constexpr size_t kDataPacketSize = 100;
constexpr size_t kInfoPacketSize = 32;   /* 16 header + 4 type + 12 (11 shared header + 1 pad) */

/* XInput wButtons layout. */
enum DsuBtn : uint16_t {
    kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kStart = 0x0010, kBack = 0x0020, kL3 = 0x0040, kR3 = 0x0080,
    kLB = 0x0100, kRB = 0x0200, kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000};

struct Pad {   /* == PadState from src/stream/stream_proto.h */
    uint16_t buttons = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;   /* -32768..32767, +y = up */
    uint8_t lt = 0, rt = 0;
    float gyro[3] = {0, 0, 0};    /* deg/s: pitch, yaw, roll */
    float accel[3] = {0, 0, 1};   /* g */
};

uint32_t crc32(const uint8_t* p, size_t n);
/* Wraps `body` (message type + payload) in a server header (magic DSUS, version, length, CRC32 over the whole packet, server id). */
std::vector<uint8_t> wrap(uint32_t server_id, const std::vector<uint8_t>& body, bool from_server = true);
/* Validates magic/version/length/CRC. On success *type = message type, *payload = bytes after the type. */
bool parse(const uint8_t* p, size_t n, bool expect_server, uint32_t* type, std::vector<uint8_t>* payload, uint32_t* sender_id = nullptr);

std::vector<uint8_t> build_version(uint32_t server_id);
std::vector<uint8_t> build_info(uint32_t server_id, int slot, bool connected);
std::vector<uint8_t> build_data(uint32_t server_id, int slot, bool connected, uint32_t packet_no, uint64_t timestamp_us, const Pad& pad);

/* Client side (tests / probes). */
std::vector<uint8_t> build_info_request(uint32_t client_id, const std::vector<uint8_t>& slots);
std::vector<uint8_t> build_data_request(uint32_t client_id, int slot /* -1 = all */);

class DsuServer {
public:
    DsuServer();
    ~DsuServer();
    bool start(uint16_t port = kDefaultPort, bool loopback_only = true);   /* false if the port is taken */
    void stop();
    void set_connected(int slot, bool on);
    void set_pad(int slot, const Pad& pad);   /* also marks the slot connected */
    uint16_t port() const;
    int clients() const;                       /* clients that requested data in the last 5 s */
    unsigned long long packets_sent() const;
private:
    struct Impl;
    Impl* d_;
};

}  // namespace dsu
