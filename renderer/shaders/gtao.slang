// Ground-truth ambient occlusion (Jimenez et al., "Practical Real-Time Strategies for Accurate Indirect Occlusion", the XeGTAO arrangement) on the scene
// targets: a horizon search per pixel (aoMain, at the preset's resolution), a depth-aware blur (denoiseMain) and the composite into the scene colour
// (compositeMain, full resolution; fades with the GX fog and skips pixels that no depth-writing draw covered).
#include "gbuffer.slang"
#include "view_space.slang"
struct Params {
    float4 viewport;     // the scene viewport in scene-target pixels: x, y, width, height
    float4 viewA;        // view parameters: xScale, yScale, xOffset, yOffset
    float2 viewB;        // depthA, depthB
    uint2 region;        // scene region size in pixels
    uint2 aoSize;        // size of the occlusion images
    float radius;        // view-space radius of the search
    float falloff;       // fraction of the radius over which samples fade out
    uint slices;
    uint steps;
    uint noise;          // frame index for the noise pattern (0: a fixed pattern)
    uint flags;
    float strength;
    float contactLength; // screen-space shadows: the ray is at least this long in view units
    float2 depthRange;   // the viewport's near and far depth
    float4 light;        // contact shadows: the dominant light, xyz = view-space position (a directional light's far point is its direction), w = 1 for a spot light
    uint2 contactSize;   // size of the contact-shadow image
    uint contactSteps;   // screen-space shadows: samples along the ray
    uint reserved;
};
// flags: bit 0 = ambient occlusion on, bit 1 = contact shadows on
[[vk::push_constant]] ConstantBuffer<Params> params;
[[vk::binding(0, 0)]] SamplerState linearSampler;
[[vk::binding(1, 0)]] Texture2D<float> sceneDepth;
[[vk::binding(2, 0)]] Texture2D<float4> sceneGBuffer;
[[vk::binding(3, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> sceneColor;
[[vk::binding(4, 0)]] Texture2D<float> rawOcclusion;
[[vk::binding(5, 0)]] Texture2D<float> occlusion;
[[vk::binding(6, 0)]] [[vk::image_format("r8")]] RWTexture2D<float> rawOcclusionOut;
[[vk::binding(7, 0)]] [[vk::image_format("r8")]] RWTexture2D<float> occlusionOut;
// Shadows (renderer/occlusion_pass.cpp ShadowBlock, dynamic offset per frame slot): the view -> light-box matrix, the map's texel size and the preset.
struct ShadowBlock {
    float4 rows[3];       // view -> light: x, y in -1..1 over the box, z in 0..1 along the light
    float4 info;          // x = resolution, y = texel size in view units, z = depth range in view units, w = penumbra width per view unit of occluder distance
    uint4 taps;           // x = filter taps, y = blocker-search taps, z = soft (PCSS), w = enabled
    float4 toLight;       // xyz = unit view-space direction to the light
};
[[vk::binding(10, 0)]] ConstantBuffer<ShadowBlock> shadow;
[[vk::binding(11, 0)]] Texture2D<float> shadowMap;
[[vk::binding(8, 0)]] Texture2D<float> contactTex;
[[vk::binding(9, 0)]] [[vk::image_format("r8")]] RWTexture2D<float> contactOut;

static const float kPi = 3.14159265;
ViewParams viewParams() { ViewParams v; v.xScale = params.viewA.x; v.yScale = params.viewA.y; v.xOffset = params.viewA.z; v.yOffset = params.viewA.w; v.depthA = params.viewB.x; v.depthB = params.viewB.y; return v; }
float projectionDepth(float stored) { return (stored - params.depthRange.x) / (params.depthRange.y - params.depthRange.x); }
// View position of a full-resolution pixel (centre) and whether it is a surface of the scene.
bool surfaceAt(int2 pixel, out float3 position, out float3 normal) {
    position = float3(0.0, 0.0, -1.0); normal = float3(0.0, 0.0, 1.0);
    if (any(pixel < int2(0, 0)) || any(pixel >= int2(params.region))) return false;
    float4 texel = sceneGBuffer.Load(int3(pixel, 0));
    const uint flags = uint(round(texel.w * 65535.0));
    if ((flags & kGBufferHasNormal) == 0 || (flags & kGBufferCutout) != 0) return false;   // cut-outs are not surfaces: no shading and (via surfaceAt users) no tap
    float stored = sceneDepth.Load(int3(pixel, 0));
    if (stored >= 1.0) return false;
    float2 ndc = ((float2(pixel) + 0.5) - params.viewport.xy) / params.viewport.zw * 2.0 - 1.0;
    position = reconstructView(viewParams(), ndc, projectionDepth(stored));
    normal = decodeGBufferNormal(texel.xy);
    return true;
}
float3 viewAt(float2 pixelCentre) {   // position of a (possibly off-surface) sample: depth only
    int2 pixel = int2(floor(pixelCentre));
    pixel = clamp(pixel, int2(0, 0), int2(params.region) - 1);
    float stored = sceneDepth.Load(int3(pixel, 0));
    float2 ndc = ((float2(pixel) + 0.5) - params.viewport.xy) / params.viewport.zw * 2.0 - 1.0;   // the position of the pixel centre the depth belongs to
    return reconstructView(viewParams(), ndc, projectionDepth(min(stored, 0.99999)));
}
// The cosine-weighted visible arc of one slice side up to horizon angle h (GTAO paper eq. 10, per side), for a normal at signed angle n.
float gtaoArc(float h, float n, float cosN) { return 0.25 * (-cos(2.0 * h - n) + cosN + 2.0 * h * sin(n)); }
// The normal of the geometry as the depth buffer shows it: the 2005 meshes are low-poly with smooth vertex normals that disagree with their flat facets, and a
// horizon search against such a normal sees the facets as occluders (a rod turned almost black). Per axis the nearer neighbour is used, so silhouettes keep
// the normal of the surface in front.
float3 geometricNormal(int2 pixel, float3 position, float3 fallback) {
    float3 left = viewAt(float2(pixel) + float2(-0.5, 0.5)), right = viewAt(float2(pixel) + float2(1.5, 0.5));
    float3 up = viewAt(float2(pixel) + float2(0.5, -0.5)), down = viewAt(float2(pixel) + float2(0.5, 1.5));
    float3 dx = abs(left.z - position.z) < abs(right.z - position.z) ? position - left : right - position;
    float3 dy = abs(up.z - position.z) < abs(down.z - position.z) ? position - up : down - position;
    float3 n = cross(dx, dy);
    float len = length(n);
    if (len < 1e-8) return fallback;
    n /= len;
    return dot(n, -position) < 0.0 ? -n : n;   // facing the viewer
}
// A cut-out pixel (net, grate): the depth there is a thin occluder with holes; it must not darken what lies behind it.
bool cutoutAt(float2 pixelCentre) {
    int2 pixel = clamp(int2(floor(pixelCentre)), int2(0, 0), int2(params.region) - 1);
    return (uint(round(sceneGBuffer.Load(int3(pixel, 0)).w * 65535.0)) & kGBufferCutout) != 0;
}
float interleavedGradientNoise(float2 pixel, uint frame) {
    pixel += 5.588238 * float(frame);
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

[shader("compute")] [numthreads(8, 8, 1)]
void aoMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.aoSize)) return;
    float scale = float(params.region.x) / float(params.aoSize.x);
    int2 pixel = int2((float2(id.xy) + 0.5) * scale);
    float3 position, normal;
    if (!surfaceAt(pixel, position, normal)) { rawOcclusionOut[id.xy] = 1.0; return; }
    normal = geometricNormal(pixel, position, normal);
    float3 view = normalize(-position);
    if (dot(normal, view) < 0.0) normal = -normal;   // two-sided: back faces and flipped normals still occlude from the viewer's side
    float2 centre = float2(pixel) + 0.5;
    float radiusPixels = params.radius * params.viewA.x / -position.z * params.viewport.z * 0.5;
    if (radiusPixels < 1.5) { rawOcclusionOut[id.xy] = 1.0; return; }
    radiusPixels = min(radiusPixels, 0.25 * params.viewport.w);
    float noiseA = interleavedGradientNoise(float2(pixel), params.noise);
    float noiseB = interleavedGradientNoise(float2(pixel) + 17.0, params.noise);
    float visibility = 0.0, unoccluded = 0.0;
    for (uint slice = 0; slice < params.slices; ++slice) {
        float phi = (float(slice) + noiseA) / float(params.slices) * kPi;
        float2 direction = float2(cos(phi), sin(phi));
        // The same screen direction in view space: a point of the screen line, put at the pixel's own view depth, spans the slice plane with the view vector
        // (the plane through the camera that holds the whole screen line); its part orthogonal to the view vector is the slice direction.
        float2 ndcAlong = ((centre + direction * 8.0) - params.viewport.xy) / params.viewport.zw * 2.0 - 1.0;
        float3 ray = float3((ndcAlong.x + params.viewA.z) * -position.z / params.viewA.x, (ndcAlong.y + params.viewA.w) * -position.z / params.viewA.y, position.z);
        float3 sliceDirection = normalize(ray - dot(ray, view) * view);
        float3 axis = normalize(cross(sliceDirection, view));
        float3 projected = normal - axis * dot(normal, axis);
        float projectedLength = length(projected);
        float cosN = clamp(dot(projected, view) / max(projectedLength, 1e-5), -1.0, 1.0);
        float n = sign(dot(sliceDirection, projected)) * acos(cosN);
        float lowHorizon0 = cos(n + kPi * 0.5), lowHorizon1 = cos(n - kPi * 0.5);   // the tangent plane: nothing occludes below it
        float horizon0 = lowHorizon0, horizon1 = lowHorizon1;
        for (uint step = 0; step < params.steps; ++step) {
            float s = (float(step) + noiseB) / float(params.steps);
            float2 offset = direction * max(s * s * radiusPixels, 1.0 + float(step));
            [unroll] for (int side = 0; side < 2; ++side) {   // side 0 walks along the slice direction (positive angles), side 1 against it
                float2 samplePoint = centre + (side == 0 ? offset : -offset);
                float3 delta = viewAt(samplePoint) - position;
                float distance = length(delta);
                float weight = saturate((params.radius - distance) / max(params.radius * params.falloff, 1e-5));
                if (cutoutAt(samplePoint)) weight = 0.0;
                float shc = dot(delta / max(distance, 1e-5), view);
                if (side == 0) horizon0 = max(horizon0, lerp(lowHorizon0, shc, weight));
                else horizon1 = max(horizon1, lerp(lowHorizon1, shc, weight));
            }
        }
        float h0 = -acos(clamp(horizon1, -1.0, 1.0)), h1 = acos(clamp(horizon0, -1.0, 1.0));
        h0 = n + clamp(h0 - n, -kPi * 0.5, kPi * 0.5);
        h1 = n + clamp(h1 - n, -kPi * 0.5, kPi * 0.5);
        visibility += projectedLength * (gtaoArc(h0, n, cosN) + gtaoArc(h1, n, cosN));
        unoccluded += projectedLength * (gtaoArc(n - kPi * 0.5, n, cosN) + gtaoArc(n + kPi * 0.5, n, cosN));   // the same slice with the horizons on the tangent plane
    }
    // Normalised by the unoccluded estimate of the same slices: a slice's weight depends on its orientation, so with a few slices the raw sum would
    // darken grazing surfaces; the ratio is exactly 1 on a flat surface whatever slices were drawn.
    rawOcclusionOut[id.xy] = pow(saturate(visibility / max(unoccluded, 1e-4)), 1.5);
}

[shader("compute")] [numthreads(8, 8, 1)]
void denoiseMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.aoSize)) return;
    float scale = float(params.region.x) / float(params.aoSize.x);
    float3 centrePosition, centreNormal;
    float centreOcclusion = rawOcclusion.Load(int3(id.xy, 0));
    if (!surfaceAt(int2((float2(id.xy) + 0.5) * scale), centrePosition, centreNormal)) { occlusionOut[id.xy] = centreOcclusion; return; }
    float sum = 0.0, total = 0.0;
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
        int2 at = clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(params.aoSize) - 1);
        float3 position, normal;
        float value = rawOcclusion.Load(int3(at, 0));
        if (!surfaceAt(int2((float2(at) + 0.5) * scale), position, normal)) continue;
        float weight = 1.0 / (1.0 + 50.0 * abs(position.z - centrePosition.z) / -centrePosition.z) * pow(saturate(dot(normal, centreNormal)), 4.0);
        sum += value * weight; total += weight;
    }
    occlusionOut[id.xy] = total > 0.0 ? sum / total : centreOcclusion;
}

