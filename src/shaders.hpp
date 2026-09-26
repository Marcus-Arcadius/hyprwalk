#pragma once

// All GLSL lives here. Everything targets GLSL ES 3.00 (what Hyprland's EGL
// context guarantees); the external-texture variant needs the OES extension.

namespace h3d::shaders {

    // ---------------------------------------------------------------- capture
    // copies client surface textures into a panel texture, row 0 = top

    constexpr const char* CAPTURE_VS = R"(#version 300 es
layout(location = 0) in vec2 aPos;
uniform vec4 uBox;    // x, y, w, h in target pixels, y down
uniform vec2 uTarget; // target size in pixels
uniform vec4 uUV;     // uv top-left, uv bottom-right
out vec2 vUV;
void main() {
    vec2 p = uBox.xy + aPos * uBox.zw;
    gl_Position = vec4(p / uTarget * 2.0 - 1.0, 0.0, 1.0);
    vUV = mix(uUV.xy, uUV.zw, aPos);
}
)";

    constexpr const char* CAPTURE_FS = R"(#version 300 es
precision highp float;
in vec2 vUV;
uniform sampler2D uTex;
uniform float uOpaque; // 1 for RGBX buffers
uniform float uAlpha;
out vec4 fragColor;
void main() {
    vec4 c = texture(uTex, vUV);
    if (uOpaque > 0.5)
        c.a = 1.0;
    fragColor = c * uAlpha;
}
)";

    constexpr const char* CAPTURE_EXT_FS = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;
in vec2 vUV;
uniform samplerExternalOES uTex;
uniform float uOpaque;
uniform float uAlpha;
out vec4 fragColor;
void main() {
    vec4 c = texture(uTex, vUV);
    if (uOpaque > 0.5)
        c.a = 1.0;
    fragColor = c * uAlpha;
}
)";

    // ---------------------------------------------------------- panel lights
    // averages every panel into one texel of a 16x1 texture, which the world
    // shader then uses as the color of that panel's area light

    constexpr const char* LIGHT_VS = R"(#version 300 es
layout(location = 0) in vec2 aPos;
uniform float uIndex;
uniform float uCount; // width of the light texture
void main() {
    float x0 = uIndex / uCount * 2.0 - 1.0;
    float x1 = (uIndex + 1.0) / uCount * 2.0 - 1.0;
    gl_Position = vec4(mix(x0, x1, aPos.x), aPos.y * 2.0 - 1.0, 0.0, 1.0);
}
)";

    constexpr const char* LIGHT_FS = R"(#version 300 es
precision highp float;
uniform sampler2D uTex;
uniform vec2 uUVMax;
uniform float uLod;
out vec4 fragColor;
void main() {
    vec4 sum = vec4(0.0);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            vec2 uv = (vec2(x, y) + 0.5) / 4.0 * uUVMax;
            sum += textureLod(uTex, uv, uLod);
        }
    }
    sum /= 16.0;
    // un-premultiply so a mostly transparent panel still has a hue, keep the coverage in a
    fragColor = vec4(sum.rgb / max(sum.a, 0.001), sum.a);
}
)";

    // ------------------------------------------------------------------ common

    constexpr const char* NOISE_GLSL = R"(
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash12(i);
    float b = hash12(i + vec2(1.0, 0.0));
    float c = hash12(i + vec2(0.0, 1.0));
    float d = hash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
float fbm(vec2 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 5; ++i) {
        v += a * vnoise(p);
        p = p * 2.03 + vec2(17.1, 3.7);
        a *= 0.5;
    }
    return v;
}
vec3 srgbToLinear(vec3 c) { return pow(c, vec3(2.2)); }
vec3 linearToSrgb(vec3 c) { return pow(max(c, 0.0), vec3(1.0 / 2.2)); }
vec3 tonemap(vec3 x) {
    // ACES fit (Narkowicz)
    x *= 0.8;
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}
)";

    constexpr const char* SKY_COMMON_GLSL = R"(
uniform vec3 uSunDir;
vec3 skyColor(vec3 dir) {
    float h = clamp(dir.y, -1.0, 1.0);
    vec3 zenith  = vec3(0.16, 0.36, 0.78);
    vec3 horizon = vec3(0.78, 0.72, 0.62);
    vec3 ground  = vec3(0.42, 0.34, 0.26);
    vec3 c = h > 0.0 ? mix(horizon, zenith, pow(h, 0.55)) : mix(horizon, ground, pow(-h, 0.4));
    float s = max(dot(dir, uSunDir), 0.0);
    c += vec3(1.0, 0.8, 0.55) * (pow(s, 12.0) * 0.35 + pow(s, 900.0) * 18.0);
    return c;
}
)";

    // --------------------------------------------------------------------- sky

    constexpr const char* SKY_VS = R"(#version 300 es
layout(location = 0) in vec2 aPos;
out vec2 vNdc;
void main() {
    vNdc = aPos * 2.0 - 1.0;
    gl_Position = vec4(vNdc, 1.0, 1.0);
}
)";

    constexpr const char* SKY_FS_BODY = R"(
in vec2 vNdc;
uniform mat4 uInvViewProj;
uniform vec3 uEye;
uniform float uTime;
uniform float uExposure;
out vec4 fragColor;
void main() {
    vec4 far = uInvViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 dir = normalize(far.xyz / far.w - uEye);
    vec3 c = skyColor(dir);
    if (dir.y > 0.02) {
        // flat cloud layer
        vec2 p = dir.xz / dir.y * 0.35 + vec2(uTime * 0.004, uTime * 0.0015);
        float n = fbm(p * 1.6);
        float cov = smoothstep(0.48, 0.78, n);
        float fade = smoothstep(0.02, 0.25, dir.y);
        vec3 cloud = mix(vec3(0.95, 0.93, 0.9), vec3(1.0, 0.92, 0.8), max(dot(dir, uSunDir), 0.0));
        c = mix(c, cloud, cov * fade * 0.85);
    }
    fragColor = vec4(linearToSrgb(tonemap(srgbToLinear(c) * 1.6 * uExposure)), 1.0);
}
)";

    // ------------------------------------------------------------------- world

    constexpr const char* WORLD_VS = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in float aMat;
layout(location = 4) in float aAO;
uniform mat4 uViewProj;
uniform mat4 uSunViewProj;
out vec3 vPos;
out vec3 vNormal;
out vec2 vUV;
flat out int vMat;
out float vAO;
out vec4 vSun;
void main() {
    vPos = aPos;
    vNormal = aNormal;
    vUV = aUV;
    vMat = int(aMat + 0.5);
    vAO = aAO;
    vSun = uSunViewProj * vec4(aPos + aNormal * 0.03, 1.0);
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)";

    // sun, sky and panel light, shared by the built-in world and loaded maps
    // (needs SKY_COMMON_GLSL)
    constexpr const char* LIGHTING_GLSL = R"(
uniform highp sampler2DShadow uShadow;
uniform sampler2D uLightTex;
uniform vec3 uEye;
uniform int uLightCount;
uniform vec4 uLightA[16]; // center xyz, intensity
uniform vec4 uLightB[16]; // right xyz, half width
uniform vec4 uLightC[16]; // normal xyz, half height
uniform float uShadowBias; // in shadow map depth
uniform float uFog;        // density per meter
uniform float uExposure;   // eyes adjusting to dark places (loaded maps)

const vec3 SUN_COLOR = vec3(1.0, 0.9, 0.74) * 3.2;

float sunShadow(vec4 sunPos) {
    vec3 s = sunPos.xyz / sunPos.w * 0.5 + 0.5;
    if (s.x < 0.0 || s.x > 1.0 || s.y < 0.0 || s.y > 1.0 || s.z > 1.0)
        return 1.0;
    float sum = 0.0;
    vec2 texel = 1.0 / vec2(textureSize(uShadow, 0));
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            sum += texture(uShadow, vec3(s.xy + vec2(x, y) * texel * 1.25, s.z - uShadowBias));
    return sum / 9.0;
}

