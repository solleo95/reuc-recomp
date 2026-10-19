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
    uint reserved;
};
[[vk::push_constant]] ConstantBuffer<Params> params;
[[vk::binding(1, 0)]] Texture2D<float4> sourceColor;
[[vk::binding(2, 0)]] Texture2D<float2> sceneMotion;
[[vk::binding(3, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> outputColor;
[[vk::binding(4, 0)]] Texture2D<float2> tileMax;
[[vk::binding(5, 0)]] [[vk::image_format("rg16f")]] RWTexture2D<float2> tileMaxOut;

float2 longer(float2 a, float2 b) { return dot(a, a) >= dot(b, b) ? a : b; }
// Motion vector (previous minus current, scene-target pixels) at an output pixel.
float2 motionAt(int2 pixel) {
    int2 scenePixel = clamp(int2((float2(pixel) + 0.5) * float2(params.renderSize) / float2(params.outputSize)), int2(0, 0), int2(params.renderSize) - 1);
    return sceneMotion.Load(int3(scenePixel, 0));
}

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

[shader("compute")]
[numthreads(8, 8, 1)]
void blurMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.outputSize)) return;
    const int2 pixel = int2(id.xy);
    const float4 centre = sourceColor.Load(int3(pixel, 0));
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
    if (length < 1.0) { outputColor[id.xy] = centre; return; }
    float4 sum = centre;
    float weights = 1.0;
    for (uint i = 0; i < params.samples; ++i) {
        const float t = (float(i) + 0.5) / float(params.samples) - 0.5;
        const int2 at = clamp(pixel + int2(round(direction * t)), int2(0, 0), int2(params.outputSize) - 1);
        const float2 sampleMotion = motionAt(at) * toOutput;
        const float w = saturate(sqrt(dot(sampleMotion, sampleMotion)) - abs(t) * length + 1.0);
        sum += sourceColor.Load(int3(at, 0)) * w;
        weights += w;
    }
    outputColor[id.xy] = sum / weights;
}
