#include "programs.h"
#include <cstdio>
#include <fstream>

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: gen_e2e_dols <program> <out.dol>\n"); return 2; }
    const e2e::Program* p = e2e::find(argv[1]);
    if (!p) { std::fprintf(stderr, "unknown program %s\n", argv[1]); return 1; }
    auto dol = p->build();
    std::ofstream f(argv[2], std::ios::binary);
    f.write(reinterpret_cast<const char*>(dol.data()), std::streamsize(dol.size()));
    return f ? 0 : 1;
}
