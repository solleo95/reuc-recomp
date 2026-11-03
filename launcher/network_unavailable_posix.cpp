// Linux: network play is not in the Linux launcher yet (its UPnP, STUN and ENet code is Windows only). The launcher's shared code still names these classes, so they exist
// here and say so: a session or a router mapping fails at once, and the launcher never offers them (the settings registry hides the Network button on Linux).
#include "network_session.h"
#include "nat_mapping.h"
#include <charconv>
#include <stdexcept>

namespace uc::net {
namespace {
constexpr const char* kUnavailable = "Network play is not available on Linux yet";
}
struct NatTunnel::Impl {};
NatTunnel::NatTunnel(Endpoint, std::vector<std::string>, std::chrono::milliseconds) { throw std::runtime_error(kUnavailable); }
NatTunnel::~NatTunnel() = default;
TunnelState NatTunnel::state() const { return {}; }
void NatTunnel::punch(const std::string&) {}

// "IPv4:port" (also "localhost:port"), spaces around it allowed.
std::optional<Endpoint> parse_connection_address(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n"), last = text.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    auto value = text.substr(first, last - first + 1);
    if (value.starts_with("localhost:")) value.replace(0, 9, "127.0.0.1");
    const auto colon = value.rfind(':');
    if (colon == std::string::npos || colon == 0) return {};
    const auto address = value.substr(0, colon);
    unsigned port = 0;
    const auto digits = value.substr(colon + 1);
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), port);
    if (digits.empty() || parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() || port == 0 || port > 65535 || !valid_address(address)) return {};
    return Endpoint{false, address, uint16_t(port), false};
}
NetworkSession::NetworkSession(Endpoint endpoint, std::vector<std::string> servers) : endpoint_(std::move(endpoint)), servers_(std::move(servers)) { throw std::runtime_error(kUnavailable); }
NetworkSession::~NetworkSession() = default;
void NetworkSession::poll(bool) {}
void NetworkSession::punch(const std::string&) {}
std::optional<Endpoint> NetworkSession::take_start() { return std::nullopt; }

std::unique_ptr<NatGateway> upnp_gateway() { return nullptr; }
NatMapping::NatMapping(uint16_t, std::unique_ptr<NatGateway>, std::chrono::milliseconds) { throw std::runtime_error(kUnavailable); }
NatMapping::~NatMapping() = default;
void NatMapping::stop() {}
NatState NatMapping::state() const { return {}; }
}  // namespace uc::net