vec3 panelLights(vec3 P, vec3 N) {
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 16; ++i) {
        if (i >= uLightCount)
            break;
        vec3 c = uLightA[i].xyz;
        vec3 r = uLightB[i].xyz;
        vec3 n = uLightC[i].xyz;
        vec3 u = cross(n, r);
        float hw = uLightB[i].w, hh = uLightC[i].w;
        vec3 d = P - c;
        if (dot(d, n) < -0.01)
            continue; // behind the panel
        // representative point: closest point on the panel, pulled a bit towards the center
        vec3 q = c + r * clamp(dot(d, r), -hw, hw) + u * clamp(dot(d, u), -hh, hh);
        q = mix(q, c, 0.25);
        vec3 L = q - P;
        float d2 = max(dot(L, L), 1e-4);
        L *= inversesqrt(d2);
        float area = 4.0 * hw * hh;
        float E = area / (3.14159 * d2 + area);
        float facing = clamp(-dot(L, n), 0.0, 1.0);
        vec4 lc = texelFetch(uLightTex, ivec2(i, 0), 0);
        sum += srgbToLinear(lc.rgb) * lc.a * E * max(dot(N, L), 0.0) * (0.3 + 0.7 * facing) * uLightA[i].w;
    }
    return sum;
}

// the sky's light and the ground's, on a surface facing N
vec3 ambientLight(vec3 N) {
    vec3 skyAmb = srgbToLinear(vec3(0.55, 0.66, 0.85)) * 0.75;
    vec3 gndAmb = srgbToLinear(vec3(0.70, 0.56, 0.40)) * 0.45;
    return mix(gndAmb, skyAmb, N.y * 0.5 + 0.5);
}

// linear albedo in, linear radiance out; ambOcc darkens the sky light, lightOcc the panels'
vec3 shade(vec3 albedo, vec3 P, vec3 N, vec4 sunPos, float ambOcc, float lightOcc, float spec) {
    vec3 V = normalize(uEye - P);
    float ndl = max(dot(N, uSunDir), 0.0);
    float shadow = ndl > 0.0 ? sunShadow(sunPos) : 0.0;

    vec3 sunCol = SUN_COLOR;
    vec3 amb = ambientLight(N);

    vec3 H = normalize(uSunDir + V);
    float sp = spec * pow(max(dot(N, H), 0.0), 48.0) * ndl * shadow;

    vec3 light = sunCol * ndl * shadow + amb * ambOcc + panelLights(P, N) * lightOcc;
    return albedo * light + sunCol * sp;
}

// the light shade() lights an albedo with
vec3 shadeLight(vec3 P, vec3 N, vec4 sunPos, float ambOcc, float lightOcc) {
    float ndl = max(dot(N, uSunDir), 0.0);
    float shadow = ndl > 0.0 ? sunShadow(sunPos) : 0.0;
    return SUN_COLOR * ndl * shadow + ambientLight(N) * ambOcc + panelLights(P, N) * lightOcc;
}

// aerial perspective
vec3 applyFog(vec3 c, vec3 P) {
    vec3 V = normalize(uEye - P);
    float fog = 1.0 - exp(-length(uEye - P) * uFog);
    return mix(c, srgbToLinear(skyColor(-V)) * 1.3, fog * 0.6);
}
)";

    constexpr const char* WORLD_FS_BODY = R"(
in vec3 vPos;
in vec3 vNormal;
in vec2 vUV;
flat in int vMat;
in float vAO;
in vec4 vSun;
out vec4 fragColor;

// returns albedo (sRGB) and writes a roughness-ish specular weight
vec3 material(int m, vec2 uv, vec3 p, vec3 n, out float spec, out float cavity) {
    spec = 0.0;
    cavity = 1.0;
    if (m == 0) {
        // sand colored floor tiles, 1m, slightly irregular
        vec2 t = uv;
        vec2 id = floor(t);
        vec2 f = fract(t);
        float edge = min(min(f.x, 1.0 - f.x), min(f.y, 1.0 - f.y));
        float grout = smoothstep(0.012, 0.03, edge);
        float v = hash12(id) * 0.12;
        float n1 = fbm(t * 3.0) * 0.18 + fbm(t * 22.0) * 0.1;
        vec3 base = vec3(0.80, 0.69, 0.50) * (0.88 + v + n1 - 0.12);
        // loose sand drifting over the tiles
        float sand = smoothstep(0.45, 0.75, fbm(p.xz * 0.35 + 3.1));
        base = mix(base, vec3(0.86, 0.74, 0.54) * (0.95 + fbm(p.xz * 40.0) * 0.1), sand * 0.7);
        grout = mix(grout, 1.0, sand * 0.8);
        cavity = mix(0.55, 1.0, grout);
        return base * mix(0.6, 1.0, grout);
    }
    if (m == 1) {
        // sandstone blocks in running bond
        vec2 t = uv / vec2(0.8, 0.4);
        float row = floor(t.y);
        t.x += mod(row, 2.0) * 0.5;
        vec2 id = floor(t);
        vec2 f = fract(t);
        vec2 e2 = min(f, 1.0 - f) * vec2(0.8, 0.4);
        float edge = min(e2.x, e2.y);
        float mortar = smoothstep(0.008, 0.022, edge);
        float v = hash12(id + row * 7.0);
        float n1 = fbm(uv * 4.0 + v * 10.0) * 0.22 + fbm(uv * 30.0) * 0.08;
        vec3 base = mix(vec3(0.78, 0.62, 0.42), vec3(0.86, 0.72, 0.52), v) * (0.84 + n1);
        // weathering near the ground
        base *= mix(0.82, 1.0, smoothstep(0.0, 0.9, p.y));
        cavity = mix(0.5, 1.0, mortar);
        return mix(vec3(0.62, 0.53, 0.40), base, mortar);
    }
    if (m == 2) {
        // wooden crate: frame, planks and a diagonal brace
        vec2 f = uv;
        float border = min(min(f.x, 1.0 - f.x), min(f.y, 1.0 - f.y));
        float frame = 1.0 - smoothstep(0.1, 0.11, border);
        float diag = abs(f.x - f.y);
        float brace = (1.0 - smoothstep(0.06, 0.07, diag)) * (1.0 - frame);
        float plankId = floor(f.y * 5.0);
        float plankEdge = abs(fract(f.y * 5.0) - 0.5);
        float gap = smoothstep(0.47, 0.5, plankEdge) * (1.0 - frame) * (1.0 - brace);
        vec2 gp = frame + brace > 0.5 ? vec2(f.y * 3.0, f.x * 40.0) : vec2(f.x * 3.0, f.y * 40.0 + plankId * 13.0);
        float grain = fbm(gp) * 0.35 + vnoise(gp * vec2(1.0, 4.0)) * 0.1;
        vec3 wood = mix(vec3(0.46, 0.31, 0.17), vec3(0.62, 0.44, 0.25), grain + hash12(vec2(plankId, 3.0)) * 0.2);
        wood *= mix(1.0, 0.8, frame + brace);
        spec = 0.05;
        cavity = 1.0 - gap * 0.6;
        return wood * (1.0 - gap * 0.6);
    }
    // plaster
    float stain = fbm(uv * 1.2 + 11.0);
    float n1 = fbm(uv * 9.0) * 0.08;
    vec3 base = vec3(0.86, 0.78, 0.64) * (0.9 + n1) * mix(0.85, 1.0, smoothstep(0.35, 0.7, stain));
    base *= mix(0.85, 1.0, smoothstep(0.0, 0.6, p.y));
    return base;
}

void main() {
    vec3 N = normalize(vNormal);
    float spec, cavity;
    vec3 albedo = srgbToLinear(material(vMat, vUV, vPos, N, spec, cavity));
    vec3 c = shade(albedo, vPos, N, vSun, vAO * cavity, vAO, spec);
    fragColor = vec4(linearToSrgb(tonemap(applyFog(c, vPos))), 1.0);
}
)";

    // --------------------------------------------------------------------- map
    // loaded glTF maps: textured, with baked occlusion; normals may face either way

    constexpr const char* MAP_VS = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec2 aUV1;
