#include "tev.slang"
TEV_CONST(66, destinationAlphaPass);

struct Input {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] uint matrixRow : BLENDINDICES;
    [[vk::location(5)]] float2 texcoords[8] : TEXCOORD3;
    [[vk::location(14)]] float4 color1 : COLOR1;
    [[vk::location(15)]] float3 objectNormal : NORMAL1;
    [[vk::location(3)]] uint2 texMatrixIds : BLENDINDICES1;   // eight per-vertex texture matrix ids, a byte each
};
// Texture matrices by id (gx::TextureBank), 3 rows each; read only when the draw has a bank (projection.bank != 0xFFFFFFFF).
struct TextureBank { float4 rows[126 * 3]; };
[[vk::binding(4, 0)]] StructuredBuffer<TextureBank> textureBanks;
// Position and normal matrices of a draw: 64 rows each (gx::PaletteBlock). A vertex stores only its row.
struct Palette { float4 position[64]; float4 normal[64]; uint4 loaded; };
[[vk::binding(3, 0)]] StructuredBuffer<Palette> palettes;
// alphaPass: ubershader only. motion/jitter/viewport: the MOTION variant (upscaling) only; jitter in target pixels, viewport = the draw's
// Vulkan viewport (x, y, width, height) in target pixels.
// effects: bit 0 = per-pixel lighting (uc::kEffectPerPixelLighting); the MOTION variant only.
struct Projection { float4 rows[4]; uint palette; uint currentRow; uint indexed; uint alphaPass; uint bank; uint motion; float2 jitter; float4 viewport; uint effects; };
[[vk::push_constant]] ConstantBuffer<Projection> projection;
// Last frame's matrices of this draw (gx::MotionTracker): kind 1 = previous palette + previous projection, kind 2 = camera only
// (prevRows = clip_to_prev_clip applied to the current clip position).
struct MotionBlock { float4 prevRows[4]; uint prevPalette; uint kind; uint2 unused; };
[[vk::binding(5, 0)]] StructuredBuffer<MotionBlock> motions;
struct Output {
    float4 position : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float3 texcoords[8] : TEXCOORD0;
    [[vk::location(9)]] float4 color1 : COLOR1;
#ifdef MOTION
    [[vk::location(10)]] float4 currentClip : TEXCOORD8;    // unjittered
    [[vk::location(11)]] float4 previousClip : TEXCOORD9;
    [[vk::location(12)]] float4 rawColor0 : TEXCOORD10;     // per-pixel lighting: the vertex colours the channels take as material and ambient, the view position and the unnormalised view normal
    [[vk::location(13)]] float4 rawColor1 : TEXCOORD11;
    [[vk::location(14)]] float3 viewPosition : TEXCOORD12;
    [[vk::location(15)]] float3 viewNormal : TEXCOORD13;
#endif
};
float4 clipOf(float4 rows[4], float4 v) { return float4(dot(rows[0], v), dot(rows[1], v), dot(rows[2], v), dot(rows[3], v)); }
float4 lightControl(float4 base, uint channel, uint control, float3 position, float3 normal) {
    int4 material = int4(round(((control & 1) != 0 ? base : drawState.materials[channel]) * 255.0));
    if((control & 2) == 0) return float4(material) / 255.0;
    int4 energy = int4(round(((control & 64) != 0 ? base : drawState.ambient[channel]) * 255.0));
    uint mask = ((control >> 2) & 15) | ((control >> 7) & 240);
    uint diffuse = (control >> 7) & 3;
    uint attenuation = (control >> 9) & 3;
    for(uint i=0; i<8; ++i) if((mask & (1u << i)) != 0) {
        NativeLight light = drawState.lights[i];
        float3 delta = light.position.xyz - position;
        float lengthSquared = dot(delta,delta);
        float distance = sqrt(lengthSquared);
        float3 direction = distance > 0.0 ? delta / distance : normal;
        float strength = 1.0;
        if(attenuation == 1) {
            float angle = dot(normal,direction) < 0.0 ? 0.0 : max(0.0,dot(normal,light.direction.xyz));
            float3 powers = float3(1.0,angle,angle*angle);
            float3 denominatorTerms = light.distance.xyz;
            if(diffuse != 0 && dot(denominatorTerms,denominatorTerms)>0.0) denominatorTerms=normalize(denominatorTerms);
            float denominator = dot(denominatorTerms,powers);
            strength = denominator > 0.0 ? max(0.0,dot(light.cosine.xyz,powers))/denominator : 0.0;
        } else if(attenuation == 3) {
            float angle = max(0.0,dot(direction,light.direction.xyz));
            float denominator = dot(light.distance.xyz,float3(1.0,distance,lengthSquared));
            strength = denominator > 0.0 ? max(0.0,dot(light.cosine.xyz,float3(1.0,angle,angle*angle)))/denominator : 0.0;
        }
        if(diffuse != 0) {
            float cosine = dot(direction,normal);
            strength *= diffuse == 2 ? max(0.0,cosine) : cosine;
        }
        energy += int4(round(light.color * strength));
    }
    energy = clamp(energy,0,255);
    return float4((material * (energy + (energy >> 7))) >> 8) / 255.0;
}
float4 shadeChannel(float4 base, uint channel, float3 position, float3 normal) {
    if(channel>=drawState.pixel.w) return float4(0.0);
    float4 color = lightControl(base,channel,drawState.channels[channel],position,normal);
    color.a = lightControl(base,channel,drawState.channels[channel+2],position,normal).a;
    return color;
}
// GX regular texgen (XF 0x1040+n): source row, AB11/ABC1 input, ST/STQ through the texture matrix,
// then the dual-texture post matrix (XF 0x1050+n, optional normalize). Q is divided per fragment.
float3 generateTexcoord(uint id, Input input) {
    uint gen = drawState.texGens[id / 4][id % 4], source = (gen >> 7) & 31;
    float4 value = float4(0.0, 0.0, 1.0, 1.0); // shortcut: colour/binormal sources and emboss types unused by RBUE08 (CPU logs them)
    if (source == 0) value.xyz = input.position;
    else if (source == 1) value.xyz = input.objectNormal;
    else if (source >= 5 && source <= 12) value.xy = input.texcoords[source - 5];
    if ((gen & 4) == 0) value.z = 1.0;
    float4 row0 = drawState.texRows[3 * id], row1 = drawState.texRows[3 * id + 1], row2 = drawState.texRows[3 * id + 2];
    uint matrixId = (input.texMatrixIds[id / 4] >> ((id % 4) * 8)) & 255;   // 255: this vertex has no index for the texgen
    if (projection.bank != 0xFFFFFFFF && matrixId < 126) {
        uint at = 3 * matrixId;
        row0 = textureBanks[projection.bank].rows[at]; row1 = textureBanks[projection.bank].rows[at + 1]; row2 = textureBanks[projection.bank].rows[at + 2];
    }
    float3 stq = float3(dot(row0, value), dot(row1, value), (gen & 2) != 0 ? dot(row2, value) : 1.0);
    if ((drawState.texPosts[id / 4][id % 4] & 256) != 0 && dot(stq, stq) > 0.0) stq = normalize(stq);
    float4 post = float4(stq, 1.0);
    return float3(dot(drawState.postRows[3 * id], post), dot(drawState.postRows[3 * id + 1], post), dot(drawState.postRows[3 * id + 2], post));
}
Output vertexMain(Input input) {
    Output output;
    float4 objectPosition = float4(input.position, 1.0);
    uint row = projection.indexed != 0 ? input.matrixRow : projection.currentRow;   // validated on the CPU: row <= 61, rows loaded
    float4 row0 = palettes[projection.palette].position[row];
    float4 row1 = palettes[projection.palette].position[row + 1];
    float4 row2 = palettes[projection.palette].position[row + 2];
    float4 viewPosition = float4(dot(row0, objectPosition), dot(row1, objectPosition), dot(row2, objectPosition), 1.0);
    output.position = float4(dot(projection.rows[0], viewPosition), dot(projection.rows[1], viewPosition), dot(projection.rows[2], viewPosition), dot(projection.rows[3], viewPosition));
#ifdef MOTION
    output.currentClip = output.position;
    output.previousClip = output.position;
    if (projection.motion != 0xFFFFFFFF) {
        MotionBlock m = motions[projection.motion];
        if (m.kind == 1) {
            float4 p0 = palettes[m.prevPalette].position[row], p1 = palettes[m.prevPalette].position[row + 1], p2 = palettes[m.prevPalette].position[row + 2];
            output.previousClip = clipOf(m.prevRows, float4(dot(p0, objectPosition), dot(p1, objectPosition), dot(p2, objectPosition), 1.0));
        } else if (m.kind == 2) output.previousClip = clipOf(m.prevRows, output.position);
    }
    output.position.xy += projection.jitter * 2.0 / projection.viewport.zw * output.position.w;
#endif
    // View-space normal: the loaded normal matrix applied to the object normal; without a loaded matrix the object normal stays (position-only fixtures).
    uint normalRow = row & 31;
    uint4 loaded = palettes[projection.palette].loaded;
    bool allLoaded = true;
    [unroll] for (uint i = 0; i < 3; ++i) { uint r = normalRow + i; uint word = r < 32 ? loaded.x : loaded.y; allLoaded = allLoaded && (((word >> (r & 31)) & 1u) != 0); }
    float3 viewNormal = input.objectNormal;
    if (allLoaded) viewNormal = float3(dot(palettes[projection.palette].normal[normalRow].xyz, input.objectNormal), dot(palettes[projection.palette].normal[normalRow + 1].xyz, input.objectNormal), dot(palettes[projection.palette].normal[normalRow + 2].xyz, input.objectNormal));
    float3 normal = dot(viewNormal,viewNormal)>0.0 ? normalize(viewNormal) : float3(0.0);
    output.color = shadeChannel(input.color,0,viewPosition.xyz,normal);
    output.color1 = shadeChannel(input.color1,1,viewPosition.xyz,normal);
#ifdef MOTION
    output.rawColor0 = input.color; output.rawColor1 = input.color1; output.viewPosition = viewPosition.xyz; output.viewNormal = viewNormal;
#endif
    [unroll] for (uint id = 0; id < 8; ++id) output.texcoords[id] = generateTexcoord(id, input);
    return output;
}
bool alphaCompare(uint value, uint reference, uint operation) {
    switch(operation) {
    case 0: return false;
    case 1: return value<reference;
    case 2: return value==reference;
    case 3: return value<=reference;
    case 4: return value>reference;
    case 5: return value!=reference;
    case 6: return value>=reference;
    default: return true;
    }
}
// GX BP fog float: 11-bit mantissa, 8-bit exponent, sign at bit 19.
float fogFloat(uint v) { return asfloat(((v>>19)&1u)<<31 | ((v>>11)&255u)<<23 | (v&2047u)<<12); }
float3 applyFog(float3 color, float depth) {
    uint fsel=(drawState.fog[0].w>>21)&7u;
    if(fsel==0) return color;
    uint z=uint(saturate(depth)*16777215.0);
    float a=fogFloat(drawState.fog[0].x), c=fogFloat(drawState.fog[0].w&0xFFFFFu);
    float ze=((drawState.fog[0].w>>20)&1u)==0 ? a*16777216.0/float(int(drawState.fog[0].y)-int(z>>(drawState.fog[0].z&31u))) : a*float(z)/16777216.0;
    float fog=saturate(ze-c);
    if(fsel==4) fog=1.0-exp2(-8.0*fog);
    else if(fsel==5) fog=1.0-exp2(-8.0*fog*fog);
    else if(fsel==6) fog=exp2(-8.0*(1.0-fog));
    else if(fsel==7) fog=exp2(-8.0*(1.0-fog)*(1.0-fog));
    int ifog=int(round(fog*256.0));
    uint rgb=drawState.fog[1].x;
    int3 fogColor=int3((rgb>>16)&255u,(rgb>>8)&255u,rgb&255u);
    int3 prev=int3(round(saturate(color)*255.0));
    return float3((prev*(256-ifog)+fogColor*ifog)>>8)/255.0;
}
struct Targets {
    float4 color : SV_Target0;
#ifdef MOTION
    float4 motion : SV_Target1;   // rg: previous minus current position in target pixels (unjittered); written only by depth-writing draws
#endif
};
float2 pixelOf(float4 clip) { return projection.viewport.xy + (clip.xy / clip.w * 0.5 + 0.5) * projection.viewport.zw; }
Targets fragmentMain(Output input) {
    Targets targets;
#ifdef MOTION
    targets.motion = float4(pixelOf(input.previousClip) - pixelOf(input.currentClip), 0.0, 0.0);
#endif
    targets.color = shadeFragment(input);
    return targets;
}
float4 shadeFragment(Output input) {
#ifdef UBER
    loadTevKey();
    destinationAlphaPass = projection.alphaPass;
#endif
    float2 uv[8];
    [unroll] for(uint i=0;i<8;++i) uv[i]=input.texcoords[i].xy / (input.texcoords[i].z != 0.0 ? input.texcoords[i].z : 1.0);
    float4 rasterColor0=input.color, rasterColor1=input.color1;
#ifdef MOTION
    if((projection.effects&1u)!=0) {   // the lighting channels again, at the pixel: the same GX equations on the interpolated normal and position
        const float3 normal = dot(input.viewNormal,input.viewNormal)>0.0 ? normalize(input.viewNormal) : float3(0.0);
        if(((drawState.channels[0]|drawState.channels[2])&2u)!=0) rasterColor0 = shadeChannel(input.rawColor0,0,input.viewPosition,normal);
        if(((drawState.channels[1]|drawState.channels[3])&2u)!=0) rasterColor1 = shadeChannel(input.rawColor1,1,input.viewPosition,normal);
    }
#endif
    float4 result=evaluateTev(uv,rasterColor0,rasterColor1);
    uint alpha=uint(round(saturate(result.a)*255.0)), word=drawState.pixel.x;
    bool first=alphaCompare(alpha,word&255,(word>>16)&7);
    bool second=alphaCompare(alpha,(word>>8)&255,(word>>19)&7);
    uint operation=(word>>22)&3;
    bool pass=operation==0 ? first&&second : operation==1 ? first||second : operation==2 ? first!=second : first==second;
    if(!pass) discard;
    result.rgb=applyFog(result.rgb,input.position.z);
    if(destinationAlphaPass!=0) result.a=float(drawState.pixel.z&255)/255.0;
    return result;
}
