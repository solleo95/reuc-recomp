#include "mod_zip.h"
#include "mod_paths.h"
#include <miniz.h>
#include <cstdio>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace uc::mods {
namespace {
struct Archive {
    mz_zip_archive zip{};
    FILE* file = nullptr;
    bool open = false;
    ~Archive() {
        if (open) mz_zip_reader_end(&zip);
        if (file) std::fclose(file);
    }
};

std::string hex8(uint64_t v) {
    char text[9];
    std::snprintf(text, sizeof text, "%08x", unsigned(v ^ (v >> 32)));
    return text;
}

size_t write_chunk(void* opaque, mz_uint64, const void* data, size_t size) {
    auto& out = *static_cast<std::ofstream*>(opaque);
    out.write(static_cast<const char*>(data), std::streamsize(size));
    return out ? size : 0;
}

bool fail(std::string& error, const std::string& text, const fs::path& cleanup = {}) {
    error = text;
    if (!cleanup.empty()) { std::error_code ignored; fs::remove_all(cleanup, ignored); }
    return false;
}
}  // namespace

bool extract_zip_mod(const fs::path& zip_path, const fs::path& cache_dir, fs::path& root_out, std::string& error, ZipLimits limits) {
    std::error_code ec;
    const auto size = fs::file_size(zip_path, ec);
    if (ec) return fail(error, "cannot read " + utf8(zip_path.filename()));
    const auto time = fs::last_write_time(zip_path, ec).time_since_epoch().count();
    fs::path dest = cache_dir / zip_path.stem();
    dest += "-" + hex8(uint64_t(size) * 1000003ull + uint64_t(time));
    const fs::path marker = dest / ".complete";

    const auto locate_root = [&](fs::path& root) {
        root = dest;
        if (fs::exists(dest / "mod.json")) return;
        std::vector<fs::path> children;
        for (const auto& entry : fs::directory_iterator(dest)) if (entry.path().filename() != ".complete") children.push_back(entry.path());
        if (children.size() == 1 && fs::is_directory(children[0]) && fs::exists(children[0] / "mod.json")) root = children[0];
    };
    if (fs::exists(marker, ec)) { locate_root(root_out); return true; }

    Archive archive;
    _wfopen_s(&archive.file, zip_path.c_str(), L"rb");
    if (!archive.file || !mz_zip_reader_init_cfile(&archive.zip, archive.file, 0, 0)) return fail(error, "not a readable zip file");
    archive.open = true;
    const mz_uint count = mz_zip_reader_get_num_files(&archive.zip);
    if (count > limits.max_entries) return fail(error, "zip has too many entries (" + std::to_string(count) + ")");
    uint64_t total = 0;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&archive.zip, i, &stat)) return fail(error, "damaged zip directory");
        total += stat.m_uncomp_size;
        if (total > limits.max_total_bytes) return fail(error, "zip expands to more than the allowed size");
    }

    fs::remove_all(dest, ec);
    fs::create_directories(dest, ec);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat;
        mz_zip_reader_file_stat(&archive.zip, i, &stat);
        std::string name = stat.m_filename;
        const bool directory = mz_zip_reader_is_file_a_directory(&archive.zip, i);
        if (directory && !name.empty() && name.back() == '/') name.pop_back();
        const auto target = confine(dest, name);
        if (!target) return fail(error, "unsafe path in zip: " + std::string(stat.m_filename), dest);
        if (directory) { fs::create_directories(*target, ec); continue; }
        fs::create_directories(target->parent_path(), ec);
        std::ofstream out(*target, std::ios::binary);
        if (!out || !mz_zip_reader_extract_to_callback(&archive.zip, i, write_chunk, &out, 0)) return fail(error, "cannot extract " + name, dest);
    }
    std::ofstream(marker) << "ok";
    locate_root(root_out);
    return true;
}

}  // namespace uc::mods
