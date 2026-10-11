#include "dol_scan.h"
#include <unordered_set>

namespace uc {

static uint32_t be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

ScanReport scan_text(std::span<const uint8_t> file, const DolImage& image) {
    ScanReport r;
    std::unordered_set<uint32_t> targets;
    for (const auto& s : image.sections) {
        if (!s.is_text) continue;
        auto b = section_bytes(file, s);
        size_t words = b.size() / 4;  // trailing partial word is ignored
        r.text_bytes += b.size();
        size_t run = 0;
        auto close_run = [&] {
            if (run > r.largest_zero_run_words) r.largest_zero_run_words = run;
            if (run >= kSuspiciousZeroRunWords) ++r.suspicious_zero_runs;
            run = 0;
        };
        for (size_t i = 0; i < words; ++i) {
            uint32_t w = be32(&b[i * 4]);
            uint32_t pc = s.address + uint32_t(i * 4);
            if (w == 0) { ++run; continue; }
            close_run();
            if ((w & 0xFFFF0000u) == 0x94210000u) ++r.stwu_prologues;
            if (w == 0x4E800020u) ++r.blr_count;
            if ((w >> 26) == 18 && (w & 3) == 1) {  // bl: AA=0, LK=1
                uint32_t li = w & 0x03FFFFFCu;
                if (li & 0x02000000u) li |= 0xFC000000u;
                targets.insert(pc + li);
            }
        }
        close_run();
    }
    for (uint32_t t : targets) {
        for (const auto& s : image.sections) {
            if (s.is_text && t >= s.address && t < s.address + (s.size & ~3u)) { ++r.unique_bl_targets; break; }
        }
    }
    return r;
}

std::vector<SdkString> find_sdk_strings(std::span<const uint8_t> file, const DolImage& image) {
    std::vector<SdkString> out;
    for (const auto& s : image.sections) {
        auto b = section_bytes(file, s);
        size_t start = 0;
        auto flush = [&](size_t end) {
            if (end - start < 8) return;
            std::string t(reinterpret_cast<const char*>(b.data()) + start, end - start);
            if (t.find("SDK") != std::string::npos) out.push_back({s.address + uint32_t(start), t});
        };
        for (size_t i = 0; i <= b.size(); ++i) {
            bool printable = i < b.size() && ((b[i] >= 0x20 && b[i] < 0x7F) || b[i] == '\t');
            if (!printable) { flush(i); start = i + 1; }
        }
    }
    return out;
}

}  // namespace uc
