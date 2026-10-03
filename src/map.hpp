#pragma once

#include "loader.hpp"
#include "world.hpp"

#include <atomic>
#include <optional>
#include <string>
#include <vector>

namespace h3d {

    // how a vertex of a map gets its light
    enum eMapLight : uint8_t {
        LIGHT_OWN = 0, // own: sky and bounce baked into SMapVertex::ao
        LIGHT_MAP,     // HYPR3D_lighting lightmap; light = lightmap uv
        LIGHT_PROBE,   // light probes; light = probe atlas texel
        LIGHT_FLAT,    // baked map, none for this: average light
    };

    // map vertex in world space (meters, y up)
    struct SMapVertex {
        float   pos[3];
        float   normal[3];
        float   uv[2];
        float   uv1[2];
        float   light[3];    // see eMapLight
        int16_t tangent[4];  // normalized TANGENT xyz, bitangent sign; 0 = none
        uint8_t color[4];    // COLOR_0, linear
        uint8_t ao[4];       // occlusion, sky visibility, sun bounce; _BLEND weight
        uint8_t lighting[4]; // eMapLight, then unused
    };

    enum eAlphaMode : uint8_t {
        ALPHA_OPAQUE = 0,
        ALPHA_MASK,
        ALPHA_BLEND,
    };

    // pixel storage: plain, or block compressed (S3TC, RGTC)
    enum eTexFormat : uint8_t {
        TEX_RGBA8 = 0,
        TEX_BC1,   // rgb, 4 bits a texel
        TEX_BC3,   // rgba, 8 bits
        TEX_BC5,   // rg (normal maps: z from x and y), 8 bits
    };

    // which of those the GPU takes (gl::textureCompression())
    enum eTexCompression : int {
        COMPRESS_S3TC      = 1,
        COMPRESS_S3TC_SRGB = 2,
        COMPRESS_RGTC      = 4,
    };

    struct SMapImage {
        std::string          name;
        int                  w = 0, h = 0;
        std::vector<uint8_t> rgba;   // TEX_RGBA8: pixels; compressed: all mips in a row
        eTexFormat           format = TEX_RGBA8;
        int                  levels = 1; // in rgba; plain images get mips on the GPU
        // sampler of the first material that uses it
        int                  wrapS = 0x2901, wrapT = 0x2901; // GL_REPEAT
        bool                 nearest = false;
        bool                 srgb    = false; // colors (base, emissive), not data
        bool                 normal  = false; // normal map: only x and y kept
        bool                 plain   = false; // never compressed (sky gradients would band)
    };

    enum eMapBlend : uint8_t {
        BLEND_NORMAL = 0, // by alpha (ALPHA_BLEND)
        BLEND_MOD2X,      // under it times twice its color (Source decals)
        BLEND_ADD,        // adds to it (glows)
    };

    enum eMapDetail : uint8_t {
        DETAIL_NONE = 0,
        DETAIL_MOD2X,   // the base color times twice the detail's
        DETAIL_OVERLAY, // Photoshop's overlay
    };

    // Unity stencil test and write (unity2hypr3d's "hypr3d_stencil": UnlitWF, lilToon, Poiyomi masks)
    enum eStencilComp : uint8_t { SC_NEVER, SC_LESS, SC_EQUAL, SC_LEQUAL, SC_GREATER, SC_NOTEQUAL, SC_GEQUAL, SC_ALWAYS };
    enum eStencilOp : uint8_t { SO_KEEP, SO_ZERO, SO_REPLACE, SO_INCR, SO_DECR, SO_INVERT, SO_INCR_WRAP, SO_DECR_WRAP };
    struct SStencil {
        bool         on  = false;
        uint8_t      ref = 0, read = 255, write = 255;
        eStencilComp comp = SC_ALWAYS;
        eStencilOp   pass = SO_KEEP, fail = SO_KEEP, zfail = SO_KEEP;
        // redrawn with againComp at this opacity (UnlitWF's MaskOut_Blend); < 0 = not
        eStencilComp againComp = SC_ALWAYS;
        float        again     = -1;
        bool         reads() const {
            return on && comp != SC_ALWAYS;
        }
    };

