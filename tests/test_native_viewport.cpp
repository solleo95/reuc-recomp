#include "check.h"
#include "native_viewport.h"
#include <limits>

int main() {
    const gx::Projection initial{{{1.3f,0,0,.2f},{0,.9f,0,-.1f},{0,0,-1,-.1f},{0,0,-1,0}}};
    for(const auto viewport:std::array<uc::EfbViewport,4>{{{-.0625f,-1.f/12,480,360},{12.3f,8.7f,320.2f,210.1f},{0,0,1280,720},{-20,-10,640,480}}}) {
        auto projection=initial; std::array<float,2> shift{.04f,-.03f}; const auto original_shift=shift;
        const auto corrected=uc::native_viewport(viewport,projection,shift);
        const gx::Projection camera{{{.98f,.03f,0,.12f},{-.02f,1.01f,0,-.07f},{0,0,1,0},{0,0,.01f,1}}};
        const auto compensated_camera=uc::native_camera_motion(viewport,camera);
        const auto project=[](const gx::Projection& p,const std::array<float,4>& v,size_t row) {
            float value=0; for(size_t i=0;i<4;++i)value+=p[row][i]*v[i]; return value;
        };
        for(const auto point:std::array<std::array<float,4>,3>{{{-.4f,.3f,-1,1},{2,-3,-8,1},{0,0,-.5f,1}}}) {
            std::array<float,4> clip{},corrected_clip{},previous{},corrected_previous{};
            for(size_t row=0;row<4;++row){clip[row]=project(initial,point,row);corrected_clip[row]=project(projection,point,row);}
            for(size_t row=0;row<4;++row){previous[row]=project(camera,clip,row);corrected_previous[row]=project(compensated_camera,corrected_clip,row);}
            const float w=project(initial,point,3);
            for(size_t axis=0;axis<2;++axis) {
                const float origin=axis ? viewport.y : viewport.x,extent=axis ? viewport.height : viewport.width;
                const float new_origin=axis ? corrected.viewport.y : corrected.viewport.x,new_extent=axis ? corrected.viewport.height : corrected.viewport.width;
                const float before=origin+(project(initial,point,axis)/w+original_shift[axis]+1)*extent*.5f;
                const float after=new_origin+(project(projection,point,axis)/w+shift[axis]+1)*new_extent*.5f;
                CHECK(std::abs(before-after)<.001f);
                const float previous_pixel=origin+(previous[axis]/previous[3]+1)*extent*.5f;
                const float corrected_pixel=new_origin+(corrected_previous[axis]/corrected_previous[3]+1)*new_extent*.5f;
                CHECK(std::abs(previous_pixel-corrected_pixel)<.001f);
            }
        }
    }
    auto projection=initial; std::array<float,2> shift{};
    const auto edge=uc::native_viewport({-.0625f,-1.f/12,480,360},projection,shift);
    CHECK((edge.clip==std::array<int32_t,4>{0,0,480,360}));
    for(const auto invalid:std::array<uc::EfbViewport,4>{{{0,0,0,360},{0,0,-1,360},{0,0,std::numeric_limits<float>::infinity(),360},{0,0,480,std::numeric_limits<float>::quiet_NaN()}}}) {
        bool rejected=false; try { uc::native_viewport(invalid,projection,shift); } catch(const std::invalid_argument&) { rejected=true; } CHECK(rejected);
    }
}
