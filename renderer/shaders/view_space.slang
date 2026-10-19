// View-space position of a pixel from its NDC xy and depth (renderer/view_reconstruct.h is the CPU mirror), for a GX perspective projection.
struct ViewParams { float xScale; float yScale; float xOffset; float yOffset; float depthA; float depthB; };
float3 reconstructView(ViewParams p, float2 ndc, float depth) {
    float z = -p.depthB / (depth + p.depthA);
    return float3((ndc.x + p.xOffset) * -z / p.xScale, (ndc.y + p.yOffset) * -z / p.yScale, z);
}
