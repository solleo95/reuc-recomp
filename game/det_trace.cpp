#include "det_trace.h"
#include "fatal.h"
#include <cstring>
#include <vector>

namespace uc {
namespace {
uint64_t hash_chunk(const uint8_t* data, size_t size) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i + 8 <= size; i += 8) {
        uint64_t word; std::memcpy(&word, data + i, 8);
        h = (h ^ word) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    return h;
}
}  // namespace

DetTrace::DetTrace(const std::filesystem::path& out, const uint8_t* mem, uint32_t frames) : mem_(mem), frames_(frames) {
    file_ = _wfopen(out.c_str(), L"wb");
    if (!file_) fatal("cannot create the determinism trace %ls", out.c_str());
}

DetTrace::~DetTrace() { if (file_) std::fclose(file_); }

void DetTrace::on_retrace(uint64_t retrace) {
    if (done_) return;
    std::vector<uint64_t> hashes(kChunks);
    for (uint32_t i = 0; i < kMem1 / kChunk; ++i) hashes[i] = hash_chunk(mem_ + size_t(i) * kChunk, kChunk);
    for (uint32_t i = 0; i < kMem2 / kChunk; ++i) hashes[kMem1 / kChunk + i] = hash_chunk(mem_ + 0x10000000u + size_t(i) * kChunk, kChunk);
    const uint32_t number = uint32_t(retrace);
    std::fwrite(&number, 4, 1, file_);
    std::fwrite(hashes.data(), 8, hashes.size(), file_);
    if (++written_ >= frames_) { std::fflush(file_); done_ = true; }
}
}