layout(location = 4) in vec4 aColor;
layout(location = 5) in vec4 aAO; // w: the second layer's weight
layout(location = 6) in vec4 aTangent;
layout(location = 7) in vec3 aLight;
layout(location = 8) in uvec4 aLighting; // x: eMapLight
uniform mat4 uViewProj;
uniform mat4 uSunViewProj;
uniform vec3 uSunDir;
uniform float uNormalOffset;
uniform int uMode; // 0 lit, 1 unlit, 2 sky
out vec3 vPos;
out vec3 vNormal;
out vec2 vUV;
out vec2 vUV1;
out vec4 vColor;
out vec3 vAO;
out float vBlend;
out vec4 vSun;
out vec4 vTangent;
out vec3 vLight;
flat out int vLightMode;
void main() {
    vPos = aPos;
    vNormal = aNormal;
    vUV = aUV;
    vUV1 = aUV1;
    vColor = aColor;
    vAO = aAO.xyz;
    vBlend = aAO.w;
    vTangent = aTangent;
    vLight = aLight;
    vLightMode = int(aLighting.x);
    // push the shadow lookup off the surface, on the side the sun lights
    vec3 n = dot(aNormal, uSunDir) < 0.0 ? -aNormal : aNormal;
    vSun = uSunViewProj * vec4(aPos + n * uNormalOffset, 1.0);
    gl_Position = uViewProj * vec4(aPos, 1.0);
    if (uMode == 2)
        gl_Position.z = gl_Position.w * 0.99999; // sky: never clipped by the far plane
}
)";

    constexpr const char* MAP_FS_BODY = R"(
in vec3 vPos;
in vec3 vNormal;
in vec2 vUV;
in vec2 vUV1;
in vec4 vColor;
in vec3 vAO;
in float vBlend;
in vec4 vSun;
in vec4 vTangent;       // xyz and the bitangent's sign; 0: none
in vec3 vLight;         // the lightmap uv, or the probe atlas texel (vLightMode)
flat in int vLightMode; // eMapLight: 0 hypr3d's own, 1 lightmap, 2 light probes, 3 flat
// the material (texture units in setMaterial())
uniform sampler2D uBaseTex;        // sRGB
uniform sampler2D uEmissiveTex;    // sRGB
uniform sampler2D uOccTex;         // r: occlusion; g roughness and b metalness when uOrm
uniform sampler2D uLayerTex;       // sRGB
uniform sampler2D uLayerMaskTex;   // g (or a): where the layers meet, r: how soft the edge is
uniform sampler2D uNormalTex;
uniform sampler2D uLayerNormalTex; // the second layer's normal, roughness in alpha
uniform sampler2D uDetailTex;      // data (mod2x's middle grey is 0.5)
uniform sampler2D uDetailMaskTex;
uniform vec4 uBaseColor;
uniform vec4 uBaseXf;           // KHR_texture_transform, mat2 columns
uniform vec2 uBaseOffset;
uniform int uLayer;             // 0 none, 1 blended by vBlend, 2 by vBlend through the mask
uniform vec4 uLayerColor;
uniform vec4 uLayerXf;
uniform vec2 uLayerOffset;
uniform float uLayerSoftness;   // < 0: the mask's red channel
uniform int uLayerMaskChannel;  // 1: g, 3: a
uniform int uLayerNormal;
uniform vec3 uEmissive;
uniform int uEmissiveUV;
uniform vec4 uEmissiveXf;
uniform vec2 uEmissiveOffset;
uniform float uSelfIllumAlbedo; // the emissive color takes this much of the base color
uniform int uGlass;             // blended, but its reflections don't fade with its opacity
uniform int uVertexColor;       // vColor is: 0 linear, 1 sRGB, 2 not a color, 3 a tint as strong as its alpha
uniform int uOccUV;
uniform float uOccStrength;
uniform int uOrm;
uniform vec2 uRoughMetal;       // factors
uniform int uNormal;            // has a normal map: 1 rgb, 2 just x and y (BC5)
uniform float uNormalScale;
uniform float uNormalY;         // -1: green points down the bitangent (Source)
uniform vec2 uSpecular;         // from the sun, from the surroundings: 0 or 1
uniform int uDetail;            // 0 none, 1 mod2x, 2 overlay
uniform vec4 uDetailXf;
uniform vec2 uDetailOffset;
uniform vec3 uDetailTint;
uniform vec2 uDetailBlend;      // how much, how much at least where the mask says none
uniform int uDetailMask;        // 0 none, 1 the base color's uvs, 2 uv1
uniform int uAlphaMode; // 0 opaque, 1 mask, 2 blend
uniform int uBlendMode; // 0 by alpha, 1 mod2x, 2 added
// CS2's csgo_effects (clouds, dust, glows): its masks are in uLayerTex, uLayerMaskTex and uDetailMaskTex
uniform int uEffect;
uniform int uEffectMasks;
uniform vec4 uEffectMask[3];    // uv scale, scroll speed
uniform vec4 uEffectA;          // color boost, opacity, fogged (0/1)
uniform vec4 uEffectFade;       // distance, falloff, min, max
uniform vec4 uEffectFresnel;    // exponent, falloff, min, max
uniform float uTime;
uniform float uCutoff;
// the avatar's (unity2hypr3d's material extras, MToon's outlines)
uniform int uOutline;       // 0: the surface, else its outline
uniform vec4 uOutlineColor; // linear
uniform vec3 uOutlineMix;   // how much of the base color it takes, how much it's multiplied by it, how much it's shaded
uniform vec2 uOutlineTex;   // its color's texture (on the detail texture's unit): the color times it, and mixed towards it
uniform vec4 uOutlineTexXf; // its uv: mat2 columns
uniform vec2 uOutlineTexOffset;
uniform int uBack;          // back faces: 0 as the front, 1 uBackColor, 2 uBackColor times uLayerTex (avatars have no layers)
uniform vec4 uBackColor;
uniform vec4 uBackXf;
uniform vec2 uBackOffset;
uniform vec3 uLightClamp;   // UnlitWF's: the light's brightness kept between x and 1, 1 from y up (y 0: not), its chroma z
// toon shading (MToon, lilToon, UnlitWF, Poiyomi): the sun lights it from its shade color to its lit one as N·L goes
// from lo to hi; and a matcap. Their textures go where the layer mask and the detail mask would (an avatar has
// neither, and the samplers are all taken)
uniform int uToon;          // 0 none, 1 toon, 2 with the shade's texture (uLayerMaskTex)
uniform vec4 uToonShade;    // the shade's color (linear), and how much it's times the base color
uniform vec3 uToonStep;     // N·L where it's all shade, where it's all lit, how much of the shade shows
uniform int uMatcap;        // 0 none, else eMatcapMode + 1: added, multiplied, mixed in, lighter and darker (uDetailMaskTex)
uniform vec4 uMatcapColor;  // its color (the median's: how much lighter, 0 darker), how much of it
uniform float uMatcapLit;   // how much it's lit as the surface is (0: as if in full light, wherever it is)
uniform vec3 uViewUp;       // the camera's up
uniform mat4 uSunViewProj;  // (as the vertex shader has them)
uniform float uNormalOffset;
uniform float uAgain;       // > 0: drawn again where the stencil hid it, this much as opaque (MaskOut_Blend)
uniform int uMode;      // 0 lit, 1 unlit, 2 sky
// the game's own lighting (HYPR3D_lighting)
uniform int uBaked;
uniform sampler2D uIrradianceTex;  // RGB9E5
uniform sampler2D uDirectionalTex; // xy: main direction in tangent space, z: directionality, a: specular occlusion
uniform sampler2D uBakedShadowTex; // r: the sun's baked shadow
uniform int uBakedShadow;          // the lighting set has one (a sun without a baked shadow channel: only ours)
uniform highp sampler3D uProbeTex; // six blocks: light along +x +y +z -x -y -z (Source's axes); a: the sun's shadow
uniform vec3 uProbeDims;           // texels of a block
uniform vec3 uAmbient;             // for what has neither
uniform vec3 uSunColor;
uniform sampler2D uSkyTex;         // the sky panorama, sRGB
uniform int uHasSky;
uniform vec3 uSkyColor;            // its brightness
uniform vec3 uSkyAverage;          // light from all of the sky
uniform float uSkyLod;             // its coarsest mip for the fog
uniform vec4 uFogA;                // start, 1 / (end - start), exponent, max opacity (0: none)
uniform vec4 uFogB;                // height: offset, scale, exponent; lod bias
uniform vec2 uFogSpace;            // distance scale, height offset: the backdrop's own units
uniform vec4 uCurveA;              // tone curve: shoulder, linear strength, linear angle, toe strength
uniform vec4 uCurveB;              // toe numerator, toe denominator, white point, 1 / curve(white point)
#ifdef H3D_DUAL
// blending's second source (EXT_blend_func_extended): how much of what's behind stays, channel by channel
layout(location = 0, index = 0) out vec4 fragColor;
layout(location = 0, index = 1) out vec4 fragKeep;
#else
out vec4 fragColor;
vec4 fragKeep;
#endif

