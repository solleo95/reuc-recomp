// The normal G-buffer texel (renderer/gbuffer.h is the CPU mirror): octahedral view normal in rg, fog factor in b, flags in a, all unorm16.
static const uint kGBufferHasNormal = 1, kGBufferLit = 2, kGBufferCutout = 16;   // cut-out: an alpha-tested draw (grates, nets, foliage): not a solid surface, neither shaded nor an occluder
float2 gbufferSignNotZero(float2 v) { return float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
float3 decodeGBufferNormal(float2 stored) {
    float2 p = stored * 2.0 - 1.0;
    float3 n = float3(p.x, p.y, 1.0 - abs(p.x) - abs(p.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * gbufferSignNotZero(n.xy);
    return normalize(n);
}
float4 encodeGBuffer(float3 normal, float fog, uint flags) {
    float2 octahedral = float2(0.0, 0.0);
    float sum = abs(normal.x) + abs(normal.y) + abs(normal.z);
    if (sum > 0.0) {
        float2 p = normal.xy / sum;
        if (normal.z < 0.0) p = (1.0 - abs(p.yx)) * gbufferSignNotZero(p);
        octahedral = p * 0.5 + 0.5;
    }
    return float4(octahedral, saturate(fog), float(flags) / 65535.0);
}
