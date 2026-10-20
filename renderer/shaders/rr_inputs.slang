// The guides DLSS Ray Reconstruction wants besides the colour, depth, motion and albedo the renderer already has (docs/superpowers/plans/2026-10-20-ray-tracing-p7-ray-reconstruction.md):
// the view-space normal with the roughness packed into its alpha, and the specular albedo. renderer/rr_math.h is the CPU mirror.
#include "gbuffer.slang"
#include "view_space.slang"
struct Params {
    float4 viewport;      // the scene viewport in scene-target pixels: x, y, width, height
    float4 viewA;         // xScale, yScale, xOffset, yOffset
    float2 viewB;         // depthA, depthB
    float2 depthRange;    // the viewport's near and far depth
    uint2 region;         // scene region size in pixels
    uint2 padding;
};
[[vk::push_constant]] ConstantBuffer<Params> params;
struct Record { float4 materialColor; float4 ambientColor; uint lightMask; uint lightsOffset; uint flags; float roughness; };   // renderer/rt_material.h RtMaterialRecord
[[vk::binding(0, 0)]] Texture2D<float> sceneDepth;
[[vk::binding(1, 0)]] Texture2D<float4> sceneGBuffer;
[[vk::binding(2, 0)]] Texture2D<uint> sceneMaterial;
[[vk::binding(3, 0)]] StructuredBuffer<Record> records;
[[vk::binding(4, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> normalRoughness;
[[vk::binding(5, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> specularAlbedo;

ViewParams viewParams() { ViewParams v; v.xScale = params.viewA.x; v.yScale = params.viewA.y; v.xOffset = params.viewA.z; v.yOffset = params.viewA.w; v.depthA = params.viewB.x; v.depthB = params.viewB.y; return v; }
float3 viewPositionAt(int2 pixel, float stored) {
    float2 ndc = ((float2(pixel) + 0.5) - params.viewport.xy) / params.viewport.zw * 2.0 - 1.0;
    return reconstructView(viewParams(), ndc, (stored - params.depthRange.x) / (params.depthRange.y - params.depthRange.x));
}
// rr_math.h rr_specular_albedo: the guide's EnvBRDFApprox2 for a grey specular colour
float envBrdf(float specularColor, float roughness, float nDotV) {
    const float nov = abs(nDotV), alpha = roughness * roughness;
    const float4 x = float4(1.0, nov, nov * nov, nov * nov * nov), y = float4(1.0, alpha, alpha * alpha, alpha * alpha * alpha);
    const float2x2 m1 = float2x2(0.99044, -1.28514, 1.29678, -0.755907);
    const float3x3 m2 = float3x3(1.0, 2.92338, 59.4188, 20.3225, -27.0302, 222.592, 121.563, 626.13, 316.627);
    const float2x2 m3 = float2x2(0.0365463, 3.32707, 9.0632, -9.04756);
    const float3x3 m4 = float3x3(1.0, 3.59685, -1.36772, 9.04401, -16.3174, 9.22949, 5.56589, 19.7886, -20.2123);
    float bias = dot(mul(m1, x.xy), y.xy) / dot(mul(m2, x.xyw), y.xyw);
    const float scale = dot(mul(m3, x.xy), y.xy) / dot(mul(m4, x.xzw), y.xyw);
    bias *= saturate(specularColor * 50.0);
    return specularColor * max(0.0, scale) + max(0.0, bias);
}

[shader("compute")] [numthreads(8, 8, 1)]
void inputsMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.region)) return;
    const int2 pixel = int2(id.xy);
    const float4 texel = sceneGBuffer.Load(int3(pixel, 0));
    const float stored = sceneDepth.Load(int3(pixel, 0));
    const uint flags = uint(round(texel.w * 65535.0));
    if ((flags & kGBufferHasNormal) == 0 || stored >= 1.0) {   // the sky, the HUD's clear: no surface
        normalRoughness[id.xy] = float4(0.0, 0.0, 1.0, 1.0);
        specularAlbedo[id.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    const uint index = sceneMaterial.Load(int3(pixel, 0));
    const float reflectivity = index == 0xFFFFFFFFu ? 0.0 : float((records[index].flags >> 12) & 255u) / 255.0;   // rt_reflect_math.h rt_reflect_unpack
    const float roughness = 1.0 - 0.7 * saturate(reflectivity * 2.0);                                              // rr_math.h rr_roughness
    const float3 normal = decodeGBufferNormal(texel.xy);
    const float3 view = normalize(viewPositionAt(pixel, stored));
    const float specularColor = lerp(0.04, 0.5, saturate(reflectivity * 2.0));
    normalRoughness[id.xy] = float4(normal, roughness);
    const float s = envBrdf(specularColor, roughness, dot(normal, -view));
    specularAlbedo[id.xy] = float4(s, s, s, 1.0);
}
