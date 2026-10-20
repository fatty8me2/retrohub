#include "save_sync.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace n64run {

namespace fs = std::filesystem;

// ---------------------------------------------------------------- helpers
uint64_t save_hash(const uint8_t* p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    h ^= (uint64_t)n * 0x9E3779B97F4A7C15ull;
    return h;
}

bool is_battery_ext(const std::string& name) {
    static const char* exts[] = {".eep", ".sra", ".fla", ".mpk"};
    std::string low;
    for (char c : name) low += (char)std::tolower((unsigned char)c);
    for (const char* e : exts) {
        size_t n = std::strlen(e);
        if (low.size() > n && low.compare(low.size() - n, n, e) == 0) return true;
    }
    return false;
}

bool valid_save_name(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    if (name == "." || name == "..") return false;
    for (unsigned char c : name) {
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '|') return false;
    }
    return is_battery_ext(name);
}

static size_t chunks_for(size_t size) { return (size + kSaveChunk - 1) / kSaveChunk; }

template <class T> static void put(std::vector<uint8_t>& o, T v) {
    for (size_t i = 0; i < sizeof(T); i++) o.push_back((uint8_t)((uint64_t)v >> (8 * i)));
}
template <class T> static bool get(const uint8_t*& p, const uint8_t* end, T* v) {
    if ((size_t)(end - p) < sizeof(T)) return false;
    uint64_t x = 0;
    for (size_t i = 0; i < sizeof(T); i++) x |= (uint64_t)p[i] << (8 * i);
    *v = (T)x;
    p += sizeof(T);
    return true;
}

// ---------------------------------------------------------------- files
bool load_save_dir(const std::string& dir, std::vector<SaveFile>* out, std::vector<std::string>* skipped) {
    out->clear();
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(fs::path(dir), ec)) return false;
    std::vector<std::string> names;
    for (auto& e : fs::directory_iterator(fs::path(dir), ec)) {
        if (!e.is_regular_file(ec)) continue;
        std::string n = e.path().filename().string();
        if (valid_save_name(n)) names.push_back(n);
    }
    std::sort(names.begin(), names.end());
    size_t total = 0;
    for (auto& n : names) {
        fs::path p = fs::path(dir) / n;
        auto sz = fs::file_size(p, ec);
        if (ec || sz == 0 || sz > kSaveMaxFile || total + sz > kSaveMaxTotal || out->size() >= kSaveMaxFiles) {
            if (skipped) skipped->push_back(n);
            continue;
        }
        std::ifstream f(p, std::ios::binary);
        SaveFile sf;
        sf.name = n;
        sf.data.resize((size_t)sz);
        f.read(reinterpret_cast<char*>(sf.data.data()), (std::streamsize)sz);
        if (!f) { if (skipped) skipped->push_back(n); continue; }
        total += (size_t)sz;
        out->push_back(std::move(sf));
    }
    return true;
}

bool write_save_file(const std::string& dir, const std::string& name, const std::vector<uint8_t>& data) {
    if (!valid_save_name(name)) return false;
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    fs::path tmp = fs::path(dir) / (name + ".syncing");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
        if (!f) return false;
    }
    fs::rename(tmp, fs::path(dir) / name, ec);   // replace-in-place so a half-written save never shows up
    return !ec;
}

// ---------------------------------------------------------------- sender (host)
SaveSender::SaveSender(const std::vector<SaveFile>& files, uint32_t slot_mask, uint32_t now_ms, uint32_t timeout_ms)
    : files_(files), clients_(slot_mask & 0xFFu & ~1u), start_ms_(now_ms), timeout_ms_(timeout_ms), now_(now_ms) {
    for (auto& f : files_) nchunks_.push_back((int)chunks_for(f.data.size()));
    peers_.resize(8);
    for (int s = 1; s < 8; s++) {
        if (!((clients_ >> s) & 1u)) continue;
        peers_[s].done.assign(files_.size(), false);
        peers_[s].have.assign(files_.size(), std::vector<bool>());
        peers_[s].nch.assign(files_.size(), 0);
    }
}