vec3 srgbEncode(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}
vec3 srgbDecode(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

// the game's tone curve (an Uncharted 2 style one) after its exposure: CS2 scales the scene by 2.8 first
vec3 gameCurve(vec3 c) {
    c = min(c * 2.8, vec3(uCurveB.z));
    vec3 num = c * (uCurveA.x * c + uCurveA.y * uCurveA.z) + uCurveB.x * uCurveA.w;
    vec3 den = c * (uCurveA.x * c + uCurveA.y) + uCurveB.y * uCurveA.w;
    return clamp((num / den - uCurveB.x / uCurveB.y) * uCurveB.w, 0.0, 1.0);
}

// the sky panorama (the dome's lat-long mapping) in a direction
vec3 skyLight(vec3 d, float lod) {
    float u = atan(d.z, d.x) * 0.15915494;
    float v = acos(clamp(d.y, -1.0, 1.0)) * 0.31830989;
    return textureLod(uSkyTex, vec2(fract(u), v), lod).rgb * uSkyColor;
}

// CS2's cubemap fog: the blurred sky, more of it farther away (horizontally) and lower down
vec3 gameFog(vec3 c, vec3 P, out float opacity) {
    opacity = 0.0;
    if (uFogA.w <= 0.0)
        return c;
    vec3 d = P - uEye;
    float dist = length(d.xz) * uFogSpace.x;
    float x = pow(max((dist - uFogA.x) * uFogA.y, 0.0001), uFogA.z);
    float h = (P.y - uFogSpace.y) * uFogSpace.x;
    float y = pow(max(h * uFogB.y + uFogB.x, 0.0001), uFogB.z);
    float blend = clamp(x, 0.0, 1.0) * clamp(y, 0.0, 1.0);
    opacity = blend * uFogA.w;
    vec3 fog = uHasSky != 0 ? skyLight(normalize(d), clamp(1.0 - blend * uFogB.w, 0.0, 1.0) * uSkyLod) : uSkyAverage;
    return mix(c, fog, opacity);
}

// Valve's directional lightmap (ComputeLightmapShading): the irradiance leans towards where most of
// it comes from, as the normal map sees it (n in tangent space)
vec3 lightmapShading(vec3 irr, vec4 dir, vec3 n, vec3 Ng) {
    vec2 xy = dir.xy * 2.0 - 1.0;
    xy *= 0.99619 / max(0.99619, length(xy)); // at least 5 degrees up
    float cosT = sqrt(max(1.0 - dot(xy, xy), 0.0));
    // curvature per inch, as Source measures it
    float curv = length(fwidth(Ng)) / max(length(fwidth(vPos)) / 0.0254, 1e-6);
    float flatness = smoothstep(0.1, 0.01, curv) * 0.8;
    float zScale = mix(1.0, mix(0.1, 2.0, clamp((1.0 - cosT) * 1.5, 0.0, 1.0)), flatness);
    vec3 L = normalize(vec3(xy, cosT * zScale));
    vec3 nonDir = irr * clamp(dir.z, 0.0, 1.0);
    return max(dot(L, n), 0.0) * (irr - nonDir) / max(L.z, 0.05) + nonDir;
}

// Valve's GGX (the lights carry 1/pi, so D doesn't)
vec3 sunSpecular(vec3 N, vec3 V, vec3 L, float rough, vec3 F0) {
    vec3 H = normalize(V + L);
    float NoL = clamp(dot(N, L), 0.0, 1.0), NoH = clamp(dot(N, H), 0.0, 1.0), NoV = clamp(dot(N, V), 0.0, 1.0), VoH = clamp(dot(L, H), 0.0, 1.0);
    float a = max(rough * rough, 1e-4);
    float an = NoH * a;
    float k = min(a / (1.0 - NoH * NoH + an * an), 453.5);
    float vr = (rough + 1.0) * (rough + 1.0) / 8.0;
    float vis = 1.0 / (4.0 * max((NoL * (1.0 - vr) + vr) * (NoV * (1.0 - vr) + vr), 0.0001));
    vec3 F = mix(F0, vec3(1.0), pow(1.0 - VoH, 5.0));
    return k * k * vis * NoL * F;
}

// split-sum environment BRDF, analytic (Karis)
vec3 envBRDF(vec3 F0, float rough, float NoV) {
    vec4 r = rough * vec4(-1.0, -0.0275, -0.572, 0.022) + vec4(1.0, 0.0425, 1.04, -0.04);
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    vec2 AB = vec2(-1.04, 1.04) * a004 + r.zw;
    return F0 * AB.x + AB.y;
}

float luma(vec3 c) {
    return dot(c, vec3(0.2125, 0.7154, 0.0721));
}

// UnlitWF's anti-glare (calcLightColorFrag): the light's brightest channel p kept between uLightClamp.x and 1,
// reaching 1 at uLightClamp.y, its color uLightClamp.z as saturated; in the units the tone curve takes
vec3 clampLight(vec3 L) {
    float p = max(max(L.r, L.g), L.b);
    if (p <= 0.0)
        return vec3(uLightClamp.x);
    return mix(vec3(p), L, uLightClamp.z) * mix(clamp(p / uLightClamp.y, 0.0, 1.0), 1.0, uLightClamp.x) / p;
}

// the sun's shadow for toon shading: looked up a little towards the sun, past where a surface's own silhouette in the
// shadow map would shadow it (grazing, near where N·L is 0, its depth changes fast across a texel): its step is a
// clean line, as N·L draws it, not the shadow map's staircase. Shadows cast from farther away show as they are
float toonShadow() {
    return sunShadow(uSunViewProj * vec4(vPos + uSunDir * max(0.1, uNormalOffset * 3.0), 1.0));
}

// toon shading: the albedo the sun lights, the shade's where N·L is under uToonStep.x, the lit one over .y; a cast
// shadow takes it to the shade (as MToon has it)
vec3 toonAlbedo(vec3 albedo, vec2 uv, vec3 N, float sunVis) {
    vec3 dark = uToonShade.rgb * mix(vec3(1.0), albedo, uToonShade.a);
    if (uToon == 2)
        dark *= texture(uLayerMaskTex, uv).rgb;
    dark = mix(albedo, dark, uToonStep.z);
    float x = mix(-1.0, 1.0, (dot(N, uSunDir) * 0.5 + 0.5) * sunVis);
    return mix(dark, albedo, clamp((x - uToonStep.x) / max(uToonStep.y - uToonStep.x, 1e-4), 0.0, 1.0));
}

// the matcap: a texture looked up by the normal as the eye sees it (turned with the eye, not rolled with it, as MToon
// has it), over c; light: the light here, full: in full light
vec3 matcap(vec3 c, vec3 N, vec3 light, vec3 full) {
    vec3 V = normalize(uEye - vPos);
    vec3 up = uViewUp - V * dot(V, uViewUp);
    up = dot(up, up) > 1e-8 ? normalize(up) : vec3(0.0, 1.0, 0.0);
    vec2 uv = vec2(dot(cross(up, V), N), dot(up, N)) * 0.495 + 0.5; // (its rim: not the far edge's texels)
    vec3 t = texture(uDetailMaskTex, uv).rgb, m = t * uMatcapColor.rgb;
    vec3 lit = mix(full, light, uMatcapLit);
    float k = uMatcapColor.a;
    if (uMatcap == 1)
        return c + m * lit * k;
    if (uMatcap == 2)
        return c * mix(vec3(1.0), m, k);
    if (uMatcap == 3)
        return mix(c, m * lit, k);
    // UnlitWF's median: lighter where it's over mid grey, darker under, the color saying how much of which
    vec3 d = t - 0.2140;
    return c + mix(min(d, 0.0), max(d, 0.0), uMatcapColor.rgb) * lit * k;
}

void shade() {
    vec2 uv = mat2(uBaseXf.xy, uBaseXf.zw) * vUV + uBaseOffset;
    vec4 base = texture(uBaseTex, uv) * uBaseColor;
    if (uBack != 0 && !gl_FrontFacing) // UnlitWF's back faces: their own color (and texture) in place of the base's
        base.rgb = uBackColor.rgb * (uBack == 2 ? texture(uLayerTex, mat2(uBackXf.xy, uBackXf.zw) * vUV + uBackOffset).rgb : vec3(1.0));
    if (uEffect != 0) {
        // csgo_effects: the color, through its scrolling masks, less of it up close and edge on
        vec4 col = base * vColor;
        float o = col.a * uEffectA.y;
        if (uEffectMasks > 0)
            o *= texture(uLayerTex, uv * uEffectMask[0].xy + uEffectMask[0].zw * uTime).r;
        if (uEffectMasks > 1)
            o *= texture(uLayerMaskTex, uv * uEffectMask[1].xy + uEffectMask[1].zw * uTime).r;
        if (uEffectMasks > 2)
            o *= texture(uDetailMaskTex, uv * uEffectMask[2].xy + uEffectMask[2].zw * uTime).r;
        vec3 ray = vPos - uEye;
        float fres = clamp(abs(dot(normalize(ray), normalize(vNormal))), 0.0001, 1.0);
        fres = mix(uEffectFresnel.z, uEffectFresnel.w, clamp(pow(fres, uEffectFresnel.x) * uEffectFresnel.y, 0.0, 1.0));
        float fade = clamp(length(ray) * uFogSpace.x / max(uEffectFade.x, 0.0001), 0.0, 1.0);
        fade = pow(max(mix(uEffectFade.z, uEffectFade.w, fade), 1e-9), uEffectFade.y);
        o = clamp(o * fres * fade, 0.0, 1.0);
        vec3 c = col.rgb * uEffectA.x;
        float fogged;
        if (uEffectA.z > 0.5)
            c = uBaked != 0 ? gameFog(c, vPos, fogged) : applyFog(c, vPos);
        vec3 shown = uBaked != 0 ? srgbEncode(gameCurve(c * uExposure)) : linearToSrgb(tonemap(c * uExposure));
        fragColor = vec4(shown * o, uBlendMode == 2 ? 0.0 : o);
        return;
    }
    vec4 nrm = uNormal != 0 ? texture(uNormalTex, uv) : vec4(0.5, 0.5, 1.0, 1.0);
    vec4 orm = texture(uOccTex, uOccUV == 0 ? vUV : vUV1);
    float rough = uOrm != 0 ? orm.g * uRoughMetal.x : uRoughMetal.x;
    float metal = uOrm != 0 ? orm.b * uRoughMetal.y : uRoughMetal.y;
    if (uLayer > 0) {
        // Source 2's vertex paint: the mask sharpens the painted weight into its own pattern
        vec2 luv = mat2(uLayerXf.xy, uLayerXf.zw) * vUV + uLayerOffset;
        float t = vBlend;
        if (uLayer == 2) {
            vec4 mask = texture(uLayerMaskTex, luv);
            float edge = uLayerMaskChannel == 3 ? mask.a : mask.g;
            float soft = max(uLayerSoftness < 0.0 ? mask.r : uLayerSoftness, 0.002);
            t = smoothstep(max(edge - soft, 0.0), min(edge + soft, 1.0), t);
        }
        base = mix(base, texture(uLayerTex, luv) * uLayerColor, t);
        if (uLayerNormal != 0) {
            // Source 2 blends the textures, not the normals
            vec4 ln = texture(uLayerNormalTex, luv);
            nrm.rgb = mix(nrm.rgb, ln.rgb, t);
            rough = mix(rough, ln.a, t);
        }
    }
    if (uDetail > 0) {
        vec3 d = texture(uDetailTex, mat2(uDetailXf.xy, uDetailXf.zw) * vUV + uDetailOffset).rgb * uDetailTint;
        float mask = uDetailMask == 0 ? 1.0 : texture(uDetailMaskTex, uDetailMask == 1 ? vUV : vUV1).r;
        mask = uDetailBlend.x * max(mask, uDetailBlend.y);
        if (uDetail == 1)
            base.rgb *= max(mix(vec3(1.0), d * 1.9922, mask), 0.0);
        else {
            // overlay, done on the gamma encoded color
            vec3 g = srgbEncode(base.rgb);
            d *= 0.9961;
            vec3 over = mix(d * g * 2.0, 1.0 - (1.0 - d) * (1.0 - g) * 2.0, step(0.5, g));
            base.rgb = mix(base.rgb, srgbDecode(over), mask);
        }
    }
    if (uVertexColor == 0)
        base *= vColor;
    else if (uVertexColor == 1)
        base *= vec4(srgbDecode(vColor.rgb), vColor.a);
    else if (uVertexColor == 3)
        base.rgb *= mix(vec3(1.0), vColor.rgb, vColor.a);
    if (uOutline != 0) {
        // none where the texture is see-through (in place of UnlitWF's canceller), then the line's color
        if (base.a < (uAlphaMode == 1 ? uCutoff : uAlphaMode == 2 ? 0.5 : 0.0))
            discard;
        vec3 line = uOutlineColor.rgb;
        if (uOutlineTex.x + uOutlineTex.y > 0.0) {
            vec3 t = texture(uDetailTex, mat2(uOutlineTexXf.xy, uOutlineTexXf.zw) * vUV + uOutlineTexOffset).rgb;
            line   = mix(line, t, uOutlineTex.y) * mix(vec3(1.0), t, uOutlineTex.x);
        }
        base = vec4(mix(line, base.rgb, uOutlineMix.x) * mix(vec3(1.0), base.rgb, uOutlineMix.y), uOutlineColor.a);
    }
    if (uAlphaMode == 1 && base.a < uCutoff)
        discard;
    float a = uAlphaMode == 2 ? base.a : 1.0;
    if (uAgain > 0.0)
        a *= uAgain;

    if (uBaked != 0 && uMode != 0) {
        // unlit and the sky: straight through the game's fog and tone curve
        vec3 c = uMode == 2 ? base.rgb * uSkyColor : base.rgb;
        float fogged = 0.0;
        if (uMode == 1)
            c = gameFog(c, vPos, fogged);
        if (uBlendMode == 1) {
            // mod2x: the scene times twice the (gamma encoded) color, in the display's terms
            vec3 f = 2.0 * mix(vec3(0.5), srgbEncode(base.rgb), base.a);
            f = mix(f, vec3(1.0), fogged);
            fragColor = vec4(0.5 * pow(f, vec3(1.0 / 2.2)), 1.0);
            return;
        }
        fragColor = vec4(srgbEncode(gameCurve(c * uExposure)) * a, a);
        return;
    }
    if (uMode == 2) {
        fragColor = vec4(linearToSrgb(min(base.rgb * uExposure, vec3(1.0))) * a, a);
        return;
    }

    vec3 c;
    if (uMode == 1)
        c = applyFog(base.rgb, vPos);
    else {
        // light the side we look at, whichever way the mesh's normals point
        vec3 Nv = normalize(vNormal);
        vec3 Ng = cross(dFdx(vPos), dFdy(vPos));
        if (dot(Ng, uEye - vPos) < 0.0)
            Ng = -Ng;
        vec3 N = uOutline != 0 || dot(Nv, Ng) >= 0.0 ? Nv : -Nv; // (an outline: the surface's under it)
        // the normal map, in tangent space the way Source has it
        vec3 nTs = vec3(0.0, 0.0, 1.0);
        if (uNormal != 0 && dot(vTangent.xyz, vTangent.xyz) > 0.01) {
            nTs = nrm.rgb * 2.0 - 1.0;
            if (uNormal == 2)
                nTs.z = sqrt(max(1.0 - dot(nTs.xy, nTs.xy), 0.0));
            nTs.xy *= uNormalScale;
            nTs.y *= uNormalY;
            nTs = normalize(nTs);
            vec3 T = normalize(vTangent.xyz - Nv * dot(Nv, vTangent.xyz));
            vec3 B = (vTangent.w < 0.0 ? -1.0 : 1.0) * cross(Nv, T);
            N = normalize(T * nTs.x + B * nTs.y + N * nTs.z);
        }
        float occ = 1.0 + uOccStrength * (orm.r - 1.0);
        if (uBaked != 0) {
            // the game's lighting: what its lightmap or probes say came from everywhere but the sun,
            // then the sun through its baked shadow and ours
            vec3 V = normalize(uEye - vPos);
            vec3 albedo = base.rgb;
            vec3 diffuse = albedo * (1.0 - metal);
            vec3 F0 = mix(vec3(0.04), albedo, metal);
            vec3 indirect = uAmbient;
            float baked = 1.0, specOcc = 1.0;
            if (vLightMode == 1) {
                vec4 dir = texture(uDirectionalTex, vLight.xy);
                indirect = lightmapShading(texture(uIrradianceTex, vLight.xy).rgb, dir, nTs, Ng);
                specOcc = dir.a;
                baked = uBakedShadow != 0 ? 1.0 - textureLod(uBakedShadowTex, vLight.xy, 0.0).r : 1.0;
            } else if (vLightMode == 2) {
                vec3 p = vLight / vec3(uProbeDims.xy, uProbeDims.z * 6.0);
                vec3 ns = vec3(N.z, N.x, N.y); // Source's axes
                vec3 block = mix(vec3(0.0, 1.0, 2.0), vec3(3.0, 4.0, 5.0), step(ns, vec3(0.0))) / 6.0;
                vec3 w = ns * ns;
                vec4 px = textureLod(uProbeTex, p + vec3(0.0, 0.0, block.x), 0.0);
                indirect = px.rgb * w.x + textureLod(uProbeTex, p + vec3(0.0, 0.0, block.y), 0.0).rgb * w.y +
                    textureLod(uProbeTex, p + vec3(0.0, 0.0, block.z), 0.0).rgb * w.z;
                baked = 1.0 - textureLod(uProbeTex, p, 0.0).a;
            }
            float ndl = max(dot(N, uSunDir), 0.0);
            float sun = ndl > 0.0 && baked > 0.001 ? baked * sunShadow(vSun) : 0.0;
            // rough enough not to sparkle where the surface curves fast (Valve's specular antialiasing)
            float geoRough = sqrt(clamp(max(dot(dFdx(Ng), dFdx(Ng)), dot(dFdy(Ng), dFdy(Ng))), 0.0, 1.0));
            float r = max(rough, geoRough);
            float specAO = min(specOcc, occ);
            vec3 spec = vec3(0.0), glint = vec3(0.0);
            if (uSpecular.x > 0.5 && sun > 0.0)
                glint = sunSpecular(N, V, uSunDir, r, F0) * uSunColor * sun * (1.0 + F0 * 0.125 * pow(2.0 * r, 4.0) * max(dot(N, V), 0.0));
            if (uSpecular.y > 0.5 && uHasSky != 0) {
                // the surroundings: the sky, dimmed as much as the diffuse light is
                vec3 R = reflect(-V, N);
                float dim = clamp(luma(indirect) / max(luma(uSkyAverage), 1e-3), 0.0, 1.0);
                spec += skyLight(R, r * uSkyLod) * envBRDF(F0, r, max(dot(N, V), 0.0)) * dim;
            }
            bool toon = uToon != 0 && uOutline == 0, cap = uMatcap != 0 && uOutline == 0;
            vec3 here = vec3(0.0), full = vec3(0.0); // the matcap's light: here, and in full light
            if (toon || cap) {
                // the sun as far as it gets here (the baked shadow), and its cast shadow, however the surface faces
                float sunVis = baked > 0.001 ? (toon ? toonShadow() : sunShadow(vSun)) : 0.0;
                vec3 around = indirect + panelLights(vPos, N);
                vec3 alb = toon ? toonAlbedo(diffuse, uv, N, sunVis) : diffuse;
                if (uLightClamp.y > 0.0) {
                    // UnlitWF's: one light, clamped; toon shading shades it, not N·L
                    float k = uExposure * 2.8;
                    here = full = clampLight((uSunColor * (toon ? baked : ndl * sun) + around) * occ * k) / k;
                    c = alb * here;
                } else {
                    here = (uSunColor * baked * sunVis + around) * occ;
                    full = (uSunColor + around) * occ;
                    // toon: the sun on the shade too, a cast shadow shading it
                    c = toon ? (uSunColor * baked * alb + diffuse * around) * occ : diffuse * (uSunColor * ndl * sun + around) * occ;
                }
            } else if (uLightClamp.y > 0.0) {
                float k = uExposure * 2.8; // (as gameCurve() scales it)
                c = diffuse * clampLight((uSunColor * ndl * sun + indirect + panelLights(vPos, N)) * occ * k) / k;
            } else {
                c = diffuse * (uSunColor * ndl * sun + indirect) * occ;
                c += diffuse * panelLights(vPos, N) * occ;
            }
            if (cap)
                c = matcap(c, N, here, full);
            if (uOutline != 0)
                c = mix(albedo, c, uOutlineMix.z); // unshaded: its color as it is
            c += uEmissive * texture(uEmissiveTex, mat2(uEmissiveXf.xy, uEmissiveXf.zw) * (uEmissiveUV == 0 ? vUV : vUV1) + uEmissiveOffset).rgb *
                mix(vec3(1.0), albedo, uSelfIllumAlbedo);
            float fogged;
            if (uGlass != 0 && a < 1.0) {
                // its own color by its opacity and what it reflects of the surroundings, over a background it hides
                // by its opacity and by as much as it reflects (Fresnel): blended as one color at that cover, and
                // encoded before it's scaled by it, as the frame is (encoded after, a pane came out two or three times
                // too bright). The sun's glint then screens all that: it takes each channel g of the way to white, g
                // being how far it takes the pane's own light there on the screen. So a glint on clear glass reaches
                // white, not only the cover, and never darkens what's behind. With a second source what's behind is
                // kept by 1 - g channel by channel; else by the least of them (a glint's edge a little brighter)
                float cover = clamp(a + (1.0 - a) * luma(envBRDF(F0, r, max(dot(N, V), 0.0))), a, 1.0);
                vec3 own = c * a + spec * specAO;
                vec3 pane = srgbEncode(gameCurve(gameFog(own / cover, vPos, fogged) * uExposure)) * cover;
                vec3 unlit = srgbEncode(gameCurve(gameFog(own, vPos, fogged) * uExposure));
                vec3 lit = srgbEncode(gameCurve(gameFog(own + glint * specAO, vPos, fogged) * uExposure));
                vec3 g = clamp((lit - unlit) / max(1.0 - unlit, 1e-4), 0.0, 1.0);
#ifdef H3D_DUAL
                fragColor = vec4(g + (1.0 - g) * pane, cover);
                fragKeep = vec4((1.0 - g) * (1.0 - cover), 1.0 - cover);
#else
                fragColor = vec4(g + (1.0 - g) * pane, 1.0 - (1.0 - min(g.r, min(g.g, g.b))) * (1.0 - cover));
#endif
                return;
            }
            c = gameFog(c + (spec + glint) * specAO, vPos, fogged);
            fragColor = vec4(srgbEncode(gameCurve(c * uExposure)) * a, a);
            return;
        }
        float local = vAO.x * occ;
        bool toon = uToon != 0 && uOutline == 0, cap = uMatcap != 0 && uOutline == 0;
        vec3 here = vec3(0.0), full = vec3(0.0);
        if (toon || cap) {
            // as above, with hypr3d's own light: the sun, the sky, the panels, the sun bounced
            float sunVis = toon ? toonShadow() : sunShadow(vSun), ndl = max(dot(N, uSunDir), 0.0);
            vec3 around = ambientLight(N) * local * mix(0.45, 1.0, vAO.y) + panelLights(vPos, N) * local + SUN_COLOR * (vAO.z * 0.8 * occ);
            vec3 alb = toon ? toonAlbedo(base.rgb, uv, N, sunVis) : base.rgb;
            if (uLightClamp.y > 0.0) {
                here = full = clampLight((SUN_COLOR * (toon ? 1.0 : ndl * sunVis) + around) * uExposure) / uExposure;
                c = alb * here;
            } else {
                here = SUN_COLOR * sunVis + around;
                full = SUN_COLOR + around;
                c = toon ? SUN_COLOR * alb + base.rgb * around : base.rgb * (SUN_COLOR * ndl * sunVis + around);
            }
        } else if (uLightClamp.y > 0.0)
            c = base.rgb * clampLight((shadeLight(vPos, N, vSun, local * mix(0.45, 1.0, vAO.y), local) + SUN_COLOR * (vAO.z * 0.8 * occ)) * uExposure) /
                uExposure;
        else {
            c = shade(base.rgb, vPos, N, vSun, local * mix(0.45, 1.0, vAO.y), local, 0.0);
            // sunlight bounced off the surroundings (baked), off surfaces of about this albedo
            c += base.rgb * SUN_COLOR * (vAO.z * 0.8 * occ);
        }
        if (cap)
            c = matcap(c, N, here, full);
        c += uEmissive * texture(uEmissiveTex, mat2(uEmissiveXf.xy, uEmissiveXf.zw) * (uEmissiveUV == 0 ? vUV : vUV1) + uEmissiveOffset).rgb;
        c = tonemap(applyFog(c, vPos) * uExposure);
        if (uOutline != 0)
            c = mix(applyFog(base.rgb, vPos), c, uOutlineMix.z); // unshaded: its color as it is
    }
    fragColor = vec4(linearToSrgb(c) * a, a);
}

void main() {
    fragKeep = vec4(-1.0);
    shade();
    if (fragKeep.a < 0.0)
        fragKeep = vec4(1.0 - fragColor.a); // (as premultiplied alpha keeps it)
}
)";

    // alpha tested map geometry in the shadow pass
    constexpr const char* MAP_DEPTH_VS = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUV;
