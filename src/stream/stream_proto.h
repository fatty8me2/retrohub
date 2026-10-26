#pragma once
/* Remote-play wire protocol and localhost pad API (see docs/REMOTE_PLAY.md).
 *
 * Media never touches the Cloudflare relay: every datagram below is a hubnet Session payload (<= hubnet::kMaxPayload)
 * and streamrun refuses to run when the Session would fall back to the WebSocket relay (Session::set_no_relay).
 * Multi-byte integers are little-endian. All structs are packed and written/read field by field via the helpers in
 * stream_core.h, so there is no ABI dependence except for PadState on the localhost API (which is 1-byte packed).
 */
#include <cstdint>

namespace rplay {

constexpr int kMaxDatagram   = 1100;               // == hubnet::kMaxPayload
constexpr int kFragData      = 1000;               // video payload bytes per fragment
constexpr int kProtoVersion  = 1;

/* ---- datagram types (first payload byte) ---- */
enum : uint8_t {
    kPktVideo    = 'V',   // video fragment or XOR parity (host -> guest)
    kPktAudio    = 'U',   // audio packet (host -> guest)
    kPktNack     = 'N',   // guest -> host: re-send these fragments
    kPktKeyReq   = 'K',   // guest -> host: please send a keyframe now
    kPktInput    = 'I',   // guest -> host: controller state (250 Hz)
    kPktStats    = 'S',   // guest -> host: receiver report (loss/jitter) for adaptive bitrate
    kPktHello    = 'h',   // guest -> host: guest is ready, start sending (repeated until video arrives)
};

/* Video datagram:  'V' flags frame_id(u32) idx(u16) count(u16) extra(u16) payload...
 *   data fragment : idx in [0,count), payload = up to kFragData bytes of the frame blob; extra = payload length of the
 *                   LAST fragment is implied; extra unused (0).
 *   parity (flag) : idx = group number, extra = group size G (data fragments idx*G .. idx*G+G-1, clipped to count),
 *                   payload = kFragData bytes: XOR of the (zero padded) group payloads, then 2 trailing bytes
 *                   = XOR of the group's payload lengths. Recovers one lost fragment per group.
 * The reassembled blob is FrameHdr followed by an H.264 Annex-B access unit.
 */
enum : uint8_t { kVfKey = 1, kVfParity = 2, kVfRetx = 4 };
constexpr int kVideoHdr = 1 + 1 + 4 + 2 + 2 + 2;

/* Frame blob header (14 bytes): capture time is steady_clock microseconds (same clock on both ends only for loopback
 * tests; across machines the guest overlay reports decode+present latency and an RTT-based estimate instead). */
constexpr int kFrameHdr = 4 + 2 + 2 + 1 + 1 + 4;   // cap_us(u32) w h (u16) flags(u8) codec(u8) seq(u32)
enum : uint8_t { kCodecH264 = 1 };

/* Audio datagram: 'U' codec(u8) seq(u32) ts_ms(u32) payload...   codec: 1 = AAC-LC ADTS, 2 = PCM s16le 48k stereo */
enum : uint8_t { kAudAac = 1, kAudPcm = 2 };
constexpr int kAudioHdr = 1 + 1 + 4 + 4;
constexpr int kAudioRate = 48000, kAudioChannels = 2;

/* NACK: 'N' n(u8) then n * { frame_id u32, idx u16 }  (n <= 64; parity is never NACKed) */
/* KeyReq: 'K' */
/* Hello: 'h' version(u8) */
/* Stats: 'S' loss_permille(u16) recv_kbps(u16) jitter_ms(u16) fps(u8) rtt_ms(u16) */

/* ---- controller state ---- */
enum PadButton : uint16_t {
    kPadA = 1 << 0, kPadB = 1 << 1, kPadX = 1 << 2, kPadY = 1 << 3,
    kPadL = 1 << 4, kPadR = 1 << 5, kPadZL = 1 << 6, kPadZR = 1 << 7,
    kPadPlus = 1 << 8, kPadMinus = 1 << 9, kPadHome = 1 << 10,
    kPadUp = 1 << 11, kPadDown = 1 << 12, kPadLeft = 1 << 13, kPadRight = 1 << 14,
    kPadStickL = 1 << 15,   // R3 (stick click) is carried in PadState::buttons2
};
enum PadButton2 : uint8_t { kPad2StickR = 1 << 0 };

#pragma pack(push, 1)
struct PadState {
    uint16_t buttons;      // PadButton bits
    int16_t  lx, ly, rx, ry;   // -32768..32767, +y = up
    uint8_t  lt, rt;       // analog triggers 0..255 (ZL/ZR digital bits are also set past ~half)
    float    gyro[3];      // rad/s (x,y,z), 0 if the pad has no gyro
    float    accel[3];     // m/s^2 incl. gravity
    uint8_t  slot;         // player slot of the guest (1..3; 0 = host's own pad)
    uint8_t  buttons2;     // PadButton2 bits
};
static_assert(sizeof(PadState) == 2 + 8 + 2 + 24 + 2, "PadState layout is part of the localhost API");

/* Input datagram payload after the 'I' byte: seq(u16) + PadState */
struct InputPkt { uint8_t type; uint16_t seq; PadState pad; };

/* Localhost pad API (host streamrun -> wiiu-input DSU server): UDP datagrams to 127.0.0.1:kPadApiPort (override with
 * --pad-port), one per received guest input packet, rate-limited to the guest's send rate:
 *   struct PadApiPacket { char magic[4] = "RPAD"; uint8 version = 1; uint8 reserved; uint16 seq; uint32 host_ms; PadState pad; }
 * A PadApiPacket with pad.buttons == 0xFFFF && pad.slot set and magic "RGON" means the guest in that slot disconnected
 * (the DSU server must then report the pad as disconnected). */
struct PadApiPacket {
    char     magic[4];
    uint8_t  version;
    uint8_t  reserved;
    uint16_t seq;
    uint32_t host_ms;
    PadState pad;
};
#pragma pack(pop)
constexpr int kPadApiPort = 26760;
constexpr int kInputHz = 250;

} // namespace rplay
