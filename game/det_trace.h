#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>

namespace uc {
// Determinism check (`--det-out <file> [--det-frames N]`): at every VI retrace the guest memory (MEM1 24 MiB, MEM2 64 MiB) is hashed in 64 KiB
// chunks and appended to the file as: u32 retrace number, then one u64 hash per chunk. Two runs of the same build must produce identical files;
// scripts/det_check.py reports the first retrace and the guest address ranges where they differ.
class DetTrace {
public:
    static constexpr uint32_t kChunk = 64 * 1024, kMem1 = 24u << 20, kMem2 = 64u << 20, kChunks = (kMem1 + kMem2) / kChunk;
    DetTrace(const std::filesystem::path& out, const uint8_t* mem, uint32_t frames);
    ~DetTrace();
    void on_retrace(uint64_t retrace);   // called by the baton holder inside the retrace event: guest memory is quiet
    bool done() const { return done_.load(); }
private:
    std::FILE* file_;
    const uint8_t* mem_;
    uint32_t frames_, written_ = 0;
    std::atomic<bool> done_{false};
};
}
