// The bridge around DLSS 5 Neural Rendering (renderer/dlss5_pass.cpp, dlss5_math.h). PREPARE turns a region of the game's RGBA8 picture into the network's signal
// (resampled to the processing size, decoded from the chosen encoding, times the exposure/diffuse-white gain) in an RGBA16F image; RESOLVE brings the network's
// RGBA16F output back into the RGBA8 picture (the inverse). The scaling domain says whether the filter averages stored values or signal values.
#if defined(PREPARE)
[[vk::binding(0, 0)]] Texture2D<float4> source;                                          // RGBA8 picture
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> target;        // bridge image
#else
[[vk::binding(0, 0)]] Texture2D<float4> source;                                          // bridge image
[[vk::binding(1, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> target;          // RGBA8 picture
#endif
struct Constants {
    int2 source_size;     // texels of the region read
    int2 target_size;     // texels of the region written
    int2 source_origin;   // top-left of the region read
    int2 target_origin;   // top-left of the region written
    float gain;           // exposure * diffuse white / reference (dlss5_math.h dlss5_gain)
    int encoding;         // 0 sRGB (auto), 1 gamma 2.2, 2 gamma 2.4, 3 linear
    int filter;           // 0 nearest, 1 bilinear, 2 bicubic (Catmull-Rom)
    int domain;           // 0 filter the stored values, 1 filter the signal
};
[[vk::push_constant]] ConstantBuffer<Constants> constants;

float3 decodeSignal(float3 v) {
    v = max(v, 0.0);
    if (constants.encoding == 1) return pow(v, 2.2);
    if (constants.encoding == 2) return pow(v, 2.4);
    if (constants.encoding == 3) return v;
    return select(v <= 0.04045, v / 12.92, pow((v + 0.055) / 1.055, 2.4));
}
float3 encodeSignal(float3 v) {
    v = max(v, 0.0);
    if (constants.encoding == 1) return pow(v, 1.0 / 2.2);
    if (constants.encoding == 2) return pow(v, 1.0 / 2.4);
    if (constants.encoding == 3) return v;
    return select(v <= 0.0031308, v * 12.92, 1.055 * pow(v, 1.0 / 2.4) - 0.055);
}
#if defined(PREPARE)
float3 tapValue(float3 raw) { return constants.domain == 1 ? constants.gain * decodeSignal(raw) : raw; }
float3 finish(float3 filtered) { return constants.domain == 1 ? filtered : constants.gain * decodeSignal(filtered); }
#else
float3 tapValue(float3 raw) { return constants.domain == 1 ? raw : encodeSignal(raw / constants.gain); }
float3 finish(float3 filtered) { return constants.domain == 1 ? encodeSignal(filtered / constants.gain) : filtered; }
#endif
float3 tap(int2 p) {
    int2 q = clamp(p, int2(0, 0), constants.source_size - 1) + constants.source_origin;
    return tapValue(source.Load(int3(q, 0)).rgb);
}
float4 catmullRom(float t) {   // weights of the four taps around t in [0, 1)
    float t2 = t * t, t3 = t2 * t;
    return float4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    int2 p = int2(id.xy);
    if (any(p >= constants.target_size)) return;
    float2 at = (float2(p) + 0.5) * float2(constants.source_size) / float2(constants.target_size) - 0.5;   // source position of the pixel's centre
    float3 value;
    if (constants.filter == 0) {
        value = tap(int2(floor(at + 0.5)));
    } else if (constants.filter == 1) {
        int2 base = int2(floor(at));
        float2 f = at - float2(base);
        value = lerp(lerp(tap(base), tap(base + int2(1, 0)), f.x), lerp(tap(base + int2(0, 1)), tap(base + int2(1, 1)), f.x), f.y);
    } else {
        int2 base = int2(floor(at));
        float2 f = at - float2(base);
        float4 wx = catmullRom(f.x), wy = catmullRom(f.y);
        value = float3(0, 0, 0);
        for (int j = 0; j < 4; ++j) {
            float3 row = float3(0, 0, 0);
            for (int i = 0; i < 4; ++i) row += wx[i] * tap(base + int2(i - 1, j - 1));
            value += wy[j] * row;
        }
    }
    value = finish(value);
    int2 out_p = p + constants.target_origin;
#if defined(PREPARE)
    target[out_p] = float4(value, 1.0);
#else
    target[out_p] = float4(saturate(value), target[out_p].a);
#endif
}
