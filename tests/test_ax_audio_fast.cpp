// mix_ax_voice (the fast path) must leave exactly what mix_ax_voice_reference leaves: the mix, the guest memory and the exception, for any voice block and sample data,
// including the ones the fast path hands to the reference (sample data over the block, outside RAM, in MMIO, sample positions that overflow).
#include "check.h"
#include "ax_audio.h"
#include "ppc_fixture.h"
#include <array>
#include <cstring>
#include <exception>
#include <random>
#include <string>
#include <vector>

namespace {
constexpr uint32_t kBlock = 0x1000, kPb = 0x80000000u + kBlock, kData = 0x2000, kImage = 0x10000;
struct Outcome { std::array<int32_t,192> mix; std::vector<uint8_t> image; std::string error; };

template <class Mix> Outcome run(Fix& f, const std::vector<uint8_t>& start, const std::array<int32_t,192>& mix0, Mix&& mix_voice) {
    std::memcpy(f.c.mem, start.data(), kImage);
    Outcome out{mix0, {}, {}};
    try { mix_voice(f.c, kPb, std::span<int32_t,192>(out.mix)); }
    catch (const std::invalid_argument& e) { out.error = std::string("invalid_argument: ") + e.what(); }
    catch (const std::exception& e) { out.error = std::string("other: ") + e.what(); }
    out.image.assign(f.c.mem, f.c.mem + kImage);
    return out;
}
}

int main() {
    Fix f;
    std::mt19937 random(12345);
    auto pick = [&](std::initializer_list<uint32_t> values) { return *(values.begin() + random() % values.size()); };
    size_t compared = 0, thrown = 0, ended = 0, looped = 0;
    for (int round = 0; round < 6000; ++round) {
        std::vector<uint8_t> image(kImage);
        for (auto& b : image) b = uint8_t(random());
        std::memset(image.data() + kBlock, 0, 320);
        auto w16 = [&](uint32_t at, uint32_t v) { image[kBlock + at] = uint8_t(v >> 8); image[kBlock + at + 1] = uint8_t(v); };
        auto w32 = [&](uint32_t at, uint32_t v) { w16(at, v >> 16); w16(at + 2, v & 0xFFFF); };
        const uint32_t format = pick({0, 0, 10, 10, 25, 25, 7});
        const uint32_t data_at = pick({kData, kData + 1, kData + 3, 0x3FF0, 0xFFF0});   // sample positions are in the format's own units
        // a position (in samples) of the data, sometimes over the block itself, outside RAM, or at an overflowing value
        auto position = [&]() -> uint32_t {
            switch (random() % 12) {
            case 0: return kBlock / (format == 10 ? 2 : format == 25 ? 1 : 0.5) + random() % 200;   // over the block
            case 1: return 0x7FFFFFF0u + random() % 32;
            case 2: return 0xFFFFFF00u + random() % 256;
            case 3: return 0x01800000u / (format == 10 ? 2 : 1) - random() % 40;   // at the end of MEM1
            case 4: return 0x66000000u;
            default: return data_at + random() % 8 + (format == 0 ? 0 : 0);
            }
        };
        w16(0x10, random() % 20 == 0 ? random() % 3 : 1);
        w16(0x12, pick({0, 1, 2}));
        w32(0x0C, random() % 8);
        w16(0x14, random()); w16(0x18, random());
        w16(0x16, pick({0, 1, 0xFFFF, 0x7FFF, 0x8000, uint32_t(random())})); w16(0x1A, pick({0, 0xFFFF, 0x1234, uint32_t(random())}));
        w16(0x6A, random()); w16(0x6C, pick({0, 1, 0xFFFF, 0x8000, uint32_t(random())}));
        w16(0x6E, random() % 3 == 0);
        w32(0x72, position());
        w16(0x70, format);
        const uint32_t pos = position();
        w32(0x7A, pos);
        w32(0x76, random() % 3 == 0 ? pos + 1 + random() % 40 : position());
        for (uint32_t i = 0; i < 8; ++i) { w16(0x7E + i * 4, random()); w16(0x80 + i * 4, random()); }
        w16(0xA0, random() % 6 == 0 ? random() : (random() % 8) * 16 + random() % 16);
        w16(0xA2, random()); w16(0xA4, random());
        w32(0xA6, pick({0x10000, 0x10000, 0x8000, 0x20000, 0x30000, 0x40000, 0x40001, 0, 0x1234, uint32_t(random() % 0x45000)}));
        w16(0xAA, random()); w16(0xB2, random());
        w16(0xB4, random() % 6 == 0 ? random() : (random() % 8) * 16 + random() % 16); w16(0xB6, random()); w16(0xB8, random());
        if (random() % 5 == 0) for (uint32_t i = 0; i < 64; ++i) image[kData + i] = random() % 3 ? 0x11 : 0;   // data that makes ADPCM headers valid

        std::array<int32_t,192> mix0{};
        for (auto& m : mix0) m = int32_t(random() % 4001) - 2000;
        const auto expected = run(f, image, mix0, uc::mix_ax_voice_reference);
        const auto actual = run(f, image, mix0, uc::mix_ax_voice);
        CHECK(actual.error == expected.error);
        CHECK(actual.mix == expected.mix);
        CHECK(actual.image == expected.image);
        ++compared;
        thrown += !expected.error.empty();
        ended += expected.image[kBlock + 0x11] == 0 && image[kBlock + 0x11] == 1;
        looped += image[kBlock + 0x6F] != 0;
    }
    // the generator must reach the paths it is there for
    CHECK(compared == 6000);
    CHECK(thrown > 100 && thrown < 4000);
    CHECK(ended > 100);
    CHECK(looped > 500);
}
