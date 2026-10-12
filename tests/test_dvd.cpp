#include "check.h"
#include "clock.h"
#include "dispatch.h"
#include "dvdfs.h"
#include "hle/os.h"
#include "hle/os_layout.h"
#include "ppc_fixture.h"
#include "sched.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <atomic>
#include <future>
#include <thread>
namespace fs = std::filesystem;
using namespace ppc;

void hle_DVDConvertPathToEntrynum(Ctx&); void hle_DVDFastOpen(Ctx&); void hle_DVDOpen(Ctx&);
void hle_DVDReadPrio(Ctx&); void hle_DVDReadAsyncPrio(Ctx&); void hle_DVDCancel(Ctx&); void hle_DVDCancelAsync(Ctx&);

static void write_file(const fs::path& p, size_t n) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    for (size_t i = 0; i < n; ++i) f.put(char(i * 7 + 3));
}
static uint8_t pat(size_t i) { return uint8_t(i * 7 + 3); }

static std::vector<std::string> g_log;
static void guest_cb(Ctx& c) { g_log.push_back("cb(" + std::to_string(int(c.r[3])) + ")"); uc::os_sched().wake_all(0x5000); }
static void put_cstr(Ctx& c, uint32_t a, const std::string& s) { for (size_t i = 0; i <= s.size(); ++i) wr8(c, a + uint32_t(i), uint32_t(i < s.size() ? s[i] : 0)); }

