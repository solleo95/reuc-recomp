// Linux/Deck only (tests/linux/CMakeLists.txt): ImageUnpack with a fake `wit` shell script.
#include "check.h"
#include "image_unpack.h"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
using namespace uc::launcher;
namespace fs = std::filesystem;
namespace {
// ID6 prints $FAKE_ID; extract creates DATA/sys/main.dol (unless $FAKE_FAIL), prints progress, and sleeps $FAKE_SLEEP seconds in the middle.
constexpr const char* kScript = R"SH(#!/bin/sh
case "$1" in
  ID6) echo "$FAKE_ID" ;;
  extract)
    echo "  17% copied in 0:04 (175.1 MiB/sec)"
    [ -n "$FAKE_SLEEP" ] && sleep "$FAKE_SLEEP"
    [ -n "$FAKE_FAIL" ] && exit 1
    mkdir -p "$3/DATA/sys" && echo dol > "$3/DATA/sys/main.dol"
    echo " 100% copied in 0:09" ;;
esac
)SH";
void write(const fs::path& path, std::string_view text, mode_t mode = 0644) {
    fs::create_directories(path.parent_path()); { std::ofstream(path, std::ios::binary) << text; } ::chmod(path.c_str(), mode);
}
UnpackStatus wait_for(const ImageUnpack& unpack) {
    for (int i = 0; i < 6000 && unpack.status() == UnpackStatus::running; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return unpack.status();
}
struct World {
    fs::path root, image, wit;
    World() {
        static unsigned serial = 0;
        root = fs::temp_directory_path() / fs::u8path("uc-unpack-\xd0\xb8\xd0\xb3\xd1\x80\xd0\xb0 " + std::to_string(::getpid()) + "-" + std::to_string(++serial));   // Cyrillic and a space
        fs::remove_all(root);
        wit = root / "tools" / "wit" / "wit"; write(wit, kScript, 0755);
        image = root / fs::u8path("\xd0\xbe\xd0\xb1\xd1\x80\xd0\xb0\xd0\xb7 \xd0\xb4\xd0\xb8\xd1\x81\xd0\xba\xd0\xb0.iso"); write(image, "disc");
        write(root / "game" / "OLD.txt", "old");
        ::setenv("FAKE_ID", "RBUE08", 1); ::unsetenv("FAKE_FAIL"); ::unsetenv("FAKE_SLEEP");
    }
    ~World() { std::error_code ec; fs::remove_all(root, ec); }
};
}
int main() {
    {   // success replaces the game folder and reports 100 %
        World w; ImageUnpack unpack(w.root, w.image);
        CHECK(wait_for(unpack) == UnpackStatus::done && unpack.error() == UnpackError::none && unpack.percent() == 100);
        CHECK(fs::exists(w.root / "game" / "DATA" / "sys" / "main.dol") && !fs::exists(w.root / "game" / "OLD.txt"));
    }
    {   // another disc: refused before anything is touched
        World w; ::setenv("FAKE_ID", "RE4E08", 1); ImageUnpack unpack(w.root, w.image);
        CHECK(wait_for(unpack) == UnpackStatus::failed && unpack.error() == UnpackError::wrong_image);
        CHECK(fs::exists(w.root / "game" / "OLD.txt"));
    }
    {   // an image inside the game folder would be deleted with it
        World w; const auto inside = w.root / "game" / "disc.iso"; write(inside, "disc"); ImageUnpack unpack(w.root, inside);
        CHECK(wait_for(unpack) == UnpackStatus::failed && unpack.error() == UnpackError::image_in_game_folder);
        CHECK(fs::exists(inside) && fs::exists(w.root / "game" / "OLD.txt"));
    }
    {   // not enough space: before any change
        World w; ImageUnpack unpack(w.root, w.image, {}, uint64_t(1) << 60);
        CHECK(wait_for(unpack) == UnpackStatus::failed && unpack.error() == UnpackError::no_space);
        CHECK(fs::exists(w.root / "game" / "OLD.txt"));
    }
    {   // wit missing
        World w; fs::remove(w.wit); ImageUnpack unpack(w.root, w.image);
        CHECK(wait_for(unpack) == UnpackStatus::failed && unpack.error() == UnpackError::wit_missing && fs::exists(w.root / "game" / "OLD.txt"));
    }
    {   // a Wit that cannot be started (no execute bit after a zip extracted without modes, a noexec mount) is a Wit problem, not "wrong image"
        World w; ::chmod(w.wit.c_str(), 0644); ImageUnpack unpack(w.root, w.image);
        CHECK(wait_for(unpack) == UnpackStatus::failed && unpack.error() == UnpackError::wit_missing && fs::exists(w.root / "game" / "OLD.txt"));
    }
    {   // a failed extraction leaves no half-written game folder
        World w; ::setenv("FAKE_FAIL", "1", 1); ImageUnpack unpack(w.root, w.image);
        CHECK(wait_for(unpack) == UnpackStatus::failed && unpack.error() == UnpackError::extract_failed && !fs::exists(w.root / "game"));
    }
    {   // cancel in the middle of the extraction stops wit's whole process group and leaves no game folder
        World w; ::setenv("FAKE_SLEEP", "30", 1); ImageUnpack unpack(w.root, w.image);
        for (int i = 0; i < 4000 && unpack.percent() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(unpack.percent() == 17);                                          // the progress line was parsed
        const auto began = std::chrono::steady_clock::now();
        unpack.cancel();
        CHECK(wait_for(unpack) == UnpackStatus::cancelled && !fs::exists(w.root / "game"));
        CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(5));   // not after wit's 30 s sleep
    }
    {   // the progress parser
        CHECK(parse_percent("  17% copied in 0:04 (175.1 MiB/sec)") == 17);
        CHECK(parse_percent("copying /path/50% copied/x.iso\n 99% copied") == 99);
        CHECK(!parse_percent("no progress here 100%"));
    }
    std::puts("image unpack posix: passed");
}
