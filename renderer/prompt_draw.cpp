#include "gx_vertices.h"
#include <cmath>
namespace uc {
std::optional<gx::PromptDraw> prompt_from_quad(const gx::DrawPacket& draw,const PromptDescription& description) {
    if(draw.primitive!=0x80 || draw.vertices.size()!=4 || draw.indices.size()!=6)return {};
    gx::PromptDraw result{description};
    const auto viewport=draw.state.viewport.value_or(std::array<float,6>{0,0,640,480,0,1});
    for(float v:viewport)if(!std::isfinite(v))return {};
    if(viewport[2]<=0 || viewport[3]<=0)return {};
    if(draw.state.scissor) {
        const auto& s=*draw.state.scissor;
        if(s[2]<0 || s[3]<0)return {};
        result.clip={float(s[0]),float(s[1]),float(s[0])+s[2],float(s[1])+s[3]};
    }
    try {
        for(size_t i=0;i<4;++i) {
            const auto vertex=gx::gpu_vertex(draw.vertices[i],draw.transforms,draw.state.descriptors[0]!=gx::Mode::none,draw.callsite);
            std::array<float,4> view{0,0,0,1},clip{};
            for(size_t row=0;row<3;++row) {
                view[row]=vertex.position_rows[row][3];
                for(size_t col=0;col<3;++col)view[row]+=vertex.position_rows[row][col]*vertex.position[col];
            }
            for(size_t row=0;row<4;++row)for(size_t col=0;col<4;++col)clip[row]+=draw.transforms.projection[row][col]*view[col];
            if(!std::isfinite(clip[3]) || clip[3]<=0)return {};
            result.efb_corners[i*2]=viewport[0]+(clip[0]/clip[3]+1)*viewport[2]/2;
            result.efb_corners[i*2+1]=viewport[1]+(clip[1]/clip[3]+1)*viewport[3]/2;
            for(size_t lane=0;lane<2;++lane)if(!std::isfinite(result.efb_corners[i*2+lane]) || std::abs(result.efb_corners[i*2+lane])>16384)return {};
            result.alpha[i]=vertex.color[3]/255.f;
        }
    } catch(const gx::Error&) {return {};}
    return result;
}
}
