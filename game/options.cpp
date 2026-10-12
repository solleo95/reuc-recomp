#include "options.h"
#include <charconv>
namespace uc {
std::variant<Options, std::string> parse_options(std::span<const std::string_view> arguments) {
    Options options;
    for (size_t i = 0; i < arguments.size(); ++i) {
        auto arg = arguments[i];
        if (arg == "--headless") options.headless = true;
        else if (arg == "--validation") options.validation = true;
        else if (arg == "--log-dvd") options.log_dvd = true;
        else if (arg == "--log-sleep") options.log_sleep = true;
        else if (arg == "--gx-audit") options.gx_audit = true;
        else if (arg == "--game" || arg == "--width" || arg == "--height" || arg == "--seconds") {
            if (++i >= arguments.size() || arguments[i].empty()) return "missing value for " + std::string(arg);
            auto value = arguments[i];
            if (arg == "--game") { options.game = std::filesystem::path(std::string(value)); continue; }
            uint32_t number = 0;
            auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || (arg != "--seconds" && number == 0)) return "invalid value for " + std::string(arg);
            if (arg == "--width") options.width = number;
            else if (arg == "--height") options.height = number;
            else options.seconds = number;
        } else return "unknown option " + std::string(arg);
    }
    return options;
}
}
