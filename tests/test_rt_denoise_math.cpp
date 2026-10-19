#include "check.h"
#include "rt_denoise_math.h"
int main() {
    CHECK(uc::history_alpha(0)==1.f && uc::history_alpha(1)==1.f);   // no history: the new sample alone
    CHECK(uc::history_alpha(2)==0.5f);
    CHECK(uc::history_alpha(32)==1/32.f && uc::history_alpha(100)==1/32.f);   // capped at 32 frames
    // 2% of the view depth and 25 degrees between the normals
    CHECK(uc::reproject_valid(10.f,10.19f,0.91f) && uc::reproject_valid(10.f,9.81f,0.91f));
    CHECK(!uc::reproject_valid(10.f,10.21f,0.91f) && !uc::reproject_valid(10.f,9.79f,0.91f));
    CHECK(!uc::reproject_valid(10.f,10.f,0.90f));
    CHECK(!uc::reproject_valid(10.f,0.f,1.f));   // no history surface (cleared history depth)
    return 0;
}
