// Screen-space light shafts: shaftMain marches from each pixel of a reduced-resolution grid toward the light's screen position and sums the bright
// scene pixels it passes (radial blur of the bright parts, the usual god-rays arrangement); compositeMain adds the result, tinted by the light, to the
// scene colour at full resolution.
struct Params {
    float2 light;        // the light's position in scene-target pixels
    uint2 region;        // scene region size in pixels
    uint2 lowSize;       // size of the shaft image
    uint samples;
    float density;       // fraction of the way to the light a ray covers
    float decay;         // weight kept per step
    float intensity;
    float4 tint;         // rgb: light colour
};
[[vk::push_constant]] ConstantBuffer<Params> params;
[[vk::binding(0, 0)]] SamplerState linearSampler;
[[vk::binding(1, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> sceneColor;
[[vk::binding(2, 0)]] Texture2D<float4> shafts;
[[vk::binding(3, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> shaftsOut;

float brightness(float3 colour) { return smoothstep(0.6, 0.95, dot(colour, float3(0.299, 0.587, 0.114))); }

[shader("compute")]
[numthreads(8, 8, 1)]
void shaftMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.lowSize)) return;
    const float2 pixel = (float2(id.xy) + 0.5) * float2(params.region) / float2(params.lowSize);
    const float2 step = (params.light - pixel) * params.density / float(params.samples);
    float2 at = pixel;
    float weight = 1.0, sum = 0.0, total = 0.0;
    for (uint i = 0; i < params.samples; ++i) {
        at += step;
        const int2 sampleAt = int2(clamp(at, float2(0.0, 0.0), float2(params.region) - 1.0));
        sum += brightness(sceneColor.Load(sampleAt).rgb) * weight;
        total += weight;
        weight *= params.decay;
    }
    shaftsOut[id.xy] = float4(sum / total, 0.0, 0.0, 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compositeMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= params.region)) return;
    const float amount = shafts.SampleLevel(linearSampler, (float2(id.xy) + 0.5) / float2(params.region), 0.0).r * params.intensity;
    const float4 colour = sceneColor[id.xy];
    sceneColor[id.xy] = float4(saturate(colour.rgb + params.tint.rgb * amount), colour.a);
}
