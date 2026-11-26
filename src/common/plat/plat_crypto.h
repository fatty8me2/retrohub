#pragma once
/* Hashes, HMAC and random bytes on both platforms (docs/MACOS.md): BCrypt on Windows, CommonCrypto on macOS. Part of hubbase.
 * Results are raw bytes; use plat::hex() for the lowercase hex form. Replacing an existing BCrypt hash sequence with this is
 * allowed (same algorithm, same bytes out); keep the AES / ECDSA code where it is until its lane ports it. */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace plat {

enum class HashAlg { Md5, Sha1, Sha256, Sha512 };

size_t hash_size(HashAlg a);   // 16 / 20 / 32 / 64

/* Streaming hash. A failed init (practically impossible) makes ok() false and finish() return "". */
class Hasher {
  public:
    explicit Hasher(HashAlg a);
    ~Hasher();
    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;
    bool ok() const;
    void update(const void* data, size_t len);
    void update(const std::string& s) { update(s.data(), s.size()); }
    std::string finish();   // raw digest bytes; the Hasher is spent afterwards
    struct Impl;   // also used by hmac()
  private:
    std::unique_ptr<Impl> p_;
};

std::string hash(HashAlg a, const void* data, size_t len);
inline std::string hash(HashAlg a, const std::string& s) { return hash(a, s.data(), s.size()); }
std::string hmac(HashAlg a, const std::string& key, const void* data, size_t len);
inline std::string hmac(HashAlg a, const std::string& key, const std::string& s) { return hmac(a, key, s.data(), s.size()); }

/* Cryptographically secure random bytes. false on failure (buf contents unspecified). */
bool random_bytes(void* buf, size_t len);

std::string hex(const std::string& raw);   // lowercase

} // namespace plat
