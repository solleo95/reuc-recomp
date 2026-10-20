// Path tracing of the scene's lighting (docs/superpowers/plans/2026-10-20-path-tracing-spike.md; renderer/rt_path_math.h is the CPU side of the rules).
// accumulateMain: the mean albedo and the mean shown colour of every material record from a sparse grid of the albedo target and the scene colour (what a hit sees of a
// surface; the game's textures are not reachable from a ray). traceMain: per signal pixel of a lit surface the light gathered by cosine-weighted paths of several bounces
// through the hit table: a surface the game shows bright (a lamp, a lit panel, a window) is an emitter with that colour, a bounce multiplies by the hit's mean albedo, a ray
// that leaves the scene (or the last bounce) adds the dome; plus the synthetic overhead light with a short shadow ray. compositeMain: the denoised light times the pixel's
// albedo replaces the game's lit colour (blended by `mix`, kept under fog; the light sources themselves keep their colour).
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
    uint paths;           // paths per signal pixel
    uint bounces;         // surface hits a path follows
    float reach;          // a ray's length in view units
    float emission;       // an emitter's radiance relative to its albedo
    float ambient;        // the dome
    float key;            // the overhead light's strength (0: none)
    float4 overhead;      // xyz: unit direction toward the overhead light (view space); w: the reach of its shadow rays
    float mixAmount;      // 1: the path-traced lighting replaces the game's
    float threshold;      // a surface whose mean shown colour is brighter than this shines
    uint recordCount;     // records of the frame
    float clampRadiance;  // the most one path may add per channel
};
[[vk::push_constant]] ConstantBuffer<Params> params;
struct Record { float4 materialColor; float4 ambientColor; uint lightMask; uint lightsOffset; uint flags; float roughness; };   // renderer/rt_material.h RtMaterialRecord
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
[[vk::binding(11, 0)]] RWStructuredBuffer<uint> albedoSums;   // four words per record: the sums of the albedo's 8-bit channels and the pixel count

ViewParams viewParams() { ViewParams v; v.xScale = params.viewA.x; v.yScale = params.viewA.y; v.xOffset = params.viewA.z; v.yOffset = params.viewA.w; v.depthA = params.viewB.x; v.depthB = params.viewB.y; return v; }
float3 viewPositionAt(int2 pixel, float stored) {
    float2 ndc = ((float2(pixel) + 0.5) - params.viewport.xy) / params.viewport.zw * 2.0 - 1.0;
    return reconstructView(viewParams(), ndc, (stored - params.depthRange.x) / (params.depthRange.y - params.depthRange.x));
}
bool litSurface(float4 texel) {
    const uint flags = uint(round(texel.w * 65535.0));
    return (flags & kGBufferHasNormal) != 0 && (flags & kGBufferCutout) == 0 && (flags & kGBufferLit) != 0;
}
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
float3 meanAlbedo(uint record) {   // rt_path_math.h rt_path_mean_albedo
    const uint count = albedoSums[record * 8 + 3];
    return count > 0 ? float3(albedoSums[record * 8], albedoSums[record * 8 + 1], albedoSums[record * 8 + 2]) / (255.0 * float(count)) : float3(0.4, 0.4, 0.4);
}
float3 meanColour(uint record) {   // the record's mean colour as the game drew it (lighting, fog and all)
    const uint count = albedoSums[record * 8 + 3];
    return count > 0 ? float3(albedoSums[record * 8 + 4], albedoSums[record * 8 + 5], albedoSums[record * 8 + 6]) / (255.0 * float(count)) : float3(0.0, 0.0, 0.0);
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
    uint* indices = geometry.indices;
    uint* positions = geometry.positions;
    const uint3 triangle = uint3(indices[3 * primitive], indices[3 * primitive + 1], indices[3 * primitive + 2]) * geometry.strideWords;
    float3 corner[3];
    for (int i = 0; i < 3; ++i) {
        const uint at = triangle[i];
        corner[i] = mul(toView, float4(asfloat(positions[at]), asfloat(positions[at + 1]), asfloat(positions[at + 2]), 1.0));
    }
    return normalize(cross(corner[1] - corner[0], corner[2] - corner[0]));
}

