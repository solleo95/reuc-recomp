#include "crash_bundle.h"
#include "dispatch.h"
#include "guest_mem.h"
#include "trace.h"
#include <cstdio>
#include <fstream>
#include <stdexcept>
namespace uc {
namespace {
void write_file(const std::filesystem::path& path, const void* data, size_t size) {
    std::ofstream out(path, std::ios::binary);
    out.write(static_cast<const char*>(data), std::streamsize(size));
    out.flush();
    if (!out) throw std::runtime_error("cannot write " + path.string());
}
void write_text(const std::filesystem::path& path, const std::string& text) { write_file(path, text.data(), text.size()); }
}
void write_crash_bundle(const std::filesystem::path& dir, const std::string& threads, const std::string& gx) {
    std::filesystem::create_directories(dir);
    const uint8_t* mem = guest_mem_base();
    write_file(dir / "mem1.bin", mem, 24u << 20);
    write_file(dir / "mem2.bin", mem + 0x10000000, 64u << 20);
    write_text(dir / "threads.txt", threads);
    write_text(dir / "gx.txt", gx);
    std::string functions;
    char line[16];
    for (const uint32_t address : function_addresses()) {
        std::snprintf(line, sizeof line, "%08X ", address);
        functions += line + symbol_name(address) + "\n";
    }
    write_text(dir / "functions.txt", functions);
}
}
