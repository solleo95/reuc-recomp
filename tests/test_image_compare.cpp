#include "check.h"
#include "image_compare.h"
#include <cmath>
int main() {
    uc::RenderPixels a{0,2,1,{0,10,20,0,100,110,120,255}};
    auto b=a;b.rgba[0]=6;b.rgba[5]=104;b.rgba[3]=255;
    auto result=fifo_replay::compare_images(a,b);
    CHECK(result.mean==2 && result.peak==6 && result.changed_pixels==2);
    CHECK(result.diff.rgba[0]==6 && result.diff.rgba[5]==6 && result.diff.rgba[3]==255);
    CHECK(fifo_replay::compare_images(a,a).mean==0);
    b.width=1;bool failed=false;try{fifo_replay::compare_images(a,b);}catch(const std::invalid_argument&){failed=true;}CHECK(failed);
    uc::save_png(a,"image-compare-fixture.png");
    auto decoded=fifo_replay::load_reference("image-compare-fixture.png");
    CHECK(decoded.width==2 && decoded.height==1 && decoded.rgba[3]==255);
    CHECK(fifo_replay::compare_images(a,decoded).mean==0);
}
