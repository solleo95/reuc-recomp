#pragma once
#include <bit>
#include <cstdint>
namespace uc {
// Network play only: while the simulation runs slower than real time (a peer cannot hold the tick yet) the guest delivers audio in
// scraps and the device repeatedly runs dry, which sounds like stutter. Five dry writes among the last sixteen mute the output;
// ninety fed writes in a row (about 1.5 s of game time) bring it back.
class AudioGapMute {
public:
    bool update(bool device_ran_dry) {
        if(writes_<32) {++writes_;return false;}   // The device is still filling up when the game starts.
        history_=(history_<<1)|(device_ran_dry ? 1u : 0u);
        fed_=device_ran_dry ? 0 : fed_+1;
        if(!muted_ && std::popcount(history_&0xFFFFu)>=5) muted_=true;
        else if(muted_ && fed_>=90) muted_=false;
        return muted_;
    }
    bool muted() const { return muted_; }
private:
    uint32_t history_=0;
    unsigned fed_=0,writes_=0;
    bool muted_=false;
};
}
