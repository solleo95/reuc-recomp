#pragma once
// Readers for the audio files a mod may ship (audio/<stream>.wav|.ogg). A loop found in the file (WAV `smpl` chunk, Vorbis comments) is returned in
// PcmAudio::loop as sample indices of the file itself (end exclusive); the converter rescales and trims it.
#include "audio/brstm.h"
#include <functional>
#include <span>
#include <string>

namespace uc::mods {

struct DecodeResult {
    std::optional<PcmAudio> audio;
    std::string error;   // set when audio is empty: says what is wrong with the file
};

// PCM 8/16/24/32-bit integer and 32-bit float, 1 or 2 channels; unknown chunks are skipped.
DecodeResult read_wav(std::span<const uint8_t> file);

// What a decode may use up: the Vorbis length is only known by decoding it, so a file that runs past `max_seconds` stops with an error, and `cancel` is polled.
struct DecodeLimits {
    uint32_t max_seconds = 0;           // 0: no limit
    std::function<bool()> cancel;       // true: stop ("cancelled")
};

// Ogg Vorbis, 1 or 2 channels.
DecodeResult read_ogg(std::span<const uint8_t> file, const DecodeLimits& limits = {});

}  // namespace uc::mods