[shader("compute")] [numthreads(8, 8, 1)]
void contactMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.contactSize)) return;
    float scale = float(params.region.x) / float(params.contactSize.x);
    int2 pixel = int2((float2(id.xy) + 0.5) * scale);
    float3 position, normal;
    if (!surfaceAt(pixel, position, normal)) { contactOut[id.xy] = 1.0; return; }
    normal = geometricNormal(pixel, position, normal);
    float3 toLight = params.light.w > 0.5 ? normalize(params.light.xyz - position) : normalize(params.light.xyz);
    float facing = dot(normal, toLight);
    if (facing <= 0.05) { contactOut[id.xy] = 1.0; return; }   // facing away from (or grazing) the light: the lighting already darkened it, and a grazing ray starts inside its own surface
    float distanceHere = -position.z;
    float maxLength = max(0.03 * distanceHere, params.contactLength);   // contact only, or as far as the preset reaches
    float count = float(params.contactSteps);
    float thickness = max(0.005 * distanceHere, 2.0 * maxLength / count);   // what counts as a blocker in front of the ray (a step apart at least)
    float lift = (0.002 + 0.006 * (1.0 - facing)) * distanceHere;   // the start moves off the surface more the more the surface grazes the light (no self-shadowing speckle)
    float jitter = interleavedGradientNoise(float2(pixel), params.noise);
    float shadow = 0.0;
    [loop] for (uint step = 0; step < params.contactSteps; ++step) {
        float t = (float(step) + 0.5 + 0.5 * jitter) / count * maxLength;
        float3 sampled = position + toLight * t + normal * lift;
        float2 ndc = float2(params.viewA.x * sampled.x / -sampled.z - params.viewA.z, params.viewA.y * sampled.y / -sampled.z - params.viewA.w);
        float2 samplePixel = params.viewport.xy + (ndc * 0.5 + 0.5) * params.viewport.zw;
        if (any(samplePixel < float2(0.0, 0.0)) || any(samplePixel >= float2(params.region))) break;
        float stored = sceneDepth.Load(int3(int2(samplePixel), 0));
        if (stored >= 1.0 || cutoutAt(samplePixel)) continue;
        float sceneDistance = params.viewB.y / (projectionDepth(stored) + params.viewB.x);   // -z of the surface at that pixel
        float sampleDistance = -sampled.z;
        if (sceneDistance < sampleDistance - 0.1 * thickness && sampleDistance - sceneDistance < thickness) shadow += (1.0 - 0.5 * float(step) / count) / (count * 0.3125);   // every blocked step adds a little: a thin ridge hit by one or two steps stays faint, a real contact adds up
    }
    contactOut[id.xy] = 1.0 - saturate(shadow) * smoothstep(0.05, 0.35, facing);
}

