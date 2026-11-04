#include "check.h"
#include "coop_roster.h"
using namespace uc;
static DeviceId pad(uint64_t id) { return {DeviceKind::gamepad,id}; }
static DeviceId mouse(uint64_t id) { return {DeviceKind::pointer,id}; }
static DeviceId remote(uint64_t id) { return {DeviceKind::wiimote,id}; }
static const DeviceId keyboard{DeviceKind::keyboard,0};
static void keyboard_never_joins() {
    CoopRoster roster;
    CHECK(!roster.press(keyboard,true));
    CHECK(!roster.second());
    CHECK(roster.route(keyboard)==Player::one);
}
static void unused_device_pressed_in_gameplay_joins() {
    CoopRoster roster;
    const auto joined=roster.press(pad(7),true);
    CHECK(joined && *joined==Player::two);
    CHECK(roster.second() && *roster.second()==pad(7));
    CHECK(roster.route(pad(7))==Player::two);
    CHECK(roster.route(pad(8))==Player::one);                     // every other device is player one's
}
static void a_device_used_in_a_menu_belongs_to_player_one() {
    CoopRoster roster;
    CHECK(!roster.press(pad(7),false));                          // pressed in a menu, pause or title screen
    CHECK(!roster.press(pad(7),true));                           // the same device later in gameplay is still player one's
    CHECK(!roster.second());
    CHECK(roster.route(pad(7))==Player::one);
}
static void a_device_pressed_while_a_second_player_exists_belongs_to_player_one() {
    CoopRoster roster;
    CHECK(roster.press(remote(1),true));
    CHECK(!roster.press(mouse(5),true));                          // another new device: player one's, not a third player
    CHECK(roster.route(mouse(5))==Player::one);
    CHECK(roster.remove(remote(1)));                              // P2 leaves
    CHECK(!roster.press(mouse(5),true));                          // the mouse stays player one's
    const auto joined=roster.press(pad(9),true);
    CHECK(joined && *joined==Player::two);                        // a really new device takes the free place
}
static void unplugging_the_second_players_device_frees_the_place() {
    CoopRoster roster;
    CHECK(roster.press(pad(2),true));
    CHECK(!roster.remove(pad(99)));                               // an unknown or player one's device: nothing happens
    CHECK(roster.second());
    CHECK(roster.remove(pad(2)));
    CHECK(!roster.second());
    CHECK(roster.route(pad(2))==Player::one);
    CHECK(roster.press(mouse(3),true));                           // someone else joins
}
static void a_released_device_is_unknown_again() {   // nobody has it: the next press is a question again, not a silent decision
    CoopRoster roster;
    CHECK(roster.press(pad(2),true));
    roster.release_second();
    CHECK(!roster.second() && !roster.known(pad(2)));
    CHECK(roster.route(pad(2))==Player::one);                     // until somebody decides, its input goes to player one
    const auto again=roster.press(pad(2),true);
    CHECK(again && *again==Player::two);                          // the roster itself would take it as new (the session asks first)
}
static void two_presses_make_one_second_player() {
    CoopRoster roster;
    const auto first=roster.press(pad(1),true),second=roster.press(pad(2),true);
    CHECK(first && !second);
    CHECK(*roster.second()==pad(1));
}
static void known_devices_are_the_ones_with_a_player() {
    CoopRoster roster;
    CHECK(!roster.known(pad(1)) && !roster.has_first());
    roster.press(pad(1),false); CHECK(roster.has_first());          // the first player has a device
    roster.remove(pad(1)); CHECK(!roster.has_first());
    roster.press(pad(1),false); CHECK(roster.known(pad(1)));       // player one's
    roster.press(pad(2),true); CHECK(roster.known(pad(2)));        // player two's
    roster.remove(pad(2)); CHECK(!roster.known(pad(2)));
}
static void a_replugged_player_one_device_is_new_again() {
    CoopRoster roster;
    CHECK(!roster.press(pad(4),false));
    CHECK(!roster.remove(pad(4)));                                // it was player one's, not the second player's
    CHECK(roster.press(pad(4),true));                             // plugged in again later it is a new device
}
int main() {
    keyboard_never_joins(); unused_device_pressed_in_gameplay_joins(); a_device_used_in_a_menu_belongs_to_player_one();
    a_device_pressed_while_a_second_player_exists_belongs_to_player_one(); unplugging_the_second_players_device_frees_the_place();
    a_released_device_is_unknown_again(); two_presses_make_one_second_player(); known_devices_are_the_ones_with_a_player(); a_replugged_player_one_device_is_new_again();
}
