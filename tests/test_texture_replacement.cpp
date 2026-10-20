#include "check.h"
#include "bc7.h"
#include "texture_replacement.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>

static std::vector<std::byte> bytes_of(const std::vector<uint8_t>& v) {
    std::vector<std::byte> out(v.size()); std::memcpy(out.data(), v.data(), v.size()); return out;
}
static std::vector<uint8_t> counting(size_t n, int first = 0, int mod = 256) { std::vector<uint8_t> v(n); for (size_t i = 0; i < n; ++i) v[i] = uint8_t(first + int(i) % mod); return v; }

static void write_dds(const std::filesystem::path& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& blocks, uint32_t mips) {
    uint32_t header[37] = {};
    header[0] = 0x20534444; header[1] = 124; header[3] = height; header[4] = width; header[7] = mips;
    header[19] = 32; header[20] = 4; header[21] = 0x30315844; header[32] = 98; header[33] = 3; header[35] = 1;
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(header), sizeof header);
    file.write(reinterpret_cast<const char*>(blocks.data()), std::streamsize(blocks.size()));
}

int main() {
    // XXH64 vectors, identical to tools/texture_extract/extract_textures.py:xxhash64 (short, 4-byte tail, 32-byte stripes).
    CHECK(gx::xxh64({}) == 0xef46db3751d8e999ull);
    CHECK(gx::xxh64(bytes_of(counting(7))) == 0x14cc643f630c72d2ull);
    CHECK(gx::xxh64(bytes_of(counting(40))) == 0xf5da40f1b11741e9ull);
    CHECK(gx::xxh64(bytes_of(counting(64))) == 0xf7c67301db6713f0ull);

    // Names as the extractor writes them.
    const auto rgba = bytes_of(counting(64));
    CHECK(gx::texture_key({4, 4, gx::TextureFormat::rgba8, 1}, rgba, {}, false) == "4x4_f7c67301db6713f0_6");
    const auto base = bytes_of([] { std::vector<uint8_t> v(32); for (size_t i = 0; i < 32; ++i) v[i] = uint8_t(i % 5); return v; }());
    const auto palette = bytes_of(counting(20, 100));
    const gx::TextureDesc c8{8, 4, gx::TextureFormat::c8, 1};
    CHECK(gx::texture_key(c8, base, {gx::PaletteFormat::rgb565, palette}, false) == "8x4_4f2e5c4e08ba0fc5_0bd24e5e2376860c_9");
    CHECK(gx::texture_key(c8, base, {gx::PaletteFormat::rgb565, palette}, true) == "8x4_4f2e5c4e08ba0fc5_$_9");
    CHECK(gx::texture_key(c8, base, {gx::PaletteFormat::rgb565, std::span(palette).first(8)}, false).empty());   // indices reach past the loaded palette

    // BC7 mode 6 round trip: a flat block is near exact, a gradient with alpha stays close.
    {
        uint8_t px[64], out[16], back[64];
        for (int i = 0; i < 16; ++i) { px[i * 4] = 100; px[i * 4 + 1] = 150; px[i * 4 + 2] = 200; px[i * 4 + 3] = 255; }
        uc::bc7_encode_block(px, out); CHECK(uc::bc7_decode_block(out, back));
        for (int i = 0; i < 64; ++i) CHECK(std::abs(int(back[i]) - px[i]) <= 1);
        for (int i = 0; i < 16; ++i) { px[i * 4] = uint8_t(i * 16); px[i * 4 + 1] = uint8_t(255 - i * 16); px[i * 4 + 2] = 40; px[i * 4 + 3] = uint8_t(i * 17); }
        uc::bc7_encode_block(px, out); CHECK(uc::bc7_decode_block(out, back));
        int total = 0; for (int i = 0; i < 64; ++i) total += std::abs(int(back[i]) - px[i]);
        CHECK(total / 64 <= 4);
    }

    // A replacement on disk is found by the game's texture name, with or without "_m", and loads as BC7 with the guest size kept.
    const auto dir = std::filesystem::temp_directory_path() / "uc_texture_replacement_test";
    std::filesystem::remove_all(dir); std::filesystem::create_directories(dir);
    const gx::TextureDesc desc{4, 4, gx::TextureFormat::rgba8, 1};
    {
        std::vector<uint8_t> blocks;
        uint8_t px[64]; for (int i = 0; i < 64; ++i) px[i] = uint8_t(i * 3);
        for (uint32_t size = 8; size >= 1; size /= 2) {   // 8x8, 4x4, 2x2, 1x1: one block each except the first
            uint8_t block[16];
            for (int n = size == 8 ? 4 : 1; n; --n) { uc::bc7_encode_block(px, block); blocks.insert(blocks.end(), block, block + 16); }
        }
        write_dds(dir / "tex1_4x4_m_f7c67301db6713f0_6.dds", 8, 8, blocks, 4);
        write_dds(dir / "tex1_4x4_ffffffffffffffff_6.dds", 8, 8, std::vector<uint8_t>(16), 1);   // wrong content for another texture, must be ignored
    }
    gx::TextureReplacements replacements;
    CHECK(replacements.load_index(dir) == 2);
    const auto found = replacements.find(desc, rgba, {});
    CHECK(found && found->bytes > 148);
    const auto loaded = gx::load_dds(found->path);
    CHECK(loaded.bc7 && loaded.levels.size() == 4 && loaded.levels[0].width == 8 && loaded.levels[0].rgba.size() == 64 && loaded.levels[3].width == 1 && loaded.levels[3].rgba.size() == 16);
    auto other = counting(64, 1);
    CHECK(!replacements.find(desc, bytes_of(other), {}));

    // Through the cache: the replacement replaces the decoded pixels and remembers the guest extent.
    gx::texture_replacements().load_index(dir);
    gx::TextureCache cache;
    const auto texture = cache.snapshot(0x80100000, desc, rgba);
    texture->wait();
    CHECK(texture->bc7 && texture->guest_width == 4 && texture->guest_height == 4 && texture->levels[0].width == 8);
    const auto plain = cache.snapshot(0x80100100, desc, bytes_of(other));
    CHECK(!plain->bc7 && plain->levels[0].width == 4);

    // A truncated file is rejected instead of read past its end.
    std::filesystem::resize_file(found->path, 148 + 64);
    bool rejected = false;
    try { gx::load_dds(found->path); } catch (const gx::TextureError&) { rejected = true; }
    CHECK(rejected);
    std::filesystem::remove_all(dir);
    return 0;
}