uniform mat4 uViewProj;
uniform vec4 uBaseXf;
uniform vec2 uBaseOffset;
out vec2 vUV;
void main() {
    vUV = mat2(uBaseXf.xy, uBaseXf.zw) * aUV + uBaseOffset;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)";

    constexpr const char* MAP_DEPTH_FS = R"(#version 300 es
precision highp float;
in vec2 vUV;
uniform sampler2D uBaseTex;
uniform float uAlphaScale;
uniform float uCutoff;
void main() {
    if (texture(uBaseTex, vUV).a * uAlphaScale < uCutoff)
        discard;
}
)";

    // ----------------------------------------------------------------- avatar
    // skinned: each joint's 3x4 matrix is 3 texels of uJoints, 64 joints a row

    constexpr const char* SKIN_GLSL = R"(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec2 aUV1;
layout(location = 4) in vec4 aColor;
layout(location = 5) in uvec4 aJoints;
layout(location = 6) in vec4 aWeights;
layout(location = 7) in vec3 aMorphPos; // what the morphs move it by
layout(location = 8) in vec3 aMorphNormal;
uniform highp sampler2D uJoints;
uniform mat4 uModel;
mat4 jointMatrix(uint j) {
    ivec2 p = ivec2(int(j % 64u) * 3, int(j / 64u));
    vec4 r0 = texelFetch(uJoints, p, 0);
    vec4 r1 = texelFetch(uJoints, p + ivec2(1, 0), 0);
    vec4 r2 = texelFetch(uJoints, p + ivec2(2, 0), 0);
    return transpose(mat4(r0, r1, r2, vec4(0.0, 0.0, 0.0, 1.0)));
}
mat4 skinMatrix() {
    mat4 s = jointMatrix(aJoints.x) * aWeights.x + jointMatrix(aJoints.y) * aWeights.y + jointMatrix(aJoints.z) * aWeights.z +
        jointMatrix(aJoints.w) * aWeights.w;
    return uModel * s;
}
)";

    // pairs with MAP_FS_BODY: it's lit like the map, with its sky and bounce from probes around the player
    constexpr const char* AVATAR_VS_BODY = R"(
