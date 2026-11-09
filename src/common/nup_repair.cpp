#include "nup_repair.h"
/* Damaged-member repair for Lichii's NUP packages (nup_repair.h has the contract). Reads the NUP only; every TOC bound is checked like
 * Lichii's nup_container.cpp Parse() / ValidateDirTree(), so a malformed file ends in Scan.ok = false and never in a crash. */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <zstd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

#include "winutil.h"

namespace hub::nuprepair {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kNoDir = 0xFFFFFFFFu;
constexpr uint64_t kSuperSize = 0x80, kTocHeader = 0x30, kPkgRec = 0x40, kDirRec = 0x18, kFileRec = 0x50;
constexpr uint32_t kTocMagic = 0x01434F54u;
constexpr uint64_t kMaxToc = 1ull << 30;            // a TOC beyond 1 GiB is not a real game
constexpr uint64_t kMaxMember = 1ull << 30;         // extract() refuses a member beyond 1 GiB
constexpr size_t kHeadRead = 128 * 1024;
constexpr uint32_t kMaxDirDepth = 128;
constexpr const char* kManifestHead = "RetroHub damaged-file repair 1";

uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

std::wstring W(const std::string& s) { return winutil::to_wide(s); }

std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

std::string tid_hex(uint64_t tid) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llX", (unsigned long long)tid);
    return b;
}

// ---- SHA-256 (Windows CNG) ----
struct Sha {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = false;
    Sha() {
        ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 && BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) >= 0;
    }
    ~Sha() {
        if (h) BCryptDestroyHash(h);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    Sha(const Sha&) = delete;
    Sha& operator=(const Sha&) = delete;
    void add(const void* d, size_t n) {
        if (!ok) return;
        const uint8_t* p = (const uint8_t*)d;
        while (n) {
            ULONG c = (ULONG)std::min<size_t>(n, 1u << 28);
            if (BCryptHashData(h, (PUCHAR)p, c, 0) < 0) ok = false;
            p += c;
            n -= c;
        }
    }
    bool finish(uint8_t out[32]) {
        if (!ok) return false;
        return BCryptFinishHash(h, out, 32, 0) >= 0;
    }
};

bool sha_of(const void* d, size_t n, uint8_t out[32]) {
    Sha s;
    s.add(d, n);
    return s.finish(out);
}

