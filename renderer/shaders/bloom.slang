// Bloom (docs/superpowers/specs/2026-10-22-bloom-design.md): a six-level pyramid of the scene, summed back up into a glow at half resolution, and (SDR only)
// added to the 8-bit colour. The constants and the expansion mirror renderer/bloom_math.h (tested there).
//   downFirstMain  scene colour (gamma 2.2 decoded, inverse tone expanded, Karis-weighted 4x4) -> level 0
//   downMain       13-tap downsample, level i-1 -> level i
//   upMain         level i = B * weight + tent(A) * weightA, then * scale (the mix, on level 0 only)
//   compositeMain  SDR: colour = encode(decode(colour) + glow) with a static ordered dither
struct Params {
    uint2 srcSize;      // size of the image the pass reads through the sampler (texels)
    uint2 dstSize;      // size of the image it writes
    uint2 region;       // downFirstMain / compositeMain: the scene part of the colour image, in pixels
    float weight;       // upMain: weight of B (the level's own image)
    float weightA;      // upMain: weight of the coarser level A (only the first step needs it: the coarsest level's own weight)
    float scale;        // upMain: the result's gain (the mix on level 0, 1 elsewhere)
    float headroom;     // downFirstMain: what 1.0 in the 8-bit scene counts as (paper white = 1)
    uint2 reserved;
};
[[vk::push_constant]] ConstantBuffer<Params> params : register(b0);
[[vk::binding(0, 0)]] SamplerState linearSampler : register(s0);
[[vk::binding(1, 0)]] Texture2D<float4> imageA : register(t0);
[[vk::binding(2, 0)]] Texture2D<float4> imageB : register(t1);
[[vk::binding(3, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outLevel : register(u0);
[[vk::binding(4, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> outColor : register(u1);

static const float kKnee = 0.6;   // kBloomKnee

float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
// bloom_expand: linear 0..1 above the knee rises to the headroom, the square keeps the slope continuous.
float expandLuma(float y) {
    y = saturate(y);
    float over = max(0.0, (y - kKnee) / (1.0 - kKnee));
    return y + over * over * (params.headroom - 1.0);
}
static const float kThreshold = 0.6;       // kBloomThreshold
static const float kThresholdKnee = 0.3;   // kBloomThresholdKnee
// bloom_soft_threshold on the expanded luminance, applied to the colour as a ratio.
float softThreshold(float y) {
    float m = clamp(y - kThreshold + kThresholdKnee, 0.0, 2.0 * kThresholdKnee);
    return max(m * m / (4.0 * kThresholdKnee), y - kThreshold);
}
float3 expandColor(float3 linearColor) {
    float y = luma(linearColor);
    if (y <= 0.0) return float3(0.0, 0.0, 0.0);
    float expanded = expandLuma(y);
    return linearColor * (softThreshold(expanded) / y);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void downFirstMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.dstSize)) return;
    float2 centre = (float2(id.xy) + 0.5) * float2(params.region) / float2(params.dstSize);
    int2 origin = int2(floor(centre - 0.5)) - 1;   // a 4x4 footprint around the destination pixel
    const float tapWeight[4] = { 1.0, 3.0, 3.0, 1.0 };
    float3 sum = float3(0.0, 0.0, 0.0);
    float total = 0.0;
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) {
            int2 p = clamp(origin + int2(i, j), int2(0, 0), int2(params.region) - 1);
            float3 e = expandColor(pow(saturate(imageA.Load(int3(p, 0)).rgb), 2.2));
            float w = tapWeight[i] * tapWeight[j] / (1.0 + luma(e));   // Karis: one hot pixel cannot flicker the level
            sum += e * w;
            total += w;
        }
    outLevel[id.xy] = float4(sum / total, 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void downMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.dstSize)) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(params.dstSize);
    float2 t = 1.0 / float2(params.srcSize);
    float3 e = imageA.SampleLevel(linearSampler, uv, 0).rgb;
    float3 a = imageA.SampleLevel(linearSampler, uv + t * float2(-2, 2), 0).rgb, b = imageA.SampleLevel(linearSampler, uv + t * float2(0, 2), 0).rgb;
    float3 c = imageA.SampleLevel(linearSampler, uv + t * float2(2, 2), 0).rgb, d = imageA.SampleLevel(linearSampler, uv + t * float2(-2, 0), 0).rgb;
    float3 f = imageA.SampleLevel(linearSampler, uv + t * float2(2, 0), 0).rgb, g = imageA.SampleLevel(linearSampler, uv + t * float2(-2, -2), 0).rgb;
    float3 h = imageA.SampleLevel(linearSampler, uv + t * float2(0, -2), 0).rgb, i = imageA.SampleLevel(linearSampler, uv + t * float2(2, -2), 0).rgb;
    float3 j = imageA.SampleLevel(linearSampler, uv + t * float2(-1, 1), 0).rgb, k = imageA.SampleLevel(linearSampler, uv + t * float2(1, 1), 0).rgb;
    float3 l = imageA.SampleLevel(linearSampler, uv + t * float2(-1, -1), 0).rgb, m = imageA.SampleLevel(linearSampler, uv + t * float2(1, -1), 0).rgb;
    float3 result = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    outLevel[id.xy] = float4(result, 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void upMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.dstSize)) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(params.dstSize);
    float2 r = 1.0 / float2(params.srcSize);
    float3 tent = imageA.SampleLevel(linearSampler, uv + r * float2(-1, 1), 0).rgb + 2.0 * imageA.SampleLevel(linearSampler, uv + r * float2(0, 1), 0).rgb + imageA.SampleLevel(linearSampler, uv + r * float2(1, 1), 0).rgb
                + 2.0 * imageA.SampleLevel(linearSampler, uv + r * float2(-1, 0), 0).rgb + 4.0 * imageA.SampleLevel(linearSampler, uv, 0).rgb + 2.0 * imageA.SampleLevel(linearSampler, uv + r * float2(1, 0), 0).rgb
                + imageA.SampleLevel(linearSampler, uv + r * float2(-1, -1), 0).rgb + 2.0 * imageA.SampleLevel(linearSampler, uv + r * float2(0, -1), 0).rgb + imageA.SampleLevel(linearSampler, uv + r * float2(1, -1), 0).rgb;
    float3 own = imageB.Load(int3(id.xy, 0)).rgb;
    outLevel[id.xy] = float4((own * params.weight + tent * (params.weightA / 16.0)) * params.scale, 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compositeMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.region)) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(params.region);
    float3 glow = imageA.SampleLevel(linearSampler, uv, 0).rgb;
#ifdef UC_DIRECTX
    // SM5 guarantees RGBA8 typed UAV stores; a separate SRV also avoids read/write aliasing.
    float4 colour = imageB.Load(int3(id.xy, 0));
#else
    float4 colour = outColor[id.xy];
#endif
    float3 lit = saturate(pow(pow(saturate(colour.rgb), 2.2) + glow, 1.0 / 2.2));
    const float bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    float dither = ((bayer[(id.y & 3) * 4 + (id.x & 3)] + 0.5) / 16.0 - 0.5) / 255.0;   // +-0.5 LSB, a fixed pattern
    outColor[id.xy] = float4(saturate(lit + dither), colour.a);
}
