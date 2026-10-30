// A confirmed save replaces the file in one step: a reader that polls the file while commit() runs sees the whole old content or the whole new
// content, never a mix or a missing file, and the staging copies stay in .backups.
#ifndef __linux__
int main() { return 0; }   // Windows keeps files open with sharing modes that forbid a concurrent reader; the POSIX rename is what is tested
#else
#include "check.h"
#include "savefs.h"
#include "test_pid.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

int main() {
    namespace fs=std::filesystem;
    fs::path folder(u8"uc save atomic тест "); folder+=std::to_string(test_process_id());
    const auto root=fs::temp_directory_path()/folder;
    fs::remove_all(root);
    constexpr size_t kSize=1<<20;   // large enough that a non-atomic rewrite would be observable
    const std::vector<uint8_t> old_bytes(kSize,'A'),new_bytes(kSize,'B');
    {
        uc::SaveFs files(root);
        CHECK(files.initialize()==0 && files.create_dir("/d")==0 && files.create_file("/d/save")==0);
        int fd=files.open("/d/save",3); CHECK(fd>=0);
        CHECK(files.write(fd,old_bytes)==int(kSize) && files.close(fd)==0);
        files.begin_rollback();
        fd=files.open("/d/save",3); CHECK(fd>=0);
        CHECK(files.write(fd,new_bytes)==int(kSize) && files.close(fd)==0);
        const auto snapshot=files.checkpoint();

        std::atomic<bool> done{false};
        std::atomic<int> mixed{0},reads{0},seen_new{0};
        std::thread reader([&] {
            while (!done) {
                std::ifstream in(root/"d"/"save",std::ios::binary);
                std::vector<char> bytes((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
                ++reads;
                const bool all_a=bytes.size()==kSize && std::all_of(bytes.begin(),bytes.end(),[](char c) { return c=='A'; });
                const bool all_b=bytes.size()==kSize && std::all_of(bytes.begin(),bytes.end(),[](char c) { return c=='B'; });
                if (all_b) ++seen_new;
                if (!all_a && !all_b) ++mixed;
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        files.commit(*snapshot);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        done=true; reader.join();
        std::printf("reads %d, saw the new file %d times, mixed reads %d\n",reads.load(),seen_new.load(),mixed.load());
        CHECK(reads>0 && seen_new>0 && mixed==0);
    }
    // nothing but the save folder and the backup folder in the root: no staging leftovers
    for (const auto& entry:fs::directory_iterator(root)) {
        const auto name=entry.path().filename().string();
        CHECK(name=="d" || name==".backups");
    }
    fs::remove_all(root);
    return 0;
}
#endif
