#pragma once
#include "options.h"

namespace uc {
inline std::optional<std::string> startup_error_message(const Options& options,std::string_view reason,const std::filesystem::path& logs) {
    if(options.features || options.headless || !options.test_control.empty() || !options.test_server.empty() ||
       !options.demo_play.empty() || !options.demo_record.empty() || options.seconds || options.stop_retrace || !options.det_out.empty()) return std::nullopt;
    const auto path=logs.u8string();
    return "Unable to start Umbrella Chronicles.\n\n"+std::string(reason)+
        "\n\nDiagnostic logs:\n"+std::string(path.begin(),path.end());
}
}
