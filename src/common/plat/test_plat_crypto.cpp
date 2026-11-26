/* plat_crypto: known-answer vectors (FIPS 180, RFC 1321, RFC 4231), streaming == one-shot, random bytes. */
#include "plat_crypto.h"

#include <algorithm>
#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main() {
    using plat::HashAlg;
    CHECK(plat::hex(plat::hash(HashAlg::Md5, "abc")) == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(plat::hex(plat::hash(HashAlg::Sha1, "abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(plat::hex(plat::hash(HashAlg::Sha256, "abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(plat::hex(plat::hash(HashAlg::Sha512, "abc")) ==
          "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    CHECK(plat::hex(plat::hash(HashAlg::Sha256, "")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    /* streaming in odd-sized pieces equals one-shot */
    std::string big;
    for (int i = 0; i < 100000; ++i) big += (char)(i * 31 + 7);
    plat::Hasher h(HashAlg::Sha256);
    CHECK(h.ok());
    for (size_t off = 0; off < big.size(); off += 977) h.update(big.data() + off, std::min<size_t>(977, big.size() - off));
    CHECK(h.finish() == plat::hash(HashAlg::Sha256, big));

    /* RFC 4231 test case 2 */
    CHECK(plat::hex(plat::hmac(HashAlg::Sha256, "Jefe", "what do ya want for nothing?")) ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    unsigned char a[32] = {}, b[32] = {};
    CHECK(plat::random_bytes(a, sizeof a) && plat::random_bytes(b, sizeof b));
    CHECK(std::string((char*)a, 32) != std::string((char*)b, 32));

    std::printf(g_fail ? "plat_crypto_test: %d FAILED\n" : "plat_crypto_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
