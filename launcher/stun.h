#pragma once
#include "netplay.h"
#include <array>
#include <string_view>

namespace uc::net {
std::optional<Endpoint> parse_peer_address(std::string_view);
std::array<uint8_t,20> stun_request(const std::array<uint8_t,12>& transaction);
std::optional<Endpoint> stun_response(std::span<const uint8_t>,const std::array<uint8_t,12>& transaction);
}