    // toon outline: inverted hull (the mesh pushed out along its normals, front faces culled)
    enum eOutlineSpace : uint8_t {
        OUTLINE_WORLD,  // width in metres
        OUTLINE_OBJECT, // metres at the model's scale
        OUTLINE_SCREEN, // NDC width (2 = screen height) to maxW, then thinner
    };
    struct SOutline {
        float         width = 0; // 0: none
        eOutlineSpace space = OUTLINE_WORLD;
        float         color[4] = {0, 0, 0, 1}; // linear
        float         base = 0, tint = 0;      // mix to base color (UnlitWF), times it (Poiyomi)
        int           maskTex     = -1; // width times one of its channels
        uint8_t       maskChannel = 0;
        bool          maskInvert  = false;
        float         shift = 0;              // metres towards the eye (< 0: away)
        float         fix = 0, fixMax = 1;    // width * mix(1, min(distance, fixMax), fix)
        float         lit = 1;                // shading amount (0 = flat color)
        float         maxW = 1;               // screen space: constant width up to here
        int           colorTex = -1;          // color texture (images index): multiplies the color
        float         colorBlend = -1;        // or, >= 0, mixed in this much (UnlitWF custom color)
        float         colorXf[6] = {1, 0, 0, 1, 0, 0}; // its uv: mat2 columns, then the offset
    };

    // toon shading (MToon, lilToon, UnlitWF, Poiyomi): shade to lit color as N·L goes lo to hi; matcap by view normal
    enum eMatcapMode : uint8_t {
        MATCAP_ADD,      // MToon, lilToon Add / Screen, UnlitWF light cap, Poiyomi Add
        MATCAP_MULTIPLY, // UnlitWF shade cap, lilToon / Poiyomi Multiply
        MATCAP_MIX,      // replaces the color (lilToon Normal, Poiyomi Replace)
        MATCAP_MEDIAN,   // lighter above mid grey, darker below (UnlitWF)
    };
    struct SToon {
        bool        on = false; // the shading (a matcap: matcapTex)
        float       shade[3] = {1, 1, 1}; // linear
        bool        shadeBase = true;     // times the base color
        int         shadeTex  = -1;       // times this (index into images)
        float       lo = -1, hi = -1;     // N·L at full shade, full lit; -1, -1 = flat lit
        float       strength = 1;         // how much of the shade shows
        int         matcapTex = -1;
        float       matcap[4] = {1, 1, 1, 1}; // color (median: 1 lighter, 0 darker), amount
        eMatcapMode matcapMode = MATCAP_ADD;
        float       matcapLit = 1; // 0 = full light, 1 = lit like the surface
    };

