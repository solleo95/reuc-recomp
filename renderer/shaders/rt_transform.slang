// Ray tracing: one scene draw's vertices into view space for its BLAS (renderer/rt_scene.cpp). The position math is smoke.slang's vertex stage:
// the palette row is the vertex's own (gx::GpuMeshVertex::row) when the draw uses per-vertex matrices, else the draw's current row.
#ifdef UC_DIRECTX
struct Push { uint count; int firstVertex; uint palette; uint currentRow; uint indexed; };
ConstantBuffer<Push> push : register(b0);
ByteAddressBuffer vertices : register(t0);
StructuredBuffer<float4> palettes : register(t1);
RWByteAddressBuffer positions : register(u0);
#else
struct Push {
    uint* vertices;       // gx::GpuMeshVertex, 27 words each: position at words 0..2, row at word 24
    float* positions;     // output: float3 per vertex, tightly packed
    float4* palettes;     // gx::PaletteBlock, 129 float4 each: position rows first
    uint count;
    int firstVertex;
    uint palette;
    uint currentRow;
    uint indexed;
};
[[vk::push_constant]] ConstantBuffer<Push> push;
#endif
static const uint kVertexWords = 27, kRowWord = 24, kPaletteRows = 129;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x;
    if (i >= push.count) return;
    const uint at = uint(push.firstVertex + int(i)) * kVertexWords;
#ifdef UC_DIRECTX
    const float4 objectPosition = float4(asfloat(vertices.Load3(at * 4)), 1.0);
    const uint row = push.indexed != 0 ? vertices.Load((at + kRowWord) * 4) : push.currentRow;
    const uint base = push.palette * kPaletteRows;
    const float4 row0 = palettes[base + row], row1 = palettes[base + row + 1], row2 = palettes[base + row + 2];
    positions.Store3(12 * i, asuint(float3(dot(row0, objectPosition), dot(row1, objectPosition), dot(row2, objectPosition))));
#else
    const float4 objectPosition = float4(asfloat(push.vertices[at]), asfloat(push.vertices[at + 1]), asfloat(push.vertices[at + 2]), 1.0);
    const uint row = push.indexed != 0 ? push.vertices[at + kRowWord] : push.currentRow;   // validated on the CPU: row <= 61, rows loaded
    const uint base = push.palette * kPaletteRows;
    const float4 row0 = push.palettes[base + row], row1 = push.palettes[base + row + 1], row2 = push.palettes[base + row + 2];
    push.positions[3 * i] = dot(row0, objectPosition);
    push.positions[3 * i + 1] = dot(row1, objectPosition);
    push.positions[3 * i + 2] = dot(row2, objectPosition);
#endif
}