// A half-resolution occlusion image at a full-resolution pixel: bilinear taps, each weighted by how close its depth is to this pixel's.
float upsampleOcclusion(Texture2D<float> source, uint2 size, uint2 pixel, float3 position) {
    float scale = float(params.region.x) / float(size.x);
    float2 at0 = (float2(pixel) + 0.5) / scale - 0.5;
    int2 base = int2(floor(at0));
    float2 fraction = at0 - float2(base);
    float sum = 0.0, total = 0.0;
    [unroll] for (int i = 0; i < 4; ++i) {
        int2 at = clamp(base + int2(i & 1, i >> 1), int2(0, 0), int2(size) - 1);
        float3 tapPosition, tapNormal;
        float value = source.Load(int3(at, 0));
        float bilinear = ((i & 1) != 0 ? fraction.x : 1.0 - fraction.x) * ((i >> 1) != 0 ? fraction.y : 1.0 - fraction.y);
        float depthWeight = 1.0;
        if (surfaceAt(int2((float2(at) + 0.5) * scale), tapPosition, tapNormal)) depthWeight = 1.0 / (0.01 + 50.0 * abs(tapPosition.z - position.z) / -position.z);
        sum += value * bilinear * depthWeight; total += bilinear * depthWeight;
    }
    return total > 0.0 ? sum / total : 1.0;
}

