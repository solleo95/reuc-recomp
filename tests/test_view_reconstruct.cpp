#include "check.h"
#include "gx_transforms.h"
#include "view_reconstruct.h"
#include <cmath>
int main() {
    // A GX perspective projection as the game builds it: fovy 60 degrees, 16:9, near 1, far 1000 (depth 0 at the near plane, 1 at the far plane).
    const float tangent=std::tan(30.f*3.14159265f/180), aspect=16.f/9, near_plane=1, far_plane=1000;
    const auto projection=gx::make_projection({1.f/(tangent*aspect),0,1.f/tangent,0,-far_plane/(far_plane-near_plane),-far_plane*near_plane/(far_plane-near_plane)},0);
    const auto params=uc::view_params(projection);
    for(const std::array<float,3>& point:{std::array<float,3>{0,0,-10},{3,-2,-50},{-1,1,-999},{40,25,-30}}) {
        float clip[4]; for(int row=0;row<4;++row) clip[row]=projection[row][0]*point[0]+projection[row][1]*point[1]+projection[row][2]*point[2]+projection[row][3];
        const auto back=uc::reconstruct_view(params,clip[0]/clip[3],clip[1]/clip[3],clip[2]/clip[3]);
        for(int axis=0;axis<3;++axis) CHECK(std::abs(back[axis]-point[axis])<=1e-3f*std::max(1.f,std::abs(point[axis])));
    }
    // An off-centre projection (windowed scene: GX offsets are in the third column) reconstructs too.
    const auto shifted=gx::make_projection({1.2f,0.3f,1.7f,-0.2f,-2.f,-1.f},0);
    const auto shifted_params=uc::view_params(shifted);
    const std::array<float,3> point{2,-1,-4};
    float clip[4]; for(int row=0;row<4;++row) clip[row]=shifted[row][0]*point[0]+shifted[row][1]*point[1]+shifted[row][2]*point[2]+shifted[row][3];
    const auto back=uc::reconstruct_view(shifted_params,clip[0]/clip[3],clip[1]/clip[3],clip[2]/clip[3]);
    for(int axis=0;axis<3;++axis) CHECK(std::abs(back[axis]-point[axis])<=1e-3f);
    return 0;
}
