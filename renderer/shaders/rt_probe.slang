// Ray tracing debug probe (UC_RT_PROBE=1, phase 0 only; removed when RT shadows land): one shadow ray per scene pixel from its reconstructed view
// position toward the dominant directional light, opaque geometry only. The result is a count of occluded pixels, nothing is drawn.
#include "view_space.slang"
struct Params {
    float4 viewport;     // the scene viewport in scene-target pixels: x, y, width, height
    float4 viewA;        // xScale, yScale, xOffset, yOffset
    float2 viewB;        // depthA, depthB
    float2 depthRange;   // the viewport's near and far depth
    float4 toLight;      // xyz: unit view-space direction to the light
    uint2 region;        // scene region size in pixels
    uint2 reserved;
};
[[vk::push_constant]] ConstantBuffer<Params> params;
[[vk::binding(0, 0)]] RaytracingAccelerationStructure scene;
[[vk::binding(1, 0)]] Texture2D<float> sceneDepth;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> occluded;
groupshared uint groupCount;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex) {
    if (index == 0) groupCount = 0;
    GroupMemoryBarrierWithGroupSync();
    const int2 pixel = int2(id.xy);
    if (all(id.xy < params.region)) {
        const float stored = sceneDepth.Load(int3(pixel, 0));
        if (stored < 1.0) {
            ViewParams v; v.xScale = params.viewA.x; v.yScale = params.viewA.y; v.xOffset = params.viewA.z; v.yOffset = params.viewA.w; v.depthA = params.viewB.x; v.depthB = params.viewB.y;
            const float2 ndc = ((float2(pixel) + 0.5) - params.viewport.xy) / params.viewport.zw * 2.0 - 1.0;
            const float3 position = reconstructView(v, ndc, (stored - params.depthRange.x) / (params.depthRange.y - params.depthRange.x));
            RayDesc ray;
            ray.Origin = position; ray.Direction = params.toLight.xyz;
            ray.TMin = 1e-3 * length(position) + 1e-3;   // past the surface the depth was reconstructed from (renderer_effects_smoke --rt-as uses the same offset)
            ray.TMax = 1e4;
            RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
            query.TraceRayInline(scene, 0, 0xFF, ray);
            query.Proceed();
            if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) InterlockedAdd(groupCount, 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (index == 0 && groupCount != 0) InterlockedAdd(occluded[0], groupCount);
}
