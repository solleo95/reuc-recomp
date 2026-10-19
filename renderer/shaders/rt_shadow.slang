// Ray-traced shadows (docs/superpowers/plans/2026-10-19-ray-tracing-p2.md, Task 8/9). traceMain: per signal pixel one visibility factor from the pixel's own
// material record and its draw's GX lights, 1 - sum(share_i * (1 - visibility_i)) with the share of a light taken from the GX lighting equations
// (renderer/rt_shadow_math.h gx_light_contribution is the CPU mirror). compositeMain: the denoised factor multiplies the scene colour.
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
    uint frame;           // noise frame (0: a fixed pattern)
    uint sunRays;         // rays per directional light (0: directional lights are not traced)
    uint localLights;     // the strongest point/spot lights traced per pixel (0: none)
    float sunCone;        // half angle of the soft-shadow cone, radians (0: hard)
    float lightRadius;    // soft-shadow disc radius as a fraction of the distance to a local light (0: hard)
    float lampClearance;  // a lamp's ray ends this far before the lamp (its fixture would block every ray); a surface closer than that is not traced
    uint reserved;
};
[[vk::push_constant]] ConstantBuffer<Params> params;
struct Record { float4 materialColor; float4 ambientColor; uint lightMask; uint lightsOffset; uint flags; float roughness; };   // renderer/rt_material.h RtMaterialRecord
struct NativeLight { float4 color; float4 cosine; float4 distance; float4 position; float4 direction; };                          // gx::LightUniform
static const uint kRtModelValid = 1;
[[vk::binding(0, 0)]] RaytracingAccelerationStructure scene;
[[vk::binding(1, 0)]] Texture2D<float> sceneDepth;
[[vk::binding(2, 0)]] Texture2D<float4> sceneGBuffer;
[[vk::binding(3, 0)]] Texture2D<uint> sceneMaterial;
[[vk::binding(4, 0)]] StructuredBuffer<Record> records;
[[vk::binding(5, 0)]] StructuredBuffer<NativeLight> lights;
[[vk::binding(6, 0)]] [[vk::image_format("r16f")]] RWTexture2D<float> signalOut;
[[vk::binding(7, 0)]] Texture2D<float4> signalIn;
[[vk::binding(8, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> sceneColor;

ViewParams viewParams() { ViewParams v; v.xScale = params.viewA.x; v.yScale = params.viewA.y; v.xOffset = params.viewA.z; v.yOffset = params.viewA.w; v.depthA = params.viewB.x; v.depthB = params.viewB.y; return v; }
float3 viewPositionAt(int2 pixel, float stored) {
    float2 ndc = ((float2(pixel) + 0.5) - params.viewport.xy) / params.viewport.zw * 2.0 - 1.0;
    return reconstructView(viewParams(), ndc, (stored - params.depthRange.x) / (params.depthRange.y - params.depthRange.x));
}
// A pixel the light model describes: a lit, model-valid surface (not a cut-out).
bool shadedSurface(float4 texel) {
    const uint flags = uint(round(texel.w * 65535.0));
    return (flags & kGBufferHasNormal) != 0 && (flags & kGBufferCutout) == 0 && (flags & kGBufferLit) != 0 && (flags & kGBufferModelValid) != 0;
}
float interleavedGradientNoise(float2 pixel, uint frame) {
    pixel += 5.588238 * float(frame);
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}
// A direction within `radius` (a tangent-plane offset at unit distance) of d.
float3 jitter(float3 d, float radius, float2 u) {
    float3 helper = abs(d.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 t = normalize(cross(helper, d)), b = cross(d, t);
    float r = radius * sqrt(u.x), a = 6.2831853 * u.y;
    return normalize(d + t * (r * cos(a)) + b * (r * sin(a)));
}
// mask: the instance classes the ray sees (renderer/rt_scene.h kRtStatic = 1 level geometry, kRtSkinned = 2 characters)
bool occluded(float3 origin, float3 direction, float tMax, uint mask) {
    RayDesc ray;
    ray.Origin = origin; ray.Direction = direction;
    ray.TMin = 1e-3 * length(origin) + 1e-3;   // past the surface the depth was reconstructed from (as the phase 0 probe)
    ray.TMax = tMax;
    RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(scene, 0, mask, ray);
    query.Proceed();
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

[shader("compute")] [numthreads(8, 8, 1)]
void traceMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.signalSize)) return;
    const int2 pixel = int2(min(id.xy * params.divisor + params.divisor / 2, params.region - 1));
    float factor = 1.0;
    const float4 texel = sceneGBuffer.Load(int3(pixel, 0));
    const float stored = sceneDepth.Load(int3(pixel, 0));
    const uint index = sceneMaterial.Load(int3(pixel, 0));
    if (shadedSurface(texel) && stored < 1.0 && index != 0xFFFFFFFFu) {
        const Record record = records[index];
        const float3 position = viewPositionAt(pixel, stored);
        const float3 normal = decodeGBufferNormal(texel.xy);
        const uint attenuation = (record.flags >> 8) & 3, diffuse = (record.flags >> 10) & 3;
        float contribution[8]; float3 toLight[8]; float distanceTo[8]; bool directional[8];
        float total = dot(record.ambientColor.rgb, float3(0.299, 0.587, 0.114));
        for (uint i = 0; i < 8; ++i) {
            contribution[i] = 0.0; toLight[i] = float3(0.0, 0.0, 1.0); distanceTo[i] = 1e4; directional[i] = false;
            if ((record.lightMask & (1u << i)) == 0 || attenuation == 1) continue;   // the specular function is not a diffuse light
            const NativeLight light = lights[record.lightsOffset + i];
            const float3 delta = light.position.xyz - position;
            const float lengthSquared = dot(delta, delta), dist = sqrt(lengthSquared);
            const float3 direction = dist > 0.0 ? delta / dist : normal;
            float strength = 1.0;
            if (attenuation == 3) {
                const float angle = max(0.0, dot(direction, light.direction.xyz));
                const float denominator = dot(light.distance.xyz, float3(1.0, dist, lengthSquared));
                strength = denominator > 0.0 ? max(0.0, dot(light.cosine.xyz, float3(1.0, angle, angle * angle))) / denominator : 0.0;
            }
            if (diffuse != 0) { const float c = dot(direction, normal); strength *= diffuse == 2 ? max(0.0, c) : c; }
            contribution[i] = max(strength, 0.0) * dot(light.color.rgb, float3(0.299, 0.587, 0.114)) / 255.0;   // signed diffuse can go negative: that is no light to remove
            toLight[i] = direction; distanceTo[i] = dist;
            directional[i] = dot(light.position.xyz, light.position.xyz) > 1e10;
            total += contribution[i];
        }
        if (total > 0.0) {
            const float3 origin = position + normal * (1e-3 * length(position) + 1e-3);
            float removed = 0.0;
            bool picked[8];
            for (uint i = 0; i < 8; ++i) picked[i] = false;
            for (uint n = 0; n < params.localLights; ++n) {   // the strongest local lights
                int best = -1; float bestValue = 0.0;
                for (uint i = 0; i < 8; ++i) if (!directional[i] && !picked[i] && contribution[i] > bestValue) { best = int(i); bestValue = contribution[i]; }
                if (best < 0) break;
                picked[best] = true;
            }
            for (uint i = 0; i < 8; ++i) {
                if (contribution[i] <= 0.0) continue;
                uint rays = directional[i] ? params.sunRays : (picked[i] ? 1u : 0u);
                if (rays == 0) continue;
                float blocked = 0.0;
                for (uint r = 0; r < rays; ++r) {
                    float3 direction = toLight[i];
                    const float radius = directional[i] ? params.sunCone : params.lightRadius;
                    if (radius > 0.0) {
                        const float2 u = float2(interleavedGradientNoise(float2(pixel), params.frame * 7u + r * 3u + i), interleavedGradientNoise(float2(pixel.yx) + 17.0, params.frame * 7u + r * 3u + i));
                        direction = jitter(direction, radius, u);
                    }
                    // a rig light follows a character and ignores the level: only characters block it; a lamp is in the level: the whole scene blocks it
                    const float reach = directional[i] ? 1e4 : distanceTo[i] - params.lampClearance;
                    if (reach > 0.0) blocked += occluded(origin, direction, reach, directional[i] ? 2u : 3u) ? 1.0 : 0.0;
                }
                removed += contribution[i] * (blocked / float(rays));
            }
            factor = saturate(min(total - removed, 1.0) / min(total, 1.0));   // GX clamps ambient + lights at 1 before the material (rt_shadow_math.h shadow_factor)
        }
    }
    signalOut[id.xy] = factor;
}

[shader("compute")] [numthreads(8, 8, 1)]
void compositeMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.region)) return;
    const int2 pixel = int2(id.xy);
    const float4 texel = sceneGBuffer.Load(int3(pixel, 0));
    const float stored = sceneDepth.Load(int3(pixel, 0));
    if (!shadedSurface(texel) || stored >= 1.0) return;
    const float centreDepth = abs(viewPositionAt(pixel, stored).z);
    // a depth-aware bilinear upsample of the signal: its taps are weighted by how close their pixel's view depth is
    // signal pixel t was traced at scene pixel t*divisor + divisor/2, whose centre is t*divisor + divisor/2 + 0.5: the tap position of a scene pixel centre is its offset from there
    const float d = float(params.divisor);
    const float2 at = (float2(pixel) + 0.5 - float(params.divisor / 2) - 0.5) / d;
    const int2 base = int2(floor(at));
    const float2 f = at - float2(base);
    float sum = 0.0, weights = 0.0;
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
        const int2 tap = clamp(base + int2(x, y), int2(0, 0), int2(params.signalSize) - 1);
        const int2 source = int2(min(uint2(tap) * params.divisor + params.divisor / 2, params.region - 1));
        const float tapStored = sceneDepth.Load(int3(source, 0));
        const float tapDepth = tapStored < 1.0 ? abs(viewPositionAt(source, tapStored).z) : 1e6;
        const float w = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y) / (1.0 + 50.0 * abs(tapDepth - centreDepth) / centreDepth);
        sum += signalIn.Load(int3(tap, 0)).x * w; weights += w;
    }
    const float factor = weights > 0.0 ? sum / weights : 1.0;
    float4 color = sceneColor[id.xy];
    color.rgb *= lerp(1.0, saturate(factor), 1.0 - texel.z);   // faded by the GX fog
    sceneColor[id.xy] = color;
}
