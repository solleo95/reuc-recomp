#pragma once
// Readers for the audio files a mod may ship (audio/<stream>.wav|.ogg). A loop found in the file (WAV `smpl` chunk, Vorbis comments) is returned in
// PcmAudio::loop as sample indices of the file itself (end exclusive); the converter rescales and trims it.
#include "audio/brstm.h"
#include <span>
#include <string>

namespace uc::mods {

struct DecodeResult {
    std::optional<PcmAudio> audio;
    std::string error;   // set when audio is empty: says what is wrong with the file
};

// PCM 8/16/24/32-bit integer and 32-bit float, 1 or 2 channels; unknown chunks are skipped.
DecodeResult read_wav(std::span<const uint8_t> file);

// Ogg Vorbis, 1 or 2 channels.
DecodeResult read_ogg(std::span<const uint8_t> file);

}  // namespace uc::mods
