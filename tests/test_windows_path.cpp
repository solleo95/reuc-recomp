#include "check.h"
#include "windows_path.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <windows.h>
namespace fs=std::filesystem;

int main() {
    // volume_root_length: the part that is a drive, a volume or a share, not a folder to hold.
    CHECK(uc::volume_root_length(L"D:\\")==3);
    CHECK(uc::volume_root_length(L"D:\\a\\b")==3);
    CHECK(uc::volume_root_length(L"\\\\?\\D:\\a")==7);
    CHECK(uc::volume_root_length(L"\\\\?\\Volume{1234}\\a")==17);
    CHECK(uc::volume_root_length(L"\\\\server\\share\\a\\b")==15);
    CHECK(uc::volume_root_length(L"\\\\?\\UNC\\server\\share\\a")==21);
    CHECK(uc::volume_root_length(L"\\\\server\\share")==14);

    const auto holder=fs::temp_directory_path()/("uc-windows-path-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    struct Cleanup { fs::path folder; ~Cleanup() { std::error_code ec; fs::remove(folder/"link",ec); fs::remove_all(folder,ec); } } cleanup{holder};
    fs::create_directories(holder/"real"/"inner");
    const auto real=uc::resolve_folder(holder/"real");   // the folder as Windows spells it (a short name or a different case may differ)
    CHECK(!real.empty() && real.find(L"\\\\?\\")==std::wstring::npos);
    CHECK(fs::equivalent(real,holder/"real"));
    const auto command="mklink /J \""+(holder/"link").string()+"\" \""+(holder/"real").string()+"\" >nul";
    CHECK(std::system(("cmd /c \""+command+"\"").c_str())==0);
    CHECK(uc::resolve_folder(holder/"link")==real);                                  // a junction leads to the real folder
    CHECK(uc::resolve_folder(holder/"link"/"inner")==real+L"\\inner");
    CHECK(uc::resolve_folder(holder/"link"/"missing"/"deeper")==real+L"\\missing\\deeper");   // names that do not exist yet follow the resolved parent
    CHECK(uc::resolve_folder(holder/"link"/"inner"/L"")==real+L"\\inner");            // a trailing separator adds nothing

    const auto text=real;                                                             // the machine's own administrative share, when reachable
    const fs::path share=L"\\\\localhost\\"+std::wstring(1,text[0])+L"$"+text.substr(2);
    std::error_code unreachable;
    if(fs::exists(share,unreachable)) {
        const auto resolved=uc::resolve_folder(share);
        CHECK(resolved.starts_with(L"\\\\") && resolved.find(L"\\\\?\\")==std::wstring::npos);
        CHECK(fs::equivalent(resolved,real));
    }
}
