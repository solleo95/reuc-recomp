// The bridge around DLSS 5 Neural Rendering (renderer/dlss5_pass.cpp; renderer/dlss5_math.h has the same formulas on the CPU).
// PREPARE: a region of the game's RGBA8 picture -> stored value -> signal L (the chosen Encoding; 1.0 = diffuse white) times the exposure gain -> Scaling Domain, resampled
//          to the processing size with the downsample filter -> back to L -> NR Signal Encoding, into an RGBA16F image.
// RESOLVE: the network's RGBA16F output -> L -> Scaling Domain, resampled to the picture's size with the upsample filter. Scale Reconstruction "RGB residual" resamples the
//          network's change (output minus the downsampled input in `original`) and adds it to the full-size picture; "processed image" resamples the output itself.
//          The result is divided by the gain and encoded back into the picture's RGBA8 region.
#if defined(UC_DIRECTX)
Texture2D<float4> source : register(t0);
RWTexture2D<float4> target : register(u0);
#if !defined(PREPARE)
Texture2D<float4> original : register(t1);
Texture2D<float4> targetRead : register(t2);
#endif
#elif defined(PREPARE)
[[vk::binding(0, 0)]] Texture2D<float4> source;                                          // RGBA8 picture
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> target;        // bridge image
#else
[[vk::binding(0, 0)]] Texture2D<float4> source;                                          // the network's output (bridge image)
[[vk::binding(1, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> target;          // RGBA8 picture
[[vk::binding(2, 0)]] Texture2D<float4> original;                                        // the network's input at the processing size (residual mode)
#define targetRead target
#endif
struct Constants {
    int2 source_size;     // texels of the region read
    int2 target_size;     // texels of the region written
    int2 source_origin;   // top-left of the region read
    int2 target_origin;   // top-left of the region written
    float gain;           // pre-network exposure gain (dlss5_pre_gain)
    float white;          // diffuse white in nits
    int encoding;         // 0 auto, 1 linear BT.709, 2 sRGB, 3 HDR10, 4 scRGB, 5 scRGB-nl
    int nr_encoding;      // 0 sRGB, 1 PQ SDR, 2 linear SDR
    int domain;           // 0 scRGB-nl, 1 signed PQ, 2 linear
    int filter;           // 0 nearest, 1 bilinear, 2 bicubic (Catmull-Rom), 3 area
    int residual;         // RESOLVE: 1 = RGB residual
    int unused;
};
#ifdef UC_DIRECTX
ConstantBuffer<Constants> constants : register(b0);
#else
[[vk::push_constant]] ConstantBuffer<Constants> constants;
#endif

static const float kM1 = 0.1593017578125, kM2 = 78.84375, kC1 = 0.8359375, kC2 = 18.8515625, kC3 = 18.6875;
float3 sgn(float3 v) { return select(v < 0.0, float3(-1, -1, -1), float3(1, 1, 1)); }
float3 srgbEotf(float3 v) { float3 a = abs(v); return sgn(v) * select(a <= 0.04045, a / 12.92, pow((a + 0.055) / 1.055, 2.4)); }
float3 srgbOetf(float3 l) { float3 a = abs(l); return sgn(l) * select(a <= 0.0031308, a * 12.92, 1.055 * pow(a, 1.0 / 2.4) - 0.055); }
float3 pqOetf(float3 nits) { float3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), kM1); return pow((kC1 + kC2 * y) / (1.0 + kC3 * y), kM2); }
float3 pqEotf(float3 e) { float3 p = pow(clamp(e, 0.0, 1.0), 1.0 / kM2); return 10000.0 * pow(max(p - kC1, 0.0) / (kC2 - kC3 * p), 1.0 / kM1); }

float3 decodeSource(float3 stored) {
    if (constants.encoding == 1 || constants.encoding == 4) return stored;
    if (constants.encoding == 3) return pqEotf(stored) / constants.white;
    return srgbEotf(stored);
}
float3 encodeSource(float3 l) {
    if (constants.encoding == 1 || constants.encoding == 4) return l;
    if (constants.encoding == 3) return pqOetf(l * constants.white);
    return srgbOetf(l);
}
float3 nrEncode(float3 l) {
    if (constants.nr_encoding == 1) return sgn(l) * pqOetf(abs(l) * constants.white);
    if (constants.nr_encoding == 2) return l;
    return srgbOetf(l);
}
float3 nrDecode(float3 e) {
    if (constants.nr_encoding == 1) return sgn(e) * pqEotf(abs(e)) / constants.white;
    if (constants.nr_encoding == 2) return e;
    return srgbEotf(e);
}
float3 domainTo(float3 l) {
    if (constants.domain == 1) return sgn(l) * pqOetf(abs(l) * constants.white);
    if (constants.domain == 2) return l;
    return srgbOetf(l);
}
float3 domainFrom(float3 d) {
    if (constants.domain == 1) return sgn(d) * pqEotf(abs(d)) / constants.white;
    if (constants.domain == 2) return d;
    return srgbEotf(d);
}

int2 clampSource(int2 p) { return clamp(p, int2(0, 0), constants.source_size - 1); }
// The value the filters average at source pixel p, in the Scaling Domain.
float3 tap(int2 p) {
    int3 q = int3(clampSource(p) + constants.source_origin, 0);
#if defined(PREPARE)
    return domainTo(constants.gain * decodeSource(source.Load(q).rgb));
#else
    float3 d = domainTo(nrDecode(source.Load(q).rgb));
    if (constants.residual != 0) d -= domainTo(nrDecode(original.Load(q).rgb));
    return d;
#endif
}
float4 catmullRom(float t) {   // weights of the four taps around t in [0, 1)
    float t2 = t * t, t3 = t2 * t;
    return float4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    int2 p = int2(id.xy);
    if (any(p >= constants.target_size)) return;
    float2 ratio = float2(constants.source_size) / float2(constants.target_size);
    float2 at = (float2(p) + 0.5) * ratio - 0.5;   // source position of the pixel's centre
    float3 value = float3(0, 0, 0);
    int filter = constants.filter;
    if (filter == 3 && (ratio.x < 1.0 || ratio.y < 1.0)) filter = 1;   // the area filter averages when shrinking; enlarging falls back to bilinear
    if (filter == 0) {
        value = tap(int2(floor(at + 0.5)));
    } else if (filter == 1) {
        int2 base = int2(floor(at));
        float2 f = at - float2(base);
        value = lerp(lerp(tap(base), tap(base + int2(1, 0)), f.x), lerp(tap(base + int2(0, 1)), tap(base + int2(1, 1)), f.x), f.y);
    } else if (filter == 2) {
        int2 base = int2(floor(at));
        float2 f = at - float2(base);
        float4 wx = catmullRom(f.x), wy = catmullRom(f.y);
        for (int j = 0; j < 4; ++j) {
            float3 row = float3(0, 0, 0);
            for (int i = 0; i < 4; ++i) row += wx[i] * tap(base + int2(i - 1, j - 1));
            value += wy[j] * row;
        }
    } else {   // area: the exact box of the target pixel over the source pixels it covers
        float2 lo = (float2(p)) * ratio, hi = (float2(p) + 1.0) * ratio;
        int2 first = int2(floor(lo)), last = min(int2(ceil(hi)) - 1, first + 3);
        float total = 0.0;
        for (int y = first.y; y <= last.y; ++y) {
            float wy = min(float(y + 1), hi.y) - max(float(y), lo.y);
            for (int x = first.x; x <= last.x; ++x) {
                float w = wy * (min(float(x + 1), hi.x) - max(float(x), lo.x));
                value += w * tap(int2(x, y));
                total += w;
            }
        }
        value /= max(total, 1e-6);
    }
    int2 out_p = p + constants.target_origin;
#if defined(PREPARE)
    target[out_p] = float4(nrEncode(domainFrom(value)), 1.0);
#else
    float3 base = float3(0, 0, 0);
    if (constants.residual != 0) base = domainTo(constants.gain * decodeSource(targetRead[out_p].rgb));
    float3 l = domainFrom(base + value) / constants.gain;
    target[out_p] = float4(saturate(encodeSource(l)), targetRead[out_p].a);
#endif
}