    struct SMapMaterial {
        std::string name;
        float       baseColor[4] = {1, 1, 1, 1};
        float       emissive[3]  = {0, 0, 0};
        int         baseTex = -1, emissiveTex = -1, occlusionTex = -1; // indices into images
        float       baseXf[6]     = {1, 0, 0, 1, 0, 0}; // KHR_texture_transform: mat2 columns, offset
        float       emissiveXf[6] = {1, 0, 0, 1, 0, 0};
        int         emissiveUV = 0, occlusionUV = 0; // 0 vertex uv (the base color's set), 1 uv1
        float       occlusionStrength = 1;
        eAlphaMode  alphaMode   = ALPHA_OPAQUE;
        float       alphaCutoff = 0.5f;
        bool        unlit  = false;
        // normal map on the base color's uvs; roughness and metalness in the occlusion texture's g, b (ORM packing)
        int         normalTex = -1;
        float       normalScale = 1;
        bool        normalYDown = false; // green down the bitangent (Source), not up
        float       roughness = 1, metalness = 0;
        bool        ormFromOcclusion = false;
        bool        specular[2] = {true, true}; // sun, environment (HYPR3D_materials_source2)
        float       selfIllumAlbedo = 0;         // share of the base color in the emissive
        bool        glass = false;               // blended; reflections at full strength
        uint8_t     vertexColor = 0;             // COLOR_0: 0 linear, 1 sRGB, 2 none, 3 alpha tint, 4 tint
        eMapBlend   blend = BLEND_NORMAL;
        bool        mod2xLinear = false;         // mod2x color is linear (CS2's unlit shader)
        bool        fog   = true;                // game fog applies (CS2's g_bFogEnabled)
        bool        doubleSided = false;         // without it CS2 effect cards show front only
        float       scroll[2]   = {0, 0};        // base color uv scroll per second (CS2)
        // HYPR3D_materials_source2 tint mask (the base color's rgb tints by its r) and decal, on the vertex uv or uv1
        int         tintMaskTex = -1, tintMaskUV = 0;
        int         decalTex = -1, decalUV = 0;
        uint8_t     decal = 0; // 0 none, 1 alpha mix, 2 multiply, 3 rgba times base
        float       decalXf[6] = {1, 0, 0, 1, 0, 0}; // decal 3's uv transform, from the vertex uv
        // HYPR3D_materials_source2's detail texture, over the base color
        eMapDetail  detail = DETAIL_NONE;
        int         detailTex = -1, detailMaskTex = -1;
        float       detailXf[6]   = {1, 0, 0, 1, 0, 0}; // from the vertex uv
        float       detailTint[3] = {1, 1, 1};
        float       detailBlend = 1, detailBlendToFull = 0;
        int         detailMaskUV = 0;
        int         detailUV = 0; // 0 vertex uv, 1 uv1
        // HYPR3D_materials_blend: a second base color painted over the first by vertex _BLEND weight (Source 2 layers)
        int         layerTex = -1, layerMaskTex = -1; // mask: g = layer boundary, r = edge softness
        int         layerNormalTex = -1;              // its normal map, roughness in alpha
        float       layerColor[4] = {1, 1, 1, 1};
        float       layerXf[6]    = {1, 0, 0, 1, 0, 0}; // applied to the vertex uv, like baseXf
        float       layerMaskXf[6] = {1, 0, 0, 1, 0, 0}; // the mask's (CS2 blend modulation)
        float       layerSoftness = -1;                // < 0: the mask's red channel
        int         layerMaskChannel = 1;              // where the layers meet: g, or a
        float       layer1Tint[3] = {1, 1, 1};         // layer 1 tint (the base color tints both)
        // CS2's border tint: layer 1 tinted in a band along the layer edge
        float       borderTint[3] = {1, 1, 1};
        float       border[3]     = {0, 0.5f, 0};      // strength (0 = none), softness, weight offset
        // csgo_effects (HYPR3D_materials_source2): unlit color times up to 3 scrolling masks; distance, angle fades
        bool        effect = false;
        int         effectMaskTex[3] = {-1, -1, -1};
        float       effectMask[3][4] = {{1, 1, 0, 0}, {1, 1, 0, 0}, {1, 1, 0, 0}}; // uv scale, scroll per second
        float       effectBoost = 1, effectOpacity = 1;
        float       effectFade[4]    = {1, 1, 0, 1};     // distance (m), falloff, min, max
        float       effectFresnel[4] = {0.001f, 1, 0, 1}; // exponent, falloff, min, max
        bool        effectFog = true;
        // avatars (unity2hypr3d "hypr3d_*" extras, MToon outline and queue): Unity render queue, -1 = by alpha mode
        int         queue = -1;
        SStencil    stencil;
        SOutline    outline;
        SToon       toon;
        int         back = 0; // back faces: 1 backColor, 2 backTex x backColor
        int         backTex      = -1;
        float       backColor[4] = {1, 1, 1, 1};
        float       backXf[6]    = {1, 0, 0, 1, 0, 0};
        float       lightClamp[3] = {0, 0, 1}; // UnlitWF light clamp: min, full at (0 = off), chroma
        int         renderQueue() const {
            return queue >= 0 ? queue : alphaMode == ALPHA_BLEND ? 3000 : alphaMode == ALPHA_MASK ? 2450 : 2000;
        }
    };

    // HDR texels (GL_RGB9_E5), mip levels back to back
    struct SHdrImage {
        int                   w = 0, h = 0, levels = 0;
        std::vector<uint32_t> texels;
        explicit operator bool() const {
            return !texels.empty();
        }
    };

    // a game's precomputed lighting (HYPR3D_lighting), for the map or its backdrop
    struct SMapLightSet {
        SHdrImage            irradiance;  // the lightmap: light arriving, linear
        SMapImage            directional; // xy: direction (tangent space), z: directionality, a: spec AO
        SMapImage            shadows;     // the sun's baked shadow (r, 1 = in shadow)
        // probe atlas, half floats: six blocks deep (+x +y +z -x -y -z, Source axes); block 0 alpha: sun shadow
        std::vector<uint16_t> probes;
        std::vector<uint8_t> probeLuma;                // 6 log lumas per block texel (exposure meter)
        int                  probeDims[3] = {0, 0, 0}; // one block
        float                average[3]   = {0, 0, 0}; // probe average, for what has neither
        struct SVolume {
            M4    toBox = M4::identity(); // world -> 0..1 in the volume
            SAABB bounds;
            int   atlasOffset[3] = {0, 0, 0}, atlasSize[3] = {1, 1, 1};
            int   priority = 0;
        };
        std::vector<SVolume> volumes;
        bool                 hasLightmaps() const {
            return (bool)irradiance;
        }
    };