uniform mat4 uViewProj;
uniform mat4 uSunViewProj;
uniform vec3 uSunDir;
uniform float uNormalOffset;
uniform float uSky;    // how much sky is around, 0..1
uniform float uBounce; // sunlight bounced off the surroundings
uniform int uLightMode;    // eMapLight: hypr3d's own (uSky, uBounce), the map's light probes, or its average light
uniform mat4 uProbeMatrix; // world -> the probe atlas (texels)
uniform vec3 uProbeMin, uProbeMax; // the volume's block of it, half a texel in
// an outline (the mesh again, pushed out: an inverted hull): 0 none, 1 in metres, 2 in NDC units (as wide on screen)
uniform int uOutline;
uniform vec4 uOutlineA;    // width, shift towards the eye (m), how much thinner up close, up to how far (m)
uniform vec4 uOutlineMask; // which of the mask's channels scales the width
uniform float uOutlineInvert;
uniform float uOutlineMaxW; // on screen: as wide up to this far, then thinner
uniform float uAspect;      // the output's height over its width
uniform sampler2D uOutlineMaskTex;
uniform vec3 uEye;
uniform vec4 uBaseXf; // the mask has the base color's uvs
uniform vec2 uBaseOffset;
out vec3 vPos;
out vec3 vNormal;
out vec2 vUV;
out vec2 vUV1;
out vec4 vColor;
out vec3 vAO;
out float vBlend;
out vec4 vSun;
out vec4 vTangent;
out vec3 vLight;
flat out int vLightMode;
void main() {
    mat4 m = skinMatrix();
    vBlend = 0.0;
    vTangent = vec4(0.0);
    vLightMode = uLightMode;
    vec4 p = m * vec4(aPos + aMorphPos, 1.0);
    vNormal = normalize(mat3(m) * (aNormal + aMorphNormal));
    float w = 0.0;
    if (uOutline != 0) {
        float k = dot(textureLod(uOutlineMaskTex, mat2(uBaseXf.xy, uBaseXf.zw) * aUV + uBaseOffset, 0.0), uOutlineMask);
        vec3 toEye = uEye - p.xyz;
        float d = length(toEye);
        w = uOutlineA.x * mix(k, 1.0 - k, uOutlineInvert) * mix(1.0, min(d, uOutlineA.w), uOutlineA.z);
        if (uOutline == 1)
            p.xyz += vNormal * w;
        p.xyz += toEye / max(d, 1e-4) * min(uOutlineA.y, d * 0.5); // along the view ray: only its depth changes
    }
    vPos = p.xyz;
    vUV = aUV;
    vUV1 = aUV1;
    vColor = aColor;
    vAO = vec3(1.0, uSky, uBounce);
    vLight = clamp((uProbeMatrix * p).xyz, uProbeMin, uProbeMax);
    vec3 n = dot(vNormal, uSunDir) < 0.0 ? -vNormal : vNormal;
    vSun = uSunViewProj * vec4(vPos + n * uNormalOffset, 1.0);
    gl_Position = uViewProj * p;
    if (uOutline == 2) {
        // MToon's screen width: along the normal as the screen shows it, none where it faces the eye
        vec2 sn = (uViewProj * vec4(vNormal, 0.0)).xy;
        float facing = abs(dot(vNormal, normalize(uEye - p.xyz)));
        gl_Position.xy += sn / max(length(sn), 1e-6) * vec2(uAspect, 1.0) * w * min(gl_Position.w, uOutlineMaxW) * (1.0 - facing);
    }
}
)";

    // pairs with MAP_DEPTH_FS
    constexpr const char* AVATAR_DEPTH_VS_BODY = R"(
