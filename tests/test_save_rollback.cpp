#include "check.h"
#include "savefs.h"
#include <array>
#include <chrono>
#include <fstream>

int main() {
    const auto root=std::filesystem::temp_directory_path()/ ("uc-save-rollback-"+std::to_string(GetCurrentProcessId())+"-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        uc::SaveFs files(root);
        CHECK(files.initialize()==0 && files.create_dir("/data")==0 && files.create_file("/data/save")==0);
        int fd=files.open("/data/save",3);
        const std::array<uint8_t,4> original{1,2,3,4},changed{5,6,7,8};
        CHECK(files.write(fd,original)==4 && files.close(fd)==0);
        files.begin_rollback();
        fd=files.open("/data/save",3); CHECK(fd>=100);
        CHECK(files.seek(fd,1,0)==1);
        const auto before=files.checkpoint();
        CHECK(files.write(fd,changed)==4 && files.close(fd)==0);
        CHECK(files.rename("/data/save","/data/future")==0);
        CHECK(files.create_dir("/discarded")==0);
        const auto future=files.checkpoint();
        files.restore(*before);
        CHECK(files.stats(fd).position==1 && files.stats(fd).length==4);
        std::array<uint8_t,4> read{};
        CHECK(files.read(fd,read)==3 && read[0]==2 && read[2]==4);
        bool directory=false;
        CHECK(files.attributes("/discarded",directory)==-106);
        CHECK(files.open("/data/future",1)==-106);
        CHECK(files.close(fd)==0);
        {
            uc::SaveFs disk(root); const int disk_fd=disk.open("/data/save",1);
            CHECK(disk.read(disk_fd,read)==4 && read==original && disk.close(disk_fd)==0);
        }
        CHECK(files.create_file("/../escape")==-101 && files.create_file("/.backups/x")==-102);
        CHECK(files.create_file("/data/CON")==-101);
        files.commit(*future);
        files.commit(*future); // Repeated confirmation has no extra effects.
        {
            uc::SaveFs disk(root);
            CHECK(disk.open("/data/save",1)==-106);
            const int disk_fd=disk.open("/data/future",1);
            std::array<uint8_t,5> bytes{};
            CHECK(disk.read(disk_fd,bytes)==5 && bytes[0]==1 && bytes[1]==5 && bytes[4]==8);
            CHECK(disk.close(disk_fd)==0);
            CHECK(disk.attributes("/discarded",directory)==0 && directory);
        }
    }
    std::filesystem::remove_all(root);
}
