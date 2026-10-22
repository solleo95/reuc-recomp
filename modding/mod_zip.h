#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace uc::mods {

struct ZipLimits {
    uint64_t max_total_bytes = 512ull << 20;   // sum of uncompressed sizes
    uint32_t max_entries = 20000;
};

// Extracts a zip mod into cache_dir/<stem>-<8 hex of size+mtime>/ and sets root_out to the folder holding mod.json
// (a single top-level folder is unwrapped). A finished extraction is reused untouched. Every entry name must stay inside
// the target folder (zip-slip); on any problem nothing is left behind, false is returned and error names the cause.
bool extract_zip_mod(const std::filesystem::path& zip, const std::filesystem::path& cache_dir, std::filesystem::path& root_out,
                     std::string& error, ZipLimits limits = {});

}  // namespace uc::mods
