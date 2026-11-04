#include "coop_roster.h"
namespace uc {
std::optional<Player> CoopRoster::press(DeviceId device,bool in_gameplay) {
    if(device.kind==DeviceKind::keyboard || second_==device || first_.contains(device)) return std::nullopt;
    if(!in_gameplay || second_) { first_.insert(device); return std::nullopt; }
    second_=device;
    return Player::two;
}
Player CoopRoster::route(DeviceId device) const { return second_==device ? Player::two : Player::one; }
bool CoopRoster::remove(DeviceId device) {
    if(second_==device) { second_.reset(); return true; }
    first_.erase(device);
    return false;
}
void CoopRoster::release_second() { second_.reset(); }   // nobody has the device any more: it is unknown again
}
