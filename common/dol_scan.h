#pragma once
#include "dol.h"
#include <cstddef>
#include <string>
#include <vector>

namespace uc {

constexpr size_t kSuspiciousZeroRunWords = 256;

struct ScanReport {
    size_t text_bytes = 0;
    size_t stwu_prologues = 0;        // stwu r1,d(r1)
    size_t blr_count = 0;
    size_t unique_bl_targets = 0;     // bl targets inside text sections = function-count estimate
    size_t largest_zero_run_words = 0;
    size_t suspicious_zero_runs = 0;  // runs >= kSuspiciousZeroRunWords (possible runtime-filled code)
};

struct SdkString {
    uint32_t address;
    std::string text;
};

ScanReport scan_text(std::span<const uint8_t> file, const DolImage& image);
std::vector<SdkString> find_sdk_strings(std::span<const uint8_t> file, const DolImage& image);

}  // namespace uc
