#pragma once
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <span>
#include <stdexcept>
#include <vector>
#include <memory>
#include <list>
#include <unordered_map>

namespace gx {
// Byte-wise equality as one memcmp; std::equal over vector/span iterators compares element by element.
inline bool same_bytes(std::span<const std::byte> a, std::span<const std::byte> b) { return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0); }
enum class TextureFormat : uint8_t { i4 = 0, i8 = 1, ia4 = 2, ia8 = 3, rgb565 = 4, rgb5a3 = 5, rgba8 = 6, c4 = 8, c8 = 9, c14x2 = 10, cmpr = 14 };
enum class PaletteFormat : uint8_t { ia8, rgb565, rgb5a3 };
struct PaletteView { PaletteFormat format = PaletteFormat::ia8; std::span<const std::byte> bytes; };
enum class TextureErrorCode { descriptor, unsupported, truncated };
struct TextureError : std::runtime_error {
    TextureError(TextureErrorCode code, const char* message) : std::runtime_error(message), code(code) {}
    TextureErrorCode code;
};
struct TextureDesc {
    uint32_t width = 0, height = 0;
    TextureFormat format = TextureFormat::rgba8;
    uint8_t levels = 1;
};
struct TextureLevel {
    uint32_t width, height;
    std::vector<uint8_t> rgba;
};
enum class TextureChannels:uint8_t { rgba,alpha_intensity,rgb565 };
// A background decode in flight: `done` is set (release) after the worker wrote TexturePixels::levels or stored the error.
struct TextureJob {
    std::mutex mutex; std::condition_variable ready; std::atomic<bool> done{false}; std::exception_ptr error;
    void finish(std::exception_ptr failure) { { std::lock_guard lock(mutex); error = std::move(failure); done.store(true, std::memory_order_release); } ready.notify_all(); }
};
struct TexturePixels {
    std::vector<TextureLevel> levels; bool gpu_only=false; TextureChannels channels=TextureChannels::rgba;
    // A replacement from `textures/*.dds`: levels[i].rgba holds BC7 blocks instead of RGBA8, and the extent is the upscaled one;
    // guest_width/height keep the game's size for code that recognises textures by it (late_aim.h).
    bool bc7=false; uint32_t guest_width=0, guest_height=0;
    std::shared_ptr<TextureJob> job;   // set while a worker still fills `levels`
    // Blocks until the pixels are decoded (no-op for finished textures) and rethrows a decode error. Call before reading `levels`.
    void wait() const {
        if (!job || job->done.load(std::memory_order_acquire)) { if (job && job->error) std::rethrow_exception(job->error); return; }
        std::unique_lock lock(job->mutex); job->ready.wait(lock, [&] { return job->done.load(std::memory_order_acquire); });
        if (job->error) std::rethrow_exception(job->error);
    }
};

// Decodes texture data into owned RGBA8 pixels; guest addresses never escape.
TexturePixels decode_texture(const TextureDesc& desc, std::span<const std::byte> pixels, PaletteView palette = {});
size_t encoded_texture_size(const TextureDesc& desc);
class TextureCache {
public:
    explicit TextureCache(size_t budget = 64 * 1024 * 1024);
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;
    TextureCache(TextureCache&&) = delete;
    TextureCache& operator=(TextureCache&&) = delete;
    std::shared_ptr<const TexturePixels> snapshot(uint32_t address, const TextureDesc& desc, std::span<const std::byte> pixels, PaletteView palette = {});
    // With async on, a cache miss returns at once and a small worker pool fills the pixels; the renderer calls TexturePixels::wait() before it reads them.
    void set_async(bool on) { async_ = on; }
    void set_budget(size_t bytes) { budget_ = bytes; }   // any thread (mods can switch texture packs on while the game runs)
    size_t resident_bytes() const { return resident_; }
    uint64_t decodes() const { return decodes_; }
    void clear() { by_address_.clear(); entries_.clear(); resident_ = 0; decodes_ = 0; }
private:
    struct Entry {
        uint32_t address;
        TextureDesc desc;
        PaletteFormat format;
        std::vector<std::byte> encoded, palette;
        std::shared_ptr<const TexturePixels> pixels;
        size_t bytes;
        uint32_t stamp = 0, skips = 0;   // write-barrier stamp of the last byte comparison; uses since then
    };
    std::list<Entry> entries_;
    std::unordered_multimap<uint32_t, std::list<Entry>::iterator> by_address_;
    static uint64_t replacement_generation();   // of the replacement table (texture_replacement.h)
    std::atomic<size_t> budget_;
    size_t resident_ = 0;
    uint64_t seen_generation_ = 0;
    uint64_t decodes_ = 0;
    bool async_ = false;
};
} // namespace gx
