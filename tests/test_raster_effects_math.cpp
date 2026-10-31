#include "check.h"
#include "raster_effects_math.h"
int main() {
    // A blur needs two samples per pixel of its length, at least 8, at most the preset's count.
    CHECK(uc::blur_sample_count(32, 0.5f, false) == 8);
    CHECK(uc::blur_sample_count(32, 3.2f, false) == 8);
    CHECK(uc::blur_sample_count(32, 10.1f, false) == 21);
    CHECK(uc::blur_sample_count(32, 100.f, false) == 32);
    CHECK(uc::blur_sample_count(8, 100.f, false) == 8);
    CHECK(uc::blur_sample_count(12, 3.f, false) == 8);
    // UC_BLUR_FULL_SAMPLES keeps the preset's count whatever the length.
    CHECK(uc::blur_sample_count(32, 1.f, true) == 32);
    CHECK(uc::blur_sample_count(12, 100.f, true) == 12);
    // The unblurred centre sample weighs as many times less as there are fewer samples than the preset takes, so a short blur is not sharper than a long one with the same smear.
    CHECK(uc::blur_centre_weight(8, 32) == 0.25f);
    CHECK(uc::blur_centre_weight(32, 32) == 1.f);
    CHECK(uc::blur_centre_weight(12, 12) == 1.f);
    CHECK(uc::blur_centre_weight(0, 0) == 1.f);
    return 0;
}
