#pragma once
// BRSTM (Nintendo NW4R stream) files: the PCM16 writer the audio converter uses and a reader (PCM16 and DSP-ADPCM) that lets tests compare against the
// game's own streams. The layout is described in docs/superpowers/plans/2026-10-22-asset-mods-audio.md (spike result); all fields are big-endian.
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace uc::mods {

struct PcmAudio {
    uint32_t rate = 0;
    std::vector<std::vector<int16_t>> channels;      // one vector per channel, all the same length
    std::optional<std::pair<uint32_t, uint32_t>> loop;   // start sample, end sample (exclusive); a BRSTM loop runs to the end of the stream
    size_t frames() const { return channels.empty() ? 0 : channels[0].size(); }
};

// Throws std::invalid_argument for 0 or more than 2 channels, no samples, channels of different lengths, a zero rate, or a loop that does not end at the
// last sample (BRSTM loops run to the end of the stream: trim the audio first).
std::vector<uint8_t> write_brstm_pcm16(const PcmAudio& audio);

// Decodes codec 1 (PCM16) and codec 2 (DSP-ADPCM); nullopt for another codec or a file that is cut short or inconsistent.
std::optional<PcmAudio> read_brstm(std::span<const uint8_t> file);

struct BrstmHeader {
    uint8_t codec = 0, channels = 0;
    uint32_t rate = 0, frames = 0;
    bool looped = false;
    uint32_t loop_start = 0;
};
// Needs the first 0x100 bytes of the file (more is fine); nullopt when they are not a BRSTM header.
std::optional<BrstmHeader> read_brstm_header(std::span<const uint8_t> first_bytes);

}  // namespace uc::mods
