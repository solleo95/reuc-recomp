#include "check.h"
#include "audio_gap.h"
int main() {
    uc::AudioGapMute gap;
    for(int write=0;write<200;++write)CHECK(!gap.update(write%40==0));      // Healthy: an odd dry write now and then changes nothing.
    uc::AudioGapMute start;
    for(int write=0;write<32;++write)CHECK(!start.update(true));            // The first writes fill the device.
    uc::AudioGapMute behind;
    for(int write=0;write<32;++write)behind.update(false);
    bool muted=false;
    for(int write=0;write<10;++write)muted=behind.update(write%2==0);       // Every other write dry: stutter.
    CHECK(muted);
    for(int write=0;write<88;++write)CHECK(behind.update(false));          // Still muted until the game has been fed for 90 writes in a row.
    CHECK(!behind.update(false));
    for(int write=0;write<5;++write)behind.update(true);                    // Behind again.
    CHECK(behind.muted());
    behind.update(false);
    for(int write=0;write<80;++write)CHECK(behind.update(false));           // A single fed write does not restore it.
}