// Visibility of a view-space point from the light: 1 lit .. 0 shadowed. The point is pushed along its normal by 1.5 texels (the receiver bias), then
// compared against the map with PCF (a grid of taps) or PCSS (a blocker search sets the filter radius).
float shadowAt(float3 lightPosition, float depthBias, float2 offsetTexels) {
    float2 uv = lightPosition.xy * 0.5 + 0.5 + offsetTexels / shadow.info.x;
    if (any(uv < 0.0) || any(uv >= 1.0)) return 1.0;
    float stored = shadowMap.Load(int3(int2(uv * shadow.info.x), 0));
    return stored + depthBias >= lightPosition.z ? 1.0 : 0.0;
}
float shadowVisibility(float3 position, float3 normal) {
    if (shadow.taps.w == 0) return 1.0;
    float texel = shadow.info.y;
    float3 p = position + normal * (1.5 * texel) + shadow.toLight.xyz * (0.5 * texel);
    float3 lp = float3(dot(shadow.rows[0].xyz, p) + shadow.rows[0].w, dot(shadow.rows[1].xyz, p) + shadow.rows[1].w, dot(shadow.rows[2].xyz, p) + shadow.rows[2].w);
    if (any(abs(lp.xy) >= 1.0) || lp.z <= 0.0 || lp.z >= 1.0) return 1.0;
    float bias = 1.5 * texel / shadow.info.z;
    float radius = 1.0;   // filter radius in texels
    if (shadow.taps.z != 0) {   // PCSS: the average depth of the blockers near the receiver gives the penumbra
        float blockerSum = 0.0, blockers = 0.0;
        for (uint i = 0; i < shadow.taps.y; ++i) {
            float r = 6.0 * sqrt((float(i) + 0.5) / float(shadow.taps.y)), a = float(i) * 2.39996323;
            float2 offset = r * float2(cos(a), sin(a));
            float2 uv = lp.xy * 0.5 + 0.5 + offset / shadow.info.x;
            if (any(uv < 0.0) || any(uv >= 1.0)) continue;
            float stored = shadowMap.Load(int3(int2(uv * shadow.info.x), 0));
            if (stored + bias < lp.z) { blockerSum += stored; blockers += 1.0; }
        }
        if (blockers == 0.0) return 1.0;
        float distanceToBlocker = (lp.z - blockerSum / blockers) * shadow.info.z;   // view units
        radius = clamp(distanceToBlocker * shadow.info.w / texel, 1.0, 8.0);
    }
    float lit = 0.0;
    if (shadow.taps.z != 0) {
        for (uint i = 0; i < shadow.taps.x; ++i) {
            float r = radius * sqrt((float(i) + 0.5) / float(shadow.taps.x)), a = float(i) * 2.39996323;
            lit += shadowAt(lp, bias, r * float2(cos(a), sin(a)));
        }
        return lit / float(shadow.taps.x);
    }
    uint grid = max(2u, uint(round(sqrt(float(shadow.taps.x)))));   // low 2x2, medium 3x3
    for (uint y = 0; y < grid; ++y) for (uint x = 0; x < grid; ++x)
        lit += shadowAt(lp, bias, (float2(x, y) - 0.5 * float(grid - 1)) * radius);
    return lit / float(grid * grid);
}

