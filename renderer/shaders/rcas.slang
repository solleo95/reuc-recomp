// Robust Contrast-Adaptive Sharpening, after AMD FidelityFX FSR 1 (ffx_fsr1.h, FsrRcasF), MIT License,
// Copyright (c) 2021-2022 Advanced Micro Devices, Inc. The constant is 2^-stops (renderer/upscale_math.h rcas_constant), as FSR's API maps
// its 0..1 sharpness. Reads a copy of the upscaled scene, writes the sharpened result back into the EFB region.
[[vk::binding(0, 0)]] Texture2D<float4> source : register(t0);
[[vk::binding(1, 0)]] [[vk::image_format("rgba8")]] RWTexture2D<float4> target : register(u0);
struct Constants { int2 size; float sharpness; float unused; };
[[vk::push_constant]] ConstantBuffer<Constants> constants : register(b0);

static const float kLimit = 0.25 - 1.0 / 16.0;   // FSR_RCAS_LIMIT: keeps the filter from ringing

float max3(float a, float b, float c) { return max(a, max(b, c)); }
float min3(float a, float b, float c) { return min(a, min(b, c)); }
float3 tap(int2 p) { return source.Load(int3(clamp(p, int2(0, 0), constants.size - 1), 0)).rgb; }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    int2 p = int2(id.xy);
    if (any(p >= constants.size)) return;
    //    b
    //  d e f
    //    h
    float3 b = tap(p + int2(0, -1)), d = tap(p + int2(-1, 0)), e = tap(p), f = tap(p + int2(1, 0)), h = tap(p + int2(0, 1));
    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));
    const float2 peak = float2(1.0, -4.0);
    float3 hitMin = min(mn4, e) / (4.0 * mx4);
    float3 hitMax = (peak.x - max(mx4, e)) / (4.0 * mn4 + peak.y);
    float3 lobeRgb = max(-hitMin, hitMax);
    float lobe = max(-kLimit, min(max3(lobeRgb.r, lobeRgb.g, lobeRgb.b), 0.0)) * constants.sharpness;
    float3 color = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
    target[p] = float4(saturate(color), source.Load(int3(p, 0)).a);
}
