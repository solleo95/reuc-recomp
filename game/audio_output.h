#pragma once
#include "vulkan_renderer.h"
#include <span>
#include <atomic>
namespace uc {
class AudioOutput {
public:
    struct Stats { uint64_t blocks,overruns; int max_queued_bytes; };
    explicit AudioOutput(bool muted=false);
    ~AudioOutput();
    AudioOutput(const AudioOutput&)=delete;
    AudioOutput& operator=(const AudioOutput&)=delete;
    void write(std::span<const int16_t> pcm);
    Stats stats() const { return {blocks_.load(),overruns_.load(),max_queued_.load()}; }
private:
    SDL_AudioStream* stream_=nullptr;
    std::atomic<uint64_t> blocks_{0},overruns_{0};
    std::atomic<int> max_queued_{0};
};
}
