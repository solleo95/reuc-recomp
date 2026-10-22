#include "dvdfs.h"
#include "fatal.h"
#include "guest_call.h"
#include "hle/os.h"
#include "hle/os_layout.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

using namespace uc;

namespace {
DvdFs* g_fs;
DvdStats g_stats;
bool g_log = false;
std::mutex g_m;
std::condition_variable g_cv;
std::deque<std::function<void()>> g_jobs;
std::thread g_thread;
bool g_stopping = true;
std::set<uint32_t> g_cancelled;   // command blocks whose in-flight read was cancelled (baton holder only)
std::map<uint32_t, DvdFs::Snapshot> g_open;   // file info address -> the version of the file that was opened there (guarded by g_m)

// The version of the file a command block opened, or, for a block this layer never opened (a guest-built file info), what the entry holds now.
std::optional<DvdFs::Snapshot> open_version(uint32_t file_info, int entry) {
    {
        std::lock_guard lock(g_m);
        const auto it = g_open.find(file_info);
        if (it != g_open.end() && it->second.entry == entry) return it->second;
    }
    return g_fs->snapshot(entry);
}

void enqueue(std::function<void()> job) {   // one worker keeps requests in order, like the drive
    std::lock_guard<std::mutex> lk(g_m);
    if (g_stopping) throw ThreadExit{};
    g_jobs.push_back(std::move(job));
    g_cv.notify_one();
}

std::string cstr(ppc::Ctx& c, uint32_t a) {
    std::string s;
    while (s.size() < 1024) { char ch = char(ppc::rd8(c, a++)); if (!ch) break; s.push_back(ch); }
    return s;
}

bool guest_range_ok(uint32_t a, uint32_t len) {   // MEM1 or MEM2 only
    uint64_t end = uint64_t(a) + len;
    return (a >= 0x80000000u && end <= 0x81800000ull) || (a >= 0x90000000u && end <= 0x94000000ull);
}

// Reads len bytes at offset into dst; the tail past the end of the file (at most 31 bytes) is zero.
// The bounds are the SDK's own (DVDReadAsyncPrio): offset <= length and offset + len < length + 32.
int64_t read_padded(const std::optional<DvdFs::Snapshot>& file, std::vector<uint8_t>& dst, uint32_t len, uint32_t offset) {
    if (!file) return -1;   // a bad entry or a directory
    const uint32_t length = file->size;
    if (offset > length || uint64_t(offset) + len >= uint64_t(length) + 32) return -1;
    dst.assign(len, 0);
    int64_t n = g_fs->read(*file, dst.data(), len, offset);
    return n < 0 ? -1 : int64_t(len);
}
}  // namespace

namespace uc {
void dvd_hle_shutdown() {
    {
        std::lock_guard lock(g_m);
        g_stopping = true;
        g_jobs.clear();
    }
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();
    g_fs = nullptr;
}
void dvd_hle_init(DvdFs& fs) {
    dvd_hle_shutdown();
    g_fs = &fs; g_cancelled.clear();
    { std::lock_guard lock(g_m); g_open.clear(); }
    { std::lock_guard lock(g_m); g_stopping = false; g_stats = {}; }
    g_thread = std::thread([] {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(g_m);
                g_cv.wait(lock, [] { return g_stopping || !g_jobs.empty(); });
                if (g_stopping) return;
                job = std::move(g_jobs.front()); g_jobs.pop_front();
            }
            try { job(); }
            catch (const std::exception& error) { fatal("DVD worker failed: %s", error.what()); }
        }
    });
}
DvdStats dvd_stats() { std::lock_guard lock(g_m); return g_stats; }
void dvd_log(bool on) { g_log = on; }
}  // namespace uc

