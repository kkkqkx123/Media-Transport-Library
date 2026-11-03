#include "sha256.h"

#include <cstring>

namespace dvs {

namespace {

const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

inline uint32_t sha_ch(uint32_t x, uint32_t y, uint32_t z) {
  return (x & y) ^ (~x & z);
}
inline uint32_t sha_maj(uint32_t x, uint32_t y, uint32_t z) {
  return (x & y) ^ (x & z) ^ (y & z);
}
inline uint32_t ep0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
inline uint32_t ep1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
inline uint32_t sig0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
inline uint32_t sig1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

}  // namespace

void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

  /* total padded length: len + 1 (0x80) + pad zeros + 8 byte length, multiple
   * of 64. We process the message in a streaming fashion over a working buffer
   * so we never need to allocate the whole padded message. */
  uint64_t bit_len = (uint64_t)len * 8;
  uint8_t buf[64];
  size_t buf_len = 0;
  size_t off = 0;

  auto process_block = [&](const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
      w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
             ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
      w[i] = sig1(w[i - 2]) + w[i - 7] + sig0(w[i - 15]) + w[i - 16];
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5],
             g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
      uint32_t t1 = hh + ep1(e) + sha_ch(e, f, g) + K[i] + w[i];
      uint32_t t2 = ep0(a) + sha_maj(a, b, c);
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  };

  while (off < len) {
    size_t take = 64 - buf_len;
    if (take > len - off) take = len - off;
    memcpy(buf + buf_len, data + off, take);
    buf_len += take;
    off += take;
    if (buf_len == 64) {
      process_block(buf);
      buf_len = 0;
    }
  }
  /* append 0x80 */
  buf[buf_len++] = 0x80;
  if (buf_len > 56) {
    while (buf_len < 64) buf[buf_len++] = 0x00;
    process_block(buf);
    buf_len = 0;
  }
  while (buf_len < 56) buf[buf_len++] = 0x00;
  for (int i = 0; i < 8; i++) {
    buf[56 + i] = (uint8_t)(bit_len >> (56 - i * 8));
  }
  process_block(buf);

  for (int i = 0; i < 8; i++) {
    out[i * 4] = (uint8_t)(h[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
    out[i * 4 + 3] = (uint8_t)h[i];
  }
}

void hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* msg,
                 size_t msg_len, uint8_t out[32]) {
  uint8_t k[64];
  if (key_len > 64) {
    sha256(key, key_len, k);
    memset(k + 32, 0, 32);
  } else {
    memcpy(k, key, key_len);
    memset(k + key_len, 0, 64 - key_len);
  }
  uint8_t k_ipad[64], k_opad[64];
  for (int i = 0; i < 64; i++) {
    k_ipad[i] = k[i] ^ 0x36;
    k_opad[i] = k[i] ^ 0x5c;
  }
  /* inner = SHA256(k_ipad || msg). The agent only HMACs passwords and short
   * salt blocks, so a bounded stack concatenation is sufficient and keeps the
   * function allocation-free. */
  uint8_t inner_msg[64 + 1024];
  size_t cap = sizeof(inner_msg) - 64;
  if (msg_len > cap) msg_len = cap;
  memcpy(inner_msg, k_ipad, 64);
  memcpy(inner_msg + 64, msg, msg_len);
  uint8_t inner[32];
  sha256(inner_msg, 64 + msg_len, inner);

  uint8_t outer_msg[64 + 32];
  memcpy(outer_msg, k_opad, 64);
  memcpy(outer_msg + 64, inner, 32);
  sha256(outer_msg, 64 + 32, out);
}

void pbkdf2_hmac_sha256(const uint8_t* password, size_t pw_len,
                         const uint8_t* salt, size_t salt_len,
                         uint32_t iterations, uint8_t out[32]) {
  uint8_t block[32];
  /* We derive exactly one 32-byte block (DKLEN = 32). */
  uint8_t salt_block[64];  // salt || 4-byte BE counter (1)
  size_t sb_len = salt_len + 4;
  if (sb_len > sizeof(salt_block)) sb_len = sizeof(salt_block);
  memcpy(salt_block, salt, salt_len);
  salt_block[salt_len + 0] = 0;
  salt_block[salt_len + 1] = 0;
  salt_block[salt_len + 2] = 0;
  salt_block[salt_len + 3] = 1;

  uint8_t U[32];
  hmac_sha256(password, pw_len, salt_block, salt_len + 4, U);
  memcpy(block, U, 32);
  for (uint32_t i = 1; i < iterations; i++) {
    hmac_sha256(password, pw_len, U, 32, U);
    for (int j = 0; j < 32; j++) block[j] ^= U[j];
  }
  memcpy(out, block, 32);
}

std::string to_hex(const uint8_t* data, size_t len) {
  static const char* d = "0123456789abcdef";
  std::string s;
  s.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    s.push_back(d[data[i] >> 4]);
    s.push_back(d[data[i] & 0xf]);
  }
  return s;
}

bool from_hex(const char* hex, uint8_t* out, size_t max_out, size_t* out_len) {
  size_t n = strlen(hex);
  if (n % 2 != 0) return false;
  size_t bytes = n / 2;
  if (bytes > max_out) return false;
  for (size_t i = 0; i < bytes; i++) {
    int hi = hex[i * 2], lo = hex[i * 2 + 1];
    auto nib = [](int c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    int h = nib(hi), l = nib(lo);
    if (h < 0 || l < 0) return false;
    out[i] = (uint8_t)((h << 4) | l);
  }
  *out_len = bytes;
  return true;
}

}  // namespace dvs
