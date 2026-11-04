#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
namespace uc {
// The game's rumble is a series of taps a frame or two long (30-60 ms), and an automatic weapon gives one per shot with a gap between: a pad's motor needs longer to be felt and
// would stutter. Each player's motor stays on until `hold_ms` after the last time the game's motor was seen on, so a lone tap is felt for `hold_ms` and a burst is one continuous
// rumble that ends `hold_ms` after the last shot. A weapon that fires faster than every `burst_ms` (0: never) widens the hold to 1.3 of its period, so the gap between its taps is
// bridged whatever its rate; the first gap of a burst, and a lone shot, keep the plain hold. `motors` and the result: bit 0 the first player, bit 1 the second.
struct RumbleHold {
    unsigned apply(unsigned motors,uint64_t now_ms,uint64_t hold_ms,uint64_t burst_ms=0) {
        unsigned result=motors;
        for(unsigned player=0;player<2;++player) {
            const unsigned bit=1u<<player;
            if((motors&bit) && !(was_&bit)) {   // a tap begins
                const uint64_t period=now_ms-rise_[player];
                hold_[player]=rise_[player]!=none && period<=burst_ms ? std::max(hold_ms,period*13/10) : hold_ms;
                rise_[player]=now_ms;
            }
            if(motors&bit) until_[player]=now_ms+hold_[player];
            if(now_ms<until_[player]) result|=bit;
        }
        was_=motors;
        return result;
    }
private:
    static constexpr uint64_t none=~uint64_t(0);
    std::array<uint64_t,2> until_{},hold_{},rise_{none,none};
    unsigned was_=0;
};
// What the host loop does with the game's motors each pass: nothing while input is not allowed (a menu, a pause, another window), the device the player is using only (a Wii
// Remote, a pad, none for the keyboard and mouse: a pad lying on the desk stays quiet), each tap held long enough to be felt. `pad` has a bit per player, `wii` is the first
// player's motor alone (a remote is player 1's). pad_starts / wii_starts count the taps that began in this pass: the diagnostics compare them with the game's own starts.
enum class RumbleDevice { none,pad,remote };
struct RumbleOutput { unsigned pad=0,wii=0,pad_starts=0,wii_starts=0; };
class RumbleRouter {
public:
    static constexpr uint64_t kPadHoldMs=100,    // the game's taps last 30-60 ms: a pad's motor needs about 100 ms to be felt
                              kPadBurstMs=250,   // a weapon firing faster than this (the SMG) is one continuous rumble
                              kWiiHoldMs=40;     // the remote's thread looks every 20 ms; it keeps the game's own rhythm
    RumbleOutput step(unsigned game_motors,bool input_allowed,RumbleDevice device,uint64_t now_ms) {
        if(!input_allowed) {pad_hold_=wii_hold_={}; game_motors=0;}   // what a tap still had to run is dropped, not resumed when the menu closes
        const unsigned pad=pad_hold_.apply(game_motors,now_ms,kPadHoldMs,kPadBurstMs), wii=wii_hold_.apply(game_motors,now_ms,kWiiHoldMs)&1;
        RumbleOutput out;
        out.pad=device==RumbleDevice::pad ? pad : 0; out.wii=device==RumbleDevice::remote ? wii : 0;
        return finish(out);
    }
    // Local two-player play with a second player: each player's motor goes to the kind of device that player uses (a remote: bit 0 of `wii` is the first player's remote, bit 1 the
    // second's; a pad: the same in `pad`; none: a mouse or a light gun has no motor).
    RumbleOutput step(unsigned game_motors,bool input_allowed,RumbleDevice device,RumbleDevice second_device,uint64_t now_ms) {
        if(!input_allowed) {pad_hold_=wii_hold_={}; game_motors=0;}
        const unsigned pad=pad_hold_.apply(game_motors,now_ms,kPadHoldMs,kPadBurstMs), wii=wii_hold_.apply(game_motors,now_ms,kWiiHoldMs);
        const auto only=[](unsigned bits,RumbleDevice first,RumbleDevice second,RumbleDevice wanted) { return ((first==wanted ? bits&1u : 0u)|(second==wanted ? bits&2u : 0u)); };
        RumbleOutput out;
        out.pad=only(pad,device,second_device,RumbleDevice::pad); out.wii=only(wii,device,second_device,RumbleDevice::remote);
        return finish(out);
    }
private:
    RumbleOutput finish(RumbleOutput out) {
        out.pad_starts=unsigned(std::popcount(out.pad&~pad_was_)); out.wii_starts=unsigned(std::popcount(out.wii&~wii_was_));
        pad_was_=out.pad; wii_was_=out.wii;
        return out;
    }
    RumbleHold pad_hold_,wii_hold_;
    unsigned pad_was_=0,wii_was_=0;
};
}