uniform mat4 uViewProj;
uniform vec4 uBaseXf;
uniform vec2 uBaseOffset;
out vec2 vUV;
void main() {
    vUV = mat2(uBaseXf.xy, uBaseXf.zw) * aUV + uBaseOffset;
    gl_Position = uViewProj * (skinMatrix() * vec4(aPos + aMorphPos, 1.0));
}
)";

    // ------------------------------------------------------------ shadow depth

    constexpr const char* DEPTH_VS = R"(#version 300 es
layout(location = 0) in vec3 aPos;
uniform mat4 uViewProj;
void main() {
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)";

    constexpr const char* DEPTH_FS = R"(#version 300 es
precision mediump float;
void main() {}
)";

    // ------------------------------------------------------------------ panels

    constexpr const char* PANEL_VS = R"(#version 300 es
layout(location = 0) in vec2 aPos;
uniform mat4 uViewProj;
uniform vec3 uOrigin; // world position of the top left corner
uniform vec3 uRight;  // full width vector
uniform vec3 uDown;   // full height vector
out vec2 vA;
invariant gl_Position; // the depth pre-pass and the color pass must produce identical depths
void main() {
    vA = aPos;
    gl_Position = uViewProj * vec4(uOrigin + uRight * aPos.x + uDown * aPos.y, 1.0);
}
)";

    constexpr const char* PANEL_FS = R"(#version 300 es