// ---- read-only file ----
struct RFile {
    HANDLE h = INVALID_HANDLE_VALUE;
    uint64_t size = 0, mtime = 0;
    ~RFile() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    bool open(const std::wstring& w) {
        h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        BY_HANDLE_FILE_INFORMATION bi{};
        if (!GetFileInformationByHandle(h, &bi) || (bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
        size = ((uint64_t)bi.nFileSizeHigh << 32) | bi.nFileSizeLow;
        mtime = ((uint64_t)bi.ftLastWriteTime.dwHighDateTime << 32) | bi.ftLastWriteTime.dwLowDateTime;
        return true;
    }
    bool read_at(uint64_t off, void* buf, size_t n) const {
        if (off > size || n > size - off) return false;
        uint8_t* p = (uint8_t*)buf;
        while (n) {
            DWORD want = (DWORD)std::min<size_t>(n, 1u << 24), got = 0;
            OVERLAPPED ov{};
            ov.Offset = (DWORD)(off & 0xFFFFFFFFu);
            ov.OffsetHigh = (DWORD)(off >> 32);
            if (!ReadFile(h, p, want, &got, &ov) || got != want) return false;
            p += got;
            off += got;
            n -= got;
        }
        return true;
    }
};

// ---- parsed NUP ----
struct Pkg { uint64_t tid = 0; uint32_t romfs_dir = kNoDir, version = 0; uint8_t kind = 0, flags = 0; };
struct DirN { std::string name; uint32_t first = 0, count = 0; };
struct FileN { std::string name; uint64_t size = 0, eoff = 0, esize = 0; uint8_t sha[32] = {}; uint8_t codec = 0; };

struct Nup {
    RFile f;
    uint64_t toc_offset = 0, toc_size = 0, blob_offset = 0, file_size = 0;
    std::string toc_hex;
    uint8_t toc_sha[32] = {};
    uint32_t pkg_count = 0;
    uint64_t dir_count = 0, file_count = 0, pool_offset = 0, pool_size = 0;
    std::vector<Pkg> pkgs;
    std::vector<DirN> dirs;
    std::vector<FileN> files;
    std::vector<uint32_t> children;
};

bool fail(std::string* err, const char* msg) {
    if (err) *err = msg;
    return false;
}

/* Superblock + TOC header + package records. full = also the whole TOC (SHA-256 checked), dirs, files, children and pool. */
bool parse(const std::string& path, Nup& n, bool full, std::string* err) {
    if (!n.f.open(W(path))) return fail(err, "cannot open the file");
    uint8_t sb[kSuperSize];
    if (n.f.size < kSuperSize || !n.f.read_at(0, sb, sizeof sb)) return fail(err, "not a NUP file (too small)");
    if (std::memcmp(sb, "NUP\x01", 4) != 0) return fail(err, "not a NUP file");
    const uint32_t header_size = (uint32_t)sb[4] | ((uint32_t)sb[5] << 8);
    n.toc_offset = rd64(sb + 0x08);
    n.toc_size = rd64(sb + 0x10);
    std::memcpy(n.toc_sha, sb + 0x18, 32);
    n.blob_offset = rd64(sb + 0x38);
    n.file_size = rd64(sb + 0x40);
    const uint32_t spkg = rd32(sb + 0x48), flags = rd32(sb + 0x4C);
    if (header_size < kSuperSize || flags != 0) return fail(err, "unsupported NUP header");
    if (n.file_size != n.f.size || n.toc_offset < kSuperSize || n.blob_offset > n.file_size || n.toc_offset > n.blob_offset ||
        n.toc_size > n.blob_offset - n.toc_offset || n.blob_offset % 0x10 != 0 || n.toc_size > kMaxToc || n.toc_size < kTocHeader)
        return fail(err, "NUP header out of range");
    n.toc_hex = hex(n.toc_sha, 32);

    std::vector<uint8_t> toc;
    if (full) {
        toc.resize((size_t)n.toc_size);
        if (!n.f.read_at(n.toc_offset, toc.data(), toc.size())) return fail(err, "cannot read the NUP table of contents");
        uint8_t h[32];
        if (!sha_of(toc.data(), toc.size(), h) || std::memcmp(h, n.toc_sha, 32) != 0) return fail(err, "NUP table of contents checksum mismatch (damaged or modified file)");
    } else {
        toc.resize(kTocHeader);
        if (!n.f.read_at(n.toc_offset, toc.data(), toc.size())) return fail(err, "cannot read the NUP table of contents");
    }
    if (rd32(toc.data()) != kTocMagic) return fail(err, "bad NUP table of contents");
    n.pkg_count = rd32(toc.data() + 4);
    n.dir_count = rd64(toc.data() + 8);
    n.file_count = rd64(toc.data() + 0x10);
    n.pool_offset = rd64(toc.data() + 0x18);
    n.pool_size = rd64(toc.data() + 0x20);
    if (n.pkg_count == 0 || n.pkg_count != spkg) return fail(err, "bad NUP package count");
    if (n.dir_count > n.toc_size / kDirRec || n.file_count > n.toc_size / kFileRec || (uint64_t)n.pkg_count > n.toc_size / kPkgRec)
        return fail(err, "NUP table of contents counts out of range");
    const uint64_t needed = kTocHeader + (uint64_t)n.pkg_count * kPkgRec + n.dir_count * kDirRec + n.file_count * kFileRec;
    if (needed > n.toc_size || n.pool_offset < needed || n.pool_offset > n.toc_size || n.pool_size > n.toc_size - n.pool_offset || (n.pool_offset - needed) % 4 != 0)
        return fail(err, "NUP table of contents layout out of range");
    if (!full) {
        // only the package records are needed here
        toc.resize((size_t)(kTocHeader + (uint64_t)n.pkg_count * kPkgRec));
        if (!n.f.read_at(n.toc_offset + kTocHeader, toc.data() + kTocHeader, (size_t)((uint64_t)n.pkg_count * kPkgRec))) return fail(err, "cannot read the NUP packages");
    }
    const uint8_t* base = toc.data();
    n.pkgs.clear();
    for (uint32_t i = 0; i < n.pkg_count; ++i) {
        const uint8_t* r = base + kTocHeader + (uint64_t)i * kPkgRec;
        Pkg p;
        p.tid = rd64(r);
        const uint32_t root = rd32(r + 8), exefs = rd32(r + 0x0C);
        p.romfs_dir = rd32(r + 0x10);
        p.version = rd32(r + 0x14);
        p.kind = r[0x18];
        p.flags = r[0x19];
        if (p.kind > 2 || (p.flags & ~1u) != 0 || root >= n.dir_count || (exefs != kNoDir && exefs >= n.dir_count) || (p.romfs_dir != kNoDir && p.romfs_dir >= n.dir_count))
            return fail(err, "bad NUP package record");
        n.pkgs.push_back(p);
    }
    if (!full) return true;

    uint64_t pos = kTocHeader + (uint64_t)n.pkg_count * kPkgRec;
    const uint64_t children_count = (n.pool_offset - needed) / 4;
    const uint8_t* pool = base + n.pool_offset;
    n.dirs.resize((size_t)n.dir_count);
    for (uint64_t i = 0; i < n.dir_count; ++i, pos += kDirRec) {
        const uint8_t* r = base + pos;
        const uint32_t no = rd32(r), nl = rd32(r + 4), fc = rd32(r + 8), cc = rd32(r + 12);
        if ((uint64_t)fc + cc > children_count || (uint64_t)no + nl > n.pool_size) return fail(err, "bad NUP directory record");
        n.dirs[i].name.assign((const char*)pool + no, nl);
        n.dirs[i].first = fc;
        n.dirs[i].count = cc;
    }
    n.files.resize((size_t)n.file_count);
    for (uint64_t i = 0; i < n.file_count; ++i, pos += kFileRec) {
        const uint8_t* r = base + pos;
        FileN& f = n.files[i];
        const uint32_t no = rd32(r), nl = rd32(r + 4);
        f.size = rd64(r + 8);
        f.eoff = rd64(r + 0x10);
        f.esize = rd64(r + 0x18);
        std::memcpy(f.sha, r + 0x20, 32);
        f.codec = r[0x40];
        const uint8_t cflags = r[0x41];
        if (f.codec > 1 || cflags != 0 || (f.size == 0 && (f.eoff != 0 || f.esize != 0)) ||
            (f.size != 0 && (f.esize == 0 || f.eoff < n.blob_offset || f.eoff % 0x10 != 0 || f.eoff > n.file_size || f.esize > n.file_size - f.eoff)) ||
            (uint64_t)no + nl > n.pool_size)
            return fail(err, "bad NUP file record");
        f.name.assign((const char*)pool + no, nl);
    }
    n.children.resize((size_t)children_count);
    if (children_count) std::memcpy(n.children.data(), base + pos, (size_t)children_count * 4);
    for (uint32_t c : n.children)
        if ((uint64_t)c >= n.dir_count + n.file_count) return fail(err, "bad NUP child index");

    // ValidateDirTree: no cycle, depth <= 128 (iterative, like the emulator)
    {
        enum : uint8_t { Unvisited, InProgress, Done };
        struct Frame { uint32_t dir, next; };
        std::vector<uint8_t> state((size_t)n.dir_count, Unvisited);
        std::vector<uint32_t> height((size_t)n.dir_count, 0);
        std::vector<Frame> stack;
        for (uint32_t start = 0; start < n.dir_count; ++start) {
            if (state[start] != Unvisited) continue;
            state[start] = InProgress;
            stack.push_back({start, 0});
            while (!stack.empty()) {
                const Frame fr = stack.back();
                const DirN& node = n.dirs[fr.dir];
                if (fr.next == node.count) {
                    state[fr.dir] = Done;
                    stack.pop_back();
                    if (!stack.empty()) { uint32_t& ph = height[stack.back().dir]; ph = std::max(ph, height[fr.dir] + 1); }
                    continue;
                }
                ++stack.back().next;
                const uint32_t child = n.children[(size_t)node.first + fr.next];
                if (child >= n.dir_count) continue;
                if (state[child] == InProgress) return fail(err, "NUP directory tree contains a cycle");
                if (stack.size() + height[child] + 1 > kMaxDirDepth) return fail(err, "NUP directory tree is nested too deeply");
                if (state[child] == Done) height[fr.dir] = std::max(height[fr.dir], height[child] + 1);
                else { state[child] = InProgress; stack.push_back({child, 0}); }
            }
        }
    }
    return true;
}

/* The package that supplies the program: the first base, else the best (highest version) update. */
int base_pkg(const Nup& n) {
    for (size_t i = 0; i < n.pkgs.size(); ++i) if (n.pkgs[i].kind == 0) return (int)i;
    return -1;
}
int best_update(const Nup& n) {
    int best = -1;
    for (size_t i = 0; i < n.pkgs.size(); ++i) {
        if (n.pkgs[i].kind != 1) continue;
        if (best < 0 || n.pkgs[i].version > n.pkgs[(size_t)best].version) best = (int)i;
    }
    return best;
}
bool program_title(const Nup& n, uint64_t* tid) {
    int b = base_pkg(n);
    if (b < 0) b = best_update(n);
    if (b < 0) return false;
    *tid = n.pkgs[(size_t)b].tid;
    return true;
}

/* Adds every file below `root` to out (path -> file index), later walks overwrite earlier paths (update over base). */
bool walk(const Nup& n, uint32_t root, std::map<std::string, uint32_t>& out, std::string* err) {
    struct Fr { uint32_t dir, next; size_t prefix_len; };
    std::vector<Fr> st;
    std::string path;
    uint64_t visited = 0;
    st.push_back({root, 0, 0});
    while (!st.empty()) {
        Fr& fr = st.back();
        const DirN& d = n.dirs[fr.dir];
        if (fr.next >= d.count) {
            path.resize(fr.prefix_len);
            st.pop_back();
            continue;
        }
        const uint32_t c = n.children[(size_t)d.first + fr.next++];
        if (++visited > n.dir_count + n.file_count + 1) return fail(err, "NUP directory tree shares entries");
        if (c < n.dir_count) {
            if (st.size() >= kMaxDirDepth + 2) return fail(err, "NUP directory tree is nested too deeply");
            const size_t keep = path.size();
            path += n.dirs[c].name;
            path += '/';
            st.push_back({c, 0, keep});
        } else {
            out[path + n.files[c - n.dir_count].name] = (uint32_t)(c - n.dir_count);
        }
    }
    return true;
}

bool build_merged(const Nup& n, std::map<std::string, uint32_t>& merged, std::string* err) {
    const int b = base_pkg(n), u = best_update(n);
    if (b >= 0 && n.pkgs[(size_t)b].romfs_dir != kNoDir && !walk(n, n.pkgs[(size_t)b].romfs_dir, merged, err)) return false;
    if (u >= 0 && n.pkgs[(size_t)u].romfs_dir != kNoDir && !walk(n, n.pkgs[(size_t)u].romfs_dir, merged, err)) return false;
    return true;
}

bool ends_with(const std::string& s, const std::string& suf) { return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0; }

bool has_magic(const uint8_t* p) { return p[0] == 0x28 && p[1] == 0xB5 && p[2] == 0x2F && p[3] == 0xFD; }

/* Does the member's uncompressed content start with the zstd frame magic? Reads at most 128 KiB, decompresses until 4 bytes. */
struct HeadChecker {
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    std::vector<uint8_t> in;
    ~HeadChecker() { if (dctx) ZSTD_freeDCtx(dctx); }
    // *io_error: the read itself failed (not a verdict)
    bool magic(const Nup& n, const FileN& f, bool* io_error) {
        if (f.size == 0 || f.esize == 0) return false;
        if (f.codec == 0) {
            uint8_t b[4];
            if (f.esize < 4) return false;
            if (!n.f.read_at(f.eoff, b, 4)) { *io_error = true; return false; }
            return has_magic(b);
        }
        const size_t len = (size_t)std::min<uint64_t>(f.esize, kHeadRead);
        in.resize(len);
        if (!n.f.read_at(f.eoff, in.data(), len)) { *io_error = true; return false; }
        if (!dctx) { *io_error = true; return false; }
        ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);
        uint8_t head[4] = {};
        ZSTD_inBuffer ib{in.data(), len, 0};
        ZSTD_outBuffer ob{head, 4, 0};
        while (ob.pos < 4) {
            const size_t before_in = ib.pos, before_out = ob.pos;
            const size_t r = ZSTD_decompressStream(dctx, &ob, &ib);
            if (ZSTD_isError(r)) return false;
            if (r == 0) break;
            if (ib.pos == before_in && ob.pos == before_out) break;   // no progress: truncated
        }
        return ob.pos >= 4 && has_magic(head);
    }
};

/* Windows-safe, manifest-safe member path: '/' separated, no empty / "." / ".." component, no \ : * ? " < > | or control character. */
bool safe_path(const std::string& p) {
    if (p.empty() || p.size() > 400) return false;
    size_t start = 0;
    while (start <= p.size()) {
        size_t e = p.find('/', start);
        if (e == std::string::npos) e = p.size();
        const std::string c = p.substr(start, e - start);
        if (c.empty() || c == "." || c == "..") return false;
        if (c.back() == ' ' || c.back() == '.') return false;
        for (unsigned char ch : c)
            if (ch < 0x20 || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') return false;
        if (e == p.size()) break;
        start = e + 1;
    }
    return true;
}

bool decompress_member(const Nup& n, const FileN& f, std::vector<uint8_t>* out, std::string* err) {
    out->clear();
    if (f.size > kMaxMember) return fail(err, "member too large");
    if (f.size == 0) return true;
    if (f.codec == 0) {
        if (f.esize != f.size) return fail(err, "raw member size mismatch");
        out->resize((size_t)f.size);   // bounded by the real file size (the extent was checked)
        if (!n.f.read_at(f.eoff, out->data(), out->size())) { out->clear(); return fail(err, "cannot read the member"); }
        return true;
    }
    std::unique_ptr<ZSTD_DCtx, size_t (*)(ZSTD_DCtx*)> ds(ZSTD_createDCtx(), &ZSTD_freeDCtx);
    if (!ds) return fail(err, "out of memory");
    std::vector<uint8_t> in(256 * 1024), chunk(256 * 1024);
    out->reserve((size_t)std::min<uint64_t>(f.size, 64u << 20));   // grows with real output only: a lying size field cannot force a huge allocation
    uint64_t read = 0;
    bool done = false;
    while (!done && read < f.esize) {
        const size_t c = (size_t)std::min<uint64_t>(in.size(), f.esize - read);
        if (!n.f.read_at(f.eoff + read, in.data(), c)) { out->clear(); return fail(err, "cannot read the member"); }
        read += c;
        ZSTD_inBuffer ib{in.data(), c, 0};
        while (ib.pos < ib.size) {
            ZSTD_outBuffer ob{chunk.data(), chunk.size(), 0};
            const size_t r = ZSTD_decompressStream(ds.get(), &ob, &ib);
            if (ZSTD_isError(r)) { out->clear(); return fail(err, "the member does not decompress"); }
            if (out->size() + ob.pos > f.size) { out->clear(); return fail(err, "the member decompresses to more than its size"); }
            out->insert(out->end(), chunk.data(), chunk.data() + ob.pos);
            if (r == 0) { done = true; break; }
        }
    }
    if (!done || out->size() != f.size) { out->clear(); return fail(err, "the member is truncated"); }
    return true;
}

bool extract_from(const Nup& n, const std::map<std::string, uint32_t>& merged, const std::string& romfs_path, std::vector<uint8_t>* out, std::string* err) {
    auto it = merged.find(romfs_path);
    if (it == merged.end()) return fail(err, "no such member");
    const FileN& f = n.files[it->second];
    if (!decompress_member(n, f, out, err)) return false;
    uint8_t h[32];
    if (!sha_of(out->data(), out->size(), h) || std::memcmp(h, f.sha, 32) != 0) { out->clear(); return fail(err, "SHA-256 mismatch (the member is damaged)"); }
    return true;
}

// ---- paths on disk ----
std::string join_user(const std::string& user_dir) {
    std::string u = user_dir;
    while (!u.empty() && (u.back() == '\\' || u.back() == '/')) u.pop_back();
    return u;
}
std::string mod_dir_of(const std::string& user_dir, uint64_t tid) { return join_user(user_dir) + "\\load\\" + tid_hex(tid) + "\\" + kModFolder; }
std::string file_in_mod(const std::string& mod_dir, const std::string& romfs_path) {
    std::string p = romfs_path;
    std::replace(p.begin(), p.end(), '/', '\\');
    return mod_dir + "\\romfs\\" + p;
}

bool file_sha_and_size(const std::string& path, std::string* sha, uint64_t* size) {
    RFile f;
    if (!f.open(W(path))) return false;
    Sha s;
    std::vector<uint8_t> buf((size_t)std::min<uint64_t>(std::max<uint64_t>(f.size, 1), 1u << 20));
    for (uint64_t off = 0; off < f.size;) {
        const size_t c = (size_t)std::min<uint64_t>(buf.size(), f.size - off);
        if (!f.read_at(off, buf.data(), c)) return false;
        s.add(buf.data(), c);
        off += c;
    }
    uint8_t h[32];
    if (!s.finish(h)) return false;
    *sha = hex(h, 32);
    *size = f.size;
    return true;
}

bool delete_file(const std::wstring& w) {
    if (DeleteFileW(w.c_str())) return true;
    const DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return false;
    SetFileAttributesW(w.c_str(), FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(w.c_str()) != 0;
}

/* Remove empty directories from `dir` upward, stopping before `stop` (which is never removed) or at the first non-empty one. */
void prune_up(std::wstring dir, const std::wstring& stop) {
    while (!dir.empty() && dir.size() > stop.size() && _wcsnicmp(dir.c_str(), stop.c_str(), stop.size()) == 0) {
        if (!RemoveDirectoryW(dir.c_str())) return;
        const size_t cut = dir.find_last_of(L'\\');
        if (cut == std::wstring::npos) return;
        dir.resize(cut);
    }
}

std::wstring parent_of(const std::wstring& p) {
    const size_t cut = p.find_last_of(L'\\');
    return cut == std::wstring::npos ? std::wstring() : p.substr(0, cut);
}

bool write_bytes_atomic(const std::string& path, const std::vector<uint8_t>& data) {
    std::error_code ec;
    fs::create_directories(fs::path(parent_of(W(path))), ec);
    return winutil::write_file_atomic(path, std::string((const char*)data.data(), data.size()));
}

bool is_hex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

bool parse_u64(const std::string& s, uint64_t* v) {
    if (s.empty() || s.size() > 20) return false;
    uint64_t x = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        const uint64_t d = (uint64_t)(c - '0');
        if (x > (UINT64_MAX - d) / 10) return false;
        x = x * 10 + d;
    }
    *v = x;
    return true;
}

std::string manifest_text(const Scan& s, const std::vector<ManifestEntry>& entries) {
    std::string t = std::string(kManifestHead) + "\n";
    t += "nup " + std::to_string(s.file_size) + " " + std::to_string(s.mtime) + " " + s.toc_sha256 + " rule " + std::to_string(kRuleVersion) + "\n";
    for (const ManifestEntry& e : entries) t += "repair " + e.sha256 + " " + std::to_string(e.size) + "\t" + e.damaged_path + "\t" + e.stand_in_path + "\n";
    return t;
}

/* Deletes the files `m` lists (those whose path is not in `keep`), counts them. */
int delete_listed(const std::string& mod, const Manifest& m, const std::set<std::string>* keep) {
    int n = 0;
    const std::wstring stop = W(mod);
    for (const ManifestEntry& e : m.entries) {
        if (keep && keep->count(e.damaged_path)) continue;
        const std::wstring w = W(file_in_mod(mod, e.damaged_path));
        if (delete_file(w)) ++n;
        prune_up(parent_of(w), stop);
    }
    return n;
}

}  // namespace

bool read_manifest(const std::string& user_dir, uint64_t title_id, Manifest* out) {
    try {
        std::string txt;
        if (!winutil::read_file(mod_dir_of(user_dir, title_id) + "\\" + kManifestName, &txt) || txt.size() > (16u << 20)) return false;
        std::vector<std::string> lines;
        size_t pos = 0;
        while (pos < txt.size()) {
            size_t e = txt.find('\n', pos);
            if (e == std::string::npos) e = txt.size();
            std::string l = txt.substr(pos, e - pos);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(l);
            pos = e + 1;
        }
        while (!lines.empty() && lines.back().empty()) lines.pop_back();
        if (lines.size() < 2 || lines[0] != kManifestHead) return false;
        Manifest m;
        {
            std::vector<std::string> t;
            size_t p = 0;
            const std::string& l = lines[1];
            while (p <= l.size()) {
                size_t e = l.find(' ', p);
                if (e == std::string::npos) e = l.size();
                t.push_back(l.substr(p, e - p));
                p = e + 1;
            }
            uint64_t rule = 0;
            if (t.size() != 6 || t[0] != "nup" || t[4] != "rule" || !parse_u64(t[1], &m.file_size) || !parse_u64(t[2], &m.mtime) || !is_hex64(t[3]) || !parse_u64(t[5], &rule) || rule > 1000000) return false;
            m.toc_sha256 = t[3];
            m.rule = (int)rule;
        }
        for (size_t i = 2; i < lines.size(); ++i) {
            const std::string& l = lines[i];
            if (l.compare(0, 7, "repair ") != 0) return false;
            const size_t t1 = l.find('\t'), t2 = t1 == std::string::npos ? t1 : l.find('\t', t1 + 1);
            if (t1 == std::string::npos || t2 == std::string::npos || l.find('\t', t2 + 1) != std::string::npos) return false;
            const std::string head = l.substr(7, t1 - 7);
            const size_t sp = head.find(' ');
            if (sp == std::string::npos) return false;
            ManifestEntry e;
            e.sha256 = head.substr(0, sp);
            if (!is_hex64(e.sha256) || !parse_u64(head.substr(sp + 1), &e.size)) return false;
            e.damaged_path = l.substr(t1 + 1, t2 - t1 - 1);
            e.stand_in_path = l.substr(t2 + 1);
            if (!safe_path(e.damaged_path) || e.stand_in_path.empty() || e.stand_in_path.find_first_of("\t\r\n") != std::string::npos) return false;
            m.entries.push_back(std::move(e));
        }
        *out = std::move(m);
        return true;
    } catch (...) {
        return false;
    }
}

Scan scan(const std::string& nup_path, const std::atomic<bool>* cancel) {
    Scan s;
    try {
        Nup n;
        std::string err;
        if (!parse(nup_path, n, true, &err)) { s.error = err; return s; }
        s.file_size = n.file_size;
        s.mtime = n.f.mtime;
        s.toc_sha256 = n.toc_hex;
        if (!program_title(n, &s.title_id)) { s.error = "the NUP has no base or update package"; return s; }
        std::map<std::string, uint32_t> merged;
        if (!build_merged(n, merged, &err)) { s.error = err; return s; }
        HeadChecker hc;
        std::vector<std::pair<std::string, uint32_t>> damaged;
        for (const auto& kv : merged) {
            if (cancel && cancel->load()) { s.error = "cancelled"; return s; }
            const FileN& f = n.files[kv.second];
            if (f.codec != 1 || f.size == 0 || !ends_with(f.name, ".zs")) continue;
            ++s.checked;
            bool io = false;
            if (!hc.magic(n, f, &io)) {
                if (io) { s.error = "cannot read the game file"; return s; }
                damaged.push_back({kv.first, kv.second});
            }
        }
        if (!damaged.empty()) {
            std::map<std::string, std::vector<std::pair<std::string, uint32_t>>> bydir;   // dir path ("" or "a/b/") -> (name, file) in name order
            for (const auto& kv : merged) {
                const size_t cut = kv.first.find_last_of('/');
                bydir[cut == std::string::npos ? std::string() : kv.first.substr(0, cut + 1)].push_back({n.files[kv.second].name, kv.second});
            }
            std::unordered_map<uint32_t, bool> magic_cache;
            auto good = [&](uint32_t idx) {
                auto it = magic_cache.find(idx);
                if (it != magic_cache.end()) return it->second;
                bool io = false;
                const bool g = hc.magic(n, n.files[idx], &io);
                magic_cache[idx] = g;
                return g;
            };
            for (const auto& d : damaged) {
                if (cancel && cancel->load()) { s.error = "cancelled"; return s; }
                const FileN& df = n.files[d.second];
                Repair r;
                r.damaged.path = d.first;
                r.damaged.size = df.size;
                if (safe_path(d.first)) {
                    const std::string& name = df.name;
                    const size_t us = name.rfind('_');
                    const std::string suffix = us == std::string::npos ? name : name.substr(us + 1);
                    const size_t cut = d.first.find_last_of('/');
                    const std::string dir = cut == std::string::npos ? std::string() : d.first.substr(0, cut + 1);
                    long best = -1;
                    size_t best_prefix = 0;
                    uint64_t best_gap = 0;
                    for (const auto& c : bydir[dir]) {
                        const std::string& other = c.first;
                        if (c.second == d.second || other.size() <= suffix.size() || !ends_with(other, suffix) || other[other.size() - suffix.size() - 1] != '_') continue;
                        size_t pre = 0;
                        while (pre < name.size() && pre < other.size() && name[pre] == other[pre]) ++pre;
                        const uint64_t cs = n.files[c.second].size, gap = cs > df.size ? cs - df.size : df.size - cs;
                        if (best >= 0 && (pre < best_prefix || (pre == best_prefix && gap >= best_gap))) continue;
                        if (!good(c.second)) continue;
                        best = (long)c.second;
                        best_prefix = pre;
                        best_gap = gap;
                    }
                    if (best >= 0) {
                        r.stand_in.path = dir + n.files[(size_t)best].name;
                        r.stand_in.size = n.files[(size_t)best].size;
                    }
                }
                s.repairs.push_back(std::move(r));
            }
        }
        s.ok = true;
    } catch (const std::exception& e) {
        s.ok = false;
        s.error = std::string("internal error: ") + e.what();
    } catch (...) {
        s.ok = false;
        s.error = "internal error";
    }
    return s;
}

bool extract(const std::string& nup_path, const std::string& romfs_path, std::vector<uint8_t>* out, std::string* err) {
    try {
        std::string e;
        Nup n;
        if (!parse(nup_path, n, true, &e)) { if (err) *err = e; return false; }
        std::map<std::string, uint32_t> merged;
        if (!build_merged(n, merged, &e)) { if (err) *err = e; return false; }
        std::vector<uint8_t> tmp;
        if (!extract_from(n, merged, romfs_path, &tmp, &e)) { if (err) *err = e; return false; }
        if (out) *out = std::move(tmp);
        return true;
    } catch (...) {
        if (err) *err = "internal error";
        return false;
    }
}

int remove(const std::string& user_dir, uint64_t title_id) {
    try {
        Manifest m;
        if (!read_manifest(user_dir, title_id, &m)) return 0;
        const std::string mod = mod_dir_of(user_dir, title_id);
        const int n = delete_listed(mod, m, nullptr);
        const std::wstring wmod = W(mod);
        DeleteFileW(W(mod + "\\" + kManifestName).c_str());
        prune_up(wmod, parent_of(wmod));   // the mod folder goes only when empty
        return n;
    } catch (...) {
        return 0;
    }
}

Applied apply(const std::string& nup_path, const Scan& sc, const std::string& user_dir) {
    Applied a;
    std::vector<std::string> fresh;   // files written now that no earlier manifest listed (removed again on failure)
    const std::string mod = mod_dir_of(user_dir, sc.title_id);
    try {
        if (!sc.ok) { a.error = "the scan did not succeed"; return a; }
        if (user_dir.empty()) { a.error = "no user folder"; return a; }
        Nup n;
        std::string err;
        if (!parse(nup_path, n, true, &err)) { a.error = err; return a; }
        if (n.toc_hex != sc.toc_sha256 || n.file_size != sc.file_size) { a.error = "the game file changed since it was scanned"; return a; }
        std::map<std::string, uint32_t> merged;
        if (!build_merged(n, merged, &err)) { a.error = err; return a; }

        Manifest old;
        const bool have_old = read_manifest(user_dir, sc.title_id, &old);
        std::vector<ManifestEntry> want;
        for (const Repair& r : sc.repairs) {
            if (r.stand_in.path.empty() || !safe_path(r.damaged.path)) { ++a.unrepaired; continue; }
            auto it = merged.find(r.stand_in.path);
            if (it == merged.end()) { a.error = "a stand-in is not in the game file"; return a; }
            ManifestEntry e;
            e.sha256 = hex(n.files[it->second].sha, 32);
            e.size = n.files[it->second].size;
            e.damaged_path = r.damaged.path;
            e.stand_in_path = r.stand_in.path;
            want.push_back(std::move(e));
        }
        std::set<std::string> keep, old_paths;
        for (const ManifestEntry& e : want) keep.insert(e.damaged_path);
        if (have_old) for (const ManifestEntry& e : old.entries) old_paths.insert(e.damaged_path);

        if (want.empty()) {
            if (have_old) a.removed = remove(user_dir, sc.title_id);
            a.ok = true;
            return a;
        }
        bool failed = false;
        for (const ManifestEntry& e : want) {
            const std::string target = file_in_mod(mod, e.damaged_path);
            std::string have_sha;
            uint64_t have_size = 0;
            if (file_sha_and_size(target, &have_sha, &have_size) && have_size == e.size && have_sha == e.sha256) { ++a.kept; continue; }
            std::vector<uint8_t> bytes;
            if (!extract_from(n, merged, e.stand_in_path, &bytes, &err)) { a.error = "cannot read the stand-in " + e.stand_in_path + ": " + err; failed = true; break; }
            if (!write_bytes_atomic(target, bytes)) { a.error = "cannot write " + e.damaged_path; failed = true; break; }
            if (!old_paths.count(e.damaged_path)) fresh.push_back(target);
            ++a.written;
        }
        if (!failed) {
            if (have_old) a.removed = delete_listed(mod, old, &keep);
            if (!winutil::write_file_atomic(mod + "\\" + kManifestName, manifest_text(sc, want))) { a.error = "cannot write the manifest"; failed = true; }
        }
        if (failed) {
            for (const std::string& f : fresh) { const std::wstring w = W(f); delete_file(w); prune_up(parent_of(w), W(mod)); }
            a.written = 0;
            a.kept = 0;
            a.removed = 0;
            a.ok = false;
            return a;
        }
        a.ok = true;
    } catch (const std::exception& e) {
        a.ok = false;
        a.error = std::string("internal error: ") + e.what();
    } catch (...) {
        a.ok = false;
        a.error = "internal error";
    }
    return a;
}

Ensure ensure(const std::string& nup_path, const std::string& user_dir, bool enabled) {
    Ensure r;
    try {
        Nup n;
        std::string err;
        uint64_t tid = 0;
        if (!parse(nup_path, n, false, &err) || !program_title(n, &tid)) {
            r.summary = "nup repair: error: " + (err.empty() ? std::string("no program package") : err);
            return r;
        }
        if (!enabled) {
            r.applied.ok = true;
            r.applied.removed = remove(user_dir, tid);
            r.summary = "nup repair: off, " + std::to_string(r.applied.removed) + " removed";
            return r;
        }
        Manifest m;
        if (read_manifest(user_dir, tid, &m) && m.file_size == n.file_size && m.mtime == n.f.mtime && m.toc_sha256 == n.toc_hex && m.rule == kRuleVersion && !m.entries.empty()) {
            const std::string mod = mod_dir_of(user_dir, tid);
            bool all = true;
            for (const ManifestEntry& e : m.entries) {
                std::string sha;
                uint64_t size = 0;
                if (!file_sha_and_size(file_in_mod(mod, e.damaged_path), &sha, &size) || size != e.size || sha != e.sha256) { all = false; break; }
            }
            if (all) {
                r.applied.ok = true;
                r.applied.kept = (int)m.entries.size();
                r.summary = "nup repair: unchanged, " + std::to_string(m.entries.size()) + " repaired";
                return r;
            }
        }
        r.scanned = true;
        r.scan = scan(nup_path);
        if (!r.scan.ok) { r.summary = "nup repair: error: " + r.scan.error; return r; }
        r.applied = apply(nup_path, r.scan, user_dir);
        if (!r.applied.ok) { r.summary = "nup repair: error: " + r.applied.error; return r; }
        const int repaired = r.applied.written + r.applied.kept;
        if (repaired == 0 && r.applied.unrepaired == 0) {
            r.summary = "nup repair: nothing to repair, " + std::to_string(r.scan.checked) + " checked";
        } else {
            r.summary = "nup repair: " + std::to_string(repaired) + " repaired (" + std::to_string(r.applied.written) + " written, " + std::to_string(r.applied.kept) + " kept), " + std::to_string(r.scan.checked) + " checked";
            if (r.applied.unrepaired) r.summary += ", " + std::to_string(r.applied.unrepaired) + " without a stand-in";
        }
    } catch (const std::exception& e) {
        r.summary = std::string("nup repair: error: ") + e.what();
    } catch (...) {
        r.summary = "nup repair: error";
    }
    return r;
}

std::string page_text(const Manifest& m) {
    const size_t cnt = m.entries.size();
    if (cnt == 0) return std::string();
    bool water = false, fairy = false, ganon = false;
    for (const ManifestEntry& e : m.entries) {
        if (e.damaged_path.find("WaterTemple") != std::string::npos) water = true;
        if (e.damaged_path.find("FairyFountain") != std::string::npos) fairy = true;
        if (e.damaged_path.find("GanonStage") != std::string::npos) ganon = true;
    }
    std::string places;
    auto add = [&](const char* p) { if (!places.empty()) places += ", "; places += p; };
    if (water) add("the Water Temple");
    if (fairy) add("the fairy fountains");
    if (ganon) add("Ganon's Castle");
    std::string t = "This copy of the game has " + std::to_string(cnt) + (cnt == 1 ? " damaged file" : " damaged files") + ", so some rooms would be invisible";
    if (!places.empty()) t += " (" + places + ")";
    t += ". RetroHub fills ";
    t += cnt == 1 ? "it" : "each one";
    t += " in with a matching file from the same copy when the game starts, so ";
    t += cnt == 1 ? "it shows up" : "they show up";
    t += ". Only a fresh dump of your own copy restores ";
    t += cnt == 1 ? "it" : "them";
    t += " exactly.";
    return t;
}

}  // namespace hub::nuprepair
