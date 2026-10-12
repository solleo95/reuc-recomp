struct Input {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float4 row0 : TEXCOORD0;
    [[vk::location(3)]] float4 row1 : TEXCOORD1;
    [[vk::location(4)]] float4 row2 : TEXCOORD2;
    [[vk::location(5)]] float2 texcoord : TEXCOORD3;
};
struct Projection { float4 rows[4]; };
[[vk::push_constant]] ConstantBuffer<Projection> projection;
struct Output {
    float4 position : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 texcoord : TEXCOORD0;
};
Output vertexMain(Input input) {
    Output output;
    float4 objectPosition = float4(input.position, 1.0);
    float4 viewPosition = float4(dot(input.row0, objectPosition), dot(input.row1, objectPosition), dot(input.row2, objectPosition), 1.0);
    output.position = float4(dot(projection.rows[0], viewPosition), dot(projection.rows[1], viewPosition), dot(projection.rows[2], viewPosition), dot(projection.rows[3], viewPosition));
    output.color = input.color;
    output.texcoord = input.texcoord;
    return output;
}
[[vk::binding(0,0)]] Texture2D<float4> image;
[[vk::binding(1,0)]] SamplerState imageSampler;
// shortcut: modulate one texture; replace with verified TEV state before game-image acceptance (TD-036).
float4 fragmentMain(Output input) : SV_Target0 { return input.color * image.Sample(imageSampler,input.texcoord); }
