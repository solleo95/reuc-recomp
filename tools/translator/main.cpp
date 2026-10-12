#include "analyzer/analyzer.h"
#include "translator/emit.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::string dol_path, out_dir;
    std::vector<std::string> cfg_paths;   // --cfg may be given several times; the files are merged in order
    size_t shard = 150;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        auto arg = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
        if (arg("--dol")) dol_path = argv[++i];
        else if (arg("--out")) out_dir = argv[++i];
        else if (arg("--cfg")) cfg_paths.push_back(argv[++i]);
        else if (arg("--shard")) shard = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--verbose")) verbose = true;
        else { std::fprintf(stderr, "usage: translator --dol <file> --out <dir> [--cfg <file>]... [--shard N]\n"); return 2; }
    }
    if (dol_path.empty() || out_dir.empty() || shard == 0) { std::fprintf(stderr, "usage: translator --dol <file> --out <dir> [--cfg <file>]... [--shard N]\n"); return 2; }

    std::ifstream in(dol_path, std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", dol_path.c_str()); return 1; }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto dol = uc::parse_dol(file);
    if (!dol.ok) { std::fprintf(stderr, "invalid DOL: %s\n", dol.error.c_str()); return 1; }

    uc::Program p;
    std::string err;
    for (const auto& cp : cfg_paths)
        if (!uc::load_config(cp, p.cfg, err)) { std::fprintf(stderr, "config error in %s: %s\n", cp.c_str(), err.c_str()); return 1; }
    p.img = uc::Image{file, &dol.image};
    p.an = uc::analyze(p.img, p.cfg);

    uc::EmitStats st;
    uc::Output out = uc::emit_program(p, shard, st);
    std::filesystem::create_directories(out_dir);
    // Remove shards of an older, larger run: they would define the same functions twice.
    for (const auto& e : std::filesystem::directory_iterator(out_dir)) {
        std::string n = e.path().filename().string();
        std::string ext = e.path().extension().string();
        if (n.rfind("game_", 0) == 0 && (ext == ".cpp" || ext == ".h")) std::filesystem::remove(e.path());
    }
    for (auto& [name, text] : out.files) {
        std::ofstream f(std::filesystem::path(out_dir) / name, std::ios::binary);
        f << text;
        if (!f) { std::fprintf(stderr, "cannot write %s\n", name.c_str()); return 1; }
    }
    std::printf("functions %zu  instructions %zu  invalid-words %zu  outside-branch-traps %zu  unresolved-switch-bctr %zu  indirect-tail-bctr %zu\n",
                st.functions, st.instructions, st.invalid, st.traps_midfunc, st.unresolved_bctr, st.indirect_tail);
    std::printf("secondary-entries %zu  fallthrough-tails %zu  OE-traps %zu  unsupported %zu  jump-tables %zu  files %zu\n",
                st.secondary_entries, st.fallthrough_tail, st.oe_traps, st.unsupported, p.an.jump_tables.size(), out.files.size());
    if (verbose) for (uint32_t a : p.an.unresolved_bctr) std::printf("unresolved-switch-bctr %08X\n", a);
    return 0;
}
