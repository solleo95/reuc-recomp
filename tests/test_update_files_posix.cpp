// Linux/Deck only: tests/linux/CMakeLists.txt; run with scripts/linux/deck.py run.
#include "check.h"
#include "posix/files_posix.h"
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>
using namespace uc::update;
namespace {
fs::path scratch() {
    auto path = fs::temp_directory_path() / ("uc-files-posix-" + std::to_string(::getpid()));
    fs::remove_all(path);
    fs::create_directories(path);
    return path;
}
void write(const fs::path& path, std::string_view text) { std::ofstream(path, std::ios::binary) << text; }
}
int main() {
    const auto root = scratch();
    // open_read refuses a symlink in the last component and non-regular files
    write(root / "real.txt", "abc");
    fs::create_symlink(root / "real.txt", root / "link.txt");
    CHECK(!failed(open_read(root / "real.txt")));
    CHECK(failed(open_read(root / "link.txt")));
    CHECK(failed(open_read(root)));
    CHECK(failed(open_read(root / "missing.txt")));
    // read_text honours the limit; sha256_fd hashes from the start
    CHECK(std::get<std::string>(read_text(root / "real.txt", 3)) == "abc");
    CHECK(failed(read_text(root / "real.txt", 2)));
    {
        auto opened = open_read(root / "real.txt");
        CHECK(!failed(opened));
        auto& fd = std::get<Fd>(opened);
        CHECK(std::get<std::string>(sha256_fd(fd.get())) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        CHECK(std::get<std::string>(sha256_fd(fd.get())) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");   // twice: it rewinds
    }
    // remove_owned_tree deletes the tree and the symlinks in it, not what they point to
    const auto outside = root / "outside";
    fs::create_directories(outside);
    write(outside / "keep.txt", "keep");
    const auto tree = root / "tree";
    fs::create_directories(tree / "a" / "b");
    write(tree / "a" / "b" / "f.txt", "f");
    fs::create_directory_symlink(outside, tree / "a" / "to_outside");
    fs::create_symlink(root / "real.txt", tree / "file_link");
    CHECK(!failed(remove_owned_tree(tree)));
    CHECK(!fs::exists(tree));
    CHECK(fs::exists(outside / "keep.txt") && fs::exists(root / "real.txt"));
    CHECK(!failed(remove_owned_tree(tree)));   // already gone: not an error
    // a root that is itself a symlink: the link goes, the target stays
    fs::create_directory_symlink(outside, root / "root_link");
    CHECK(!failed(remove_owned_tree(root / "root_link")));
    CHECK(!fs::exists(fs::symlink_status(root / "root_link")) && fs::exists(outside / "keep.txt"));
    // replace_file replaces an existing target atomically and keeps the content
    write(root / "new.txt", "new");
    write(root / "target.txt", "old");
    CHECK(!failed(replace_file(root / "new.txt", root / "target.txt")));
    CHECK(std::get<std::string>(read_text(root / "target.txt", 10)) == "new" && !fs::exists(root / "new.txt"));
    CHECK(failed(replace_file(root / "absent.txt", root / "target.txt")));
    // set_file_mode: sets the mode where it can; where the file system keeps modes as a mount option (exFAT, FAT, NTFS) fchmod fails with EPERM and the mode the file
    // already shows decides: executable wanted and there: fine; executable wanted and absent: an error; no execute bit wanted: fine.
    {
        write(root / "mode.txt", "x");
        auto opened = open_read(root / "mode.txt"); CHECK(!failed(opened));
        CHECK(!failed(set_file_mode(std::get<Fd>(opened).get(), 0755)));
        CHECK((fs::status(root / "mode.txt").permissions() & fs::perms::owner_exec) != fs::perms::none);
        Fd dir(::open("/proc", O_RDONLY));                                     // root-owned procfs refuses chmod with EPERM; the directory shows an execute bit
        if (dir.valid() && ::geteuid() != 0) {
            CHECK(!failed(set_file_mode(dir.get(), 0755)));                     // execute wanted and shown: tolerated
            CHECK(!failed(set_file_mode(dir.get(), 0644)));                     // no execute bit wanted: tolerated
        }
        Fd status(::open("/proc/self/status", O_RDONLY));                       // 0444: no execute bit, and chmod is refused
        if (status.valid() && ::geteuid() != 0) {
            CHECK(failed(set_file_mode(status.get(), 0755)));                   // cannot become executable: an error
            CHECK(!failed(set_file_mode(status.get(), 0644)));
        }
    }
    // errno mapping
    CHECK(file_error("x", ENOSPC).code == ErrorCode::no_space);
    CHECK(file_error("x", EACCES).code == ErrorCode::permission);
    CHECK(file_error("x", EROFS).code == ErrorCode::permission);
    CHECK(file_error("x", ENOENT).code == ErrorCode::io);
    fs::remove_all(root);
    std::puts("update files posix: passed");
}
