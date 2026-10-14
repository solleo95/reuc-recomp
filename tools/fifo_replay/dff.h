#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <vector>

namespace fifo_replay {
struct Range { uint64_t offset=0; uint32_t size=0; };
struct Update {
    uint32_t position=0,address=0;
    Range data;
    uint8_t type=0;
};
struct Frame {
    Range fifo;
    uint32_t fifo_start=0,fifo_end=0;
    std::vector<Update> updates;
};
struct Dff {
    std::vector<std::byte> bytes;
    std::array<uint32_t,256> bp{},cp{};
    std::array<uint32_t,4096> xf{};
    std::array<uint32_t,88> xf_registers{};
    Range tmem;
    uint32_t mem1_size=0,mem2_size=0;
    bool wii=false;
    std::array<char,8> game_id{};
    std::vector<Frame> frames;
    std::span<const std::byte> data(Range range) const;
};
Dff parse_dff(std::vector<std::byte> bytes);
Dff load_dff(const std::filesystem::path& path);
}
