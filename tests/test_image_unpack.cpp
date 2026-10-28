#include "check.h"
#include "image_unpack.h"
#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <iostream>
#include <string>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
using namespace uc::launcher;
namespace fs=std::filesystem;

// This executable doubles as a fake Wit: the image file holds the ID6 followed by flags (FAIL, SLOW, NODOL).
static int fake_wit(int argc,wchar_t** argv) {
    if(argc<3)return 2;
    std::ifstream in{fs::path(argv[2])};std::string text((std::istreambuf_iterator<char>(in)),{});
    wchar_t locale[32]{};GetEnvironmentVariableW(L"LC_ALL",locale,32);
    if(!std::wcscmp(argv[1],L"ID6")) {std::cout<<(!std::wcscmp(locale,L"en_US.UTF-8") ? text.substr(0,6) : std::string("LOCALE"))<<"\n";return 0;}
    if(std::wcscmp(argv[1],L"extract") || argc<5 || std::wcscmp(argv[4],L"--progress"))return 2;
    // Like Wit with one partition on the image (a scrubbed WBFS): the files land in the folder itself unless --pmode name asks for the DATA prefix.
    const bool prefixed=argc>=7 && !std::wcscmp(argv[5],L"--pmode") && !std::wcscmp(argv[6],L"name");
    const fs::path destination=prefixed ? fs::path(argv[3])/"DATA" : fs::path(argv[3]);
    fs::create_directories(destination/"sys");
    std::cout<<"wit: EXTRACT 1/1\n  10% copied in 0:01 (1.0 MiB/sec)  \r"<<std::flush;
    if(text.find("SLOW")!=std::string::npos)for(;;)Sleep(100);
    if(text.find("FAIL")!=std::string::npos)return 1;
    std::cout<<"  60% copied in 0:02 (1.0 MiB/sec)  \r"<<std::flush;
    if(text.find("NODOL")==std::string::npos)std::ofstream(destination/"sys"/"main.dol")<<"dol";
    return 0;
}

static fs::path self() {
    std::wstring value(32768,L'\0');value.resize(GetModuleFileNameW(nullptr,value.data(),DWORD(value.size())));
    return value;
}
static UnpackStatus wait_for(const ImageUnpack& unpack) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    while(unpack.status()==UnpackStatus::running && std::chrono::steady_clock::now()<deadline)Sleep(10);
    return unpack.status();
}
static fs::path make_root(const char* name) {
    // A non-ASCII name: Wit has to be given the paths as UTF-8.
    auto root=fs::temp_directory_path()/(std::wstring(L"uc-unpack-тест-")+std::to_wstring(GetCurrentProcessId()))/name;
    fs::remove_all(root);fs::create_directories(root/"game");std::ofstream(root/"game"/"old.txt")<<"old";
    return root;
}
static fs::path make_image(const fs::path& root,const std::string& text) {
    const auto image=root/"образ.iso";std::ofstream(image)<<text;return image;
}

