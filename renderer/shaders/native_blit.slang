Texture2D<float4> sourceImage : register(t0);
SamplerState sourceSampler : register(s0);
struct Blit { float4 uvRect; uint channels; };
ConstantBuffer<Blit> blit : register(b0);
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Output vertexMain(uint id : SV_VertexID) {
    Output output;
    float2 uv = float2((id << 1) & 2, id & 2);
    output.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    output.uv = blit.uvRect.xy + uv * blit.uvRect.zw;
    return output;
}
float4 fragmentMain(Output input) : SV_Target0 {
    float4 color = sourceImage.SampleLevel(sourceSampler, input.uv, 0);
    if (blit.channels == 1) return color.aaaa;
    if (blit.channels == 2) color = float4(round(saturate(color.rgb) * float3(31,63,31)) / float3(31,63,31), 1.0);
    return color;
}
