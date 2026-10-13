#include "audio_output.h"
#include <stdexcept>
namespace uc {
AudioOutput::AudioOutput(bool muted) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) throw std::runtime_error(SDL_GetError());
    const SDL_AudioSpec spec{SDL_AUDIO_S16,2,32000};
    stream_=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,nullptr,nullptr);
    if (!stream_) { SDL_QuitSubSystem(SDL_INIT_AUDIO); throw std::runtime_error(SDL_GetError()); }
    if ((muted && !SDL_SetAudioStreamGain(stream_,0)) || !SDL_ResumeAudioStreamDevice(stream_)) {
        const std::string message=SDL_GetError(); SDL_DestroyAudioStream(stream_); SDL_QuitSubSystem(SDL_INIT_AUDIO);
        throw std::runtime_error(message);
    }
}
AudioOutput::~AudioOutput() { SDL_DestroyAudioStream(stream_); SDL_QuitSubSystem(SDL_INIT_AUDIO); }
void AudioOutput::write(std::span<const int16_t> pcm) {
    const auto queued=SDL_GetAudioStreamQueued(stream_);
    if (queued<0) throw std::runtime_error(SDL_GetError());
    auto previous=max_queued_.load();
    while(queued>previous && !max_queued_.compare_exchange_weak(previous,queued)) {}
    // shortcut: discard an overrun above100ms; native clock/device drift control is TD-042.
    if (queued>32000*2*2/10) {
        if(!SDL_ClearAudioStream(stream_)) throw std::runtime_error(SDL_GetError());
        ++overruns_;
    }
    if (pcm.size_bytes()>INT_MAX || !SDL_PutAudioStreamData(stream_,pcm.data(),int(pcm.size_bytes()))) throw std::runtime_error(SDL_GetError());
    ++blocks_;
}
}
