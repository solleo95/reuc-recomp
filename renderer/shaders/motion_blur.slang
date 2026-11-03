// Per-object motion blur (McGuire et al., "A Reconstruction Filter for Plausible Motion Blur"), reduced to two compute passes on the upscaled frame:
// tileMain keeps the longest motion vector of each 16x16 tile of the scene's vector image, blurMain gathers the colour along the pixel's own vector, or
// along the longest vector of the surrounding tiles when the pixel itself stands still (so a moving object smears over the background next to it).
// A sample counts when its own vector reaches back to the pixel: the sharp background does not leak into a moving object.
struct Params {
    uint2 outputSize;    // output pixels of the scene region
    uint2 renderSize;    // scene-target pixels of the scene region
    float shutter;       // fraction of the frame the shutter is open
    uint samples;
    uint tileSize;       // scene-target pixels per tile
    uint fullSamples;    // nonzero: always the preset's sample count (UC_BLUR_FULL_SAMPLES), else two samples per pixel of the blur (blur_sample_count in raster_effects_math.h)
};
[[vk::push_constant]] ConstantBuffer<Params> params : register(b0);
[[vk::binding(1, 0)]] Texture2D<float4> sourceColor : register(t0);
[[vk::binding(2, 0)]] Texture2D<float2> sceneMotion : register(t1);
[[vk::binding(3, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> outputColor : register(u0);
[[vk::binding(4, 0)]] Texture2D<float2> tileMax : register(t2);
[[vk::binding(5, 0)]] [[vk::image_format("rg16f")]] RWTexture2D<float2> tileMaxOut : register(u1);

float2 longer(float2 a, float2 b) { return dot(a, a) >= dot(b, b) ? a : b; }
// Motion vector (previous minus current, scene-target pixels) at an output pixel.
float2 motionAt(int2 pixel) {
#ifdef UC_BLUR_NATIVE_GRID
    // Both callers supply clamped output pixels; at 1:1 these coordinates map to themselves.
    if (all(params.outputSize == params.renderSize) && all(params.outputSize <= uint2(16384, 16384))) return sceneMotion.Load(int3(pixel, 0));
#endif
    int2 scenePixel = clamp(int2((float2(pixel) + 0.5) * float2(params.renderSize) / float2(params.outputSize)), int2(0, 0), int2(params.renderSize) - 1);
    return sceneMotion.Load(int3(scenePixel, 0));
}

#ifdef UC_BLUR_TILE_GROUP
// Linux only: one workgroup per tile, one thread per sampled vector (the 8x8 samples of the 16x16 tile at a stride of 2) and a reduction in groupshared memory, instead of one thread
// walking all 64 samples (6000 threads, latency bound: 0.36 ms of the 0.88 ms blur pass at rest). The sequential walk keeps the earliest of equally long vectors and starts from
// zero, so the reduction takes the longest, then the lowest sample index, and zero when nothing is longer than zero: the same vector, bit for bit.
groupshared float2 tileVector[64];
groupshared float tileLength[64];
groupshared uint tileIndex[64];
[shader("compute")]
[numthreads(8, 8, 1)]
void tileMain(uint3 tileId : SV_GroupID, uint3 laneId : SV_GroupThreadID) {
    const uint lane = laneId.y * 8 + laneId.x;
    const int2 pixel = int2(tileId.xy * params.tileSize + laneId.xy * 2);
    float2 motion = float2(0.0, 0.0);
    float length2 = -1.0;   // a sample outside the scene never wins
    if (all(pixel < int2(params.renderSize))) {
        motion = sceneMotion.Load(int3(pixel, 0));
        length2 = dot(motion, motion);
    }
    tileVector[lane] = motion; tileLength[lane] = length2; tileIndex[lane] = lane;
    for (uint stride = 32; stride > 0; stride >>= 1) {
        GroupMemoryBarrierWithGroupSync();
        if (lane < stride) {
            const uint other = lane + stride;
            if (tileLength[other] > tileLength[lane] || (tileLength[other] == tileLength[lane] && tileIndex[other] < tileIndex[lane])) {
                tileVector[lane] = tileVector[other]; tileLength[lane] = tileLength[other]; tileIndex[lane] = tileIndex[other];
            }
        }
    }
    if (lane == 0) tileMaxOut[tileId.xy] = tileLength[0] > 0.0 ? tileVector[0] : float2(0.0, 0.0);
}
#else
[shader("compute")]
[numthreads(8, 8, 1)]
void tileMain(uint3 id : SV_DispatchThreadID) {
    const int2 tiles = int2((params.renderSize + params.tileSize - 1) / params.tileSize);
    if (any(int2(id.xy) >= tiles)) return;
    float2 best = float2(0.0, 0.0);
    for (uint y = 0; y < params.tileSize; y += 2)
        for (uint x = 0; x < params.tileSize; x += 2) {
            const int2 pixel = int2(id.xy * params.tileSize + uint2(x, y));
            if (all(pixel < int2(params.renderSize))) best = longer(best, sceneMotion.Load(int3(pixel, 0)));
        }
    tileMaxOut[id.xy] = best;
}
#endif

[shader("compute")]
[numthreads(8, 8, 1)]
void blurMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.outputSize)) return;
    const int2 pixel = int2(id.xy);
    const float2 toOutput = float2(params.outputSize) / float2(params.renderSize) * params.shutter;
    float2 direction = motionAt(pixel) * toOutput;
    if (dot(direction, direction) < 1.0) {
        const int2 tiles = int2((params.renderSize + params.tileSize - 1) / params.tileSize);
        const int2 tile = int2((float2(pixel) + 0.5) * float2(params.renderSize) / float2(params.outputSize)) / int(params.tileSize);
        float2 longest = float2(0.0, 0.0);
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int2 neighbour = tile + int2(dx, dy);
                if (all(neighbour >= int2(0, 0)) && all(neighbour < tiles)) longest = longer(longest, tileMax.Load(int3(neighbour, 0)));
            }
        direction = longest * toOutput;
    }
    const float length = sqrt(dot(direction, direction));
