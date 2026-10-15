// Native TEV: specialization constants fold stage control and selectors.
struct NativeLight { float4 color; float4 cosine; float4 distance; float4 position; float4 direction; };
struct DrawState {
    float4 registers[4];
    float4 konst[4];
    float4 texRows[24];
    uint4 texGens[2];
    uint4 texPosts[2];
    float4 postRows[24];
    uint4 pixel;
    NativeLight lights[8];
    float4 materials[2];
    float4 ambient[2];
    uint4 channels;
    uint4 fog[2];
    uint4 indirect[3];
    uint4 indCmd[4];
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
// GX TEV stage in hardware integers: A/B/C use their low 8 bits, C extends to 0..256, D is signed 11-bit;
// the stage result is rounded to an integer step and clamped like the register it lands in.
float tevInteger(float af, float bf, float cf, float df, uint env) {
    int a = int(round(af * 255.0)) & 255, b = int(round(bf * 255.0)) & 255, c = int(round(cf * 255.0)) & 255;
    int d = clamp(int(round(df * 255.0)), -1024, 1023);
    c += c >> 7;
    int lerp = a * (256 - c) + b * c;
    bool subtract = ((env >> 18) & 1) != 0;
    uint biasMode = (env >> 16) & 3, scaleMode = (env >> 20) & 3;
    int value = (d << 8) + (subtract ? -lerp : lerp) + (biasMode == 1 ? 128 << 8 : (biasMode == 2 ? -(128 << 8) : 0));
    if (scaleMode == 1) value <<= 1; else if (scaleMode == 2) value <<= 2; else if (scaleMode == 3) value >>= 1;
    return float((value + 128) >> 8) / 255.0; // +128 after scaling: best Dolphin same-frame match (G21)
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
        result = float3(tevInteger(a.r, b.r, c.r, d.r, env), tevInteger(a.g, b.g, c.g, d.g, env), tevInteger(a.b, b.b, c.b, d.b, env));
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
    } else result = tevInteger(a, b, c, d, env);
    return ((env >> 19) & 1) != 0 ? clamp(result, 0.0, 1.0) : clamp(result, -4.0, 4.0);
}
float2 imageSize(uint id) {
    uint w = 1, h = 1;
    switch (id) {
    case 0: images[0].GetDimensions(w, h); break;
    case 1: images[1].GetDimensions(w, h); break;
    case 2: images[2].GetDimensions(w, h); break;
    case 3: images[3].GetDimensions(w, h); break;
    case 4: images[4].GetDimensions(w, h); break;
    case 5: images[5].GetDimensions(w, h); break;
    case 6: images[6].GetDimensions(w, h); break;
    case 7: images[7].GetDimensions(w, h); break;
    }
    return float2(w, h);
}
uint indirectWord(uint index) { return drawState.indirect[index / 4][index % 4]; }
int signed11(uint value) { return int(value << 21) >> 21; }
int indirectWrap(int coord, uint mode) { return mode == 0 ? coord : (mode < 6 ? (coord & int(0xFFFEu >> mode)) : 0); }
// GX indirect texturing in hardware fixed point: texcoords are texels*128, matrices s.10, scale 2^(e-17).
float2 indirectCoord(uint map, float2 coord, float2 uv[8], uint command, inout int2 previous) {
    float2 size = imageSize(map);
    int2 fixedUV = int2(floor(coord * size * 128.0));
    uint stage = command & 3, format = (command >> 2) & 3, bias = (command >> 4) & 7, matrix = (command >> 9) & 15;
    uint iref = indirectWord(9), scale = indirectWord(10 + stage / 2) >> ((stage & 1) * 8);
    uint indirectMap = (iref >> (stage * 6)) & 7, indirectCoordinate = (iref >> (stage * 6 + 3)) & 7;
    float2 indirectSize = imageSize(indirectMap);
    int2 indirectUV = int2(floor(uv[indirectCoordinate] * indirectSize * 128.0));
    indirectUV = int2(indirectUV.x >> (scale & 15), indirectUV.y >> ((scale >> 4) & 15));
    float4 texel = sampleImage(indirectMap, (float2(indirectUV) + 0.5) / (indirectSize * 128.0));
    int3 offset = int3(round(float3(texel.a, texel.b, texel.g) * 255.0));
    if (format == 0) offset += int3((bias & 1) != 0 ? -128 : 0, (bias & 2) != 0 ? -128 : 0, (bias & 4) != 0 ? -128 : 0);
    else {
        int shift = format == 1 ? 3 : (format == 2 ? 4 : 5);
        offset = int3(offset.x >> shift, offset.y >> shift, offset.z >> shift) + int3((bias & 1) != 0 ? 1 : 0, (bias & 2) != 0 ? 1 : 0, (bias & 4) != 0 ? 1 : 0);
    }
    int2 translation = int2(0);
    uint id = matrix & 3;
    if (id != 0) {
        uint a = indirectWord((id - 1) * 3), c = indirectWord((id - 1) * 3 + 1), e = indirectWord((id - 1) * 3 + 2);
        int exponent = int(((a >> 22) & 3) | (((c >> 22) & 3) << 2) | (((e >> 22) & 3) << 4));
        if (matrix < 4)
            translation = int2(signed11(a) * offset.x + signed11(c) * offset.y + signed11(e) * offset.z,
                               signed11(a >> 11) * offset.x + signed11(c >> 11) * offset.y + signed11(e >> 11) * offset.z) >> 3;
        else if (matrix < 8) translation = (fixedUV * offset.xx) >> 8;
        else translation = (fixedUV * offset.yy) >> 8;
        int shift = 17 - exponent;
        translation = shift >= 0 ? (translation >> shift) : (translation << (-shift));
    }
    int2 result = int2(indirectWrap(fixedUV.x, (command >> 13) & 7), indirectWrap(fixedUV.y, (command >> 16) & 7)) + translation;
    if (((command >> 20) & 1) != 0) result += previous;
    previous = result;
    return (float2(result) + 0.5) / (size * 128.0);
}
void tevStage(inout float4 r[4], float2 uv[8], float4 vertexColor, float4 vertexColor1,
              uint ce, uint ae, uint order, uint konst, uint stageIndex, inout int2 previousCoord) {
    float4 tex = float4(1.0);
    uint command = drawState.indCmd[stageIndex / 4][stageIndex % 4];
    if ((order & 64) != 0) {
        float2 coord = uv[(order >> 3) & 7];
        if (command != 0) coord = indirectCoord(order & 7, coord, uv, command, previousCoord);
        tex = sampleImage(order & 7, coord);
    }
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
    int2 previousCoord = int2(0);
    float4 result = r[0];
    if (stageCount > 0) {
        tevStage(r, uv, color, color1, colorEnv0, alphaEnv0, order0, konst0, 0, previousCoord);
        result = float4(r[(colorEnv0 >> 22) & 3].rgb, r[(alphaEnv0 >> 22) & 3].a);
    }
    if (stageCount > 1) {
        tevStage(r, uv, color, color1, colorEnv1, alphaEnv1, order1, konst1, 1, previousCoord);
        result = float4(r[(colorEnv1 >> 22) & 3].rgb, r[(alphaEnv1 >> 22) & 3].a);
    }
    if (stageCount > 2) {
        tevStage(r, uv, color, color1, colorEnv2, alphaEnv2, order2, konst2, 2, previousCoord);
        result = float4(r[(colorEnv2 >> 22) & 3].rgb, r[(alphaEnv2 >> 22) & 3].a);
    }
    if (stageCount > 3) {
        tevStage(r, uv, color, color1, colorEnv3, alphaEnv3, order3, konst3, 3, previousCoord);
        result = float4(r[(colorEnv3 >> 22) & 3].rgb, r[(alphaEnv3 >> 22) & 3].a);
    }
    if (stageCount > 4) {
        tevStage(r, uv, color, color1, colorEnv4, alphaEnv4, order4, konst4, 4, previousCoord);
        result = float4(r[(colorEnv4 >> 22) & 3].rgb, r[(alphaEnv4 >> 22) & 3].a);
    }
    if (stageCount > 5) {
        tevStage(r, uv, color, color1, colorEnv5, alphaEnv5, order5, konst5, 5, previousCoord);
        result = float4(r[(colorEnv5 >> 22) & 3].rgb, r[(alphaEnv5 >> 22) & 3].a);
    }
    if (stageCount > 6) {
        tevStage(r, uv, color, color1, colorEnv6, alphaEnv6, order6, konst6, 6, previousCoord);
        result = float4(r[(colorEnv6 >> 22) & 3].rgb, r[(alphaEnv6 >> 22) & 3].a);
    }
    if (stageCount > 7) {
        tevStage(r, uv, color, color1, colorEnv7, alphaEnv7, order7, konst7, 7, previousCoord);
        result = float4(r[(colorEnv7 >> 22) & 3].rgb, r[(alphaEnv7 >> 22) & 3].a);
    }
    if (stageCount > 8) {
        tevStage(r, uv, color, color1, colorEnv8, alphaEnv8, order8, konst8, 8, previousCoord);
        result = float4(r[(colorEnv8 >> 22) & 3].rgb, r[(alphaEnv8 >> 22) & 3].a);
    }
    if (stageCount > 9) {
        tevStage(r, uv, color, color1, colorEnv9, alphaEnv9, order9, konst9, 9, previousCoord);
        result = float4(r[(colorEnv9 >> 22) & 3].rgb, r[(alphaEnv9 >> 22) & 3].a);
    }
    if (stageCount > 10) {
        tevStage(r, uv, color, color1, colorEnv10, alphaEnv10, order10, konst10, 10, previousCoord);
        result = float4(r[(colorEnv10 >> 22) & 3].rgb, r[(alphaEnv10 >> 22) & 3].a);
    }
    if (stageCount > 11) {
        tevStage(r, uv, color, color1, colorEnv11, alphaEnv11, order11, konst11, 11, previousCoord);
        result = float4(r[(colorEnv11 >> 22) & 3].rgb, r[(alphaEnv11 >> 22) & 3].a);
    }
    if (stageCount > 12) {
        tevStage(r, uv, color, color1, colorEnv12, alphaEnv12, order12, konst12, 12, previousCoord);
        result = float4(r[(colorEnv12 >> 22) & 3].rgb, r[(alphaEnv12 >> 22) & 3].a);
    }
    if (stageCount > 13) {
        tevStage(r, uv, color, color1, colorEnv13, alphaEnv13, order13, konst13, 13, previousCoord);
        result = float4(r[(colorEnv13 >> 22) & 3].rgb, r[(alphaEnv13 >> 22) & 3].a);
    }
    if (stageCount > 14) {
        tevStage(r, uv, color, color1, colorEnv14, alphaEnv14, order14, konst14, 14, previousCoord);
        result = float4(r[(colorEnv14 >> 22) & 3].rgb, r[(alphaEnv14 >> 22) & 3].a);
    }
    if (stageCount > 15) {
        tevStage(r, uv, color, color1, colorEnv15, alphaEnv15, order15, konst15, 15, previousCoord);
        result = float4(r[(colorEnv15 >> 22) & 3].rgb, r[(alphaEnv15 >> 22) & 3].a);
    }
    return result;
}