void hle_DVDInit(ppc::Ctx&) {}
void hle_DVDConvertPathToEntrynum(ppc::Ctx& c) { c.r[3] = uint32_t(g_fs->entry_of(cstr(c, c.r[3]))); }
static void open_entry(ppc::Ctx& c, int e, uint32_t fi) {
    if (e < 0 || e >= int(g_fs->entry_count()) || g_fs->is_dir(e)) { c.r[3] = 0; return; }
    const auto version = g_fs->snapshot(e);                 // the file as it is now: a later overlay swap does not change what this handle reads
    if (!version) { c.r[3] = 0; return; }
    { std::lock_guard lock(g_m); g_open[fi] = *version; }
    ppc::wr32(c, fi + layout::kFiStart, uint32_t(e));       // the disc offset is replaced by the entry number
    ppc::wr32(c, fi + layout::kFiLength, version->size);
    ppc::wr32(c, fi + layout::kFiCallback, 0);
    ppc::wr32(c, fi + layout::kCbState, 0);                 // DVD_STATE_END
    { std::lock_guard lock(g_m); ++g_stats.opens; }
    if (g_log) std::fprintf(stderr, "[dvd] open %s\n", g_fs->path_of(e).c_str());
    c.r[3] = 1;
}
void hle_DVDFastOpen(ppc::Ctx& c) { open_entry(c, int(c.r[3]), c.r[4]); }
void hle_DVDOpen(ppc::Ctx& c) { open_entry(c, g_fs->entry_of(cstr(c, c.r[3])), c.r[4]); }
void hle_DVDClose(ppc::Ctx& c) {
    { std::lock_guard lock(g_m); g_open.erase(c.r[3]); }
    c.r[3] = 1;
}

// DVDReadPrio(fileInfo, buf, len, offset, prio) -> bytes read or -1
void hle_DVDReadPrio(ppc::Ctx& c) {
    uint32_t fi = c.r[3], buf = c.r[4], len = c.r[5], off = c.r[6];
    int e = int(ppc::rd32(c, fi + layout::kFiStart));
    if (!guest_range_ok(buf, len)) fatal("DVDReadPrio into an invalid guest buffer %08X+%u", buf, len);
    std::vector<uint8_t> data;
    int64_t n = read_padded(open_version(fi, e), data, len, off);
    if (n < 0) { c.r[3] = uint32_t(-1); return; }
    std::memcpy(ppc::hp(c, buf), data.data(), len);
    { std::lock_guard lock(g_m); ++g_stats.reads; g_stats.bytes += len; }
    c.r[3] = len;
}

// DVDReadAsyncPrio(fileInfo, buf, len, offset, callback, prio) -> 1; callback(result, fileInfo) runs in interrupt context
void hle_DVDReadAsyncPrio(ppc::Ctx& c) {
    uint32_t fi = c.r[3], buf = c.r[4], len = c.r[5], off = c.r[6], cb = c.r[7];
    int e = int(ppc::rd32(c, fi + layout::kFiStart));
    if (!guest_range_ok(buf, len)) fatal("DVDReadAsyncPrio into an invalid guest buffer %08X+%u", buf, len);
    ppc::wr32(c, fi + layout::kCbState, 1);                 // DVD_STATE_BUSY
    const auto version = open_version(fi, e);
    Sched& s = os_sched();
    uint8_t* mem = c.mem;
    s.async_begin();   // deterministic mode: the idle scheduler waits for this read's completion event
    enqueue([=, &s] {
        auto data = std::make_shared<std::vector<uint8_t>>();
        int64_t n = read_padded(version, *data, len, off);
        s.post_external([=, &s] {                           // runs on the baton holder, with interrupts enabled
            ppc::Ctx& ic = s.irq_ctx();
            if (g_cancelled.erase(fi)) return;               // DVDCancel already answered for this command block
            if (n >= 0) { std::memcpy(mem + (buf & 0x3FFFFFFFu), data->data(), len); gx::note_write_range(buf, len); std::lock_guard lock(g_m); ++g_stats.reads; g_stats.bytes += len; }
            ppc::wr32(ic, fi + layout::kCbState, n < 0 ? uint32_t(-1) : 0u);   // DVD_STATE_END or FATAL_ERROR
            if (cb) call_guest(ic, cb, {uint32_t(n < 0 ? -1 : int64_t(len)), fi});
        });
    });
    c.r[3] = 1;
}

// DVDCancel(block) -> 0 / DVDCancelAsync(block, callback) -> 1. A read in flight (state 1) is dropped and the block becomes
// DVD_STATE_CANCELED (10); the cancel callback runs in interrupt context.
static void cancel_block(ppc::Ctx& c, uint32_t block) {
    if (ppc::rd32(c, block + layout::kCbState) == 1) { g_cancelled.insert(block); ppc::wr32(c, block + layout::kCbState, 10); }
}
void hle_DVDCancel(ppc::Ctx& c) { cancel_block(c, c.r[3]); c.r[3] = 0; }
void hle_DVDCancelAsync(ppc::Ctx& c) {
    uint32_t block = c.r[3], cb = c.r[4];
    cancel_block(c, block);
    Sched& s = os_sched();
    if (cb) s.post_external([=, &s] { call_guest(s.irq_ctx(), cb, {0u, block}); });
    c.r[3] = 1;
}