#ifdef UC_BLUR_IN_PLACE
    // Linux only: the output image is the one the source copy was taken from, so a pixel that stays as it is already holds its colour: no centre read, no store.
    if (length < 1.0) return;
    const float4 centre = sourceColor.Load(int3(pixel, 0));
#else
    const float4 centre = sourceColor.Load(int3(pixel, 0));
    if (length < 1.0) { outputColor[id.xy] = centre; return; }
#endif
#ifdef UC_BLUR_SAMPLES_PER_PIXEL
    // Linux only: fewer samples per pixel of the blur than the two of blur_sample_count; the centre weight below keeps its share.
    const uint count = params.fullSamples != 0 ? params.samples : min(params.samples, max(8u, uint(ceil(UC_BLUR_SAMPLES_PER_PIXEL * length))));
#else
    const uint count = params.fullSamples != 0 ? params.samples : min(params.samples, max(8u, uint(ceil(2.0 * length))));
#endif
    const float centreWeight = float(count) / float(params.samples);   // blur_centre_weight in raster_effects_math.h: fewer samples must not make the centre count more
    float4 sum = centre * centreWeight;
    float weights = centreWeight;
    int2 previousAt = int2(-1, -1);
    float4 sampleColor = float4(0.0);
    float sampleLength = 0.0;
    for (uint i = 0; i < count; ++i) {
        const float t = (float(i) + 0.5) / float(count) - 0.5;
        const int2 at = clamp(pixel + int2(round(direction * t)), int2(0, 0), int2(params.outputSize) - 1);
        // Rounded gather positions repeat; retain every weight and accumulation in order.
        if (any(at != previousAt)) {
            const float2 sampleMotion = motionAt(at) * toOutput;
            sampleLength = sqrt(dot(sampleMotion, sampleMotion));
            sampleColor = sourceColor.Load(int3(at, 0));
            previousAt = at;
        }
        const float w = saturate(sampleLength - abs(t) * length + 1.0);
        sum += sampleColor * w;
        weights += w;
    }
    outputColor[id.xy] = sum / weights;
}
