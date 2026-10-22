#include "check.h"
#include "savefs.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <chrono>

int main() {
    const auto suffix=std::to_string(GetCurrentProcessId())+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto root=std::filesystem::temp_directory_path()/ ("uc-native-save-test-"+suffix);
    uc::SaveFs fs(root);
    CHECK(fs.initialize()==0);
    CHECK(fs.create_dir("/title")==0);
    CHECK(fs.create_dir("/title/data")==0);
    CHECK(fs.open("/title/data/missing",1)==-106);
    for (auto path : {"/../outside","/title/../../outside","/title/a:stream","/title/CON","/title/a\\b","//server/share"})
        CHECK(fs.create_file(path)==-101);
    CHECK(fs.create_file("/title/data/save.bin")==0);
    CHECK(fs.create_file("/title/data/save.bin")==-105);
    int fd=fs.open("/title/data/save.bin",3); CHECK(fd>=0);
    CHECK(!MoveFileExW((root/"title").c_str(),(root/"moved").c_str(),0));
    CHECK(GetLastError()==ERROR_SHARING_VIOLATION || GetLastError()==ERROR_ACCESS_DENIED);
    HANDLE writer=CreateFileW((root/"title/data").c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    CHECK(writer==INVALID_HANDLE_VALUE);
    CHECK(GetLastError()==ERROR_SHARING_VIOLATION || GetLastError()==ERROR_ACCESS_DENIED);
    const std::array<uint8_t,4> original{1,2,3,4};
    CHECK(fs.write(fd,original)==4);
    auto stats=fs.stats(fd); CHECK(stats.result==0 && stats.length==4 && stats.position==4);
    CHECK(fs.seek(fd,0,0)==0);
    CHECK(fs.seek(fd,INT_MAX,0)==INT_MAX);
    CHECK(fs.seek(fd,1,1)==-101 && fs.stats(fd).position==INT_MAX);
    CHECK(fs.seek(fd,0,0)==0);
    std::array<uint8_t,8> output{};
    CHECK(fs.read(fd,output)==4 && std::equal(original.begin(),original.end(),output.begin()));
    CHECK(fs.close(fd)==0 && fs.close(fd)==-101);
    {
        uc::SaveFs reopened(root); CHECK(reopened.initialize()==0);
        fd=reopened.open("/title/data/save.bin",1); CHECK(fd>=0);
        CHECK(reopened.write(fd,original)==-102);
        CHECK(reopened.seek(fd,-1,0)==-101);
        CHECK(reopened.read(fd,output)==4 && output[3]==4);
        CHECK(reopened.close(fd)==0);
    }
    auto listing=fs.read_dir("/title/data"); CHECK(listing.result==0 && listing.names==std::vector<std::string>{"save.bin"});
    auto usage=fs.usage("/title/data"); CHECK(usage.result==0 && usage.blocks==1 && usage.entries==1);
    const auto renamed=fs.rename("/title/data/save.bin","/title/data/renamed.bin");
    if (renamed) std::fprintf(stderr,"native rename result %d\n",renamed);
    CHECK(renamed==0);
    CHECK(fs.open("/title/data/save.bin",1)==-106);
    fd=fs.open("/title/data/renamed.bin",3); CHECK(fd>=0);
    const std::array<uint8_t,1> change{9}; CHECK(fs.write(fd,change)==1); CHECK(fs.close(fd)==0);
    CHECK(!std::filesystem::is_empty(root/".backups"));
    std::array<uint8_t,4> preserved{};
    std::ifstream backup(std::filesystem::directory_iterator(root/".backups")->path(),std::ios::binary);
    backup.read(reinterpret_cast<char*>(preserved.data()),preserved.size());
    CHECK(backup.gcount()==4 && preserved==original);
    CHECK(fs.remove("/title/data/renamed.bin")==0);
    CHECK(fs.open("/title/data/renamed.bin",1)==-106 && !std::filesystem::is_empty(root/".deleted"));
    std::ifstream archived(std::filesystem::directory_iterator(root/".deleted")->path(),std::ios::binary);
    archived.read(reinterpret_cast<char*>(preserved.data()),preserved.size());
    CHECK(archived.gcount()==4 && preserved[0]==9 && preserved[3]==4);
    CHECK(fs.create_file("/title/data/new.bin")==0);
    CHECK(fs.create_file("/title/data/source.bin")==0);
    CHECK(fs.create_file("/title/data/replace.bin")==0);
    fd=fs.open("/title/data/replace.bin",3); CHECK(fd>=0);
    const std::array<uint8_t,1> replaced{7}; CHECK(fs.write(fd,replaced)==1); CHECK(fs.close(fd)==0);
    CHECK(fs.rename("/title/data/source.bin","/title/data/replace.bin")==0);
    fd=fs.open("/title/data/replace.bin",1); CHECK(fd>=0 && fs.stats(fd).length==0); CHECK(fs.close(fd)==0);
    bool archived_replacement=false;
    for (const auto& entry:std::filesystem::directory_iterator(root/".backups")) {
        std::ifstream previous(entry.path(),std::ios::binary);
        uint8_t value=0; previous.read(reinterpret_cast<char*>(&value),1);
        archived_replacement|=previous.gcount()==1 && value==7;
    }
    CHECK(archived_replacement);
    CHECK(fs.rename("/title/data/replace.bin","/title/data/a")==0);
    CHECK(fs.create_dir("/title/empty")==0 && fs.remove("/title/empty")==0);
    CHECK(CreateHardLinkW((root/"title/data/alias.bin").c_str(),(root/"title/data/new.bin").c_str(),nullptr));
    CHECK(fs.open("/title/data/alias.bin",3)==-102);
    CHECK(fs.create_file("/.BACKUPS/escape")==-102);
    CHECK(fs.remove("/title/data")==-115);
    const auto outside=root.parent_path()/("uc-save-outside-"+suffix);
    std::filesystem::create_directories(outside);
    if (CreateSymbolicLinkW((root/"link").c_str(),outside.c_str(),SYMBOLIC_LINK_FLAG_DIRECTORY|SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
        CHECK(fs.create_file("/link/escape.bin")==-102 && !std::filesystem::exists(outside/"escape.bin"));
    else CHECK(GetLastError()==ERROR_PRIVILEGE_NOT_HELD); // platform cannot create this fixture
}