int wmain(int argc,wchar_t** argv) {
    if(argc>1)return fake_wit(argc,argv);
    CHECK(parse_percent("  4% copied in 0:01 (190.3 MiB/sec)  \r  9% copied in 0:02 (192.6 MiB/sec)")==9);
    CHECK(parse_percent("100% copied")==100);
    CHECK(parse_percent("17")==std::nullopt);
    CHECK(parse_percent("")==std::nullopt);
    CHECK(parse_percent("wit: EXTRACT 1/1 ISO:x -> y\n")==std::nullopt);
    CHECK(parse_percent("wit: EXTRACT 1/1 ISO:/cygdrive/c/100%/uc.iso -> /cygdrive/c/game/\n")==std::nullopt);   // a % in an echoed path is not progress
    CHECK(parse_percent("EXTRACT 1/1 ISO:/c/100%/uc.iso\n  12% copied in 0:01 (1.0 MiB/sec)")==12);
    {   // unpacks, replaces the old game folder
        const auto root=make_root("ok"),image=make_image(root,"RBUE08");
        ImageUnpack unpack(root,image,self(),0);
        CHECK(wait_for(unpack)==UnpackStatus::done);
        CHECK(unpack.error()==UnpackError::none && unpack.percent()==100);
        CHECK(fs::is_regular_file(root/"game"/"DATA"/"sys"/"main.dol") && !fs::exists(root/"game"/"old.txt"));
        CHECK(fs::is_regular_file(image));
    }
    {   // another game or region: nothing is deleted
        const auto root=make_root("region"),image=make_image(root,"RSPE01");
        ImageUnpack unpack(root,image,self(),0);
        CHECK(wait_for(unpack)==UnpackStatus::failed && unpack.error()==UnpackError::wrong_image);
        CHECK(fs::is_regular_file(root/"game"/"old.txt"));
    }
    {   // Wit fails halfway: the half-unpacked folder is removed
        const auto root=make_root("fail"),image=make_image(root,"RBUE08 FAIL");
        ImageUnpack unpack(root,image,self(),0);
        CHECK(wait_for(unpack)==UnpackStatus::failed && unpack.error()==UnpackError::extract_failed);
        CHECK(!fs::exists(root/"game"));
    }
    {   // Wit succeeds but there is no main.dol
        const auto root=make_root("nodol"),image=make_image(root,"RBUE08 NODOL");
        ImageUnpack unpack(root,image,self(),0);
        CHECK(wait_for(unpack)==UnpackStatus::failed && unpack.error()==UnpackError::extract_failed);
        CHECK(!fs::exists(root/"game"));
    }
    {   // the image lies inside the folder that would be deleted
        const auto root=make_root("inside"),image=root/"game"/"x.iso";std::ofstream(image)<<"RBUE08";
        ImageUnpack unpack(root,image,self(),0);
        CHECK(wait_for(unpack)==UnpackStatus::failed && unpack.error()==UnpackError::image_in_game_folder);
        CHECK(fs::is_regular_file(image) && fs::is_regular_file(root/"game"/"old.txt"));
    }
    {   // no Wit next to the launcher
        const auto root=make_root("nowit"),image=make_image(root,"RBUE08");
        ImageUnpack unpack(root,image);
        CHECK(wait_for(unpack)==UnpackStatus::failed && unpack.error()==UnpackError::wit_missing);
        CHECK(fs::is_regular_file(root/"game"/"old.txt"));
    }
    {   // cancel while Wit runs: the process is killed and the half-unpacked folder removed
        const auto root=make_root("cancel"),image=make_image(root,"RBUE08 SLOW");
        ImageUnpack unpack(root,image,self(),0);
        for(int i=0;i<3000 && unpack.percent()<10;++i)Sleep(10);
        CHECK(unpack.percent()==10);
        CHECK(fs::is_directory(root/"game"/"DATA"/"sys") && !fs::exists(root/"game"/"old.txt"));   // Wit writes straight into game (no rename: a scanner holding fresh files blocks it)
        unpack.cancel();
        CHECK(wait_for(unpack)==UnpackStatus::cancelled);
        CHECK(!fs::exists(root/"game"));
    }
    {   // closing the launcher (the destructor) during the unpack cleans up like Cancel
        const auto root=make_root("destroy"),image=make_image(root,"RBUE08 SLOW");
        {
            ImageUnpack unpack(root,image,self(),0);
            for(int i=0;i<3000 && unpack.percent()<10;++i)Sleep(10);
            CHECK(unpack.percent()==10);
        }
        CHECK(!fs::exists(root/"game"));
    }
    {   // not enough room: nothing is deleted
        const auto root=make_root("space"),image=make_image(root,"RBUE08");
        ImageUnpack unpack(root,image,self(),~0ull);
        CHECK(wait_for(unpack)==UnpackStatus::failed && unpack.error()==UnpackError::no_space);
        CHECK(fs::is_regular_file(root/"game"/"old.txt"));
    }
    {   // the image is reached through a junction that points at a subfolder of the game folder
        const auto root=make_root("junction");
        fs::create_directories(root/"game"/"backup");std::ofstream(root/"game"/"backup"/"x.iso")<<"RBUE08";
        const auto link=root/"isos";
        const auto command=L"cmd /c mklink /J \""+link.wstring()+L"\" \""+(root/"game"/"backup").wstring()+L"\" >nul";
        CHECK(_wsystem(command.c_str())==0);
        ImageUnpack unpack(root,link/"x.iso",self(),0);
        CHECK(wait_for(unpack)==UnpackStatus::failed && unpack.error()==UnpackError::image_in_game_folder);
        CHECK(fs::is_regular_file(root/"game"/"backup"/"x.iso") && fs::is_regular_file(root/"game"/"old.txt"));
    }
    fs::remove_all(fs::temp_directory_path()/(std::wstring(L"uc-unpack-тест-")+std::to_wstring(GetCurrentProcessId())));
    return 0;
}
