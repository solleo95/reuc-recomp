#include "check.h"
#include "mod_paths.h"
#include "mod_zip.h"
#include <miniz.h>
#include <fstream>
#include <map>

namespace fs = std::filesystem;
using namespace uc::mods;

static fs::path fresh(const std::u8string& name) {
    const auto dir = fs::current_path() / name;
    std::error_code error;
    fs::remove_all(dir, error);
    fs::create_directories(dir);
    return dir;
}

// Builds a zip with the given entry names (stored verbatim, including "../" names) and contents.
static void make_zip(const fs::path& path, const std::map<std::string, std::string>& entries) {
    mz_zip_archive zip{};
    FILE* file = nullptr;
    _wfopen_s(&file, path.c_str(), L"wb");
    CHECK(file && mz_zip_writer_init_cfile(&zip, file, 0));
    for (const auto& [name, data] : entries) CHECK(mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), MZ_BEST_SPEED));
    CHECK(mz_zip_writer_finalize_archive(&zip));
    mz_zip_writer_end(&zip);
    std::fclose(file);
}

static std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }

// miniz's writer refuses unsafe names, so write a same-length placeholder and patch the real name into the local and central headers.
static void make_malicious_zip(const fs::path& path, const std::string& bad) {
    const std::string placeholder(bad.size(), 'x');
    make_zip(path, {{"mod.json", "{}"}, {placeholder, "x"}});
    auto bytes = slurp(path);
    for (size_t at = bytes.find(placeholder); at != std::string::npos; at = bytes.find(placeholder, at + 1)) bytes.replace(at, bad.size(), bad);
    std::ofstream(path, std::ios::binary | std::ios::trunc).write(bytes.data(), std::streamsize(bytes.size()));
}

static void zip_extract_ok() {
    const auto dir = fresh(u8"mod_zip_ok");
    make_zip(dir / "m.zip", {{"mod.json", "{\"id\":\"m\"}"}, {"scripts/main.lua", "print(1)"}});
    fs::path root; std::string error;
    CHECK(extract_zip_mod(dir / "m.zip", dir / "cache", root, error));
    CHECK(slurp(root / "mod.json") == "{\"id\":\"m\"}" && slurp(root / "scripts" / "main.lua") == "print(1)");
}

static void zip_slip() {
    for (const char* bad : {"../evil.txt", "..\\evil.txt", "/abs.txt", "a/../../evil.txt", "C:/evil.txt"}) {
        const auto dir = fresh(u8"mod_zip_slip");
        make_malicious_zip(dir / "m.zip", bad);
        fs::path root; std::string error;
        CHECK(!extract_zip_mod(dir / "m.zip", dir / "cache", root, error));
        CHECK(error.find(bad) != std::string::npos);
        CHECK(!fs::exists(dir / "evil.txt") && !fs::exists(dir / "cache" / "evil.txt") && !fs::exists(fs::current_path() / "evil.txt"));
    }
}

static void zip_single_folder_unwrapped() {
    const auto dir = fresh(u8"mod_zip_wrap");
    make_zip(dir / "m.zip", {{"inner/mod.json", "{\"id\":\"w\"}"}, {"inner/files/a.bin", "1"}});
    fs::path root; std::string error;
    CHECK(extract_zip_mod(dir / "m.zip", dir / "cache", root, error));
    CHECK(root.filename() == "inner" && fs::exists(root / "mod.json") && fs::exists(root / "files" / "a.bin"));
}

static void zip_too_big() {
    const auto dir = fresh(u8"mod_zip_big");
    make_zip(dir / "m.zip", {{"mod.json", "{}"}, {"a.bin", std::string(1000, 'a')}, {"b.bin", std::string(1000, 'b')}});
    fs::path root; std::string error;
    CHECK(!extract_zip_mod(dir / "m.zip", dir / "cache", root, error, ZipLimits{1500, 100}));   // total bytes
    CHECK(!error.empty());
    CHECK(!extract_zip_mod(dir / "m.zip", dir / "cache2", root, error, ZipLimits{1u << 20, 2}));   // entry count
    CHECK(extract_zip_mod(dir / "m.zip", dir / "cache3", root, error, ZipLimits{1u << 20, 3}));
}

static void zip_reextract_cached() {
    const auto dir = fresh(u8"mod_zip_cache");
    make_zip(dir / "m.zip", {{"mod.json", "{}"}, {"x.txt", "x"}});
    fs::path first, second; std::string error;
    CHECK(extract_zip_mod(dir / "m.zip", dir / "cache", first, error));
    fs::remove(first / "x.txt");
    CHECK(extract_zip_mod(dir / "m.zip", dir / "cache", second, error));
    CHECK(first == second && !fs::exists(second / "x.txt"));   // nothing was extracted again
}

static void discovery_extracts_zips() {
    const auto dir = fresh(u8"mod_zip_discovery_\u043c\u043e\u0434\u044b");
    make_zip(dir / "pack.zip", {{"mod.json", "{\"id\":\"pack\"}"}});
    make_zip(dir / "broken.zip", {{"readme.txt", "no manifest"}});
    fs::create_directories(dir / "plain");
    std::ofstream(dir / "plain" / "mod.json") << "{\"id\":\"plain\"}";
    const auto found = find_mod_folders(dir);
    CHECK(found.size() == 3);
    int ok = 0;
    for (const auto& mod : found) {
        if (mod.folder_name == "pack.zip") { CHECK(mod.manifest_text == "{\"id\":\"pack\"}" && mod.error.empty()); ++ok; }
        if (mod.folder_name == "broken.zip") { CHECK(mod.manifest_text.empty() && !mod.error.empty()); ++ok; }
        if (mod.folder_name == "plain") ++ok;
    }
    CHECK(ok == 3);
}

static void unreadable_zip_with_a_non_code_page_name() {
    const auto dir = fresh(u8"zip_names");
    fs::path root;
    std::string error;
    CHECK(!extract_zip_mod(dir / fs::path(u8"日本語.zip"), dir / ".cache", root, error, ZipLimits{}) && error.find(reinterpret_cast<const char*>(u8"日本語.zip")) != std::string::npos);
}

int main() {
    unreadable_zip_with_a_non_code_page_name();
    zip_extract_ok(); zip_slip(); zip_single_folder_unwrapped(); zip_too_big(); zip_reextract_cached(); discovery_extracts_zips();
    std::puts("ok");
}
