#pragma once
#include "audio_gap.h"
#include "vulkan_renderer.h"
#include <span>
#include <atomic>
namespace uc {
class AudioOutput {
public:
    struct Stats { uint64_t blocks,overruns; int max_queued_bytes; uint64_t dry_writes; };   // dry: the device had played everything before the next block arrived
    explicit AudioOutput(bool muted=false);
    ~AudioOutput();
    AudioOutput(const AudioOutput&)=delete;
    AudioOutput& operator=(const AudioOutput&)=delete;
    void write(std::span<const int16_t> pcm);
    void mute_on_gaps(bool on) { gap_mute_enabled_=on; }   // network play: silence instead of stutter while the game runs behind
    void pause(bool on);   // game pause: silence the device
    uint64_t refills() const { return refills_.load(); }   // network play: times the queue was topped up with silence
    Stats stats() const { return {blocks_.load(),overruns_.load(),max_queued_.load(),dry_.load()}; }
private:
    SDL_AudioStream* stream_=nullptr;
    bool gap_mute_enabled_=false,user_muted_=false,gap_muted_=false;
    AudioGapMute gaps_;
    AudioCatchUp catch_up_;
    float ratio_=1.f;
    std::atomic<uint64_t> blocks_{0},overruns_{0},dry_{0},refills_{0};
    std::atomic<int> max_queued_{0};
};
}