void SaveSender::on_packet(int from, const uint8_t* d, size_t n) {
    if (n < 1 || d[0] != kPktSaveAck) return;
    if (from < 1 || from >= 8 || !((clients_ >> from) & 1u)) return;
    const uint8_t* p = d + 1;
    const uint8_t* end = d + n;
    uint8_t slot, file, verified;
    uint16_t nch;
    if (!get(p, end, &slot) || !get(p, end, &file) || !get(p, end, &verified) || !get(p, end, &nch)) return;
    if (slot != from || file >= files_.size()) return;
    int want = nchunks_[file];
    if (nch != 0 && (int)nch != want) return;
    Peer& pe = peers_[from];
    if (nch == 0) { pe.nch[file] = 0; pe.have[file].clear(); pe.done[file] = false; return; }
    size_t bytes = ((size_t)nch + 7) / 8;
    if ((size_t)(end - p) < bytes) return;
    std::vector<bool> have((size_t)nch);
    for (size_t c = 0; c < nch; c++) have[c] = (p[c / 8] >> (c % 8)) & 1u;
    pe.nch[file] = nch;
    pe.have[file] = have;
    pe.done[file] = verified != 0;
}

bool SaveSender::all_verified() const {
    for (int s = 1; s < 8; s++)
        if ((clients_ >> s) & 1u)
            for (size_t f = 0; f < files_.size(); f++)
                if (!peers_[s].done[f]) return false;
    return true;
}

bool SaveSender::finished() const {
    return all_verified() && go_t0_ && now_ - go_t0_ >= kGoGraceMs;
}

void SaveSender::tick(uint32_t now_ms, const SendFn& send) {
    now_ = now_ms;
    if (failed()) return;
    if (!started_) { started_ = true; last_hdr_ = 0; }

    if (all_verified()) {
        if (!go_t0_) go_t0_ = now_ms ? now_ms : 1;
        if (!last_go_ || now_ms - last_go_ >= 20) {
            last_go_ = now_ms ? now_ms : 1;
            if (files_.empty()) {   // explicit "host has no save files"
                std::vector<uint8_t> h{kPktSaveHdr, 0, 0, 0};
                put<uint32_t>(h, 0u); put<uint64_t>(h, 0ull);
                send(h);
            }
            std::vector<uint8_t> go{kPktSaveGo};
            send(go);
        }
        return;
    }
    if (now_ms - start_ms_ > timeout_ms_) {
        for (int s = 1; s < 8; s++)
            if ((clients_ >> s) & 1u)
                for (size_t f = 0; f < files_.size(); f++)
                    if (!peers_[s].done[f]) {
                        error_ = "peer " + std::to_string(s) + " did not verify " + files_[f].name;
                        return;
                    }
    }
    if (last_hdr_ && now_ms - last_hdr_ < kHdrIntervalMs) {
        // HDR is paced; DATA below still runs every tick
    } else {
        last_hdr_ = now_ms ? now_ms : 1;
        for (size_t f = 0; f < files_.size(); f++) {
            const SaveFile& sf = files_[f];
            std::vector<uint8_t> h{kPktSaveHdr, (uint8_t)f, (uint8_t)files_.size(), (uint8_t)sf.name.size()};
            h.insert(h.end(), sf.name.begin(), sf.name.end());
            put<uint32_t>(h, (uint32_t)sf.data.size());
            put<uint64_t>(h, save_hash(sf.data.data(), sf.data.size()));
            send(h);
        }
    }

    // DATA: round-robin over (file, chunk) pairs that some unverified peer lacks.
    std::vector<std::pair<size_t, size_t>> want;
    for (size_t f = 0; f < files_.size(); f++) {
        int nc = nchunks_[f];
        for (int c = 0; c < nc; c++) {
            bool need = false;
            for (int s = 1; s < 8 && !need; s++) {
                if (!((clients_ >> s) & 1u)) continue;
                const Peer& pe = peers_[s];
                if (pe.done[f] || pe.nch[f] == 0) continue;
                if (!pe.have[f][(size_t)c]) need = true;
            }
            if (need) want.push_back({f, (size_t)c});
        }
    }
    if (want.empty()) return;
    if (last_data_ && now_ms - last_data_ < 10) return;   // pace DATA to about 2.4 MB/s
    last_data_ = now_ms ? now_ms : 1;
    int budget = kDataPerTick;
    for (size_t k = 0; k < want.size() && budget > 0; k++, budget--) {
        auto [f, c] = want[(cursor_ + k) % want.size()];
        const SaveFile& sf = files_[f];
        size_t off = c * kSaveChunk;
        size_t len = std::min(kSaveChunk, sf.data.size() - off);
        std::vector<uint8_t> m{kPktSaveData, (uint8_t)f};
        put<uint16_t>(m, (uint16_t)c);
        put<uint32_t>(m, (uint32_t)save_hash(sf.data.data() + off, len));
        m.insert(m.end(), sf.data.begin() + (long)off, sf.data.begin() + (long)(off + len));
        send(m);
        bytes_sent_ += len;
    }
    cursor_ = (cursor_ + kDataPerTick) % want.size();
}

