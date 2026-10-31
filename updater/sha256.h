#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
namespace uc::update {
// Portable SHA-256 (the Windows updater hashes through BCrypt; the POSIX one uses this).
class Sha256 {
public:
    Sha256();
    void update(std::span<const unsigned char> bytes);
    std::string finish_hex();   // lower-case hex; the object is spent afterwards
private:
    void block(const unsigned char* data);
    std::array<uint32_t, 8> state_;
    std::array<unsigned char, 64> buffer_{};
    uint64_t length_ = 0;
    size_t used_ = 0;
};
std::string sha256_hex(std::string_view bytes);
}
