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
};
struct Projection { float4 rows[4]; };
[[vk::push_constant]] ConstantBuffer<Projection> projection;
struct Output {
    float4 position : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 texcoords[8] : TEXCOORD0;
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
Output vertexMain(Input input) {
    Output output;
    float4 objectPosition = float4(input.position, 1.0);
    float4 viewPosition = float4(dot(input.row0, objectPosition), dot(input.row1, objectPosition), dot(input.row2, objectPosition), 1.0);
    output.position = float4(dot(projection.rows[0], viewPosition), dot(projection.rows[1], viewPosition), dot(projection.rows[2], viewPosition), dot(projection.rows[3], viewPosition));
    float3 normal = dot(input.normal,input.normal)>0.0 ? normalize(input.normal) : float3(0.0);
    output.color = shadeChannel(input.color,0,viewPosition.xyz,normal);
    output.color1 = shadeChannel(input.color1,1,viewPosition.xyz,normal);
    [unroll] for (uint id = 0; id < 8; ++id) {
        uint source = drawState.texSources[id / 4][id % 4] & 7;
        float4 uv = float4(input.texcoords[source], 1.0, 1.0);
        output.texcoords[id] = float2(dot(drawState.texRows[2 * id], uv),
                                    dot(drawState.texRows[2 * id + 1], uv));
    }
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
float4 fragmentMain(Output input) : SV_Target0 {
    float4 result=evaluateTev(input.texcoords,input.color,input.color1);
    uint alpha=uint(round(saturate(result.a)*255.0)), word=drawState.pixel.x;
    bool first=alphaCompare(alpha,word&255,(word>>16)&7);
    bool second=alphaCompare(alpha,(word>>8)&255,(word>>19)&7);
    uint operation=(word>>22)&3;
    bool pass=operation==0 ? first&&second : operation==1 ? first||second : operation==2 ? first!=second : first==second;
    if(!pass) discard;
    if(destinationAlphaPass!=0) result.a=float(drawState.pixel.z&255)/255.0;
    return result;
}
