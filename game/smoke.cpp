#include "dispatch.h"
#include "dol.h"
#include "game_decl.h"
#include "guest_mem.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

extern const uc::FnEntry uc_functions[];
extern const size_t uc_function_count;
extern const uint32_t uc_text_begin, uc_text_end;

int main(int argc, char** argv) {
    uc::guest_mem_init();
    uc::dispatch_init(uc_functions, uc_function_count, uc_text_begin, uc_text_end);
    std::printf("functions %zu  text %08X..%08X\n", uc_function_count, uc_text_begin, uc_text_end);
    if (argc == 2) {
        std::ifstream in(argv[1], std::ios::binary);
        std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto d = uc::parse_dol(f);
        if (!d.ok) { std::fprintf(stderr, "invalid DOL: %s\n", d.error.c_str()); return 1; }
        for (const auto& s : d.image.sections) std::memcpy(uc::guest_mem_base() + (s.address & 0x3FFFFFFF), f.data() + s.file_offset, s.size);
        std::printf("entry %08X known: %s\n", d.image.entry, uc::has_function(d.image.entry) ? "yes" : "NO");
        return uc::has_function(d.image.entry) ? 0 : 1;
    }
    return 0;
}
