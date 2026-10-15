#include "tev.slang"
[[vk::constant_id(66)]] const uint destinationAlphaPass=0;

struct Input {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float4 row0 : TEXCOORD0;
    [[vk::location(3)]] float4 row1 : TEXCOORD1;
    [[vk::location(4)]] float4 row2 : TEXCOORD2;
    [[vk::location(5)]] float2 texcoords[8] : TEXCOORD3;
    [[vk::location(13)]] float3 normal : NORMAL;
    [[vk::location(14)]] float4 color1 : COLOR1;
    [[vk::location(15)]] float3 objectNormal : NORMAL1;
};
struct Projection { float4 rows[4]; };
[[vk::push_constant]] ConstantBuffer<Projection> projection;
struct Output {
    float4 position : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float3 texcoords[8] : TEXCOORD0;
    [[vk::location(9)]] float4 color1 : COLOR1;
};
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
    float3 stq = float3(dot(drawState.texRows[3 * id], value), dot(drawState.texRows[3 * id + 1], value),
                        (gen & 2) != 0 ? dot(drawState.texRows[3 * id + 2], value) : 1.0);
    if ((drawState.texPosts[id / 4][id % 4] & 256) != 0 && dot(stq, stq) > 0.0) stq = normalize(stq);
    float4 post = float4(stq, 1.0);
    return float3(dot(drawState.postRows[3 * id], post), dot(drawState.postRows[3 * id + 1], post), dot(drawState.postRows[3 * id + 2], post));
}
Output vertexMain(Input input) {
    Output output;
    float4 objectPosition = float4(input.position, 1.0);
    float4 viewPosition = float4(dot(input.row0, objectPosition), dot(input.row1, objectPosition), dot(input.row2, objectPosition), 1.0);
    output.position = float4(dot(projection.rows[0], viewPosition), dot(projection.rows[1], viewPosition), dot(projection.rows[2], viewPosition), dot(projection.rows[3], viewPosition));
    float3 normal = dot(input.normal,input.normal)>0.0 ? normalize(input.normal) : float3(0.0);
    output.color = shadeChannel(input.color,0,viewPosition.xyz,normal);
    output.color1 = shadeChannel(input.color1,1,viewPosition.xyz,normal);
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
float4 fragmentMain(Output input) : SV_Target0 {
    float2 uv[8];
    [unroll] for(uint i=0;i<8;++i) uv[i]=input.texcoords[i].xy / (input.texcoords[i].z != 0.0 ? input.texcoords[i].z : 1.0);
    float4 result=evaluateTev(uv,input.color,input.color1);
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
