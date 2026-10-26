#include "audio_output.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace uc {
AudioOutput::AudioOutput(bool muted) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) throw std::runtime_error(SDL_GetError());
    const SDL_AudioSpec spec{SDL_AUDIO_S16,2,32000};
    stream_=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,nullptr,nullptr);
    if (!stream_) { SDL_QuitSubSystem(SDL_INIT_AUDIO); throw std::runtime_error(SDL_GetError()); }
    user_muted_=muted;
    if ((muted && !SDL_SetAudioStreamGain(stream_,0)) || !SDL_ResumeAudioStreamDevice(stream_)) {
        const std::string message=SDL_GetError(); SDL_DestroyAudioStream(stream_); SDL_QuitSubSystem(SDL_INIT_AUDIO);
        throw std::runtime_error(message);
    }
}
AudioOutput::~AudioOutput() { SDL_DestroyAudioStream(stream_); SDL_QuitSubSystem(SDL_INIT_AUDIO); }
void AudioOutput::pause(bool on) {
    // The guest keeps mixing while paused (write() caps the queue at 100 ms); what piled up meanwhile is dropped on resume.
    if (!(on ? SDL_PauseAudioStreamDevice(stream_) : SDL_ClearAudioStream(stream_) && SDL_ResumeAudioStreamDevice(stream_))) throw std::runtime_error(SDL_GetError());
}
namespace {
// UC_AUDIO_DUMP=<prefix>: every block the game hands over, to <prefix>.<pid> as {steady ns u64, bytes u32, queued bytes u32, pcm}; analysed offline for gaps and clicks.
std::FILE* dump_file() {
    static std::FILE* file=[]()->std::FILE* {
        const char* prefix=std::getenv("UC_AUDIO_DUMP");
        if(!prefix) return nullptr;
        return std::fopen((std::string(prefix)+"."+std::to_string(GetCurrentProcessId())).c_str(),"wb");
    }();
    return file;
}
}
void AudioOutput::write(std::span<const int16_t> pcm) {
    const auto queued=SDL_GetAudioStreamQueued(stream_);
    if (queued<0) throw std::runtime_error(SDL_GetError());
    if (auto* dump=dump_file()) {
        const uint64_t now=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        const uint32_t bytes=uint32_t(pcm.size_bytes());
        const uint32_t queued_bytes=uint32_t(queued);
        std::fwrite(&now,8,1,dump); std::fwrite(&bytes,4,1,dump); std::fwrite(&queued_bytes,4,1,dump); std::fwrite(pcm.data(),1,bytes,dump);
    }
    auto previous=max_queued_.load();
    while(queued>previous && !max_queued_.compare_exchange_weak(previous,queued)) {}
    if (queued==0) ++dry_;
    // Network play: the guest delivers audio in bursts (a 30-80 ms gap is common around the start and under rollbacks) and the queue holds only
    // ~15 ms, so every gap ran the device dry and crackled. Whenever the queue is nearly empty it is topped up with silence to a cushion that
    // outlasts such gaps: one inaudible hiccup instead of a crackle at every gap; the cushion is also the added latency (50 ms).
    bool low=queued==0;
    if (gap_mute_enabled_) {
        constexpr int low_water=32000*4*12/1000, cushion=32000*4*50/1000;
        low=queued<low_water;
        if (low) {
            static const std::vector<char> silence(cushion,0);
            if (!SDL_PutAudioStreamData(stream_,silence.data(),cushion-queued)) throw std::runtime_error(SDL_GetError());
            ++refills_;
        }
    }
    if (gap_mute_enabled_ && !user_muted_) {
        const bool muted=gaps_.update(low);
        if (muted!=gap_muted_) {
            gap_muted_=muted;
            if(!SDL_SetAudioStreamGain(stream_,muted ? 0.f : 1.f)) throw std::runtime_error(SDL_GetError());
            std::fprintf(stderr,"[audio] %s\n",muted ? "the game runs behind: sound muted until it catches up" : "sound restored");
        }
    }
    const int cap=32000*2*2/10 + (gap_mute_enabled_ ? 32000*4*50/1000 : 0);
    if (const float ratio=catch_up_.update(queued,cap); ratio!=ratio_) {
        ratio_=ratio;
        if (!SDL_SetAudioStreamFrequencyRatio(stream_,ratio)) throw std::runtime_error(SDL_GetError());
    }
    // shortcut: discard an overrun above the cap if the faster playback did not keep up; native clock/device drift control is TD-042.
    if (queued>cap) {
        if(!SDL_ClearAudioStream(stream_)) throw std::runtime_error(SDL_GetError());
        ++overruns_;
    }
    if (pcm.size_bytes()>INT_MAX || !SDL_PutAudioStreamData(stream_,pcm.data(),int(pcm.size_bytes()))) throw std::runtime_error(SDL_GetError());
    ++blocks_;
}
}
