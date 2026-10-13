#include "gx_transforms.h"
#include "gx_vertices.h"
#include <cmath>

namespace gx {
Projection make_projection(const std::array<float,6>& p,uint32_t type) {
    if (type>1) throw Error(ErrorCode::layout,0x8028873C,1,type);
    for (float value : p) if (!std::isfinite(value)) throw Error(ErrorCode::layout,0x8028873C,6,0);
    Projection result{};
    result[0][0]=p[0]; result[1][1]=-p[2];
    if (type==1) {
        result[0][3]=p[1]; result[1][3]=-p[3];
        result[2]={0,0,p[4],p[5]+1}; result[3][3]=1;
    } else {
        result[0][2]=p[1]; result[1][2]=-p[3];
        result[2]={0,0,p[4]-1,p[5]}; result[3][2]=-1;
    }
    // GX clip depth -w..0 becomes Vulkan0..w; positive Vulkan viewport needs the Y flip above.
    return result;
}
void set_position_matrix(Transforms& transforms,uint32_t row,const PositionMatrix& matrix) {
    if (row>61) throw Error(ErrorCode::layout,0x8028882C,61,row);
    for (const auto& values : matrix) for (float value : values)
        if (!std::isfinite(value)) throw Error(ErrorCode::layout,0x8028882C,12,0);
    for (size_t i=0;i<3;++i) { transforms.position_rows[row+i]=matrix[i]; transforms.loaded_rows.set(row+i); }
}
void set_normal_matrix(Transforms& transforms,uint32_t row,const NormalMatrix& matrix) {
    if (row>61) throw Error(ErrorCode::layout,0x802888A8,61,row);
    for (const auto& values : matrix) for (float value : values)
        if (!std::isfinite(value)) throw Error(ErrorCode::layout,0x802888A8,9,0);
    for (size_t i=0;i<3;++i) { transforms.normal_rows[row+i]=matrix[i]; transforms.loaded_normal_rows.set(row+i); }
}
GpuVertex gpu_vertex(const Vertex& vertex,const Transforms& transforms,bool indexed,uint32_t callsite) {
    const uint32_t row=indexed ? vertex.position_matrix : transforms.current;
    if (row>61) throw Error(ErrorCode::layout,callsite,61,row);
    for (size_t i=0;i<3;++i) if (!transforms.loaded_rows[row+i]) throw Error(ErrorCode::layout,callsite,row+i,0);
    GpuVertex result{vertex.position,vertex.color[0],{transforms.position_rows[row],transforms.position_rows[row+1],transforms.position_rows[row+2]},vertex.texcoord[0]};
    std::copy(vertex.texcoord.begin()+1,vertex.texcoord.end(),result.extra_texcoords.begin());
    result.normal={vertex.normal[0],vertex.normal[1],vertex.normal[2]};
    const auto normal_row=row&31;
    // shortcut: unloaded normal rows use identity for position-only fixtures, revisit when actual missing-normal handling is verified.
    if (transforms.loaded_normal_rows[normal_row] && transforms.loaded_normal_rows[normal_row+1] && transforms.loaded_normal_rows[normal_row+2]) {
        for (size_t i=0;i<3;++i) {
            const auto& values=transforms.normal_rows[normal_row+i];
            result.normal[i]=values[0]*vertex.normal[0]+values[1]*vertex.normal[1]+values[2]*vertex.normal[2];
        }
    }
    result.color1=vertex.color[1];
    return result;
}
}
