// Native TEV: specialization constants fold stage control and selectors.
struct NativeLight { float4 color; float4 cosine; float4 distance; float4 position; float4 direction; };
struct DrawState {
    float4 registers[4];
    float4 konst[4];
    float4 texRows[16];
    uint4 texSources[2];
    uint4 pixel;
    NativeLight lights[8];
    float4 materials[2];
    float4 ambient[2];
    uint4 channels;
};
[[vk::binding(2,0)]] ConstantBuffer<DrawState> drawState;
[[vk::binding(0,0)]] Texture2D<float4> images[8];
[[vk::binding(1,0)]] SamplerState imageSamplers[8];
[[vk::constant_id(0)]] const uint stageCount = 0;
[[vk::constant_id(1)]] const uint swapTables = 0;
[[vk::constant_id(2)]] const uint colorEnv0 = 0;
[[vk::constant_id(3)]] const uint alphaEnv0 = 0;
[[vk::constant_id(4)]] const uint order0 = 0;
[[vk::constant_id(5)]] const uint konst0 = 0;
[[vk::constant_id(6)]] const uint colorEnv1 = 0;
[[vk::constant_id(7)]] const uint alphaEnv1 = 0;
[[vk::constant_id(8)]] const uint order1 = 0;
[[vk::constant_id(9)]] const uint konst1 = 0;
[[vk::constant_id(10)]] const uint colorEnv2 = 0;
[[vk::constant_id(11)]] const uint alphaEnv2 = 0;
[[vk::constant_id(12)]] const uint order2 = 0;
[[vk::constant_id(13)]] const uint konst2 = 0;
[[vk::constant_id(14)]] const uint colorEnv3 = 0;
[[vk::constant_id(15)]] const uint alphaEnv3 = 0;
[[vk::constant_id(16)]] const uint order3 = 0;
[[vk::constant_id(17)]] const uint konst3 = 0;
[[vk::constant_id(18)]] const uint colorEnv4 = 0;
[[vk::constant_id(19)]] const uint alphaEnv4 = 0;
[[vk::constant_id(20)]] const uint order4 = 0;
[[vk::constant_id(21)]] const uint konst4 = 0;
[[vk::constant_id(22)]] const uint colorEnv5 = 0;
[[vk::constant_id(23)]] const uint alphaEnv5 = 0;
[[vk::constant_id(24)]] const uint order5 = 0;
[[vk::constant_id(25)]] const uint konst5 = 0;
[[vk::constant_id(26)]] const uint colorEnv6 = 0;
[[vk::constant_id(27)]] const uint alphaEnv6 = 0;
[[vk::constant_id(28)]] const uint order6 = 0;
[[vk::constant_id(29)]] const uint konst6 = 0;
[[vk::constant_id(30)]] const uint colorEnv7 = 0;
[[vk::constant_id(31)]] const uint alphaEnv7 = 0;
[[vk::constant_id(32)]] const uint order7 = 0;
[[vk::constant_id(33)]] const uint konst7 = 0;
[[vk::constant_id(34)]] const uint colorEnv8 = 0;
[[vk::constant_id(35)]] const uint alphaEnv8 = 0;
[[vk::constant_id(36)]] const uint order8 = 0;
[[vk::constant_id(37)]] const uint konst8 = 0;
[[vk::constant_id(38)]] const uint colorEnv9 = 0;
[[vk::constant_id(39)]] const uint alphaEnv9 = 0;
[[vk::constant_id(40)]] const uint order9 = 0;
[[vk::constant_id(41)]] const uint konst9 = 0;
[[vk::constant_id(42)]] const uint colorEnv10 = 0;
[[vk::constant_id(43)]] const uint alphaEnv10 = 0;
[[vk::constant_id(44)]] const uint order10 = 0;
[[vk::constant_id(45)]] const uint konst10 = 0;
[[vk::constant_id(46)]] const uint colorEnv11 = 0;
[[vk::constant_id(47)]] const uint alphaEnv11 = 0;
[[vk::constant_id(48)]] const uint order11 = 0;
[[vk::constant_id(49)]] const uint konst11 = 0;
[[vk::constant_id(50)]] const uint colorEnv12 = 0;
[[vk::constant_id(51)]] const uint alphaEnv12 = 0;
[[vk::constant_id(52)]] const uint order12 = 0;
[[vk::constant_id(53)]] const uint konst12 = 0;
[[vk::constant_id(54)]] const uint colorEnv13 = 0;
[[vk::constant_id(55)]] const uint alphaEnv13 = 0;
[[vk::constant_id(56)]] const uint order13 = 0;
[[vk::constant_id(57)]] const uint konst13 = 0;
[[vk::constant_id(58)]] const uint colorEnv14 = 0;
[[vk::constant_id(59)]] const uint alphaEnv14 = 0;
[[vk::constant_id(60)]] const uint order14 = 0;
[[vk::constant_id(61)]] const uint konst14 = 0;
[[vk::constant_id(62)]] const uint colorEnv15 = 0;
[[vk::constant_id(63)]] const uint alphaEnv15 = 0;
[[vk::constant_id(64)]] const uint order15 = 0;
[[vk::constant_id(65)]] const uint konst15 = 0;

