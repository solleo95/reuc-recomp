// Path tracing of the scene's lighting (docs/superpowers/plans/2026-10-20-path-tracing-spike.md; renderer/rt_path_math.h is the CPU side of the rules).
// accumulateMain: the mean albedo and the mean shown colour of every material record from a sparse grid of the albedo target and the scene colour (what a hit sees of a
// surface; the game's textures are not reachable from a ray). traceMain: per signal pixel of a lit surface the light gathered by cosine-weighted paths of several bounces
// through the hit table: a surface the game shows bright (a lamp, a lit panel, a window) is an emitter with that colour, a bounce multiplies by the hit's mean albedo, a ray
// that leaves the scene (or the last bounce) adds the dome; plus the synthetic overhead light with a short shadow ray. compositeMain: the denoised light times the pixel's
// scales the game's own lit colour (blended by `mix`, kept under fog; the light sources themselves keep their colour).
#include "gbuffer.slang"
#include "view_space.slang"
struct Params {
    float4 viewport;      // the scene viewport in scene-target pixels: x, y, width, height
    float4 viewA;         // xScale, yScale, xOffset, yOffset
    float2 viewB;         // depthA, depthB
    float2 depthRange;    // the viewport's near and far depth
    uint2 region;         // scene region size in pixels
    uint2 signalSize;     // size of the signal image
    uint divisorFrame;    // divisor | noise frame << 8 (the signal pixel covers divisor x divisor scene pixels)
    uint counts;          // paths per signal pixel | bounces << 8 (surface hits a path follows) | ao rays << 16; renderer/rt_path_math.h rt_path_pack
    float aoRadius;       // the contact occlusion's rays: their length in view units
    float aoStrength;     // and how dark a fully blocked surface gets
    float reach;         // a ray's length in view units
    float emission;       // an emitter's radiance relative to its albedo
    float ambient;        // the dome
    float key;            // the overhead light's strength (0: none)
    float4 overhead;      // xyz: unit direction toward the overhead light (view space); w: the reach of its shadow rays
    float mixAmount;      // 1: the path-traced lighting replaces the game's
    float threshold;      // a surface whose mean shown colour is brighter than this shines
    uint recordCount;     // records of the frame
    float clampRadiance;  // the most one path may add per channel
};
#ifdef UC_DIRECTX
ConstantBuffer<Params> params : register(b0);
#else
[[vk::push_constant]] ConstantBuffer<Params> params;
#endif
static uint pathsOf() { return params.counts & 255u; }
static uint bouncesOf() { return (params.counts >> 8) & 255u; }
static uint aoRaysOf() { return (params.counts >> 16) & 255u; }
static uint lightsOf() { return params.counts >> 24; }
static uint divisorOf(){ return params.divisorFrame & 255u; }
static uint frameOf() { return params.divisorFrame >> 8; }
struct Record { float4 materialColor; float4 ambientColor; uint lightMask; uint lightsOffset; uint flags; float roughness; };   // renderer/rt_material.h RtMaterialRecord
#ifdef UC_DIRECTX
// Descriptor index and first word replace Vulkan device pointers; the remaining hit record stays shared.
struct HitGeometry { uint2 positions; uint2 indices; uint record; uint strideWords; uint flags; uint padding; };
ByteAddressBuffer hitBuffers[] : register(t0, space1);
RaytracingAccelerationStructure scene : register(t0);
Texture2D<float> sceneDepth : register(t1);
Texture2D<float4> sceneGBuffer : register(t2);
Texture2D<uint> sceneMaterial : register(t3);
StructuredBuffer<Record> records : register(t4);
Texture2D<float4> signalIn : register(t6);
Texture2D<float4> colorRead : register(t7);
Texture2D<float4> sceneAlbedo : register(t8);
StructuredBuffer<HitGeometry> hitTable : register(t9);
StructuredBuffer<uint> hitBases : register(t10);
RWTexture2D<float4> signalOut : register(u0);
RWTexture2D<float4> sceneColor : register(u1);
RWStructuredBuffer<uint> albedoSums : register(u2);
RWStructuredBuffer<uint> lightList : register(u3);
#else
struct HitGeometry { uint* positions; uint* indices; uint record; uint strideWords; uint flags; uint padding; };           // renderer/rt_scene.h RtHitGeometry
[[vk::binding(0, 0)]] RaytracingAccelerationStructure scene;
[[vk::binding(1, 0)]] Texture2D<float> sceneDepth;
[[vk::binding(2, 0)]] Texture2D<float4> sceneGBuffer;
[[vk::binding(3, 0)]] Texture2D<uint> sceneMaterial;
[[vk::binding(4, 0)]] StructuredBuffer<Record> records;
[[vk::binding(5, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> signalOut;
[[vk::binding(6, 0)]] Texture2D<float4> signalIn;
[[vk::binding(7, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> sceneColor;
[[vk::binding(8, 0)]] Texture2D<float4> sceneAlbedo;
[[vk::binding(9, 0)]] StructuredBuffer<HitGeometry> hitTable;
[[vk::binding(10, 0)]] StructuredBuffer<uint> hitBases;
[[vk::binding(11, 0)]] RWStructuredBuffer<uint> albedoSums;   // 12 words per record: the sums of the albedo's 8-bit channels (0-2), the sample count (3), the shown colour (4-6) and the view position x 16 (7-9, signed)
[[vk::binding(12, 0)]] RWStructuredBuffer<uint> lightList;     // word 0: the number of emitters found; light i from word 4 + 8 i: position xyz, area, radiance rgb, weight (floats as bits)
#define colorRead sceneColor
#endif
static const uint kStride = 12, kMaxLights = 32;
static const float kKeyShadow = 0.65;   // rt_shadow_math.h kRtOverheadStrength
static const float kLightCone = 1.0, kKeyCone = 0.06, kGridStep = 4.0, kPositionScale = 16.0;   // rt_path_math.h: the emitter's soft shadow, the overhead light's (rt_shadow_math.h), the accumulation grid

ViewParams viewParams() { ViewParams v; v.xScale = params.viewA.x; v.yScale = params.viewA.y; v.xOffset = params.viewA.z; v.yOffset = params.viewA.w; v.depthA = params.viewB.x; v.depthB = params.viewB.y; return v; }
float3 viewPositionAt(int2 pixel, float stored) {
    float2 ndc = ((float2(pixel) + 0.5) - params.viewport.xy) / params.viewport.zw * 2.0 - 1.0;
    return reconstructView(viewParams(), ndc, (stored - params.depthRange.x) / (params.depthRange.y - params.depthRange.x));
}
bool litSurface(float4 texel) {
    const uint flags = uint(round(texel.w * 65535.0));
    return (flags & kGBufferHasNormal) != 0 && (flags & kGBufferCutout) == 0 && (flags & kGBufferLit) != 0;
}
static const float kReference = 0.45, kMinScale = 0.25, kMaxScale = 1.8;   // rt_path_math.h: the light that changes nothing, and the range of the scale
static const float3 kWarm = float3(1.0, 0.86, 0.66);   // rt_path_math.h kRtPathWarm: the colour of the dome and of the overhead light
float luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
uint pcg(uint v) { const uint state = v * 747796405u + 2891336453u; const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u; return (word >> 22u) ^ word; }
float nextRandom(inout uint state) { state = pcg(state); return float(state >> 8) * (1.0 / 16777216.0); }
float3 cosineDirection(float3 normal, float2 u) {   // rt_ao_math.h rt_ao_direction
    const float radius = sqrt(u.x), angle = 6.2831853 * u.y;
    const float3 local = float3(radius * cos(angle), radius * sin(angle), sqrt(max(1.0 - u.x, 0.0)));
    const float sgn = normal.z >= 0.0 ? 1.0 : -1.0, a = -1.0 / (sgn + normal.z), b = normal.x * normal.y * a;
    const float3 tangent = float3(1.0 + sgn * normal.x * normal.x * a, sgn * b, -sgn * normal.x), bitangent = float3(b, sgn + normal.y * normal.y * a, -normal.y);
    return normalize(tangent * local.x + bitangent * local.y + normal * local.z);
}
float3 jitterDirection(float3 d, float radius, float2 u) {   // within a cone of the given half angle around d
    const float3 helper = abs(d.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    const float3 t = normalize(cross(helper, d)), b = cross(d, t);
    const float r = radius * sqrt(u.x), a = 6.2831853 * u.y;
    return normalize(d + t * (r * cos(a)) + b * (r * sin(a)));
}
float3 meanAlbedo(uint record) {   // rt_path_math.h rt_path_mean_albedo
    const uint count = albedoSums[record * 12 + 3];
    return count > 0 ? float3(albedoSums[record * 12], albedoSums[record * 12 + 1], albedoSums[record * 12 + 2]) / (255.0 * float(count)) : float3(0.4, 0.4, 0.4);
}
float3 meanColour(uint record) {   // the record's mean colour as the game drew it (lighting, fog and all)
    const uint count = albedoSums[record * 12 + 3];
    return count > 0 ? float3(albedoSums[record * 12 + 4], albedoSums[record * 12 + 5], albedoSums[record * 12 + 6]) / (255.0 * float(count)) : float3(0.0, 0.0, 0.0);
}
bool emits(float3 colour) { return luminance(colour) > params.threshold; }   // rt_path_math.h rt_path_emits: a bright surface is a light source
bool occluded(float3 origin, float3 direction, float reach) {
    RayDesc ray; ray.Origin = origin; ray.Direction = direction; ray.TMin = 1e-3 * length(origin) + 1e-3; ray.TMax = reach;
    RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(scene, 0, 3u, ray);
    query.Proceed();
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}
// The geometric normal of the triangle a ray hit, in view space (the object-to-world rows bring a cached mesh from object space)
float3 hitNormal(HitGeometry geometry, uint primitive, float3x4 toView) {
#ifdef UC_DIRECTX
    const uint3 triangle = hitBuffers[NonUniformResourceIndex(geometry.indices.x)].Load3((geometry.indices.y + 3 * primitive) * 4) * geometry.strideWords;
#else
    uint* indices = geometry.indices;
    uint* positions = geometry.positions;
    const uint3 triangle = uint3(indices[3 * primitive], indices[3 * primitive + 1], indices[3 * primitive + 2]) * geometry.strideWords;
#endif
    float3 corner[3];
    for (int i = 0; i < 3; ++i) {
        const uint at = triangle[i];
#ifdef UC_DIRECTX
        corner[i] = mul(toView, float4(asfloat(hitBuffers[NonUniformResourceIndex(geometry.positions.x)].Load3((geometry.positions.y + at) * 4)), 1.0));
#else
        corner[i] = mul(toView, float4(asfloat(positions[at]), asfloat(positions[at + 1]), asfloat(positions[at + 2]), 1.0));
#endif
    }
    const float3 normal = cross(corner[1] - corner[0], corner[2] - corner[0]);
    const float length2 = dot(normal, normal);
    return length2 > 1e-20 ? normal * rsqrt(length2) : float3(0.0, 0.0, 0.0);   // a degenerate triangle has no normal: the caller turns back
}

[shader("compute")] [numthreads(8, 8, 1)]
void accumulateMain(uint3 id : SV_DispatchThreadID) {   // a grid with a step of 4 pixels
    const int2 pixel = int2(id.xy) * 4 + 2;
    if (any(pixel >= int2(params.region))) return;
    if (sceneDepth.Load(int3(pixel, 0)) >= 1.0) return;
    const uint record = sceneMaterial.Load(int3(pixel, 0));
    if (record == 0xFFFFFFFFu || record >= params.recordCount) return;
    const uint3 rgb = uint3(saturate(sceneAlbedo.Load(int3(pixel, 0)).rgb) * 255.0 + 0.5);
    const uint3 shown = uint3(saturate(colorRead[uint2(pixel)].rgb) * 255.0 + 0.5);
    InterlockedAdd(albedoSums[record * 12], rgb.x);
    InterlockedAdd(albedoSums[record * 12 + 1], rgb.y);
    InterlockedAdd(albedoSums[record * 12 + 2], rgb.z);
    InterlockedAdd(albedoSums[record * 12 + 3], 1u);
    InterlockedAdd(albedoSums[record * 12 + 4], shown.x);
    InterlockedAdd(albedoSums[record * 12 + 5], shown.y);
    InterlockedAdd(albedoSums[record * 12 + 6], shown.z);
    const int3 position = int3(round(viewPositionAt(pixel, sceneDepth.Load(int3(pixel, 0))) * kPositionScale));   // signed, wrapping in the unsigned atomics
    InterlockedAdd(albedoSums[record * 12 + 7], asuint(position.x));
    InterlockedAdd(albedoSums[record * 12 + 8], asuint(position.y));
    InterlockedAdd(albedoSums[record * 12 + 9], asuint(position.z));
}

// The emitters: every record whose mean shown colour is bright becomes a light at the mean of its view positions with the area its samples cover (rt_path_math.h
// rt_path_emitter_area), radiance = shown colour x emission, weight = luminance x area. One group walks all records.
[shader("compute")] [numthreads(64, 1, 1)]
void listMain(uint3 id : SV_DispatchThreadID) {
    for (uint record = id.x; record < params.recordCount; record += 64) {
        const uint count = albedoSums[record * kStride + 3];
        if (count < 2) continue;
        const float3 colour = meanColour(record);
        if (!emits(colour)) continue;
        uint slot;
        InterlockedAdd(lightList[0], 1u, slot);
        if (slot >= kMaxLights) continue;
        const float3 position = float3(asint(albedoSums[record * kStride + 7]), asint(albedoSums[record * kStride + 8]), asint(albedoSums[record * kStride + 9])) / (kPositionScale * float(count));
        const float pixelSize = 2.0 * abs(position.z) / (params.viewA.x * params.viewport.z);
        const float area = float(count) * kGridStep * kGridStep * pixelSize * pixelSize;
        const float3 radiance = colour * params.emission;
        const uint at = 4 + 8 * slot;
        lightList[at] = asuint(position.x); lightList[at + 1] = asuint(position.y); lightList[at + 2] = asuint(position.z); lightList[at + 3] = asuint(area);
        lightList[at + 4] = asuint(radiance.x); lightList[at + 5] = asuint(radiance.y); lightList[at + 6] = asuint(radiance.z); lightList[at + 7] = asuint(luminance(radiance) * area);
    }
}

[shader("compute")] [numthreads(8, 8, 1)]
void traceMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.signalSize)) return;
    const int2 pixel = int2(min(id.xy * divisorOf() + divisorOf() / 2, params.region - 1));
    const float4 texel = sceneGBuffer.Load(int3(pixel, 0));
    const float stored = sceneDepth.Load(int3(pixel, 0));
    float3 light = float3(0.0, 0.0, 0.0);
    if (litSurface(texel) && stored < 1.0) {
        const float3 position = viewPositionAt(pixel, stored);
        const float3 normal = decodeGBufferNormal(texel.xy);
        const float eps = min(6e-3 * length(position) + 6e-3, 0.1);
        const float3 start = position + normal * eps;
        uint seed = pcg(uint(pixel.x) + 65536u * uint(pixel.y)) ^ pcg(frameOf() * 9781u + 17u);
        float3 sum = float3(0.0, 0.0, 0.0);
        const uint lightCount = min(min(lightList[0], kMaxLights), lightsOf());
        for (uint p = 0; p < pathsOf(); ++p) {
            float3 origin = start, direction = cosineDirection(normal, float2(nextRandom(seed), nextRandom(seed)));
            float3 throughput = float3(1.0, 1.0, 1.0), radiance = float3(0.0, 0.0, 0.0);
            for (uint b = 0; b < bouncesOf(); ++b) {
                RayDesc ray; ray.Origin = origin; ray.Direction = direction; ray.TMin = 1e-3 * length(origin) + 1e-3; ray.TMax = params.reach;
                RayQuery<RAY_FLAG_CULL_NON_OPAQUE> query;
                query.TraceRayInline(scene, 0, 3u, ray);
                query.Proceed();
                if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) { radiance += throughput * params.ambient * kWarm; break; }   // the dome
                const HitGeometry geometry = hitTable[hitBases[query.CommittedInstanceID()] + query.CommittedGeometryIndex()];
                float3 albedo = float3(0.4, 0.4, 0.4), shown = float3(0.0, 0.0, 0.0);
                if (geometry.record != 0xFFFFFFFFu && geometry.record < params.recordCount) { albedo = meanAlbedo(geometry.record); shown = meanColour(geometry.record); }
                if (emits(shown)) {   // the surface shines with the colour the game shows
                    if (b > 0 || lightCount == 0) radiance += throughput * shown * params.emission;   // the first hit is the direct light, sampled below with its shadow ray
                    break;
                }
                throughput *= albedo;
                if (b + 1 == bouncesOf()) { radiance += throughput * params.ambient * kWarm; break; }   // out of bounces: the dome stands for the rest
                const float t = query.CommittedRayT();
                float3 hitNormalView = hitNormal(geometry, query.CommittedPrimitiveIndex(), query.CommittedObjectToWorld3x4());
                if (dot(hitNormalView, hitNormalView) == 0.0) hitNormalView = -direction;   // a degenerate triangle would send NaN rays: bounce straight back instead
                if (dot(hitNormalView, direction) > 0.0) hitNormalView = -hitNormalView;
                const float3 hit = origin + direction * t;
                origin = hit + hitNormalView * min(6e-3 * length(hit) + 6e-3, 0.1);
                direction = cosineDirection(hitNormalView, float2(nextRandom(seed), nextRandom(seed)));
            }
            sum += min(radiance, params.clampRadiance);   // a path that reaches an emitter in one lucky ray would show as a firefly
        }
        light = sum / float(max(pathsOf(), 1u));
        if (aoRaysOf() > 0 && params.aoStrength > 0.0) {   // contact occlusion: short cosine rays around the surface darken the fill (rt_path_math.h rt_path_ao); the overhead light below has its own shadow ray
            uint blocked = 0;
            for (uint r = 0; r < aoRaysOf(); ++r)
                if (occluded(start, cosineDirection(normal, float2(nextRandom(seed), nextRandom(seed))), params.aoRadius)) ++blocked;
            light *= 1.0 - saturate(params.aoStrength) * float(blocked) / float(aoRaysOf());
        }
        if (lightCount > 0) {   // direct light from the emitters: one is picked by weight and its shadow ray aims within its radius (rt_path_math.h rt_path_nee)
            float total = 0.0;
            for (uint i = 0; i < lightCount; ++i) total += asfloat(lightList[4 + 8 * i + 7]);
            float pick = nextRandom(seed) * total;
            uint chosen = lightCount - 1;
            for (uint j = 0; j < lightCount; ++j) { pick -= asfloat(lightList[4 + 8 * j + 7]); if (pick <= 0.0) { chosen = j; break; } }
            const uint at = 4 + 8 * chosen;
            const float3 centre = float3(asfloat(lightList[at]), asfloat(lightList[at + 1]), asfloat(lightList[at + 2]));
            const float area = asfloat(lightList[at + 3]), weight = asfloat(lightList[at + 7]);
            const float3 radiance = float3(asfloat(lightList[at + 4]), asfloat(lightList[at + 5]), asfloat(lightList[at + 6]));
            const float radius = sqrt(area / 3.14159265);
            float3 toLight = centre - start;
            const float3 side = normalize(cross(toLight, abs(toLight.y) < 0.99 * length(toLight) ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0)));
            const float3 up = cross(normalize(toLight), side);
            const float2 disk = float2(nextRandom(seed), nextRandom(seed));
            toLight += (side * cos(6.2831853 * disk.y) + up * sin(6.2831853 * disk.y)) * (radius * kLightCone * sqrt(disk.x));
            const float d2 = dot(toLight, toLight), dist = sqrt(d2);
            const float3 wi = toLight / max(dist, 1e-4);
            const float cosSurface = dot(normal, wi);
            if (cosSurface > 0.0 && weight > 0.0 && !occluded(start, wi, max(dist * 0.95 - 0.1, 0.05))) {
                const float3 nee = radiance * area * cosSurface / (3.14159265 * max(d2, area / 3.14159265));   // cos at the light taken as 1
                light += min(nee * (total / weight), params.clampRadiance * 2.0);   // divided by the pick's probability weight / total
            }
        }
        if (params.key > 0.0) {   // the synthetic overhead light: a soft shadow ray, the surfaces that face it
            const float facing = saturate(dot(normal, params.overhead.xyz));
            const float3 keyAim = jitterDirection(params.overhead.xyz, kKeyCone, float2(nextRandom(seed), nextRandom(seed)));
            if (facing > 0.0) {
                if (occluded(start, keyAim, params.overhead.w)) light *= 1.0 - kKeyShadow * smoothstep(0.05, 0.3, facing);   // rt_shadow_math.h rt_overhead_factor: the shadow darkens what faces the light, as the RT shadow pass does
                else light += params.key * facing * kWarm;
            }
        }
    }
    if (any(isnan(light)) || any(isinf(light))) light = float3(kReference, kReference, kReference);   // a NaN would stay in the denoiser's history for good
    signalOut[id.xy] = float4(light, 1.0);
}

[shader("compute")] [numthreads(8, 8, 1)]
void compositeMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.region)) return;
    const int2 pixel = int2(id.xy);
    const float4 texel = sceneGBuffer.Load(int3(pixel, 0));
    const float stored = sceneDepth.Load(int3(pixel, 0));
    if (!litSurface(texel) || stored >= 1.0) return;
    const uint record = sceneMaterial.Load(int3(pixel, 0));
    if (record != 0xFFFFFFFFu && record < params.recordCount && emits(meanColour(record))) return;   // a light source keeps the colour the game draws
    const float centreDepth = abs(viewPositionAt(pixel, stored).z);
    // a depth-aware bilinear upsample of the denoised light (as rt_reflect.slang)
    const float d = float(divisorOf());
    const float2 at = (float2(pixel) + 0.5 - float(divisorOf() / 2) - 0.5) / d;
    const int2 base = int2(floor(at));
    const float2 f = at - float2(base);
    float3 sum = 0.0; float weights = 0.0;
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
        const int2 tap = clamp(base + int2(x, y), int2(0, 0), int2(params.signalSize) - 1);
        const int2 source = int2(min(uint2(tap) * divisorOf() + divisorOf() / 2, params.region - 1));
        const float tapStored = sceneDepth.Load(int3(source, 0));
        const float tapDepth = tapStored < 1.0 ? abs(viewPositionAt(source, tapStored).z) : 1e6;
        const float w = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y) / (1.0 + 50.0 * abs(tapDepth - centreDepth) / centreDepth);
        sum += signalIn.Load(int3(tap, 0)).rgb * w; weights += w;
    }
    float3 light = weights > 0.0 ? max(sum / weights, 0.0) : float3(kReference, kReference, kReference);
    if (any(isnan(light)) || any(isinf(light))) light = float3(kReference, kReference, kReference);   // a bad sample leaves the pixel as the game drew it
    float4 color = colorRead[id.xy];
    const float3 scale = clamp(light / kReference, kMinScale, kMaxScale);   // rt_path_math.h rt_path_compose: the game's colour keeps its textures
    const float3 gain = lerp(float3(1.0, 1.0, 1.0), scale, saturate(params.mixAmount) * (1.0 - texel.z));
    // Roll positive gain off toward white: multiplying the already lit SDR colour would clip its texture detail before bloom.
    color.rgb = color.rgb * gain / (1.0 + color.rgb * max(gain - 1.0, 0.0));
    sceneColor[id.xy] = color;
}