// Like upsampleOcclusion with a wider tent (1.5 half-resolution pixels) over a 4x4 neighbourhood: the contact shadows are noisy by construction (jittered
// 16-step rays), so they are filtered, with depth and normal weights keeping edges.
float filterContact(uint2 pixel, float3 position, float3 normal) {
    float scale = float(params.region.x) / float(params.contactSize.x);
    float2 centre = (float2(pixel) + 0.5) / scale - 0.5;
    int2 base = int2(floor(centre));
    float sum = 0.0, total = 0.0;
    for (int y = -1; y <= 2; ++y) for (int x = -1; x <= 2; ++x) {
        int2 at = clamp(base + int2(x, y), int2(0, 0), int2(params.contactSize) - 1);
        float2 d = abs(float2(base + int2(x, y)) - centre);
        float tent = max(0.0, 1.5 - d.x) * max(0.0, 1.5 - d.y);
        float3 tapPosition, tapNormal;
        float weight = tent;
        if (surfaceAt(int2((float2(at) + 0.5) * scale), tapPosition, tapNormal)) weight *= 1.0 / (0.01 + 50.0 * abs(tapPosition.z - position.z) / -position.z) * saturate(dot(tapNormal, normal));   // the normals only keep the filter from crossing a crease
        else weight = 0.0;
        sum += contactTex.Load(int3(at, 0)) * weight; total += weight;
    }
    return total > 0.0 ? sum / total : 1.0;
}

[shader("compute")] [numthreads(8, 8, 1)]
void compositeMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.region)) return;
    float3 position, normal;
    if (!surfaceAt(int2(id.xy), position, normal)) return;
    float fog = sceneGBuffer.Load(int3(int2(id.xy), 0)).z;
    float ao = (params.flags & 1u) != 0 ? upsampleOcclusion(occlusion, params.aoSize, id.xy, position) : 1.0;
    ao = lerp(0.3, 1.0, ao);   // occlusion never takes more than 70% of the light: a fully black result is always an artefact
    float contact = (params.flags & 2u) != 0 ? filterContact(id.xy, position, normal) : 1.0;
    float visibility = (params.flags & 4u) != 0 ? shadowVisibility(position, normal) : 1.0;
    bool lit = (uint(round(sceneGBuffer.Load(int3(int2(id.xy), 0)).w * 65535.0)) & kGBufferLit) != 0;
    float facing = lit ? smoothstep(0.0, 0.3, dot(normal, shadow.toLight.xyz)) : 1.0;   // a lit surface turned away from the light is dark already
    float4 color = sceneColor[id.xy];
    color.rgb *= lerp(1.0, ao, saturate(params.strength) * (1.0 - fog)) * lerp(1.0, contact, 0.6 * (1.0 - fog)) * lerp(1.0, visibility, 0.55 * facing * (1.0 - fog));
    sceneColor[id.xy] = color;
}
