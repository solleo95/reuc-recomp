#pragma once
#include "download.h"
// Shared by the WinHTTP and libcurl transports (download_common.cpp).
namespace uc::update::detail {
Error cancelled();
bool ascii_url(std::string_view url);
Error status_error(unsigned status);   // 403/429 -> rate_limited, the rest -> network
}
