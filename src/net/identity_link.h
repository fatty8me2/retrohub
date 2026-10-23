#pragma once
/* Link a new PC to the same RetroHub account, without a sync folder (server: server/cloudflare/src/link.js).
 *
 * Old PC: link_create_code() asks the server for an 8-character code (shown as XXXX-XXXX, valid 10 minutes,
 * single use), encrypts the identity under a key derived from the code and uploads only the ciphertext.
 * New PC: link_redeem_code() fetches the ciphertext, decrypts it and returns the identity; then call
 * FriendsClient::adopt_identity() to back up the current identity, adopt it and reconnect.
 *
 * Crypto: PBKDF2-HMAC-SHA256 (200,000 iterations, 16-byte salt) -> AES-256-GCM (12-byte IV, AAD "rh-link-v1").
 * Both sides run the same code; the server never sees the key or the plaintext.
 * Blocking calls (a few short HTTPS requests): run them from a button handler, not every frame. */

#include "friends.h"
#include <string>

namespace hubnet {

/* "abcd-efgh" / "ABCD EFGH" / "abcdefgh" -> "ABCDEFGH", or "" when malformed. */
std::string link_normalize_code(const std::string& code);

/* Old PC. server "" = default_server(). On success *code_out is the code to type on the new PC. */
bool link_create_code(const std::string& server, const Identity& id, std::string* code_out, std::string* err);

/* New PC. On success *out is the linked identity; pass it to FriendsClient::adopt_identity(). */
bool link_redeem_code(const std::string& server, const std::string& code, Identity* out, std::string* err);

/* Encrypted envelope (base64 fields). Pure: no network, used by the tests. */
struct LinkEnvelope { std::string salt, iv, ct; };
bool link_seal(const std::string& code, const Identity& id, LinkEnvelope* out);
bool link_open(const std::string& code, const LinkEnvelope& env, Identity* out);

/* Fallback without the server: a plain identity.json-format file. It contains the account secret, so keep it
 * private. Import validates the file and then the caller uses FriendsClient::adopt_identity(). */
bool identity_export_file(const Identity& id, const std::string& path, std::string* err);
bool identity_import_file(const std::string& path, Identity* out, std::string* err);

} // namespace hubnet
