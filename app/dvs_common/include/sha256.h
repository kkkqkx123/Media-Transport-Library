#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace dvs {

/* Self-contained SHA-256 / HMAC-SHA256 / PBKDF2-HMAC-SHA256.
 * Zero external dependencies (no OpenSSL/Argon2) so the agent builds on any
 * host and the RK3588 cross toolchain without extra packages. */

void sha256(const uint8_t* data, size_t len, uint8_t out[32]);

void hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* msg,
                 size_t msg_len, uint8_t out[32]);

/* PBKDF2-HMAC-SHA256. output must be >= 32 bytes (one block is enough for our
 * use; we only ever derive a 32-byte key). */
void pbkdf2_hmac_sha256(const uint8_t* password, size_t pw_len,
                         const uint8_t* salt, size_t salt_len,
                         uint32_t iterations, uint8_t out[32]);

std::string to_hex(const uint8_t* data, size_t len);
/* Returns false if the hex string is malformed or longer than max_out. */
bool from_hex(const char* hex, uint8_t* out, size_t max_out, size_t* out_len);

/* SHA-1 for the WebSocket handshake accept derivation (RFC6455).
 * Self-contained, same zero-dependency rule as SHA-256. */
void sha1(const uint8_t* data, size_t len, uint8_t out[20]);

/* Base64 for Sec-WebSocket-Key / Accept. No line breaks, standard alphabet. */
std::string base64_encode(const uint8_t* data, size_t len);
bool base64_decode(const std::string& s, std::string& out);

}  // namespace dvs
