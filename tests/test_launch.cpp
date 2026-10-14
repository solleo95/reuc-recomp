#include "check.h"
#include "launch.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
int main() {
    for(auto value:{L"",L"a b",L"тест",L"a\"b",L"a\\",L"x\\\"y",L"C:\\A B\\"}) {
        auto command=L"program.exe "+uc::quote_windows_arg(value);int count=0;
        auto arguments=CommandLineToArgvW(command.c_str(),&count);
        CHECK(arguments && count==2 && std::wstring(arguments[1])==value);LocalFree(arguments);
    }
    bool failed=false;try {uc::launch_game(L"Z:/missing/uc.exe",L"Z:/missing/config.json");}catch(const std::exception&){failed=true;}CHECK(failed);
    failed=false;try {uc::quote_windows_arg(std::wstring_view(L"a\0b",3));}catch(const std::invalid_argument&){failed=true;}CHECK(failed);
    return 0;
}