float4 sampleImage(uint id, float2 uv) {
    switch (id) {
    case 0: return images[0].Sample(imageSamplers[0], uv);
    case 1: return images[1].Sample(imageSamplers[1], uv);
    case 2: return images[2].Sample(imageSamplers[2], uv);
    case 3: return images[3].Sample(imageSamplers[3], uv);
    case 4: return images[4].Sample(imageSamplers[4], uv);
    case 5: return images[5].Sample(imageSamplers[5], uv);
    case 6: return images[6].Sample(imageSamplers[6], uv);
    case 7: return images[7].Sample(imageSamplers[7], uv);
    }
    return float4(1.0);
}
float4 swapColor(float4 value, uint table) {
    uint bits = (swapTables >> (table * 8)) & 255;
    return float4(value[bits & 3], value[(bits >> 2) & 3],
                  value[(bits >> 4) & 3], value[(bits >> 6) & 3]);
}
float3 konstColor(uint selector) {
    if (selector < 8) return float3(float(8 - selector) / 8.0);
    if (selector >= 12 && selector < 16) return drawState.konst[selector - 12].rgb;
    if (selector >= 16) return float3(drawState.konst[(selector - 16) & 3][(selector - 16) >> 2]);
    return float3(0.0);
}
float konstAlpha(uint selector) {
    if (selector < 8) return float(8 - selector) / 8.0;
    if (selector >= 16) return drawState.konst[(selector - 16) & 3][(selector - 16) >> 2];
    return 0.0;
}
float3 colorSource(uint selector, float4 r[4], float4 tex, float4 ras, float3 k) {
    switch (selector) {
    case 0: return r[0].rgb;
    case 1: return float3(r[0].a);
    case 2: return r[1].rgb;
    case 3: return float3(r[1].a);
    case 4: return r[2].rgb;
    case 5: return float3(r[2].a);
    case 6: return r[3].rgb;
    case 7: return float3(r[3].a);
    case 8: return tex.rgb;
    case 9: return float3(tex.a);
    case 10: return ras.rgb;
    case 11: return float3(ras.a);
    case 12: return float3(1.0);
    case 13: return float3(0.5);
    case 14: return k;
    }
    return float3(0.0);
}
float alphaSource(uint selector, float4 r[4], float4 tex, float4 ras, float k) {
    switch (selector) {
    case 0: return r[0].a;
    case 1: return r[1].a;
    case 2: return r[2].a;
    case 3: return r[3].a;
    case 4: return tex.a;
    case 5: return ras.a;
    case 6: return k;
    }
    return 0.0;
}
// Compare inputs use their unsigned eight-bit TEV representation.
uint quantize(float value) { return uint(int(round(value * 255.0))) & 255; }
uint packedCompare(float3 value, uint mode) {
    uint result = quantize(value.r);
    if (mode >= 1) result |= quantize(value.g) << 8;
    if (mode >= 2) result |= quantize(value.b) << 16;
    return result;
}
float tevScale(uint env) {
    switch ((env >> 20) & 3) {
    case 1: return 2.0;
    case 2: return 4.0;
    case 3: return 0.5;
    }
    return 1.0;
}
float tevBias(uint env) {
    uint bias = (env >> 16) & 3;
    return bias == 1 ? 0.5 : (bias == 2 ? -0.5 : 0.0);
}
float3 combineColor(float3 a, float3 b, float3 c, float3 d, uint env) {
    float3 result;
    bool equal = ((env >> 18) & 1) != 0;
    if (((env >> 16) & 3) == 3) {
        uint mode = (env >> 20) & 3;
        if (mode == 3) {
            result = d + float3(
                (equal ? quantize(a.r) == quantize(b.r) : quantize(a.r) > quantize(b.r)) ? c.r : 0.0,
                (equal ? quantize(a.g) == quantize(b.g) : quantize(a.g) > quantize(b.g)) ? c.g : 0.0,
                (equal ? quantize(a.b) == quantize(b.b) : quantize(a.b) > quantize(b.b)) ? c.b : 0.0);
        } else {
            uint qa = packedCompare(a, mode);
            uint qb = packedCompare(b, mode);
            result = d + ((equal ? qa == qb : qa > qb) ? c : float3(0.0));
        }
    } else {
        float3 mix = a * (1.0 - c) + b * c;
        result = (d + (equal ? -mix : mix) + tevBias(env)) * tevScale(env);
    }
    return ((env >> 19) & 1) != 0 ? clamp(result, 0.0, 1.0) : clamp(result, -4.0, 4.0);
}
float combineAlpha(float a, float b, float c, float d, float3 colorA, float3 colorB, uint env) {
    bool equal = ((env >> 18) & 1) != 0;
    float result;
    if (((env >> 16) & 3) == 3) {
        uint mode = (env >> 20) & 3;
        uint qa = mode == 3 ? quantize(a) : packedCompare(colorA, mode);
        uint qb = mode == 3 ? quantize(b) : packedCompare(colorB, mode);
        result = d + ((equal ? qa == qb : qa > qb) ? c : 0.0);
    } else {
        float mix = a * (1.0 - c) + b * c;
        result = (d + (equal ? -mix : mix) + tevBias(env)) * tevScale(env);
    }
    return ((env >> 19) & 1) != 0 ? clamp(result, 0.0, 1.0) : clamp(result, -4.0, 4.0);
}
void tevStage(inout float4 r[4], float2 uv[8], float4 vertexColor, float4 vertexColor1,
              uint ce, uint ae, uint order, uint konst) {
    float4 tex = float4(1.0);
    if ((order & 64) != 0) tex = sampleImage(order & 7, uv[(order >> 3) & 7]);
    uint channel = (order >> 7) & 7;
    float4 ras = channel == 0 ? vertexColor : channel == 1 ? vertexColor1 : float4(0.0);
    tex = swapColor(tex, (ae >> 2) & 3);
    ras = swapColor(ras, ae & 3);
    float3 kc = konstColor(konst & 31);
    float ka = konstAlpha((konst >> 5) & 31);
    float3 a = colorSource((ce >> 12) & 15, r, tex, ras, kc);
    float3 b = colorSource((ce >> 8) & 15, r, tex, ras, kc);
    float3 c = colorSource((ce >> 4) & 15, r, tex, ras, kc);
    float3 d = colorSource(ce & 15, r, tex, ras, kc);
    float aa = alphaSource((ae >> 13) & 7, r, tex, ras, ka);
    float ab = alphaSource((ae >> 10) & 7, r, tex, ras, ka);
    float ac = alphaSource((ae >> 7) & 7, r, tex, ras, ka);
    float ad = alphaSource((ae >> 4) & 7, r, tex, ras, ka);
    float3 rgb = combineColor(a, b, c, d, ce);
    float alpha = combineAlpha(aa, ab, ac, ad, a, b, ae);
    r[(ce >> 22) & 3].rgb = rgb;
    r[(ae >> 22) & 3].a = alpha;
}
float4 evaluateTev(float2 uv[8], float4 color, float4 color1) {
    if (stageCount == 0) return color * images[0].Sample(imageSamplers[0], uv[0]);
    float4 r[4];
    [unroll] for (uint i = 0; i < 4; ++i) r[i] = drawState.registers[i];
    float4 result = r[0];
    if (stageCount > 0) {
        tevStage(r, uv, color, color1, colorEnv0, alphaEnv0, order0, konst0);
        result = float4(r[(colorEnv0 >> 22) & 3].rgb, r[(alphaEnv0 >> 22) & 3].a);
    }
    if (stageCount > 1) {
        tevStage(r, uv, color, color1, colorEnv1, alphaEnv1, order1, konst1);
        result = float4(r[(colorEnv1 >> 22) & 3].rgb, r[(alphaEnv1 >> 22) & 3].a);
    }
    if (stageCount > 2) {
        tevStage(r, uv, color, color1, colorEnv2, alphaEnv2, order2, konst2);
        result = float4(r[(colorEnv2 >> 22) & 3].rgb, r[(alphaEnv2 >> 22) & 3].a);
    }
    if (stageCount > 3) {
        tevStage(r, uv, color, color1, colorEnv3, alphaEnv3, order3, konst3);
        result = float4(r[(colorEnv3 >> 22) & 3].rgb, r[(alphaEnv3 >> 22) & 3].a);
    }
    if (stageCount > 4) {
        tevStage(r, uv, color, color1, colorEnv4, alphaEnv4, order4, konst4);
        result = float4(r[(colorEnv4 >> 22) & 3].rgb, r[(alphaEnv4 >> 22) & 3].a);
    }
    if (stageCount > 5) {
        tevStage(r, uv, color, color1, colorEnv5, alphaEnv5, order5, konst5);
        result = float4(r[(colorEnv5 >> 22) & 3].rgb, r[(alphaEnv5 >> 22) & 3].a);
    }
    if (stageCount > 6) {
        tevStage(r, uv, color, color1, colorEnv6, alphaEnv6, order6, konst6);
        result = float4(r[(colorEnv6 >> 22) & 3].rgb, r[(alphaEnv6 >> 22) & 3].a);
    }
    if (stageCount > 7) {
        tevStage(r, uv, color, color1, colorEnv7, alphaEnv7, order7, konst7);
        result = float4(r[(colorEnv7 >> 22) & 3].rgb, r[(alphaEnv7 >> 22) & 3].a);
    }
    if (stageCount > 8) {
        tevStage(r, uv, color, color1, colorEnv8, alphaEnv8, order8, konst8);
        result = float4(r[(colorEnv8 >> 22) & 3].rgb, r[(alphaEnv8 >> 22) & 3].a);
    }
    if (stageCount > 9) {
        tevStage(r, uv, color, color1, colorEnv9, alphaEnv9, order9, konst9);
        result = float4(r[(colorEnv9 >> 22) & 3].rgb, r[(alphaEnv9 >> 22) & 3].a);
    }
    if (stageCount > 10) {
        tevStage(r, uv, color, color1, colorEnv10, alphaEnv10, order10, konst10);
        result = float4(r[(colorEnv10 >> 22) & 3].rgb, r[(alphaEnv10 >> 22) & 3].a);
    }
    if (stageCount > 11) {
        tevStage(r, uv, color, color1, colorEnv11, alphaEnv11, order11, konst11);
        result = float4(r[(colorEnv11 >> 22) & 3].rgb, r[(alphaEnv11 >> 22) & 3].a);
    }
    if (stageCount > 12) {
        tevStage(r, uv, color, color1, colorEnv12, alphaEnv12, order12, konst12);
        result = float4(r[(colorEnv12 >> 22) & 3].rgb, r[(alphaEnv12 >> 22) & 3].a);
    }
    if (stageCount > 13) {
        tevStage(r, uv, color, color1, colorEnv13, alphaEnv13, order13, konst13);
        result = float4(r[(colorEnv13 >> 22) & 3].rgb, r[(alphaEnv13 >> 22) & 3].a);
    }
    if (stageCount > 14) {
        tevStage(r, uv, color, color1, colorEnv14, alphaEnv14, order14, konst14);
        result = float4(r[(colorEnv14 >> 22) & 3].rgb, r[(alphaEnv14 >> 22) & 3].a);
    }
    if (stageCount > 15) {
        tevStage(r, uv, color, color1, colorEnv15, alphaEnv15, order15, konst15);
        result = float4(r[(colorEnv15 >> 22) & 3].rgb, r[(alphaEnv15 >> 22) & 3].a);
    }
    return result;
}
