// Game-specific matrix fast paths, separate from the established SDK math code.
#include "ppc_rt.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
using ppc::Ctx;
void hle_Leaf_80394374(Ctx&);
namespace {
const bool euler_off=std::getenv("UC_EULER_FINISH_OFF")!=nullptr;
const bool sincos_off=std::getenv("UC_SINCOS_FAST_OFF") || std::getenv("UC_LEAF_OFF") || std::getenv("UC_LEAF_VERIFY");
inline bool ram(uint32_t p,uint32_t n) {
    return p-0x80000000u<=0x01800000u-n || p-0x90000000u<=0x04000000u-n;
}
inline double number(Ctx& c,uint32_t address) {
    const uint32_t bits=ppc::rd32_ram(c,address); float value; std::memcpy(&value,&bits,4); return value;
}
inline double rounded(double value) { return ppc::rnd_s(value); }
inline void put(Ctx& c,uint32_t address,double value) {
    const float f=float(value); uint32_t bits; std::memcpy(&bits,&f,4);
    bits=_byteswap_ulong(bits); std::memcpy(ppc::hp(c,address),&bits,4);
}
#ifndef UC_TRACE_MEM
bool euler_fast(Ctx& c) {
    const uint32_t out=c.r[31],sp=c.r[1],constant=c.r[2]-4888;
    if(euler_off || !ram(out,48) || !ram(sp,88) || !ram(constant,4)
        || (out<sp+88 && sp<out+48)) return false;
    double values[6];
    for(unsigned i=0;i<6;++i) {
        const uint32_t bits=ppc::rd32_ram(c,sp+8+4*i);
        if((bits&0x7FFFFFFFu)>0x3F800000u) return false;
        float value; std::memcpy(&value,&bits,4); values[i]=value;
    }
    const double a=values[0],b=values[1],d=values[2],e=values[3],f=values[4],g=values[5];
    const double zero=number(c,constant);
    const double ga=rounded(g*a),fb=rounded(f*b),fa=rounded(f*a),gb=rounded(g*b);
    const double result=rounded(fa+rounded(gb*e));
    // This bounded core has no guest safe point and writes the complete matrix.
    gx::note_write_range(out,48);
    put(c,out+32,-e); put(c,out,rounded(a*d)); put(c,out+16,rounded(b*d));
    put(c,out+36,rounded(d*g)); put(c,out+40,rounded(d*f));
    put(c,out+4,rounded(rounded(ga*e)-fb)); put(c,out+24,rounded(rounded(fb*e)-ga));
    put(c,out+8,rounded(gb+rounded(fa*e)));
    put(c,out+12,zero); put(c,out+28,zero); put(c,out+44,zero); put(c,out+20,result);
    c.r[3]=out;
    const double regs[6]={zero,result,gb,b,g,fa};
    for(unsigned i=0;i<6;++i) c.f[i]=c.ps1[i]=regs[i];
    return true;
}
bool sincos_bounded(Ctx& c) {
    const uint32_t sp=c.r[1]-16,constants=c.r[2]-4936;
    if(sincos_off || c.gqr[3]!=0x00050005 || !ram(sp,16) || !ram(constants,8)
        || !ram(c.r[3],4) || !ram(c.r[4],4) || (sp<constants+8 && constants<sp+16)) return false;
    const double magnitude=std::fabs(c.f[1]),period=number(c,constants);
    // The original loop and all its polls stay in the fallback.
    if(!std::isfinite(period) || !(magnitude<period) || !(magnitude<65536)) return false;
    const uint16_t index=uint16_t(magnitude);
    const uint32_t offset=(index&255u)*16,table=0x80435E38u+offset;
    if(sp<table+16 && table<sp+16) return false;
    double values[4];
    for(unsigned i=0;i<4;++i) {
        const uint32_t bits=ppc::rd32_ram(c,table+4*i);
        if((bits&0x7FFFFFFFu)>0x3F800000u) return false;
        float value; std::memcpy(&value,&bits,4); values[i]=value;
    }
    const double zero=number(c,constants+4),fraction=rounded(magnitude-index);
    const double sine_part=rounded(fraction*values[2]),cosine_part=rounded(fraction*values[3]);
    const double sine=rounded(values[0]+sine_part),cosine=rounded(values[1]+cosine_part);
    ppc::op_stwu(c,1,1,-16);
    ppc::wr16(c,sp+12,index); ppc::wr16(c,sp+8,index);
    c.f[0]=zero; ppc::op_fcmpo(c,0,1,0);
    c.r[0]=offset; c.r[5]=table; c.r[6]=0x80435E38u;
    c.f[0]=c.ps1[0]=cosine; c.f[1]=c.ps1[1]=cosine_part;
    c.f[2]=ppc::crbit(c,0)?-sine:sine; c.ps1[2]=sine;
    c.f[3]=c.ps1[3]=sine_part; c.f[4]=c.ps1[4]=fraction;
    ppc::op_stfs(c,2,3,0); ppc::op_stfs(c,0,4,0); ppc::op_addi(c,1,1,16);
    return true;
}
#endif
void euler_core(Ctx& c) {
#ifndef UC_TRACE_MEM
    if(euler_fast(c)) return;
#endif
    using namespace ppc;
    op_lfs(c,1,1,20); op_or(c,31,3,31,false); op_lfs(c,0,2,-4888);
    op_fneg(c,1,1,false); op_stfs(c,1,31,32);
    op_lfs(c,2,1,8); op_lfs(c,1,1,16); op_fmuls(c,1,2,1,false); op_stfs(c,1,31,0);
    op_lfs(c,2,1,12); op_lfs(c,1,1,16); op_fmuls(c,1,2,1,false); op_stfs(c,1,31,16);
    op_lfs(c,2,1,16); op_lfs(c,1,1,28); op_fmuls(c,1,2,1,false); op_stfs(c,1,31,36);
    op_lfs(c,2,1,16); op_lfs(c,1,1,24); op_fmuls(c,1,2,1,false); op_stfs(c,1,31,40);
    op_lfs(c,2,1,28); op_lfs(c,1,1,8); op_lfs(c,4,1,24); op_fmuls(c,5,2,1,false);
    op_lfs(c,3,1,12); op_lfs(c,1,1,20); op_fmuls(c,2,4,3,false);
    op_fmuls(c,1,5,1,false); op_fsubs(c,1,1,2,false); op_stfs(c,1,31,4);
    op_lfs(c,1,1,20); op_fmuls(c,1,2,1,false); op_fsubs(c,1,1,5,false); op_stfs(c,1,31,24);
    op_lfs(c,2,1,24); op_lfs(c,1,1,8); op_lfs(c,4,1,28); op_fmuls(c,5,2,1,false);
    op_lfs(c,3,1,12); op_lfs(c,1,1,20); op_fmuls(c,2,4,3,false);
    op_fmuls(c,1,5,1,false); op_fadds(c,1,2,1,false); op_stfs(c,1,31,8);
    op_lfs(c,1,1,20); op_fmuls(c,1,2,1,false); op_stfs(c,0,31,12); op_stfs(c,0,31,28);
    op_fadds(c,1,5,1,false); op_stfs(c,0,31,44); op_stfs(c,1,31,20);
}
}
void hle_MatrixEulerFinish(Ctx& c) {
    euler_core(c);
    ppc::op_psq_l(c,31,1,72,0,0); ppc::op_lfd(c,31,1,64);
    ppc::op_psq_l(c,30,1,56,0,0); ppc::op_lfd(c,30,1,48);
    ppc::op_lwz(c,31,1,44); ppc::op_lwz(c,0,1,84); ppc::op_mtspr(c,0,8); ppc::op_addi(c,1,1,80);
}
void hle_SinCosFIdx(Ctx& c) {
#ifndef UC_TRACE_MEM
    if(sincos_bounded(c)) return;
#endif
    hle_Leaf_80394374(c);
}