    struct SMapLighting {
        bool                      present = false;
        std::vector<SMapLightSet> sets; // the map, then its backdrop
        V3                        sunColor{3.2f, 2.88f, 2.37f}; // linear, times its brightness
        V3                        sunDir{0, 0, 0};              // towards it; 0 0 0: the map's light node says
        // CS2's cubemap fog, its color from the sky
        bool                      fog = false;
        float                     fogStart = 0, fogEnd = 100, fogExponent = 1, fogMaxOpacity = 1, fogLodBias = 0;
        float                     fogHeightStart = 0, fogHeightEnd = 0, fogHeightExponent = 1; // end <= start: none
        int                       skyImage = -1; // into SMapModel::images
        V3                        skyColor{1, 1, 1};
        V3                        skyAverage{0.7f, 0.75f, 0.8f}; // light from all of the sky, linear
        // the exposure range (post_processing_volume) and tone curve (its .vpost)
        float                     exposureMin = 1, exposureMax = 1, exposureSpeedUp = 1, exposureSpeedDown = 1;
        bool                      exposureAuto = false;
        float                     curve[8] = {0.15f, 0.5f, 0.1f, 0.2f, 0.02f, 0.3f, 4.f, 0.f}; // shoulder, linear strength / angle, toe strength / num / denom, white point, exposure bias
    };

    // one draw call: every triangle of a material
    struct SMapBatch {
        int      material = 0;
        uint32_t first = 0, count = 0; // in indices
        bool     render     = true;  // false: shadow only (tool textures like nodraw)
        bool     castShadow = true;
        bool     sky        = false; // skybox / dome: unlit, unfogged, drawn behind
        bool     backdrop   = false; // hypr3d_backdrop: drawn first, own depth range
    };

    struct SMapModel {
        std::string             path;
        std::vector<SMapVertex> vertices;
        std::vector<uint32_t>   indices;
        std::vector<SMapImage>  images;
        std::vector<SMapMaterial> materials;
        std::vector<SMapBatch>  batches; // opaque, alpha tested, sky, blended, then shadow only
        SAABB                   geometryBounds = SAABB::empty(); // without the sky and the backdrop
        SAABB                   backdropBounds = SAABB::empty();
        size_t                  triangles = 0;
        SMapLighting            lighting; // the game's own, when the file has it
        M4                      backdropTransform = M4::identity(); // hypr3d_backdrop space -> world

        // call after the renderer's GPU upload; the lighting numbers stay
        void releaseCpuData() {
            vertices      = {};
            indices       = {};
            for (auto& i : images)
                i.rgba = {};
            for (auto& s : lighting.sets) {
                s.irradiance.texels = {};
                s.directional.rgba  = {};
                s.shadows.rgba      = {};
                s.probes            = {};
            }
        }
    };

    struct SMapRequest {
        std::string path;
        int         compress = 0; // eTexCompression flags the GPU supports
        float       scale = 0;   // 0 = guess from the size
        float       aspect = 16.f / 9.f; // monitor aspect, to fit the desktop on a wall
        float       desktopHeight = 2.4f;
    };

    struct SMapResult {
        SMapRequest            req;
        std::unique_ptr<SWorld> world;
        std::string            error;
        std::vector<std::string> log;
    };

    // SMapLightSet::probeLuma's encoding: 2^-10 to 2^4 in 255 steps
    inline uint8_t probeLumaByte(float l) {
        return l <= 0.f ? 0 : (uint8_t)std::clamp(std::lround((std::log2(l) + 10.f) / 14.f * 254.f) + 1, 1l, 255l);
    }
    inline float probeLumaValue(uint8_t b) {
        return b ? std::exp2((b - 1) / 254.f * 14.f - 10.f) : 0.f;
    }

    // CS2's probe volume for p: top priority (indoor_outdoor_level) containing it, most central on ties, else nearest
    const SMapLightSet::SVolume* chooseLightProbe(const SMapLightSet& set, const V3& p);

    // auto exposure (plugin and test harness): exposureSample() measures from `eye` along exposureDirection(i),
    // exposureFor() gives the exposure for `view`, exposureStep() eases toward it
    constexpr int EXPOSURE_SAMPLES = 64;
    V3            exposureDirection(int i);
    float         exposureSample(const SWorld& world, const V3& eye, const V3& dir);
    float         exposureFor(const SWorld& world, const float* samples, const V3& view);
    float         exposureStep(const SWorld& world, float current, float target, float dt);

    // loads a map (glTF / GLB), on a worker thread through CMapLoader
    SMapResult loadMap(const SMapRequest& req, const std::atomic<bool>& cancel);

    class CMapLoader : public CBackgroundLoader<SMapRequest, SMapResult> {
      public:
        CMapLoader() : CBackgroundLoader(loadMap) {}
    };

    // where the map's anchors (spawn and desktop) are saved when set by hand
    std::string mapStatePath(const std::string& mapPath);
    bool        saveMapState(const std::string& mapPath, const SWorld& world, float scale);
}
