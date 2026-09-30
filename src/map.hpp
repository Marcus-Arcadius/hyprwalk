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
        LIGHT_OWN = 0, // hypr3d's: sky and sunlight bounce baked into SMapVertex::ao
        LIGHT_MAP,     // the map's lightmap (HYPR3D_lighting): light holds the lightmap uv
        LIGHT_PROBE,   // the map's light probes: light holds where the vertex is in the probe atlas (texels)
        LIGHT_FLAT,    // a map with baked lighting, but none for this: its average light
    };

    // a vertex of a loaded map, already in world space (meters, y up)
    struct SMapVertex {
        float   pos[3];
        float   normal[3];
        float   uv[2];
        float   uv1[2];
        float   light[3];    // see eMapLight
        int16_t tangent[4];  // normalized: TANGENT's direction, then the bitangent's sign (0 0 0 0: none)
        uint8_t color[4];    // COLOR_0, linear
        uint8_t ao[4];       // baked: local occlusion, how much sky is visible, sunlight bounced off what's around; then the _BLEND weight
        uint8_t lighting[4]; // eMapLight, then unused
    };

    enum eAlphaMode : uint8_t {
        ALPHA_OPAQUE = 0,
        ALPHA_MASK,
        ALPHA_BLEND,
    };

    // how an image's pixels are kept: plain, or block compressed (S3TC and RGTC)
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
        std::vector<uint8_t> rgba;   // TEX_RGBA8: the pixels; compressed: every mip level, one after the other
        eTexFormat           format = TEX_RGBA8;
        int                  levels = 1; // in rgba (compressed only; plain ones get theirs on the GPU)
        // sampler of the first material that uses it
        int                  wrapS = 0x2901, wrapT = 0x2901; // GL_REPEAT
        bool                 nearest = false;
        bool                 srgb    = false; // holds colors (base, emissive) rather than data (occlusion)
        bool                 normal  = false; // a normal map: only x and y need keeping
        bool                 plain   = false; // never compressed (the sky: blocks would band its gradients)
    };

    enum eMapBlend : uint8_t {
        BLEND_NORMAL = 0, // by alpha (ALPHA_BLEND)
        BLEND_MOD2X,      // multiplies what's under it by twice its colour (Source's decals)
        BLEND_ADD,        // adds to it (glows)
    };

    enum eMapDetail : uint8_t {
        DETAIL_NONE = 0,
        DETAIL_MOD2X,   // the base color times twice the detail's
        DETAIL_OVERLAY, // Photoshop's overlay
    };

    // Unity's stencil test and write (unity2hypr3d's "hypr3d_stencil": UnlitWF's stencil masks, lilToon's, Poiyomi's)
    enum eStencilComp : uint8_t { SC_NEVER, SC_LESS, SC_EQUAL, SC_LEQUAL, SC_GREATER, SC_NOTEQUAL, SC_GEQUAL, SC_ALWAYS };
    enum eStencilOp : uint8_t { SO_KEEP, SO_ZERO, SO_REPLACE, SO_INCR, SO_DECR, SO_INVERT, SO_INCR_WRAP, SO_DECR_WRAP };
    struct SStencil {
        bool         on  = false;
        uint8_t      ref = 0, read = 255, write = 255;
        eStencilComp comp = SC_ALWAYS;
        eStencilOp   pass = SO_KEEP, fail = SO_KEEP, zfail = SO_KEEP;
        // drawn again with another test, this much as opaque (UnlitWF's MaskOut_Blend: fainter where its mask is);
        // < 0: not
        eStencilComp againComp = SC_ALWAYS;
        float        again     = -1;
        bool         reads() const {
            return on && comp != SC_ALWAYS;
        }
    };

    // a toon outline: the mesh drawn again pushed out along its normals, its front faces culled (an inverted hull)
    enum eOutlineSpace : uint8_t {
        OUTLINE_WORLD,  // width in metres
        OUTLINE_OBJECT, // metres at the model's scale
        OUTLINE_SCREEN, // width in NDC units (2 = the screen's height) up to maxW away, then thinner
    };
    struct SOutline {
        float         width = 0; // 0: none
        eOutlineSpace space = OUTLINE_WORLD;
        float         color[4] = {0, 0, 0, 1}; // linear
        float         base = 0, tint = 0;      // mixed towards the base color (UnlitWF), multiplied by it (Poiyomi)
        int           maskTex     = -1; // width times one of its channels
        uint8_t       maskChannel = 0;
        bool          maskInvert  = false;
        float         shift = 0;              // metres towards the eye (< 0: away)
        float         fix = 0, fixMax = 1;    // thinner up close: width * mix(1, min(distance, fixMax), fix)
        float         lit = 1;                // how much it's shaded (0: its color as it is)
        float         maxW = 1;               // screen space: the distance it stops staying as wide at
        int           colorTex = -1;          // its color's texture (index into images): the color times it,
        float         colorBlend = -1;        // or (>= 0) mixed this far towards it (UnlitWF's custom color)
        float         colorXf[6] = {1, 0, 0, 1, 0, 0}; // its uv: mat2 columns, then the offset
    };

    // toon shading (MToon, lilToon, UnlitWF, Poiyomi): the sun lights a surface from its shade color to its lit one as
    // N·L goes from lo to hi, not by N·L itself; and a matcap, looked up by the normal as the camera sees it
    enum eMatcapMode : uint8_t {
        MATCAP_ADD,      // added (MToon's, lilToon's Add and Screen, UnlitWF's light cap, Poiyomi's Add)
        MATCAP_MULTIPLY, // the color times it (UnlitWF's shade cap, lilToon's and Poiyomi's Multiply)
        MATCAP_MIX,      // in place of the color, this much (lilToon's Normal, Poiyomi's Replace)
        MATCAP_MEDIAN,   // lighter where it's over mid grey, darker under (UnlitWF's median cap)
    };
    struct SToon {
        bool        on = false; // the shading (a matcap: matcapTex)
        float       shade[3] = {1, 1, 1}; // linear
        bool        shadeBase = true;     // times the base color
        int         shadeTex  = -1;       // times this (index into images)
        float       lo = -1, hi = -1;     // N·L where it's all shade, and all lit (lo = hi = -1: lit all round, flat)
        float       strength = 1;         // how much of the shade shows
        int         matcapTex = -1;
        float       matcap[4] = {1, 1, 1, 1}; // color (the median's: how much lighter, 0 darker), and how much of it
        eMatcapMode matcapMode = MATCAP_ADD;
        float       matcapLit = 1; // how much it's lit as the surface is (0: as if in full light, wherever it is)
    };

    struct SMapMaterial {
        std::string name;
        float       baseColor[4] = {1, 1, 1, 1};
        float       emissive[3]  = {0, 0, 0};
        int         baseTex = -1, emissiveTex = -1, occlusionTex = -1; // indices into images
        float       baseXf[6]     = {1, 0, 0, 1, 0, 0}; // KHR_texture_transform: mat2 columns, then the offset
        float       emissiveXf[6] = {1, 0, 0, 1, 0, 0};
        int         emissiveUV = 0, occlusionUV = 0; // 0: the vertex uv (the base color's set), 1: uv1
        float       occlusionStrength = 1;
        eAlphaMode  alphaMode   = ALPHA_OPAQUE;
        float       alphaCutoff = 0.5f;
        bool        unlit  = false;
        // normal map (the base color's uvs), and roughness and metalness: the occlusion texture's g
        // and b when it's the same image as the metallic-roughness one (glTF's usual ORM packing)
        int         normalTex = -1;
        float       normalScale = 1;
        bool        normalYDown = false; // green points down the bitangent (Source), not up (glTF)
        float       roughness = 1, metalness = 0;
        bool        ormFromOcclusion = false;
        bool        specular[2] = {true, true}; // from the sun, from the surroundings (HYPR3D_materials_source2)
        float       selfIllumAlbedo = 0;         // the emissive color takes this much of the base color
        bool        glass = false;               // blended, its reflections as strong as if it were opaque
        uint8_t     vertexColor = 0;             // COLOR_0 is: 0 linear (glTF's), 1 sRGB, 2 not a color, 3 a tint as strong as its alpha,
                                                 // 4 part of the tint (under the tint mask; all 0: none)
        eMapBlend   blend = BLEND_NORMAL;
        bool        mod2xLinear = false;         // mod2x's color is linear (CS2's unlit shader), not as it's stored
        bool        fog   = true;                // the game's fog covers it (CS2's g_bFogEnabled)
        bool        doubleSided = false;         // glTF's (CS2's effect cards are seen from their front only without it)
        float       scroll[2]   = {0, 0};        // the base color's uvs move by this much a second (CS2's scrolling textures)
        // HYPR3D_materials_source2's tint mask: the base color's rgb (Source 2's tint) only as much as its r says; and
        // its decal texture over the base color, mixed in by its alpha or multiplied; each on the vertex uv or uv1
        int         tintMaskTex = -1, tintMaskUV = 0;
        int         decalTex = -1, decalUV = 0;
        uint8_t     decal = 0; // 0 none, 1 mixed in by its alpha, 2 multiplied, 3 a second color texture (rgba) the first is multiplied by
        float       decalXf[6] = {1, 0, 0, 1, 0, 0}; // the second color texture's, from the vertex uv
        // HYPR3D_materials_source2's detail texture, over the base color
        eMapDetail  detail = DETAIL_NONE;
        int         detailTex = -1, detailMaskTex = -1;
        float       detailXf[6]   = {1, 0, 0, 1, 0, 0}; // from the vertex uv
        float       detailTint[3] = {1, 1, 1};
        float       detailBlend = 1, detailBlendToFull = 0;
        int         detailMaskUV = 0;
        int         detailUV = 0; // the detail texture on the vertex uv, or uv1
        // HYPR3D_materials_blend: a second base color painted over the first by
        // the vertices' _BLEND weight, the way Source 2 blends its layers
        int         layerTex = -1, layerMaskTex = -1; // the mask: g = where the layers meet, r = how soft the edge is
        int         layerNormalTex = -1;              // its normal map, roughness in alpha
        float       layerColor[4] = {1, 1, 1, 1};
        float       layerXf[6]    = {1, 0, 0, 1, 0, 0}; // applied to the vertex uv, like baseXf
        float       layerMaskXf[6] = {1, 0, 0, 1, 0, 0}; // the mask's (CS2's blend modulation can have its own)
        float       layerSoftness = -1;                // < 0: the mask's red channel
        int         layerMaskChannel = 1;              // where the layers meet: g, or a
        float       layer1Tint[3] = {1, 1, 1};         // the first layer's own tint (the base color's tints both)
        // CS2's border tint: the first layer tinted in a band along the edge between the layers
        float       borderTint[3] = {1, 1, 1};
        float       border[3]     = {0, 0.5f, 0};      // strength (0: none), softness, offset of the painted weight
        // HYPR3D_materials_source2's effect (CS2's csgo_effects: clouds, dust, glows): unlit, its color
        // times up to three masks scrolling over it, faded by distance and by how square on it's seen
        bool        effect = false;
        int         effectMaskTex[3] = {-1, -1, -1};
        float       effectMask[3][4] = {{1, 1, 0, 0}, {1, 1, 0, 0}, {1, 1, 0, 0}}; // uv scale, then scroll speed a second
        float       effectBoost = 1, effectOpacity = 1;
        float       effectFade[4]    = {1, 1, 0, 1};     // distance (m), falloff, min, max
        float       effectFresnel[4] = {0.001f, 1, 0, 1}; // exponent, falloff, min, max
        bool        effectFog = true;
        // the avatar's (unity2hypr3d's "hypr3d_*" extras; MToon's outline and queue in VRMs): Unity's render queue,
        // -1 its alpha mode's (2000, 2450, 3000); its stencil; an outline; UnlitWF's back faces and light clamp
        int         queue = -1;
        SStencil    stencil;
        SOutline    outline;
        SToon       toon;
        int         back = 0; // 1: back faces take backColor, 2: backTex times backColor (in place of the base color's rgb)
        int         backTex      = -1;
        float       backColor[4] = {1, 1, 1, 1};
        float       backXf[6]    = {1, 0, 0, 1, 0, 0};
        float       lightClamp[3] = {0, 0, 1}; // the light's brightness between min and max (0: not clamped), its chroma
        int         renderQueue() const {
            return queue >= 0 ? queue : alphaMode == ALPHA_BLEND ? 3000 : alphaMode == ALPHA_MASK ? 2450 : 2000;
        }
    };

    // HDR pixels, shared exponent (GL_RGB9_E5), each mip level after the other
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
        SMapImage            directional; // where most of it comes from, in tangent space (xy), how much (z), specular occlusion (a)
        SMapImage            shadows;     // the sun's baked shadow (r, 1 = in shadow)
        // light probes: an atlas of 3D grids, six blocks deep (the light arriving along +x +y +z -x -y -z,
        // Source's axes), the first with the sun's baked shadow in alpha; half floats
        std::vector<uint16_t> probes;
        std::vector<uint8_t> probeLuma;                // per texel of one block, six of them: each block's luminance, log encoded
                                                       // (probeLumaValue(); kept for the exposure meter)
        int                  probeDims[3] = {0, 0, 0}; // one block
        float                average[3]   = {0, 0, 0}; // of the probes: light for what has no lightmap or probe
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
        float                     curve[8] = {0.15f, 0.5f, 0.1f, 0.2f, 0.02f, 0.3f, 4.f, 0.f}; // shoulder, linear strength, linear angle, toe strength, toe num, toe denom, white point, exposure bias
    };

    // one draw call: every triangle of a material
    struct SMapBatch {
        int      material = 0;
        uint32_t first = 0, count = 0; // in indices
        bool     render     = true;  // false: only casts shadows (tool textures like nodraw)
        bool     castShadow = true;
        bool     sky        = false; // a skybox/dome: unlit, no fog, drawn behind everything
        bool     backdrop   = false; // under a hypr3d_backdrop node (a game's 3D skybox): scenery far
                                     // out, drawn before the map with its own depth range
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
        M4                      backdropTransform = M4::identity(); // the hypr3d_backdrop node's: its own space -> world

        // the renderer copied everything it needs to the GPU (the lighting's numbers stay)
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
        int         compress = 0; // eTexCompression: block compressed textures the GPU takes
        float       scale = 0;   // 0 = guess from the size
        float       aspect = 16.f / 9.f; // of the monitor, for fitting the desktop on a wall
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

    // the light probe volume CS2 lights something at p with: of the ones it's in, the highest
    // priority (indoor_outdoor_level), nearest the middle among equals; else the nearest one
    const SMapLightSet::SVolume* chooseLightProbe(const SMapLightSet& set, const V3& p);

    // The eyes adjusting to how bright it is (used by the plugin and its test harness alike).
    // exposureSample() guesses how bright the world looks from `eye` along exposureDirection(i) (a
    // ray to what's there, and whether the sun reaches it); exposureFor() is the exposure for a full
    // set of samples, looking along `view`; exposureStep() moves the exposure towards it over dt.
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
