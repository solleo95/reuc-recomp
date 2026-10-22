// HDR10 encode of the finished SDR frame (renderer/hdr_pass.h). The math mirrors renderer/hdr_math.h (tested there).
// Gamma 2.2 decode (not the piecewise sRGB curve: its linear toe lifts the shadows, the "grey blacks" of SDR content in Windows HDR),
// luminance-only highlight expansion from paper white to the peak, the port's overlays at their own brightness, BT.709 -> BT.2020, PQ.
[[vk::binding(0, 0)]] Texture2D<float4> scene;
[[vk::binding(1, 0)]] Texture2D<float4> overlay;   // premultiplied (the ImGui blend over a cleared layer), the target's size
[[vk::binding(2, 0)]] Texture2D<float4> glow;   // the bloom's glow (renderer/bloom_pass.h): scene-referred linear light, 1.0 = paper white
[[vk::binding(3, 0)]] SamplerState glowSampler;
struct Constants { int2 origin; int2 size; int2 extent; float paper; float peak; float ui; uint has_overlay; float2 bloom_origin; float2 bloom_size; uint has_bloom; uint reserved; };
[[vk::push_constant]] ConstantBuffer<Constants> constants;

static const float kKnee = 0.6;   // kHdrKnee
static const float3x3 kBt709To2020 = float3x3(
    0.6274040, 0.3292820, 0.0433136,
    0.0690970, 0.9195400, 0.0113612,
    0.0163916, 0.0880132, 0.8955950);

float expand(float y) {
    y = saturate(y);
    if (constants.peak <= constants.paper) return min(y * constants.paper, constants.peak);
    float over = max(0.0, (y - kKnee) / (1.0 - kKnee));
    return (y + over * over * (constants.peak / constants.paper - 1.0)) * constants.paper;
}
float3 pq(float3 nits) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 y = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

struct VertexOut { float4 position : SV_Position; };
[shader("vertex")]
VertexOut vertexMain(uint id : SV_VulkanVertexID) {   // gl_VertexIndex as is: no DrawParameters capability (not enabled on the device)
    float2 uv = float2((id << 1) & 2, id & 2);   // one triangle over the target
    VertexOut output; output.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

[shader("fragment")]
float4 fragmentMain(float4 position : SV_Position) : SV_Target {
    int2 p = int2(position.xy);
    int2 source = constants.origin + p * constants.size / constants.extent;   // nearest, as the SDR present blit
    float3 lin = pow(saturate(scene.Load(int3(source, 0)).rgb), 2.2);
    float y = dot(lin, float3(0.2126, 0.7152, 0.0722));
    float3 nits = y > 0.0 ? lin * (expand(y) / y) : float3(0.0, 0.0, 0.0);
    if (constants.has_bloom != 0) {   // hdr_with_glow: the glow adds nits on top of the expansion; the peak clamp below keeps the hue
        float2 uv = constants.bloom_origin + position.xy / float2(constants.extent) * constants.bloom_size;   // position is the pixel centre
        if (all(uv >= 0.0) && all(uv <= 1.0)) nits += glow.SampleLevel(glowSampler, uv, 0).rgb * constants.paper;
    }
    float brightest = max(nits.r, max(nits.g, nits.b));
    if (brightest > constants.peak) nits *= constants.peak / brightest;   // saturated highlights keep their hue
    if (constants.has_overlay != 0) {
        float4 ui = overlay.Load(int3(p, 0));
        if (ui.a > 0.0) nits = nits * (1.0 - ui.a) + pow(saturate(ui.rgb / ui.a), 2.2) * constants.ui * ui.a;
    }
    return float4(pq(mul(kBt709To2020, nits)), 1.0);
}