[shader("compute")] [numthreads(8, 8, 1)]
void accumulateMain(uint3 id : SV_DispatchThreadID) {   // a grid with a step of 4 pixels
    const int2 pixel = int2(id.xy) * 4 + 2;
    if (any(pixel >= int2(params.region))) return;
    if (sceneDepth.Load(int3(pixel, 0)) >= 1.0) return;
    const uint record = sceneMaterial.Load(int3(pixel, 0));
    if (record == 0xFFFFFFFFu || record >= params.recordCount) return;
    const uint3 rgb = uint3(saturate(sceneAlbedo.Load(int3(pixel, 0)).rgb) * 255.0 + 0.5);
    const uint3 shown = uint3(saturate(sceneColor[uint2(pixel)].rgb) * 255.0 + 0.5);
    InterlockedAdd(albedoSums[record * 8], rgb.x);
    InterlockedAdd(albedoSums[record * 8 + 1], rgb.y);
    InterlockedAdd(albedoSums[record * 8 + 2], rgb.z);
    InterlockedAdd(albedoSums[record * 8 + 3], 1u);
    InterlockedAdd(albedoSums[record * 8 + 4], shown.x);
    InterlockedAdd(albedoSums[record * 8 + 5], shown.y);
    InterlockedAdd(albedoSums[record * 8 + 6], shown.z);
}

[shader("compute")] [numthreads(8, 8, 1)]
void traceMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.signalSize)) return;
    const int2 pixel = int2(min(id.xy * params.divisor + params.divisor / 2, params.region - 1));
    const float4 texel = sceneGBuffer.Load(int3(pixel, 0));
    const float stored = sceneDepth.Load(int3(pixel, 0));
    float3 light = float3(0.0, 0.0, 0.0);
    if (litSurface(texel) && stored < 1.0) {
        const float3 position = viewPositionAt(pixel, stored);
        const float3 normal = decodeGBufferNormal(texel.xy);
        const float eps = min(6e-3 * length(position) + 6e-3, 0.1);
        const float3 start = position + normal * eps;
        uint seed = pcg(uint(pixel.x) + 65536u * uint(pixel.y)) ^ pcg(params.frame * 9781u + 17u);
        float3 sum = float3(0.0, 0.0, 0.0);
        for (uint p = 0; p < params.paths; ++p) {
            float3 origin = start, direction = cosineDirection(normal, float2(nextRandom(seed), nextRandom(seed)));
            float3 throughput = float3(1.0, 1.0, 1.0), radiance = float3(0.0, 0.0, 0.0);
            for (uint b = 0; b < params.bounces; ++b) {
                RayDesc ray; ray.Origin = origin; ray.Direction = direction; ray.TMin = 1e-3 * length(origin) + 1e-3; ray.TMax = params.reach;
                RayQuery<RAY_FLAG_CULL_NON_OPAQUE> query;
                query.TraceRayInline(scene, 0, 3u, ray);
                query.Proceed();
                if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) { radiance += throughput * params.ambient * kWarm; break; }   // the dome
                const HitGeometry geometry = hitTable[hitBases[query.CommittedInstanceID()] + query.CommittedGeometryIndex()];
                float3 albedo = float3(0.4, 0.4, 0.4), shown = float3(0.0, 0.0, 0.0);
                if (geometry.record != 0xFFFFFFFFu && geometry.record < params.recordCount) { albedo = meanAlbedo(geometry.record); shown = meanColour(geometry.record); }
                if (emits(shown)) { radiance += throughput * shown * params.emission; break; }   // the surface shines with the colour the game shows
                throughput *= albedo;
                if (b + 1 == params.bounces) { radiance += throughput * params.ambient * kWarm; break; }   // out of bounces: the dome stands for the rest
                const float t = query.CommittedRayT();
                float3 hitNormalView = hitNormal(geometry, query.CommittedPrimitiveIndex(), query.CommittedObjectToWorld3x4());
                if (dot(hitNormalView, direction) > 0.0) hitNormalView = -hitNormalView;
                const float3 hit = origin + direction * t;
                origin = hit + hitNormalView * min(6e-3 * length(hit) + 6e-3, 0.1);
                direction = cosineDirection(hitNormalView, float2(nextRandom(seed), nextRandom(seed)));
            }
            sum += min(radiance, params.clampRadiance);   // a path that reaches an emitter in one lucky ray would show as a firefly
        }
        light = sum / float(max(params.paths, 1u));
        if (params.key > 0.0) {   // the synthetic overhead light: a short shadow ray, the surfaces that face it
            const float facing = saturate(dot(normal, params.overhead.xyz));
            if (facing > 0.0 && !occluded(start, params.overhead.xyz, params.overhead.w)) light += params.key * facing * kWarm;
        }
    }
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
    const float3 light = weights > 0.0 ? max(sum / weights, 0.0) : float3(0.0, 0.0, 0.0);
    float4 color = sceneColor[id.xy];
    const float3 traced = min(sceneAlbedo.Load(int3(pixel, 0)).rgb * light, 1.0);   // rt_path_math.h rt_path_compose
    color.rgb = lerp(color.rgb, traced, saturate(params.mixAmount) * (1.0 - texel.z));
    sceneColor[id.xy] = color;
}
