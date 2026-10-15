#include "check.h"
#include "crash_bundle.h"
#include "dispatch.h"
#include "guest_mem.h"
#include "trace.h"
#include <filesystem>
#include <fstream>
#include <sstream>

static void stub(ppc::Ctx&) {}
static std::string slurp(const std::filesystem::path& path) { std::ifstream in(path, std::ios::binary); std::stringstream s; s << in.rdbuf(); return s.str(); }

int main() {
    uc::guest_mem_init();
    uint8_t* mem = uc::guest_mem_base();
    mem[0x1234] = 0xAB; mem[(24u << 20) - 1] = 0x77;                                  // MEM1 at 0x80000000
    mem[0x10000000 + 0x20] = 0xCD; mem[0x10000000 + (64u << 20) - 1] = 0x88;          // MEM2 at 0x90000000
    const uc::FnEntry table[] = {{0x80001000, stub}, {0x80001040, stub}};
    uc::dispatch_init(table, 2, 0x80001000, 0x80002000);
    const auto base = std::filesystem::temp_directory_path() / "uc-crash-bundle-writer";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    { std::ofstream(base / "symbols.txt") << "80001000 my_function\n"; }
    uc::symbols_load((base / "symbols.txt").string());

    const auto dir = base / "crash-1.dump";
    uc::write_crash_bundle(dir, "thread 1 osthread 00000000 state running priority 16 waitkey 00000000 current 1\n", "array 12 address 80400000 stride 48\n");
    CHECK(std::filesystem::file_size(dir / "mem1.bin") == (24u << 20) && std::filesystem::file_size(dir / "mem2.bin") == (64u << 20));
    const auto mem1 = slurp(dir / "mem1.bin"), mem2 = slurp(dir / "mem2.bin");
    CHECK(uint8_t(mem1[0x1234]) == 0xAB && uint8_t(mem1.back()) == 0x77 && uint8_t(mem2[0x20]) == 0xCD && uint8_t(mem2.back()) == 0x88);
    CHECK(slurp(dir / "threads.txt").rfind("thread 1 osthread", 0) == 0);
    CHECK(slurp(dir / "gx.txt") == "array 12 address 80400000 stride 48\n");
    const auto functions = slurp(dir / "functions.txt");   // one line per translated function entry: address and its symbol ("" when unknown)
    CHECK(functions.find("80001000 my_function\n") != std::string::npos && functions.find("80001040 ") != std::string::npos);
    CHECK(functions.find("80001040 ") < functions.size() && functions.find("80001000") < functions.find("80001040"));   // ascending
    return 0;
}
