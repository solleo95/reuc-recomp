Texture2D<float> sourceDepth : register(t0);
struct Extents { uint2 source; uint2 target; };
ConstantBuffer<Extents> sizes : register(b0);
float4 vertexMain(uint id : SV_VertexID) : SV_Position {
    float2 uv=float2((id<<1)&2,id&2);
    return float4(uv.x*2-1,1-uv.y*2,0,1);
}
float fragmentMain(float4 position : SV_Position) : SV_Depth {
    // Nearest sampling uses pixel centres, including noninteger upscale ratios.
    uint2 pixel=((uint2(position.xy)*2+1)*sizes.source)/(sizes.target*2);
    return sourceDepth.Load(int3(min(pixel,sizes.source-1),0));
}
