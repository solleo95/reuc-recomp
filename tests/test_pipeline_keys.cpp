#include "check.h"
#include "pipeline_keys.h"
#include <fstream>

int main() {
    const auto path=std::filesystem::temp_directory_path()/"uc_test_pipeline_keys.bin";
    std::filesystem::remove(path);
    CHECK(uc::load_pipeline_keys(path).empty());
    uc::PipelineKey a{}, b{};
    a[69]=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; b=a; b[1]=4; b[3]=0xABCD;
    CHECK(uc::append_pipeline_key(path,a));
    CHECK(uc::append_pipeline_key(path,b));
    auto keys=uc::load_pipeline_keys(path);
    CHECK(keys.size()==2 && keys[0]==a && keys[1]==b);
    { std::ofstream(path,std::ios::binary|std::ios::app).write("torn",4); }   // torn trailing record
    CHECK(uc::load_pipeline_keys(path).size()==2);
    { std::ofstream(path,std::ios::binary|std::ios::trunc).write("garbage garbage garbage",23); }   // wrong header
    CHECK(uc::load_pipeline_keys(path).empty());
    CHECK(uc::append_pipeline_key(path,a));
    CHECK(uc::load_pipeline_keys(path).size()==1);
    std::filesystem::remove(path);

    // The ubershader pipeline depends on the fixed-function words only.
    uc::PipelineKey c=a; c[0]=0x1234; c[68]=9; c[1]=3; c[2]=7; c[66]=5; c[67]=1;
    uc::PipelineKey d=c; d[1]=9; d[40]=77; d[67]=0;
    CHECK(uc::uber_key(c)==uc::uber_key(d));
    CHECK(uc::uber_key(c)[0]==0x1234 && uc::uber_key(c)[68]==9 && uc::uber_key(c)[69]==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && uc::uber_key(c)[1]==0 && uc::uber_key(c)[67]==0);
    CHECK(uc::uber_key(c)!=uc::uber_key([&]{ auto e=c; e[0]^=1; return e; }()));

    // Cull and the depth state are dynamic: keys differing only there share a pipeline; whether the draw writes depth stays in it.
    uc::PipelineKey s=a; s[0]=1|(1u<<14); s[68]=17|(3<<1);
    uc::PipelineKey t=s; t[0]=1|(2u<<14); t[68]=17|(6<<1);
    CHECK(uc::static_key(s)==uc::static_key(t) && uc::static_key(s)[68]==17 && (uc::static_key(s)[0]>>14)==0 && (uc::static_key(s)[0]&0x3FFF)==1);
    t[68]=1|(6<<1); CHECK(uc::static_key(t)[68]==0 && uc::static_key(s)!=uc::static_key(t));
    CHECK(uc::cull_mode(s)==VK_CULL_MODE_BACK_BIT && uc::cull_mode(a)==VK_CULL_MODE_NONE && uc::static_key(uc::static_key(s))==uc::static_key(s));

    // The ubershader closure combines every blend/cull word, depth word and topology seen, so a new TEV key never needs a synchronous compile.
    uc::PipelineKey f=a, g=a, h=a; f[0]=1; f[68]=1; g[0]=2; g[68]=17; h[0]=1; h[68]=1; h[69]=VK_PRIMITIVE_TOPOLOGY_LINE_LIST; h[1]=5;
    const auto closure=uc::uber_key_closure({f,g,h});
    CHECK(closure.size()==2*2*2);
    CHECK(std::find(closure.begin(),closure.end(),uc::uber_key(h))!=closure.end());
    auto cross=a; cross[0]=2; cross[68]=1; CHECK(std::find(closure.begin(),closure.end(),cross)!=closure.end());   // blend word of g with the depth word of f
    CHECK(uc::uber_key_closure({}).empty());

    // Motion vectors are written only by draws that write depth (compare + update), so blended effects keep the vectors underneath.
    uc::PipelineKey z=a; z[68]=17|(3<<1);
    CHECK(uc::motion_write_mask(z)==(VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT));
    z[68]=16; CHECK(uc::motion_write_mask(z)==0);   // update without compare: Vulkan writes no depth either
    z[68]=1; CHECK(uc::motion_write_mask(z)==0);
    z[68]=5; CHECK(uc::motion_write_mask(z)==0);    // destination-alpha second pass
    // The G-buffer follows the same rule: only draws that write depth define a surface.
    z[68]=17|(3<<1); CHECK(uc::gbuffer_write_mask(z)==(VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT));
    z[68]=16; CHECK(uc::gbuffer_write_mask(z)==0);
    z[68]=1; CHECK(uc::gbuffer_write_mask(z)==0);
    CHECK(uc::valid_pipeline_key(a,false));
    auto bad=a; bad[69]=0; CHECK(!uc::valid_pipeline_key(bad,false));
    bad=a; bad[1]=17; CHECK(!uc::valid_pipeline_key(bad,false));
    bad=a; bad[68]=32; CHECK(!uc::valid_pipeline_key(bad,false));
    bad=a; bad[0]=2; CHECK(!uc::valid_pipeline_key(bad,false) && uc::valid_pipeline_key(bad,true));
}
