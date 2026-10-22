#pragma once
// Turns what a mod ships (any rate, 1 or 2 channels, an optional loop) into what the game's stream expects: the original stream's rate and channel count,
// and a loop that runs to the end of the audio (a BRSTM loop always does). Offline and single-threaded; call it from a worker.
#include "audio/brstm.h"
#include <functional>
#include <string>
#include <vector>

namespace uc::mods {

struct ConvertOptions {
    uint32_t target_rate = 32000;
    uint32_t target_channels = 1;
    std::optional<std::pair<uint32_t, uint32_t>> loop_override;   // in samples of the source; wins over the loop the source carries
    std::function<bool()> cancel;                                  // polled during long work; true stops it ("cancelled")
};

struct ConvertResult {
    std::optional<PcmAudio> audio;
    std::vector<std::string> notes;   // what was done that an author may want to know (downmix, dropped loop), one line each
    std::string error;                // set when audio is empty
};

inline constexpr uint32_t kMaxSourceSeconds = 30 * 60;

ConvertResult convert_audio(PcmAudio source, const ConvertOptions& options);

}  // namespace uc::mods