precision highp float;
in vec2 vA;
uniform sampler2D uTex;
uniform vec2 uUVMax;
uniform vec2 uSize;      // logical px
uniform float uRadius;   // logical px
uniform float uAlpha;
uniform float uFront;    // 1 when seen from the front
uniform vec4 uOutline;   // rgb, width in logical px (0 = none)
uniform vec4 uClip;      // visible part in panel px: x0, y0, x1, y1
uniform float uMinAlpha; // anything more transparent is discarded
out vec4 fragColor;
void main() {
    vec2 p = vA * uSize;
    if (p.x < uClip.x || p.y < uClip.y || p.x > uClip.z || p.y > uClip.w)
        discard;
    vec2 h = uSize * 0.5;
    vec2 q = abs(p - h) - (h - vec2(uRadius));
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - uRadius;
    float aa = max(fwidth(d), 1e-3);
    float cover = 1.0 - smoothstep(-aa * 0.5, aa * 0.5, d);

    vec4 c = uFront > 0.5 ? texture(uTex, vA * uUVMax) : vec4(0.05, 0.05, 0.06, 1.0);
    if (uOutline.a > 0.0) {
        float w = uOutline.a;
        float o = 1.0 - smoothstep(w - aa, w, -d);
        c = mix(c, vec4(uOutline.rgb, 1.0), o * cover);
    }
    c *= cover * uAlpha;
    if (c.a < uMinAlpha)
        discard;
    fragColor = c;
}
)";

    // panels casting sun shadows: only their mostly opaque parts
    constexpr const char* PANEL_DEPTH_FS = R"(#version 300 es
precision highp float;
in vec2 vA;
uniform sampler2D uTex;
uniform vec2 uUVMax;
uniform vec2 uSize;
uniform float uRadius;
uniform vec4 uClip;
void main() {
    vec2 p = vA * uSize;
    if (p.x < uClip.x || p.y < uClip.y || p.x > uClip.z || p.y > uClip.w)
        discard;
    vec2 h = uSize * 0.5;
    vec2 q = abs(p - h) - (h - vec2(uRadius));
    if (length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - uRadius > 0.0)
        discard;
    if (texture(uTex, vA * uUVMax).a < 0.5)
        discard;
}
)";

    // --------------------------------------------------------------- crosshair

    constexpr const char* CROSS_VS = R"(#version 300 es
layout(location = 0) in vec2 aPos;
uniform vec2 uViewport; // pixels
uniform float uExtent;  // half size in pixels
out vec2 vP;
void main() {
    vP = (aPos * 2.0 - 1.0) * uExtent;
    gl_Position = vec4(vP / uViewport * 2.0, 0.0, 1.0);
}
)";

    constexpr const char* CROSS_FS = R"(#version 300 es
precision highp float;
in vec2 vP;
uniform vec4 uColor;
uniform float uScale;
uniform float uDot;
uniform float uAlpha;
out vec4 fragColor;
float box(vec2 p, vec2 c, vec2 hs) {
    vec2 d = abs(p - c) - hs;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}
void main() {
    vec2 p = vP / uScale;
    float gap = 4.0, len = 7.0, th = 1.0;
    float d = 1e9;
    d = min(d, box(p, vec2(gap + len * 0.5, 0.0), vec2(len * 0.5, th)));
    d = min(d, box(p, vec2(-gap - len * 0.5, 0.0), vec2(len * 0.5, th)));
    d = min(d, box(p, vec2(0.0, gap + len * 0.5), vec2(th, len * 0.5)));
    d = min(d, box(p, vec2(0.0, -gap - len * 0.5), vec2(th, len * 0.5)));
    if (uDot > 0.5)
        d = min(d, length(p) - 1.6);
    d *= uScale;
    float fill = 1.0 - smoothstep(-0.5, 0.5, d);
    float outline = 1.0 - smoothstep(-0.5, 0.5, d - 1.0 * uScale);
    vec4 c = mix(vec4(0.0, 0.0, 0.0, 0.6) * outline, uColor, fill);
    fragColor = vec4(c.rgb * c.a, c.a) * uAlpha;
}
)";

    // --------------------------------------------------------------- HUD (the Action Menu)

    constexpr const char* HUD_VS = R"(#version 300 es
layout(location = 0) in vec2 aPos;
uniform vec2 uViewport; // pixels
uniform vec4 uRect;     // x, y of the top left, w, h: output pixels, y down
out vec2 vUV;
void main() {
    vUV = aPos;
    vec2 p = uRect.xy + aPos * uRect.zw;
    // the output is upside down (row 0 = top), so y down is NDC y up
    gl_Position = vec4(p / uViewport * 2.0 - 1.0, 0.0, 1.0);
}
)";

    constexpr const char* HUD_FS = R"(#version 300 es
precision highp float;
in vec2 vUV;
uniform sampler2D uTex; // cairo's premultiplied ARGB, which is BGRA in memory
uniform vec2 uSize;     // its pixels
uniform vec3 uCursor;   // x, y in its pixels, radius (0 = none)
uniform float uAlpha;
out vec4 fragColor;
void main() {
    vec4 c = texture(uTex, vUV).bgra;
    if (uCursor.z > 0.0) {
        float d = length(vUV * uSize - uCursor.xy);
        float dotA = clamp(uCursor.z - d + 0.5, 0.0, 1.0);
        float ring = clamp(uCursor.z + 2.0 - d, 0.0, 1.0);
        vec4 cur = vec4(vec3(dotA), dotA) + vec4(0.0, 0.0, 0.0, 0.55) * max(ring - dotA, 0.0);
        c = cur + c * (1.0 - cur.a);
    }
    fragColor = c * uAlpha;
}
)";
}
