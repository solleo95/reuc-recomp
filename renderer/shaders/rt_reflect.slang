// Ray-traced reflections (docs/superpowers/plans/2026-10-20-ray-tracing-p5-reflections.md) and, as mode 1, ray-traced lighting (plan p6). traceMain: per signal pixel of a reflective surface one ray along the mirror
// direction (within a roughness cone); where it hits, the colour the camera itself sees at the hit's screen position (when the hit is on screen and not hidden) is the
// reflected radiance, and the signal is its DIFFERENCE from the pixel's own colour (zero where nothing is reflected: the denoiser's blending and the divisor's subsampling then leave the surface's detail alone). compositeMain: the denoised difference, weighted, is added
// into the scene colour by fresnel x reflectivity. renderer/rt_reflect_math.h is the CPU mirror.
#include "gbuffer.slang"
#include "view_space.slang"
struct Params {
    float4 viewport;      // the scene viewport in scene-target pixels: x, y, width, height
    float4 viewA;         // xScale, yScale, xOffset, yOffset
    float2 viewB;         // depthA, depthB
    float2 depthRange;    // the viewport's near and far depth
    uint2 region;         // scene region size in pixels
    uint2 signalSize;     // size of the signal image
    uint divisor;         // the signal pixel covers divisor x divisor scene pixels
    uint frame;           // noise frame
    uint rays;            // rays per pixel
    float cone;           // roughness cone half angle, radians (0: a mirror)
    float reach;          // the rays' length in view units
    float strength;       // overall weight of the effect (reflections: rt_reflect_math.h kRtReflectStrength; lighting: the preset's share of the bounce)
    uint mode;            // 0: reflections, 1: lighting (one diffuse bounce, plan p6: the same machinery with cosine-weighted rays, the signal is the mean hit colour)
    float padding;
};
#ifdef UC_DIRECTX
ConstantBuffer<Params> params : register(b0);
#else
[[vk::push_constant]] ConstantBuffer<Params> params;
#endif
struct Record { float4 materialColor; float4 ambientColor; uint lightMask; uint lightsOffset; uint flags; float roughness; };   // renderer/rt_material.h RtMaterialRecord
#ifdef UC_DIRECTX
RaytracingAccelerationStructure scene : register(t0);
Texture2D<float> sceneDepth : register(t1);
Texture2D<float4> sceneGBuffer : register(t2);
Texture2D<uint> sceneMaterial : register(t3);
StructuredBuffer<Record> records : register(t4);
Texture2D<float4> signalIn : register(t6);
Texture2D<float4> colorRead : register(t7);
Texture2D<float4> sceneAlbedo : register(t8);
RWTexture2D<float4> signalOut : register(u0);
RWTexture2D<float4> sceneColor : register(u1);
#else
[[vk::binding(0, 0)]] RaytracingAccelerationStructure scene;
[[vk::binding(1, 0)]] Texture2D<float> sceneDepth;
[[vk::binding(2, 0)]] Texture2D<float4> sceneGBuffer;
[[vk::binding(3, 0)]] Texture2D<uint> sceneMaterial;
[[vk::binding(4, 0)]] StructuredBuffer<Record> records;
[[vk::binding(5, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> signalOut;
[[vk::binding(6, 0)]] Texture2D<float4> signalIn;
[[vk::binding(7, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> sceneColor;
[[vk::binding(8, 0)]] Texture2D<float4> sceneAlbedo;   // the RT albedo target (stage-0 texture x material colour): the lighting composite's reflectance
#define colorRead sceneColor
#endif

static const float kFresnelF0 = 0.04;
ViewParams viewParams() { ViewParams v; v.xScale = params.viewA.x; v.yScale = params.viewA.y; v.xOffset = params.viewA.z; v.yOffset = params.viewA.w; v.depthA = params.viewB.x; v.depthB = params.viewB.y; return v; }
float3 viewPositionAt(int2 pixel, float stored) {
    float2 ndc = ((float2(pixel) + 0.5) - params.viewport.xy) / params.viewport.zw * 2.0 - 1.0;
    return reconstructView(viewParams(), ndc, (stored - params.depthRange.x) / (params.depthRange.y - params.depthRange.x));
}
// A GX-lit depth-writing surface with a normal that is not a cut-out
bool litSurface(float4 texel) {
    const uint flags = uint(round(texel.w * 65535.0));
    return (flags & kGBufferHasNormal) != 0 && (flags & kGBufferCutout) == 0 && (flags & kGBufferLit) != 0;
}
float reflectivityOf(uint index) { return index == 0xFFFFFFFFu ? 0.0 : float((records[index].flags >> 12) & 255u) / 255.0; }   // rt_reflect_math.h rt_reflect_unpack
float fresnel(float nDotV) { const float m = 1.0 - saturate(nDotV); const float m2 = m * m; return kFresnelF0 + (1.0 - kFresnelF0) * m2 * m2 * m; }
float interleavedGradientNoise(float2 pixel, uint frame) {
    pixel += 5.588238 * float(frame);
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}
float3 jitter(float3 d, float radius, float2 u) {
    float3 helper = abs(d.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 t = normalize(cross(helper, d)), b = cross(d, t);
    float r = radius * sqrt(u.x), a = 6.2831853 * u.y;
    return normalize(d + t * (r * cos(a)) + b * (r * sin(a)));
}

// rt_ao_math.h rt_ao_direction: a cosine-weighted direction in the hemisphere of the unit normal (u in [0, 1))
float3 cosineDirection(float3 normal, float2 u) {
    const float radius = sqrt(u.x), angle = 6.2831853 * u.y;
    const float3 local = float3(radius * cos(angle), radius * sin(angle), sqrt(max(1.0 - u.x, 0.0)));
    const float sgn = normal.z >= 0.0 ? 1.0 : -1.0, a = -1.0 / (sgn + normal.z), b = normal.x * normal.y * a;
    const float3 tangent = float3(1.0 + sgn * normal.x * normal.x * a, sgn * b, -sgn * normal.x), bitangent = float3(b, sgn + normal.y * normal.y * a, -normal.y);
    return normalize(tangent * local.x + bitangent * local.y + normal * local.z);
}
// What one ray brings: the frame's colour at the hit (rgb) and how much it counts (a) when the hit is on screen and visible from the camera (faded toward the screen
// edge and with the distance); a = 0 otherwise (nothing).
float4 hitSample(float3 origin, float3 direction) {
    const float4 none = float4(0.0, 0.0, 0.0, 0.0);
    RayDesc ray; ray.Origin = origin; ray.Direction = direction; ray.TMin = 1e-3 * length(origin) + 1e-3; ray.TMax = params.reach;
    RayQuery<RAY_FLAG_CULL_NON_OPAQUE> query;
    query.TraceRayInline(scene, 0, 3u, ray);
    query.Proceed();
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return none;
    const float t = query.CommittedRayT();
    const float3 hit = origin + direction * t;
    if (!(hit.z < -1e-4)) return none;
    // the hit's screen position (rt_reflect_math.h rt_project) and the frame's own view of it
    const float2 ndc = float2(params.viewA.x * hit.x / -hit.z - params.viewA.z, params.viewA.y * hit.y / -hit.z - params.viewA.w);
    const float2 at = params.viewport.xy + (ndc * 0.5 + 0.5) * params.viewport.zw;
    const int2 hitPixel = int2(floor(at));
    if (any(hitPixel < int2(0, 0)) || any(hitPixel >= int2(params.region))) return none;
    const float hitStored = sceneDepth.Load(int3(hitPixel, 0));
    if (hitStored >= 1.0) return none;
    const float frameZ = abs(viewPositionAt(hitPixel, hitStored).z);
    if (abs(frameZ - abs(hit.z)) > 0.03 * abs(hit.z)) return none;   // hidden from the camera: the frame shows something else there
    const float2 edge = saturate(min(at - params.viewport.xy, params.viewport.xy + params.viewport.zw - at) / (0.08 * params.viewport.zw));   // fade out toward the screen edge
    const float confidence = edge.x * edge.y * (1.0 - (t / params.reach) * (t / params.reach));   // rt_light_math.h rt_hit_confidence
    return float4(colorRead[hitPixel].rgb, saturate(confidence));
}
// A reflection ray's contribution: the hit's colour as its difference from `own` (zero where nothing is reflected).
float3 reflectedDelta(float3 origin, float3 direction, float3 own) {
    const float4 s = hitSample(origin, direction);
    return (s.rgb - own) * s.a;
}

[shader("compute")] [numthreads(8, 8, 1)]
void traceMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.signalSize)) return;
    const int2 pixel = int2(min(id.xy * params.divisor + params.divisor / 2, params.region - 1));
    const float3 own = colorRead[pixel].rgb;
    float3 result = float3(0.0, 0.0, 0.0);   // no reflection: no difference; no bounce: no light
    const float4 texel = sceneGBuffer.Load(int3(pixel, 0));
    const float stored = sceneDepth.Load(int3(pixel, 0));
    const uint index = sceneMaterial.Load(int3(pixel, 0));
    if (params.mode == 1u) {   // lighting: the mean colour the surrounding surfaces send back (cosine-weighted rays)
        if (litSurface(texel) && stored < 1.0) {
            const float3 position = viewPositionAt(pixel, stored);
            const float3 normal = decodeGBufferNormal(texel.xy);
            const float3 origin = position + normal * min(6e-3 * length(position) + 6e-3, 0.1);
            float3 sum = 0.0;
            for (uint r = 0; r < params.rays; ++r) {
                const uint seed = params.frame * 7u + r * 3u + 31u;
                const float4 s = hitSample(origin, cosineDirection(normal, float2(interleavedGradientNoise(float2(pixel), seed), interleavedGradientNoise(float2(pixel.yx) + 83.0, seed))));
                sum += s.rgb * s.a;
            }
            result = sum / float(max(params.rays, 1u));
        }
    } else if (litSurface(texel) && stored < 1.0 && reflectivityOf(index) > 0.0) {
        const float3 position = viewPositionAt(pixel, stored);
        const float3 normal = decodeGBufferNormal(texel.xy);
        const float3 mirror = reflect(normalize(position), normal);
        const float3 origin = position + normal * min(6e-3 * length(position) + 6e-3, 0.1);
        float3 sum = 0.0;
        for (uint r = 0; r < params.rays; ++r) {
            float3 direction = mirror;
            if (params.cone > 0.0) {
                const uint seed = params.frame * 5u + r * 3u + 13u;
                direction = jitter(mirror, params.cone, float2(interleavedGradientNoise(float2(pixel), seed), interleavedGradientNoise(float2(pixel.yx) + 59.0, seed)));
            }
            sum += dot(direction, normal) > 0.02 ? reflectedDelta(origin, direction, own) : float3(0.0, 0.0, 0.0);   // a ray that would go into the surface brings nothing
        }
        result = sum / float(max(params.rays, 1u));
    }
    signalOut[id.xy] = float4(result, 1.0);
}

[shader("compute")] [numthreads(8, 8, 1)]
void compositeMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.region)) return;
    const int2 pixel = int2(id.xy);
    const float4 texel = sceneGBuffer.Load(int3(pixel, 0));
    const float stored = sceneDepth.Load(int3(pixel, 0));
    if (!litSurface(texel) || stored >= 1.0) return;
    const float reflectivity = params.mode == 1u ? 1.0 : reflectivityOf(sceneMaterial.Load(int3(pixel, 0)));
    if (reflectivity <= 0.0) return;
    const float3 position = viewPositionAt(pixel, stored);
    const float centreDepth = abs(position.z);
    // a depth-aware bilinear upsample of the denoised difference (as rt_shadow.slang)
    const float d = float(params.divisor);
    const float2 at = (float2(pixel) + 0.5 - float(params.divisor / 2) - 0.5) / d;
    const int2 base = int2(floor(at));
    const float2 f = at - float2(base);
    float3 sum = 0.0; float weights = 0.0;
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
        const int2 tap = clamp(base + int2(x, y), int2(0, 0), int2(params.signalSize) - 1);
        const int2 source = int2(min(uint2(tap) * params.divisor + params.divisor / 2, params.region - 1));
        const float tapStored = sceneDepth.Load(int3(source, 0));
        const float tapDepth = tapStored < 1.0 ? abs(viewPositionAt(source, tapStored).z) : 1e6;
        const float w = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y) / (1.0 + 50.0 * abs(tapDepth - centreDepth) / centreDepth);
        sum += signalIn.Load(int3(tap, 0)).rgb * w; weights += w;
    }
    const float3 delta = weights > 0.0 ? sum / weights : float3(0.0, 0.0, 0.0);
    if (params.mode == 1u) {   // lighting: albedo x the bounced colour x the preset's share, only ever adding light (rt_light_math.h rt_light_add)
        float4 lit = colorRead[id.xy];
        lit.rgb = min(lit.rgb + sceneAlbedo.Load(int3(pixel, 0)).rgb * max(delta, 0.0) * params.strength * (1.0 - texel.z), 1.0);
        sceneColor[id.xy] = lit;
        return;
    }
    const float3 normal = decodeGBufferNormal(texel.xy);
    const float weight = saturate(fresnel(dot(normal, -normalize(position))) * reflectivity * params.strength) * (1.0 - texel.z);   // faded by the GX fog
    float4 color = colorRead[id.xy];
    color.rgb = saturate(color.rgb + delta * weight);   // lerp(own, hit, w) = own + w (hit - own)
    sceneColor[id.xy] = color;
}
