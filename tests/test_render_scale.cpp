#include "check.h"
#include "render_scale.h"
#include <array>
#include <limits>
int main() {
    for(auto scale:std::array{0.5,1.0,1.25,1.5,2.0,3.0,6.0}) {
        const auto extent=uc::manual_render_extent(scale);
        CHECK(extent.width==uint32_t(1280*scale) && extent.height==uint32_t(720*scale));
    }
    CHECK(uc::monitor_render_scale(2560,1440)==2 && uc::monitor_render_scale(3840,2160)==3);
    CHECK(uc::monitor_render_scale(7680,4320)==6);
    const auto wide=uc::manual_render_extent(2,3440.0/1440);
    CHECK(wide.width==3440 && wide.height==1440);
    CHECK(uc::device_render_scale(6,4096)==2 && uc::device_render_scale(6,8192)==4);
    CHECK(uc::device_render_scale(6,16384)==6 && uc::device_render_scale(1.5,4096)==1.5);
    for(double value:std::array{0.0,0.49,6.01,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        bool rejected=false; try { uc::manual_render_extent(value); } catch(const std::invalid_argument&) { rejected=true; }
        CHECK(rejected);
    }
    for(double aspect:std::array{0.0,9.0,std::numeric_limits<double>::quiet_NaN()}) {
        bool rejected=false; try { uc::manual_render_extent(1,aspect); } catch(const std::invalid_argument&) { rejected=true; }
        CHECK(rejected);
    }
    bool rejected=false; try { uc::manual_render_extent(6,3440.0/1440); } catch(const std::invalid_argument&) { rejected=true; } CHECK(rejected);
    rejected=false; try { uc::device_render_scale(1,512); } catch(const std::invalid_argument&) { rejected=true; } CHECK(rejected);
    rejected=false; try { uc::monitor_render_scale(0,1440); } catch(const std::invalid_argument&) { rejected=true; } CHECK(rejected);
    // A windowed start opens at the selected render resolution, shrunk (same aspect) only when the desktop cannot hold it with its frame.
    const auto fits=uc::windowed_extent(2,16.0/9,3840,2100);
    CHECK(fits.width==2560 && fits.height==1440);
    const auto shrunk=uc::windowed_extent(3,16.0/9,2560,1400);   // 3840x2160 > 2544x1352 usable client area
    CHECK(shrunk.width==2403 && shrunk.height==1352);
    const auto ultrawide=uc::windowed_extent(1,3440.0/1440,3440,1400);
    CHECK(ultrawide.height<=1352 && ultrawide.width<=3424 && std::abs(double(ultrawide.width)/ultrawide.height-3440.0/1440)<0.01);
    rejected=false; try { uc::windowed_extent(1,16.0/9,10,10); } catch(const std::invalid_argument&) { rejected=true; } CHECK(rejected);
    // GX samples at pixel centre 7/12 without AA: the scaled viewport moves by -1/12 EFB pixel.
    const auto viewport=uc::efb_viewport({10,20,600,400,0,1},1280,960);
    CHECK(viewport.x==(10-1.f/12)*2 && viewport.y==(20-1.f/12)*2 && viewport.width==1200 && viewport.height==800);
}
