#include "check.h"
#include "rumble_hold.h"
using uc::RumbleHold;
int main() {
    {   // the motor goes off hold_ms after the game's motor was last seen on: a tap of 30 ms is felt for 100, the gap between two shots is bridged
        RumbleHold hold;
        CHECK(hold.apply(0,0,100)==0);
        CHECK(hold.apply(1,10,100)==1);
        CHECK(hold.apply(0,40,100)==1);       // the game's tap is over after 30 ms: the pad keeps going
        CHECK(hold.apply(0,109,100)==1);
        CHECK(hold.apply(0,110,100)==0);      // 100 ms after the last time it was on
        CHECK(hold.apply(1,200,100)==1 && hold.apply(1,350,100)==1);
        CHECK(hold.apply(0,351,100)==1 && hold.apply(0,449,100)==1);      // a long rumble ends one hold after the game ended it
        CHECK(hold.apply(0,450,100)==0);
    }
    {   // an automatic weapon: a tap of one frame every 60 ms never lets the motor go until the shooting stops
        RumbleHold hold; bool gap=false;
        for(uint64_t now=0;now<1000;now+=15) {
            const bool tap=(now/30)%2==0;                   // 30 ms on, 30 ms off
            if(hold.apply(tap ? 1 : 0,now,100)!=1) gap=true;
        }
        CHECK(!gap);
        CHECK(hold.apply(0,1100,100)==0);
    }
    {   // a weapon that fires every 170 ms with a tap of 30 ms: after the second tap the hold follows the rate (1.3 periods), so the gaps are bridged
        RumbleHold hold; unsigned gaps=0;
        for(uint64_t now=0;now<1700;now+=10) if(hold.apply((now%170)<30 ? 1 : 0,now,100,250)!=1 && now>=170) ++gaps;
        CHECK(gaps==0);
        CHECK(hold.apply(0,1770,100,250)==1 && hold.apply(0,1771,100,250)==0);   // and it lets go 221 ms (1.3 periods) after the last tap, seen on at 1550
    }
    {   // shots slower than the burst limit keep the plain hold, and a lone shot after a burst is back to it
        RumbleHold hold;
        CHECK(hold.apply(1,0,100,250)==1 && hold.apply(0,50,100,250)==1 && hold.apply(0,130,100,250)==0);
        CHECK(hold.apply(1,400,100,250)==1 && hold.apply(0,450,100,250)==1 && hold.apply(0,520,100,250)==0);   // 400 ms apart: no widening
        for(uint64_t now=1000;now<1400;now+=10) hold.apply((now%100)<30 ? 1 : 0,now,100,250);                    // a quick burst
        CHECK(hold.apply(1,4000,100,250)==1 && hold.apply(0,4110,100,250)==0);                                   // a lone shot later: 100 ms again
    }
    {   // each player has a clock of their own
        RumbleHold hold;
        CHECK(hold.apply(2,0,100)==2);
        CHECK(hold.apply(1,20,100)==3);       // player 2's tap is still held, player 1 started (held until 120)
        CHECK(hold.apply(0,60,100)==3);
        CHECK(hold.apply(1,70,100)==3);       // player 1 taps again: held until 170
        CHECK(hold.apply(0,105,100)==1);      // player 2's hold ended at 100
        CHECK(hold.apply(0,125,100)==1);
        CHECK(hold.apply(0,170,100)==0);
    }
    CHECK(RumbleHold{}.apply(3,5,0)==3 && RumbleHold{}.apply(0,5,0)==0);   // hold 0: the game's own timing
    using uc::RumbleDevice;
    {   // the router: one tap of the game is one tap of the pad, however many loops it spans
        uc::RumbleRouter router; uint64_t now=1000; unsigned pad_starts=0,wii_starts=0;
        const auto step=[&](unsigned motors,bool allowed,RumbleDevice device,uint64_t advance) {
            now+=advance; const auto out=router.step(motors,allowed,device,now); pad_starts+=out.pad_starts; wii_starts+=out.wii_starts; return out;
        };
        const auto pad=RumbleDevice::pad;
        CHECK(step(1,true,pad,0).pad==1 && pad_starts==1);
        for(int loop=0;loop<5;++loop) CHECK(step(1,true,pad,8).pad==1);            // the game's motor stays on for several loops: still one tap (last seen on at 1040)
        CHECK(pad_starts==1);
        CHECK(step(0,true,pad,8).pad==1);                                           // the game ended it: the pad is held for 100 ms from the last time
        CHECK(step(0,true,pad,70).pad==1 && pad_starts==1);                         // 1118: still held, and no second start
        CHECK(step(0,true,pad,30).pad==0 && pad_starts==1);                         // 1148: let go
        CHECK(step(1,true,pad,300).pad==1 && pad_starts==2);                        // the next tap, 450 ms later (slower than a burst) is the second one
        CHECK(step(0,true,pad,200).pad==0);
        CHECK(step(0,true,pad,8).pad==0 && pad_starts==2 && wii_starts==0);         // nothing while the game is quiet
    }
    {   // a slower automatic weapon (a tap of 30 ms every 170 ms): the pause before the second tap, then one continuous rumble
        uc::RumbleRouter router; unsigned starts=0,gaps=0;
        for(uint64_t now=2000;now<4000;now+=7) {
            const auto out=router.step((now%170)<30 ? 1 : 0,true,RumbleDevice::pad,now);
            starts+=out.pad_starts; if(!out.pad && now>=2210) ++gaps;   // 2210: the second tap
        }
        CHECK(starts<=2 && gaps==0);
    }
    {   // shooting with an automatic weapon is one continuous rumble: one start, however many taps
        uc::RumbleRouter router; unsigned starts=0; bool gap=false;
        for(uint64_t now=1020;now<2000;now+=7) {                                 // the first tap at 1020, then 30 ms on and 30 ms off
            const auto out=router.step((now/30)%2==0 ? 1 : 0,true,RumbleDevice::pad,now);
            starts+=out.pad_starts; if(!out.pad) gap=true;
        }
        CHECK(starts==1 && !gap);
    }
    {   // the device in use gets it, never both, and nothing when the player is on the keyboard; a tap while input is not allowed is never played afterwards
        uc::RumbleRouter router; uint64_t now=5000;
        auto out=router.step(1,true,RumbleDevice::remote,now);
        CHECK(out.pad==0 && out.wii==1 && out.wii_starts==1 && out.pad_starts==0);   // the Wii Remote is in use: the pads stay quiet
        out=router.step(1,true,RumbleDevice::pad,now+=10);
        CHECK(out.pad==1 && out.wii==0 && out.pad_starts==1);                        // the pad is in use now: the remote stops
        out=router.step(0,true,RumbleDevice::pad,now+=300); CHECK(out.pad==0 && out.wii==0);
        out=router.step(1,true,RumbleDevice::none,now+=10); CHECK(out.pad==0 && out.wii==0 && out.pad_starts==0 && out.wii_starts==0);   // keyboard and mouse: a pad on the desk stays quiet
        out=router.step(0,true,RumbleDevice::none,now+=300);
        out=router.step(1,false,RumbleDevice::pad,now+=10); CHECK(out.pad==0 && out.wii==0 && out.pad_starts==0);   // a menu is open or the window lost focus
        out=router.step(0,true,RumbleDevice::pad,now+=10); CHECK(out.pad==0 && out.pad_starts==0);                  // closing it plays nothing from before
        out=router.step(2,true,RumbleDevice::pad,now+=10); CHECK(out.pad==2 && out.wii==0);      // player 2's motor goes to the pads only
        out=router.step(2,true,RumbleDevice::remote,now+=300); CHECK(out.pad==0 && out.wii==0);  // the remote is player 1's alone
    }
    {   // a tap held back by the hold does not come back after a pause or a menu
        uc::RumbleRouter router;
        CHECK(router.step(1,true,RumbleDevice::pad,0).pad==1);
        CHECK(router.step(0,false,RumbleDevice::pad,10).pad==0);                     // the menu opens 10 ms into the tap
        CHECK(router.step(0,true,RumbleDevice::pad,20).pad==0 && router.step(0,true,RumbleDevice::pad,60).pad==0);   // and closes within the 100 ms: still quiet
    }
}
