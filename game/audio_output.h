#pragma once
#include "vulkan_renderer.h"
#include <span>
namespace uc {
class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();
    AudioOutput(const AudioOutput&)=delete;
    AudioOutput& operator=(const AudioOutput&)=delete;
    void write(std::span<const int16_t> pcm);
private:
    SDL_AudioStream* stream_=nullptr;
};
}
