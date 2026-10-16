#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <atomic>

namespace uc {

struct MmioDevice {
    virtual ~MmioDevice() = default;
    virtual uint32_t read(uint32_t off, int size) = 0;
    virtual void write(uint32_t off, int size, uint32_t v) = 0;
};

// Plain registers: big-endian bytes, reads return the last write.
struct RegFile : MmioDevice {
    explicit RegFile(uint32_t size) : bytes(size) {}
    uint32_t read(uint32_t off, int size) override;
    void write(uint32_t off, int size, uint32_t v) override;
    std::vector<uint8_t> bytes;
};

class MmioBus {
public:
    void map(uint32_t base, uint32_t size, MmioDevice* dev, std::string name);   // throws std::invalid_argument on overlap
    uint32_t read(uint32_t ea, int size);
    void write(uint32_t ea, int size, uint32_t v);
    size_t unmapped_count() const { return unmapped_.load(std::memory_order_relaxed); }
private:
    struct Range { uint32_t base, size; MmioDevice* dev; std::string name; };
    const Range* find(uint32_t ea, int size) const;
    void report(const char* what, uint32_t ea, int size);
    std::vector<Range> ranges_;   // sorted by base
    std::set<std::pair<uint32_t, int>> seen_;
    std::atomic<size_t> unmapped_{0};
};

MmioBus& mmio_bus();
// Stores to the GX FIFO port (0xCC008000) are the hottest MMIO access (every immediate-mode vertex word): they skip the range search of the bus.
void mmio_set_gx_fifo(MmioDevice* device);
}  // namespace uc
