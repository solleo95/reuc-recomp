#include "windows_args.h"
#include <stdexcept>
namespace uc {
std::wstring quote_windows_arg(std::wstring_view argument) {
    if(argument.find(L'\0')!=std::wstring_view::npos)throw std::invalid_argument("argument contains NUL");
    std::wstring result=L"\"";size_t slashes=0;
    for(wchar_t c:argument) {
        if(c==L'\\') {++slashes;continue;}
        result.append(c==L'\"' ? slashes*2+1 : slashes,L'\\');slashes=0;result+=c;
    }
    result.append(slashes*2,L'\\');result+=L'\"';return result;
}
}