// ---------------------------------------------------------------- receiver (client)
SaveReceiver::SaveReceiver(int my_slot, int num_slots, const std::string& dir)
    : slot_(my_slot), num_slots_(num_slots), dir_(dir) {}

void SaveReceiver::on_packet(int from, const uint8_t* d, size_t n) {
    if (n < 1 || failed()) return;
    if (from != 0) return;   // the host is the only sender
    const uint8_t* p = d + 1;
    const uint8_t* end = d + n;
    switch (d[0]) {
        case kPktSaveGo: go_ = true; return;
        case kPktSaveHdr: {
            uint8_t idx, count, nlen;
            uint32_t size; uint64_t hash;
            if (!get(p, end, &idx) || !get(p, end, &count) || !get(p, end, &nlen)) return;
            if ((size_t)(end - p) < nlen) return;
            std::string name((const char*)p, nlen);
            p += nlen;
            if (!get(p, end, &size) || !get(p, end, &hash)) return;
            if (count == 0 && idx == 0 && nlen == 0) { if (nfiles_ < 0) { nfiles_ = 0; files_.clear(); } return; }
            if (count > kSaveMaxFiles || idx >= count) return;
            if (nfiles_ < 0) { nfiles_ = count; files_.assign(count, File()); }
            if (count != nfiles_) return;
            if (!valid_save_name(name) || size == 0 || size > kSaveMaxFile) { error_ = "host sent an invalid save entry: " + name; return; }
            File& f = files_[idx];
            if (f.set) return;   // headers repeat; first one wins
            size_t total = 0;
            for (auto& g : files_) total += g.set ? g.size : 0;
            if (total + size > kSaveMaxTotal) { error_ = "host saves exceed the size limit"; return; }
            f.set = true; f.name = name; f.size = size; f.hash = hash;
            f.nch = chunks_for(size);
            f.buf.assign(size, 0);
            f.have.assign(f.nch, false);
            return;
        }
        case kPktSaveData: {
            uint8_t idx; uint16_t c; uint32_t check;
            if (!get(p, end, &idx) || !get(p, end, &c) || !get(p, end, &check)) return;
            if (nfiles_ < 0 || idx >= files_.size()) return;
            File& f = files_[idx];
            if (!f.set || f.verified || c >= f.nch || f.have[c]) return;
            size_t off = (size_t)c * kSaveChunk;
            size_t len = std::min(kSaveChunk, (size_t)f.size - off);
            if ((size_t)(end - p) != len) return;
            if ((uint32_t)save_hash(p, len) != check) return;   // corrupt chunk: drop it, the host resends
            std::memcpy(f.buf.data() + off, p, len);
            f.have[c] = true;
            f.nhave++;
            if (f.nhave == f.nch) {
                if (save_hash(f.buf.data(), f.buf.size()) == f.hash) {
                    if (write_save_file(dir_, f.name, f.buf)) f.verified = true;
                    else error_ = "cannot write save file " + f.name + " into " + dir_;
                } else {
                    // Corrupt: clear and let the host resend. The ACK bitmap goes back to zero.
                    f.have.assign(f.nch, false);
                    f.nhave = 0;
                }
            }
            return;
        }
        default: return;
    }
}

void SaveReceiver::tick(uint32_t now_ms, const SendFn& send) {
    if (failed() || finished()) return;
    if (now_ms - last_ack_ < 50 && last_ack_) return;
    last_ack_ = now_ms ? now_ms : 1;
    for (size_t f = 0; f < files_.size(); f++) {
        const File& fl = files_[f];
        if (!fl.set) continue;
        std::vector<uint8_t> a{kPktSaveAck, (uint8_t)slot_, (uint8_t)f, (uint8_t)(fl.verified ? 1 : 0)};
        put<uint16_t>(a, (uint16_t)fl.nch);
        size_t bytes = (fl.nch + 7) / 8;
        size_t base = a.size();
        a.resize(base + bytes, 0);
        for (size_t c = 0; c < fl.nch; c++) if (fl.have[c]) a[base + c / 8] |= (uint8_t)(1u << (c % 8));
        send(a);
    }
}

bool SaveReceiver::finished() const {
    if (!go_) return false;
    for (auto& f : files_) if (f.set && !f.verified) return false;
    return true;
}

std::vector<std::string> SaveReceiver::written() const {
    std::vector<std::string> o;
    for (auto& f : files_) if (f.set && f.verified) o.push_back(f.name);
    return o;
}

}  // namespace n64run
