#pragma once
#include <algorithm>
#include <cmath>
namespace uc {
// DLSS Ray Reconstruction inputs (docs/superpowers/plans/2026-10-20-ray-tracing-p7-ray-reconstruction.md). The game has no roughness: a draw's reflectivity (rt_reflect_math.h,
// 0 matte .. 0.5 polished) says how glossy it is; matte surfaces are fully rough.
inline float rr_roughness(float reflectivity) { return 1.f-0.7f*std::min(std::max(reflectivity*2.f,0.f),1.f); }
// The specular albedo of the guide (section 4.2.1, EnvBRDFApprox2, Ray Tracing Gems ch. 32) for a grey specular colour; alpha = roughness squared.
inline float rr_specular_albedo(float specular_color, float roughness, float n_dot_v) {
    const float nov=std::fabs(n_dot_v), alpha=roughness*roughness;
    const float x[4]={1.f,nov,nov*nov,nov*nov*nov}, y[4]={1.f,alpha,alpha*alpha,alpha*alpha*alpha};
    const float m1[2][2]={{0.99044f,-1.28514f},{1.29678f,-0.755907f}};
    const float m2[3][3]={{1.f,2.92338f,59.4188f},{20.3225f,-27.0302f,222.592f},{121.563f,626.13f,316.627f}};
    const float m3[2][2]={{0.0365463f,3.32707f},{9.0632f,-9.04756f}};
    const float m4[3][3]={{1.f,3.59685f,-1.36772f},{9.04401f,-16.3174f,9.22949f},{5.56589f,19.7886f,-20.2123f}};
    const float xy[2]={x[0],x[1]}, xyw[3]={x[0],x[1],x[3]}, xzw[3]={x[0],x[2],x[3]}, yw[3]={y[0],y[1],y[3]};
    const float a0=m1[0][0]*xy[0]+m1[0][1]*xy[1], a1=m1[1][0]*xy[0]+m1[1][1]*xy[1];
    float bias=(a0*y[0]+a1*y[1]);
    float d[3]; for (int i=0;i<3;++i) d[i]=m2[i][0]*xyw[0]+m2[i][1]*xyw[1]+m2[i][2]*xyw[2];
    bias/=(d[0]*yw[0]+d[1]*yw[1]+d[2]*yw[2]);
    const float b0=m3[0][0]*xy[0]+m3[0][1]*xy[1], b1=m3[1][0]*xy[0]+m3[1][1]*xy[1];
    float scale=(b0*y[0]+b1*y[1]);
    float e[3]; for (int i=0;i<3;++i) e[i]=m4[i][0]*xzw[0]+m4[i][1]*xzw[1]+m4[i][2]*xzw[2];
    scale/=(e[0]*yw[0]+e[1]*yw[1]+e[2]*yw[2]);
    bias*=std::min(std::max(specular_color*50.f,0.f),1.f);   // the guide's hack for a specular reflectance of 0
    return specular_color*std::max(0.f,scale)+std::max(0.f,bias);
}
// Whether the frame goes through Ray Reconstruction: DLSS is the upscaler, the GPU supports it and an ray-traced effect produced a noisy signal this frame.
inline bool rr_use(bool dlss_selected, bool rr_supported, bool effect_ran) { return dlss_selected && rr_supported && effect_ran; }
}
