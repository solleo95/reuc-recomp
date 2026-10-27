// SMAA 1x (Jimenez et al., MIT licence, third_party/smaa) as three compute passes over a region-sized copy of the scene:
// luma edge detection, blending weight calculation, neighbourhood blending into the output. Preset HIGH.
// -DSMAA_SOURCE_PROBE builds all three entry points for a quick compile check.
#define SMAA_CUSTOM_SL 1   // the library's HLSL 4.1 mapping declares its own samplers; ours live in the descriptor set
#define SMAA_PRESET_HIGH 1
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)   // compute has no derivatives: every sample is explicit LOD 0
#define SMAASamplePoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define discard return float2(0.0, 0.0)   // the edge detectors' "no edge" exit; a compute pass writes the zero instead of discarding the fragment
struct Metrics { float4 rt; };   // 1/width, 1/height, width, height of the region
[[vk::push_constant]] ConstantBuffer<Metrics> pushed : register(b0);
#define SMAA_RT_METRICS pushed.rt
[[vk::binding(0, 0)]] SamplerState LinearSampler : register(s0);
[[vk::binding(1, 0)]] SamplerState PointSampler : register(s1);
[[vk::binding(2, 0)]] Texture2D colorTex : register(t0);
[[vk::binding(3, 0)]] Texture2D edgesTex : register(t1);
[[vk::binding(4, 0)]] Texture2D blendTex : register(t2);
[[vk::binding(5, 0)]] Texture2D areaTex : register(t3);
[[vk::binding(6, 0)]] Texture2D searchTex : register(t4);
[[vk::binding(7, 0)]] [[vk::image_format("rg8")]] RWTexture2D<float2> edgesOut : register(u0);
[[vk::binding(8, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> weightsOut : register(u1);
[[vk::binding(9, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> colorOut : register(u2);
#include "SMAA.hlsl"

[shader("compute")] [numthreads(8, 8, 1)]
void edgesMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= uint2(pushed.rt.zw))) return;
    float2 texcoord = (float2(id.xy) + 0.5) * pushed.rt.xy;
    float4 offset[3];
    SMAAEdgeDetectionVS(texcoord, offset);
    edgesOut[id.xy] = SMAALumaEdgeDetectionPS(texcoord, offset, colorTex);
}
[shader("compute")] [numthreads(8, 8, 1)]
void weightsMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= uint2(pushed.rt.zw))) return;
    float2 texcoord = (float2(id.xy) + 0.5) * pushed.rt.xy;
    float2 pixcoord;
    float4 offset[3];
    SMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);
    weightsOut[id.xy] = SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, edgesTex, areaTex, searchTex, float4(0, 0, 0, 0));
}
[shader("compute")] [numthreads(8, 8, 1)]
void blendMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= uint2(pushed.rt.zw))) return;
    float2 texcoord = (float2(id.xy) + 0.5) * pushed.rt.xy;
    float4 offset;
    SMAANeighborhoodBlendingVS(texcoord, offset);
    colorOut[id.xy] = SMAANeighborhoodBlendingPS(texcoord, offset, colorTex, blendTex);
}
