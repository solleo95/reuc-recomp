#include "image_unpack.h"
#include <cctype>
#include <string>
namespace uc::launcher {
std::optional<int> parse_percent(std::string_view output) {
    constexpr std::string_view marker="% copied";   // not a bare '%': Wit echoes the image path too
    for(size_t sign=output.rfind(marker);sign!=std::string_view::npos;sign=sign ? output.rfind(marker,sign-1) : std::string_view::npos) {
        size_t begin=sign;while(begin && std::isdigit(static_cast<unsigned char>(output[begin-1])))--begin;
        if(begin<sign && sign-begin<=3)return std::stoi(std::string(output.substr(begin,sign-begin)));
    }
    return std::nullopt;
}
}