int main() {
    fs::path root = fs::temp_directory_path() / ("uc_dvd_test_" + std::to_string(uc::now_ticks()));
    CHECK(fs::create_directory(root));
    write_file(root / "Data" / "a.bin", 100);
    write_file(root / "Data" / "sub" / "b.bin", 64);
    write_file(root / "zero.bin", 0);
    write_file(root / "Data" / fs::path(u8"日本.bin"), 10);   // a name outside the ANSI code page must not break the tree walk
    { std::ofstream pending(root / "pending.bin", std::ios::binary); pending.seekp((4u << 20) - 1); pending.put(0); CHECK(pending.good()); }

    uc::DvdFs d;
    CHECK(d.open_root(root));
    CHECK(!uc::DvdFs().open_root(root / "missing"));
    CHECK(d.entry_of("") == 0 && d.is_dir(0));
    int a = d.entry_of("Data/a.bin");
    CHECK(a > 0 && !d.is_dir(a) && d.length(a) == 100);
    CHECK(d.entry_of("/data/A.BIN") == a);              // leading slash, any case
    CHECK(d.entry_of("Data\\a.bin") == a);              // either separator
    CHECK(d.entry_of("Data/sub/b.bin") > 0 && d.is_dir(d.entry_of("Data/sub")));
    CHECK(d.entry_of("Data/missing.bin") == -1);
    CHECK(d.entry_of("../x") == -1 && d.entry_of("Data/../Data/a.bin") == -1);   // no way out of the root
    CHECK(d.entry_count() == 8);                         // includes the large pending-read lifecycle fixture
    CHECK(d.entry_of("Data/\xE6\x97\xA5\xE6\x9C\xAC.bin") > 0);   // looked up by its UTF-8 bytes
    // reads
    uint8_t buf[256];
    CHECK(d.read(a, buf, 100, 0) == 100 && buf[0] == pat(0) && buf[99] == pat(99));
    CHECK(d.read(a, buf, 10, 50) == 10 && buf[0] == pat(50));
    CHECK(d.read(a, buf, 64, 80) == 20);                 // short read at the end of the file
    CHECK(d.read(a, buf, 8, 100) == 0 && d.read(a, buf, 8, 5000) == 0);
    CHECK(d.read(a, buf, 0, 10) == 0);
    CHECK(d.read(d.entry_of("zero.bin"), buf, 32, 0) == 0);
    CHECK(d.read(d.entry_of("Data"), buf, 4, 0) == -1);  // directory
    CHECK(d.read(999, buf, 4, 0) == -1);

    // HLE glue
    Fix f; Ctx& c = f.c;
    uc::Sched s(c.mem);
    static const uc::FnEntry table[] = {{0x80100000, guest_cb}};
    uc::dispatch_init(table, 1, 0x80100000, 0x80100004);
    uc::os_hle_init(s, c.mem, 0x817F0000);
    s.irq_ctx().r[1] = 0x817EFF00;
    uc::dvd_hle_init(d);
    uc::Thread* m = s.create(0, 16, [&](Ctx& t) {
        put_cstr(t, 0x80360000, "/Data/a.bin");
        t.r[3] = 0x80360000; hle_DVDConvertPathToEntrynum(t);
        CHECK(int(t.r[3]) == a);
        put_cstr(t, 0x80360100, "/nope");
        t.r[3] = 0x80360100; hle_DVDConvertPathToEntrynum(t);
        CHECK(int(t.r[3]) == -1);
        t.r[3] = 0x80360000; t.r[4] = 0x80361000; hle_DVDOpen(t);               // fileInfo at 0x80361000
        CHECK(t.r[3] == 1 && rd32(t, 0x80361000 + uc::layout::kFiLength) == 100);
        t.r[3] = 0x80360100; t.r[4] = 0x80361100; hle_DVDOpen(t);
        CHECK(t.r[3] == 0);
        // synchronous read (the SDK accepts offset + len < length + 32; the tail past the file is zero)
        t.r[3] = 0x80361000; t.r[4] = 0x80362000; t.r[5] = 64; t.r[6] = 64; t.r[7] = 2; hle_DVDReadPrio(t);   // 64 + 64 = 128 < 100 + 32
        CHECK(t.r[3] == 64 && rd8(t, 0x80362000) == pat(64) && rd8(t, 0x80362000 + 35) == pat(99) && rd8(t, 0x80362000 + 36) == 0);
        t.r[3] = 0x80361000; t.r[4] = 0x80362000; t.r[5] = 96; t.r[6] = 64; t.r[7] = 2; hle_DVDReadPrio(t);   // 160 >= 132: the SDK rejects it
        CHECK(int(t.r[3]) == -1);
        t.r[3] = 0x80361000; t.r[4] = 0x80362000; t.r[5] = 32; t.r[6] = 128; t.r[7] = 2; hle_DVDReadPrio(t);
        CHECK(int(t.r[3]) == -1);                                                 // starts past the end of the file (128 > 100)
        // asynchronous read: completes while this thread is blocked; the callback runs in interrupt context
        for (int i = 0; i < 64; ++i) wr8(t, 0x80363000 + uint32_t(i), 0xEE);
        t.r[3] = 0x80361000; t.r[4] = 0x80363000; t.r[5] = 32; t.r[6] = 0; t.r[7] = 0x80100000; t.r[8] = 2;
        hle_DVDReadAsyncPrio(t);
        CHECK(t.r[3] == 1);
        s.block_on(0x5000);                                                       // the callback wakes us
        CHECK(g_log.size() == 1 && g_log[0] == "cb(32)");
        CHECK(rd8(t, 0x80363000) == pat(0) && rd8(t, 0x80363000 + 31) == pat(31) && rd8(t, 0x80363000 + 32) == 0xEE);
        // an entry number past the end of the file table is not a file
        t.r[3] = 9999; t.r[4] = 0x80361200; hle_DVDFastOpen(t);
        CHECK(t.r[3] == 0);
        // a failed asynchronous read leaves the command block in the fatal-error state (-1)
        t.r[3] = 0x80361000; t.r[4] = 0x80363000; t.r[5] = 64; t.r[6] = 96; t.r[7] = 0x80100000; t.r[8] = 2;
        hle_DVDReadAsyncPrio(t);
        s.block_on(0x5000);
        CHECK(g_log.size() == 2 && g_log[1] == "cb(-1)" && int(rd32(t, 0x80361000 + 0x0C)) == -1);
        // cancel an asynchronous read that is still in flight: no data, no read callback, state "canceled" (10)
        for (int i = 0; i < 64; ++i) wr8(t, 0x80364000 + uint32_t(i), 0xEE);
        t.r[3] = 0x80361000; t.r[4] = 0x80364000; t.r[5] = 32; t.r[6] = 0; t.r[7] = 0x80100000; t.r[8] = 2;
        hle_DVDReadAsyncPrio(t);
        t.r[3] = 0x80361000; hle_DVDCancel(t);
        CHECK(t.r[3] == 0 && rd32(t, 0x80361000 + 0x0C) == 10);
        s.sleep_ticks(uc::ms_to_ticks(80));                                       // long enough for the worker to finish and post
        CHECK(g_log.size() == 2 && rd8(t, 0x80364000) == 0xEE);                   // the cancelled read left no trace
        // asynchronous cancel of an idle block reports through its callback
        t.r[3] = 0x80361000; t.r[4] = 0x80100000; hle_DVDCancelAsync(t);
        CHECK(t.r[3] == 1);
        s.block_on(0x5000);
        CHECK(g_log.size() == 3 && g_log[2] == "cb(0)");
    });
    s.run(m);
    uc::dvd_hle_shutdown();
    // Close while requests/completions remain pending; callbacks and guest writes are canceled.
    g_log.clear();
    uc::Sched pending(c.mem);
    uc::os_hle_init(pending, c.mem, 0x817F0000);
    uc::dvd_hle_init(d);
    std::promise<void> queued; auto ready = queued.get_future();
    std::atomic<bool> observe{true};
    std::thread observer([&] { while (observe.load()) { auto stats = uc::dvd_stats(); CHECK(stats.reads <= stats.opens); std::this_thread::yield(); } });
    auto* main_pending = pending.create(0, 16, [&](Ctx& t) {
        pending.set_interrupts(false);
        wr8(t, 0x80400000, 0xEE);
        for (uint32_t i = 0; i < 32; ++i) {
            uint32_t fi = 0x80371000 + i * 64;
            t.r[3] = uint32_t(d.entry_of("pending.bin")); t.r[4] = fi; hle_DVDFastOpen(t);
            t.r[3] = fi; t.r[4] = 0x80400000; t.r[5] = 4u << 20; t.r[6] = 0; t.r[7] = 0x80100000; t.r[8] = 2;
            hle_DVDReadAsyncPrio(t);
        }
        queued.set_value();
        for (;;) pending.safe_point();
    });
    std::thread coordinator([&] { pending.run(main_pending); });
    CHECK(ready.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    pending.shutdown(); coordinator.join();
    uc::dvd_hle_shutdown();
    observe = false; observer.join();
    CHECK(g_log.empty() && rd8(c, 0x80400000) == 0xEE);
    CHECK(uc::dvd_stats().opens == 32 && uc::dvd_stats().reads == 0);
    return 0;
}
