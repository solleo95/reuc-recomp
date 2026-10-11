#include "dol.h"
#include "dol_scan.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: s1_scan <main.dol>\n"); return 2; }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto dol = uc::parse_dol(file);
    if (!dol.ok) { std::fprintf(stderr, "invalid DOL: %s\n", dol.error.c_str()); return 1; }

    std::printf("entry %08X  bss %08X size %08X\n", dol.image.entry, dol.image.bss_address, dol.image.bss_size);
    for (const auto& s : dol.image.sections)
        std::printf("%s addr %08X size %08X off %08X\n", s.is_text ? "text" : "data", s.address, s.size, s.file_offset);

    auto r = uc::scan_text(file, dol.image);
    std::printf("text bytes            %zu\n", r.text_bytes);
    std::printf("stwu r1 prologues     %zu\n", r.stwu_prologues);
    std::printf("blr                   %zu\n", r.blr_count);
    std::printf("unique bl targets     %zu\n", r.unique_bl_targets);
    std::printf("largest zero run (w)  %zu\n", r.largest_zero_run_words);
    std::printf("suspicious zero runs  %zu\n", r.suspicious_zero_runs);

    auto strs = uc::find_sdk_strings(file, dol.image);
    std::printf("SDK strings: %zu\n", strs.size());
    for (size_t i = 0; i < strs.size() && i < 300; ++i) std::printf("%08X  %s\n", strs[i].address, strs[i].text.c_str());
    return 0;
}
