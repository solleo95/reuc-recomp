#include "sha256.h"
#include <algorithm>
#include <cstring>
namespace uc::update {
namespace {
constexpr uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
constexpr uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
}
Sha256::Sha256() : state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}
void Sha256::block(const unsigned char* data) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = uint32_t(data[4 * i]) << 24 | uint32_t(data[4 * i + 1]) << 16 | uint32_t(data[4 * i + 2]) << 8 | data[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto s = state_;
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = s[7] + (rotr(s[4], 6) ^ rotr(s[4], 11) ^ rotr(s[4], 25)) + ((s[4] & s[5]) ^ (~s[4] & s[6])) + kRound[i] + w[i];
        const uint32_t t2 = (rotr(s[0], 2) ^ rotr(s[0], 13) ^ rotr(s[0], 22)) + ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
        s[7] = s[6]; s[6] = s[5]; s[5] = s[4]; s[4] = s[3] + t1; s[3] = s[2]; s[2] = s[1]; s[1] = s[0]; s[0] = t1 + t2;
    }
    for (int i = 0; i < 8; ++i) state_[i] += s[i];
}
void Sha256::update(std::span<const unsigned char> bytes) {
    length_ += bytes.size();
    while (!bytes.empty()) {
        if (!used_ && bytes.size() >= 64) { block(bytes.data()); bytes = bytes.subspan(64); continue; }
        const size_t take = std::min(bytes.size(), buffer_.size() - used_);
        std::memcpy(buffer_.data() + used_, bytes.data(), take);
        used_ += take; bytes = bytes.subspan(take);
        if (used_ == 64) { block(buffer_.data()); used_ = 0; }
    }
}
std::string Sha256::finish_hex() {
    const uint64_t bits = length_ * 8;
    unsigned char pad[72] = {0x80};
    const size_t zeros = (used_ < 56 ? 56 : 120) - used_;
    for (int i = 0; i < 8; ++i) pad[zeros + i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
    update({pad, zeros + 8});
    static constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    for (uint32_t word : state_)
        for (int shift = 28; shift >= 0; shift -= 4) hex.push_back(digits[(word >> shift) & 15]);
    return hex;
}
std::string sha256_hex(std::string_view bytes) {
    Sha256 hash;
    hash.update({reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()});
    return hash.finish_hex();
}
}
