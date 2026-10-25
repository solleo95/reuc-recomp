#include "analyzer/analyzer.h"
#include "translator/emit.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <algorithm>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

static bool same_generation(const std::filesystem::path& dir, const uc::Output& out) {
    for (const auto& [name, text] : out.files) {
        std::ifstream file(dir / name, std::ios::binary);
        if (!file) return false;
        std::string saved((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (saved != text) return false;
    }
    return true;
}

int main(int argc, char** argv) {
    std::string dol_path, out_dir, cache_key;
    std::vector<std::string> cfg_paths;   // --cfg may be given several times; the files are merged in order
    size_t shard = 150;
    bool verbose = false, cr_opt = true;
    for (int i = 1; i < argc; ++i) {
        auto arg = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
        if (arg("--dol")) dol_path = argv[++i];
        else if (arg("--out")) out_dir = argv[++i];
        else if (arg("--cfg")) cfg_paths.push_back(argv[++i]);
        else if (arg("--shard")) shard = std::strtoull(argv[++i], nullptr, 10);
        else if (arg("--cache-key")) cache_key = argv[++i];
        else if (!std::strcmp(argv[i], "--verbose")) verbose = true;
        else if (!std::strcmp(argv[i], "--no-cr-opt")) cr_opt = false;
        else { std::fprintf(stderr, "usage: translator --dol <file> --out <dir> [--cfg <file>]... [--shard N]\n"); return 2; }
    }
    if (dol_path.empty() || out_dir.empty() || shard == 0) { std::fprintf(stderr, "usage: translator --dol <file> --out <dir> [--cfg <file>]... [--shard N]\n"); return 2; }
    if (!cache_key.empty() && (cache_key.size() != 256 || !std::all_of(cache_key.begin(), cache_key.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))) {
        std::fprintf(stderr, "invalid --cache-key: expected four SHA-256 digests\n"); return 2;
    }

    std::ifstream in(dol_path, std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", dol_path.c_str()); return 1; }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto dol = uc::parse_dol(file);
    if (!dol.ok) { std::fprintf(stderr, "invalid DOL: %s\n", dol.error.c_str()); return 1; }

    uc::Program p;
    p.cr_opt = cr_opt;
    std::string err;
    for (const auto& cp : cfg_paths)
        if (!uc::load_config(cp, p.cfg, err)) { std::fprintf(stderr, "config error in %s: %s\n", cp.c_str(), err.c_str()); return 1; }
    p.img = uc::Image{file, &dol.image};
    p.an = uc::analyze(p.img, p.cfg);
    uc::add_resume_entries(p);

    uc::EmitStats st;
    uc::Output out = uc::emit_program(p, shard, st);
    std::filesystem::create_directories(out_dir);
    // Fingerprint is a directory hint; byte comparison prevents collision reuse.
    uint64_t fingerprint = 14695981039346656037ull;
    for (const auto& [name, text] : out.files) {
        for (const std::string* value : {&name, &text}) {
            for (unsigned char byte : *value) fingerprint = (fingerprint ^ byte) * 1099511628211ull;
            fingerprint = (fingerprint ^ 0) * 1099511628211ull;
        }
    }
    char id[32]; std::snprintf(id, sizeof id, "generation_%016llx", (unsigned long long)fingerprint);
    std::filesystem::path generation;
    bool reused = false;
    for (size_t suffix = 0;; ++suffix) {
        generation = std::filesystem::path(out_dir) / (std::string(id) + (suffix ? "_" + std::to_string(suffix) : ""));
        if (std::filesystem::create_directory(generation)) break;
        if (same_generation(generation, out)) { reused = true; break; }
    }
    for (auto& [name, text] : out.files) {
        if (reused) break;
        std::ofstream f(generation / name, std::ios::binary);
        f << text;
        f.close();
        if (!f) { std::fprintf(stderr, "cannot write %s\n", name.c_str()); return 1; }
    }
    // Old shards remain on disk; only this generation participates in the build.
    auto temporary = generation / ("manifest_" + std::to_string(GetCurrentProcessId()) + ".tmp");
    std::ofstream manifest(temporary, std::ios::binary);
    std::string relative = generation.filename().string();
    manifest << "# translation-input: " << cache_key << "\n";
    manifest << "set(UC_GENERATED_INCLUDE_DIR \"${CMAKE_CURRENT_LIST_DIR}/" << relative << "\")\nset(UC_GENERATED_SOURCES\n";
    for (const auto& [name, text] : out.files)
        if (std::filesystem::path(name).extension() == ".cpp")
            manifest << "  \"${CMAKE_CURRENT_LIST_DIR}/" << relative << "/" << name << "\"\n";
    manifest << ")\n";
    manifest.close();
    if (!manifest) { std::fprintf(stderr, "cannot write game_sources.cmake\n"); return 1; }
    auto published = std::filesystem::path(out_dir) / "game_sources.cmake";
    if (!MoveFileExW(temporary.c_str(), published.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::fprintf(stderr, "cannot publish game_sources.cmake (Windows error %lu); previous generation retained\n", GetLastError()); return 1;
    }
    std::printf("functions %zu  instructions %zu  invalid-words %zu  outside-branch-traps %zu  unresolved-switch-bctr %zu  indirect-tail-bctr %zu\n",
                st.functions, st.instructions, st.invalid, st.traps_midfunc, st.unresolved_bctr, st.indirect_tail);
    std::printf("secondary-entries %zu  fallthrough-tails %zu  OE-traps %zu  unsupported %zu  jump-tables %zu  files %zu\n",
                st.secondary_entries, st.fallthrough_tail, st.oe_traps, st.unsupported, p.an.jump_tables.size(), out.files.size());
    std::printf("cr-dead-compares %zu  cr-fused-branches %zu  cr-dropped-records %zu\n", st.cr_dead_compares, st.cr_fused_branches, st.cr_dropped_records);
    if (verbose) for (uint32_t a : p.an.unresolved_bctr) std::printf("unresolved-switch-bctr %08X\n", a);
    return 0;
}
