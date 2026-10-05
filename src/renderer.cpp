#include "renderer.hpp"
#include "shaders.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <numeric>
#include <ranges>

#include <EGL/egl.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_COMPRESSED_SRGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_SRGB_S3TC_DXT1_EXT 0x8C4C
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT 0x8C4F
#endif
#ifndef GL_COMPRESSED_RED_GREEN_RGTC2_EXT
#define GL_COMPRESSED_RED_GREEN_RGTC2_EXT 0x8DBD
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT     0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

namespace hyprwalk {

    namespace {
        // backdrop fog density relative to the map's: it's kilometres away and would be all fog otherwise
        constexpr float BACKDROP_FOG = 0.125f;

        // per-program uniform locations, keyed by the name's address, not its text (hashing names costs a third of a
        // frame's CPU time): names must be string literals
        std::unordered_map<GLuint, std::unordered_map<const char*, GLint>> g_uniforms;
        GLuint                                                              g_lastProg = 0;
        std::unordered_map<const char*, GLint>*                             g_last     = nullptr; // g_uniforms[g_lastProg]

        GLint U(GLuint prog, const char* name) {
            if (!g_last || prog != g_lastProg) {
                g_last     = &g_uniforms[prog];
                g_lastProg = prog;
            }
            if (auto it = g_last->find(name); it != g_last->end())
                return it->second;
            const GLint loc = glGetUniformLocation(prog, name);
            g_last->emplace(name, loc);
            return loc;
        }

        // geometry that never changes: immutable storage where there's EXT_buffer_storage, of which the NVIDIA driver
        // keeps no copy in system memory (glBufferData's: 140 MB for de_mirage)
        void staticBuffer(GLenum target, size_t size, const void* data) {
            static const auto storage =
                gl::hasExtension("GL_EXT_buffer_storage") ? (PFNGLBUFFERSTORAGEEXTPROC)eglGetProcAddress("glBufferStorageEXT") : nullptr;
            if (storage)
                storage(target, (GLsizeiptr)size, data, 0);
            else
                glBufferData(target, (GLsizeiptr)size, data, GL_STATIC_DRAW);
        }

        // gives freed CPU copies back to the system: glibc keeps freed heap memory (~550 MB after de_dust2)
        void trimHeap() {
#ifdef __GLIBC__
            malloc_trim(0);
#endif
        }

        std::string withCommon(const char* body, bool sky, bool lit = false, bool dual = false) {
            std::string s = "#version 300 es\n";
            if (dual)
                s += "#extension GL_EXT_blend_func_extended : require\n#define HYPRWALK_DUAL 1\n";
            s += "precision highp float;\nprecision highp int;\n";
            s += shaders::NOISE_GLSL;
            if (sky || lit)
                s += shaders::SKY_COMMON_GLSL;
            if (lit)
                s += shaders::LIGHTING_GLSL;
            s += body;
            return s;
        }

        void uMat(GLuint prog, const char* name, const M4& m) {
            glUniformMatrix4fv(U(prog, name), 1, GL_FALSE, m.m);
        }
        void uVec3(GLuint prog, const char* name, const V3& v) {
            glUniform3f(U(prog, name), v.x, v.y, v.z);
        }

        M4 flipY() {
            M4 f = M4::identity();
            f.m[5] = -1.f;
            return f;
        }

        // client pixel layout may have been left changed by Hyprland; tightly packed while alive
        class CUnpackGuard {
          public:
            CUnpackGuard() {
                glGetIntegerv(GL_UNPACK_ROW_LENGTH, &m_rowLength);
                glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &m_skipPixels);
                glGetIntegerv(GL_UNPACK_SKIP_ROWS, &m_skipRows);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1); // the state guard restores this one
            }
            ~CUnpackGuard() {
                glPixelStorei(GL_UNPACK_ROW_LENGTH, m_rowLength);
                glPixelStorei(GL_UNPACK_SKIP_PIXELS, m_skipPixels);
                glPixelStorei(GL_UNPACK_SKIP_ROWS, m_skipRows);
            }
            CUnpackGuard(const CUnpackGuard&)            = delete;
            CUnpackGuard& operator=(const CUnpackGuard&) = delete;

          private:
            GLint m_rowLength = 0, m_skipPixels = 0, m_skipRows = 0;
        };

        GLuint whiteTexture() {
            const uint8_t white[4] = {255, 255, 255, 255};
            GLuint        tex      = 0;
            glActiveTexture(GL_TEXTURE0);
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, white);
            return tex;
        }

        // mipmapped textures of a model's images (0 where one didn't decode); returns the bytes used
        size_t uploadImages(const std::vector<SMapImage>& images, std::vector<GLuint>& out) {
            const char* ext   = (const char*)glGetString(GL_EXTENSIONS);
            float       aniso = 0;
            if (ext && std::strstr(ext, "GL_EXT_texture_filter_anisotropic")) {
                glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &aniso);
                aniso = std::min(aniso, 8.f);
            }

            glActiveTexture(GL_TEXTURE0);
            size_t bytes = 0;
            out.assign(images.size(), 0);
            for (size_t i = 0; i < images.size(); ++i) {
                const auto& img = images[i];
                if (img.rgba.empty() || img.w < 1 || img.h < 1)
                    continue;
                GLuint tex = 0;
                glGenTextures(1, &tex);
                glBindTexture(GL_TEXTURE_2D, tex);
                if (img.format == TEX_RGBA8) {
                    const int levels = 1 + (int)std::floor(std::log2((float)std::max(img.w, img.h)));
                    glTexStorage2D(GL_TEXTURE_2D, levels, img.srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, img.w, img.h);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, img.w, img.h, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
                    glGenerateMipmap(GL_TEXTURE_2D);
                    bytes += (size_t)img.w * img.h * 4 * 4 / 3;
                } else {
                    // block compressed on the loader thread, every level
                    GLenum format = 0;
                    switch (img.format) {
                        case TEX_BC1: format = img.srgb ? GL_COMPRESSED_SRGB_S3TC_DXT1_EXT : GL_COMPRESSED_RGB_S3TC_DXT1_EXT; break;
                        case TEX_BC3: format = img.srgb ? GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; break;
                        default: format = GL_COMPRESSED_RED_GREEN_RGTC2_EXT; break;
                    }
                    const size_t block = img.format == TEX_BC1 ? 8 : 16;
                    glTexStorage2D(GL_TEXTURE_2D, img.levels, format, img.w, img.h);
                    size_t at = 0;
                    for (int l = 0, w = img.w, h = img.h; l < img.levels; ++l, w = std::max(1, w / 2), h = std::max(1, h / 2)) {
                        const size_t size = (size_t)((w + 3) / 4) * ((h + 3) / 4) * block;
                        if (at + size > img.rgba.size())
                            break;
                        glCompressedTexSubImage2D(GL_TEXTURE_2D, l, 0, 0, w, h, format, (GLsizei)size, img.rgba.data() + at);
                        at += size;
                    }
                    bytes += img.rgba.size();
                }
                const auto wrap = [](int w) { return w == GL_CLAMP_TO_EDGE || w == GL_MIRRORED_REPEAT ? w : GL_REPEAT; };
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap(img.wrapS));
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap(img.wrapT));
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, img.nearest ? GL_NEAREST_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, img.nearest ? GL_NEAREST : GL_LINEAR);
                if (aniso > 1.f)
                    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, aniso);
                out[i] = tex;
            }
            return bytes;
        }

        // texture units of MAP_FS_BODY (5 is the avatar's joints, in its vertex shader)
        enum eUnit : int {
            UNIT_BASE = 0,
            UNIT_EMISSIVE = 1,
            UNIT_PANEL_LIGHTS = 2,
            UNIT_SHADOW = 3,
            UNIT_OCCLUSION = 4,
            UNIT_LAYER = 6,
            UNIT_LAYER_MASK = 7,
            UNIT_NORMAL = 8,
            UNIT_LAYER_NORMAL = 9,
            UNIT_DETAIL = 10,
            UNIT_DETAIL_MASK = 11,
            UNIT_IRRADIANCE = 12,
            UNIT_DIRECTIONAL = 13,
            UNIT_BAKED_SHADOW = 14,
            UNIT_PROBES = 15,
            UNIT_SKY = 16,
            UNIT_OUTLINE_MASK = 17, // the avatar's, in its vertex shader
        };

        void setSamplerUnits(GLuint prog) {
            static constexpr std::pair<const char*, int> UNITS[] = {
                {"uBaseTex", UNIT_BASE},         {"uEmissiveTex", UNIT_EMISSIVE},       {"uOccTex", UNIT_OCCLUSION},       {"uLayerTex", UNIT_LAYER},
                {"uLayerMaskTex", UNIT_LAYER_MASK}, {"uNormalTex", UNIT_NORMAL},       {"uLayerNormalTex", UNIT_LAYER_NORMAL}, {"uDetailTex", UNIT_DETAIL},
                {"uDetailMaskTex", UNIT_DETAIL_MASK}, {"uIrradianceTex", UNIT_IRRADIANCE}, {"uDirectionalTex", UNIT_DIRECTIONAL},
                {"uBakedShadowTex", UNIT_BAKED_SHADOW}, {"uProbeTex", UNIT_PROBES},     {"uSkyTex", UNIT_SKY},
                {"uOutlineMaskTex", UNIT_OUTLINE_MASK},
            };
            for (const auto& [name, unit] : UNITS)
                glUniform1i(U(prog, name), unit);
        }

        void setMaterial(GLuint prog, const SMapMaterial& m, const std::vector<GLuint>& textures, const std::vector<SMapImage>& images, GLuint white) {
            const auto loaded = [&](int image) { return image >= 0 && (size_t)image < textures.size() && textures[image]; };
            const auto tex    = [&](int unit, int image) {
                glActiveTexture(GL_TEXTURE0 + unit);
                glBindTexture(GL_TEXTURE_2D, loaded(image) ? textures[image] : white);
            };
            tex(UNIT_BASE, m.baseTex);
            tex(UNIT_EMISSIVE, m.emissiveTex);
            tex(UNIT_OCCLUSION, m.occlusionTex);
            tex(UNIT_NORMAL, m.normalTex);
            glUniform1i(U(prog, "uNormal"), !loaded(m.normalTex) ? 0 : m.normalTex < (int)images.size() && images[m.normalTex].format == TEX_BC5 ? 2 : 1);
            glUniform1f(U(prog, "uNormalScale"), m.normalScale);
            glUniform1f(U(prog, "uNormalY"), m.normalYDown ? -1.f : 1.f);
            glUniform1i(U(prog, "uOrm"), m.ormFromOcclusion && loaded(m.occlusionTex) ? 1 : 0);
            glUniform2f(U(prog, "uRoughMetal"), m.roughness, m.metalness);
            glUniform2f(U(prog, "uSpecular"), m.specular[0] ? 1.f : 0.f, m.specular[1] ? 1.f : 0.f);
            glUniform1f(U(prog, "uSelfIllumAlbedo"), m.selfIllumAlbedo);
            glUniform1i(U(prog, "uGlass"), m.glass ? 1 : 0);
            glUniform1i(U(prog, "uVertexColor"), m.vertexColor);
            glUniform1i(U(prog, "uBlendMode"), (int)m.blend);
            glUniform1i(U(prog, "uEffect"), m.effect ? 1 : 0);
            if (m.effect) {
                // its masks go where the layers' textures would
                static constexpr int MASK_UNITS[3] = {UNIT_LAYER, UNIT_LAYER_MASK, UNIT_DETAIL_MASK};
                int                  masks         = 0;
                float                xf[12]        = {};
                for (int k = 0; k < 3; ++k)
                    if (loaded(m.effectMaskTex[k])) {
                        tex(MASK_UNITS[masks], m.effectMaskTex[k]);
                        std::copy_n(m.effectMask[k], 4, xf + masks * 4);
                        ++masks;
                    }
                glUniform1i(U(prog, "uEffectMasks"), masks);
                glUniform4fv(U(prog, "uEffectMask[0]"), 3, xf);
                glUniform4f(U(prog, "uEffectA"), m.effectBoost, m.effectOpacity, m.effectFog ? 1.f : 0.f, 0.f);
                glUniform4f(U(prog, "uEffectFade"), m.effectFade[0], m.effectFade[1], m.effectFade[2], m.effectFade[3]);
                glUniform4f(U(prog, "uEffectFresnel"), m.effectFresnel[0], m.effectFresnel[1], m.effectFresnel[2], m.effectFresnel[3]);
            }
            glUniform1i(U(prog, "uNoFog"), m.fog ? 0 : 1);
            glUniform1i(U(prog, "uMod2xLinear"), m.mod2xLinear ? 1 : 0);
            glUniform2f(U(prog, "uScroll"), m.scroll[0], m.scroll[1]);
            glUniform1i(U(prog, "uDoubleSided"), m.doubleSided ? 1 : 0);
            const int layer = !loaded(m.layerTex) ? 0 : loaded(m.layerMaskTex) ? 2 : 1;
            glUniform1i(U(prog, "uLayer"), layer);
            // tint mask and decal use the layer units (CS2 has them on materials without layers)
            const int tintMask = layer || m.effect || !loaded(m.tintMaskTex) ? 0 : 1 + m.tintMaskUV;
            const int decal    = layer || m.effect || !loaded(m.decalTex) ? 0 : m.decal;
            glUniform1i(U(prog, "uTintMask"), tintMask);
            glUniform1i(U(prog, "uDecal"), decal);
            if (tintMask)
                tex(UNIT_LAYER_MASK, m.tintMaskTex);
            if (decal) {
                tex(UNIT_LAYER, m.decalTex);
                glUniform1i(U(prog, "uDecalUV"), m.decalUV);
                glUniform4f(U(prog, "uDecalXf"), m.decalXf[0], m.decalXf[1], m.decalXf[2], m.decalXf[3]);
                glUniform2f(U(prog, "uDecalOffset"), m.decalXf[4], m.decalXf[5]);
            }
            if (layer) {
                tex(UNIT_LAYER, m.layerTex);
                tex(UNIT_LAYER_MASK, m.layerMaskTex);
                tex(UNIT_LAYER_NORMAL, m.layerNormalTex);
                glUniform4f(U(prog, "uLayerColor"), m.layerColor[0], m.layerColor[1], m.layerColor[2], m.layerColor[3]);
                glUniform4f(U(prog, "uLayerXf"), m.layerXf[0], m.layerXf[1], m.layerXf[2], m.layerXf[3]);
                glUniform2f(U(prog, "uLayerOffset"), m.layerXf[4], m.layerXf[5]);
                glUniform1f(U(prog, "uLayerSoftness"), m.layerSoftness);
                glUniform1i(U(prog, "uLayerMaskChannel"), m.layerMaskChannel);
                glUniform1i(U(prog, "uLayerNormal"), loaded(m.layerNormalTex) ? 1 : 0);
                glUniform4f(U(prog, "uLayerMaskXf"), m.layerMaskXf[0], m.layerMaskXf[1], m.layerMaskXf[2], m.layerMaskXf[3]);
                glUniform2f(U(prog, "uLayerMaskOffset"), m.layerMaskXf[4], m.layerMaskXf[5]);
                glUniform3f(U(prog, "uLayer1Tint"), m.layer1Tint[0], m.layer1Tint[1], m.layer1Tint[2]);
                glUniform3f(U(prog, "uBorderTint"), m.borderTint[0], m.borderTint[1], m.borderTint[2]);
                glUniform3f(U(prog, "uBorder"), m.border[0], m.border[1], m.border[2]);
            }
            const int detail = m.detail != DETAIL_NONE && loaded(m.detailTex) ? (int)m.detail : 0;
            glUniform1i(U(prog, "uDetail"), detail);
            if (detail) {
                tex(UNIT_DETAIL, m.detailTex);
                tex(UNIT_DETAIL_MASK, m.detailMaskTex);
                glUniform4f(U(prog, "uDetailXf"), m.detailXf[0], m.detailXf[1], m.detailXf[2], m.detailXf[3]);
                glUniform2f(U(prog, "uDetailOffset"), m.detailXf[4], m.detailXf[5]);
                glUniform3f(U(prog, "uDetailTint"), m.detailTint[0], m.detailTint[1], m.detailTint[2]);
                glUniform2f(U(prog, "uDetailBlend"), m.detailBlend, m.detailBlendToFull);
                glUniform1i(U(prog, "uDetailMask"), loaded(m.detailMaskTex) ? 1 + m.detailMaskUV : 0);
                glUniform1i(U(prog, "uDetailUV"), m.detailUV);
            }
            glUniform4f(U(prog, "uBaseColor"), m.baseColor[0], m.baseColor[1], m.baseColor[2], m.baseColor[3]);
            glUniform4f(U(prog, "uBaseXf"), m.baseXf[0], m.baseXf[1], m.baseXf[2], m.baseXf[3]);
            glUniform2f(U(prog, "uBaseOffset"), m.baseXf[4], m.baseXf[5]);
            glUniform3f(U(prog, "uEmissive"), m.emissive[0], m.emissive[1], m.emissive[2]);
            glUniform1i(U(prog, "uEmissiveUV"), m.emissiveUV);
            glUniform4f(U(prog, "uEmissiveXf"), m.emissiveXf[0], m.emissiveXf[1], m.emissiveXf[2], m.emissiveXf[3]);
            glUniform2f(U(prog, "uEmissiveOffset"), m.emissiveXf[4], m.emissiveXf[5]);
            glUniform1i(U(prog, "uOccUV"), m.occlusionUV);
            glUniform1f(U(prog, "uOccStrength"), m.occlusionTex >= 0 ? m.occlusionStrength : 0.f);
            glUniform1i(U(prog, "uAlphaMode"), (int)m.alphaMode);
            glUniform1f(U(prog, "uCutoff"), m.alphaCutoff);
        }
    }

    namespace {
        void clampedLinear(GLenum target, bool mipmaps) {
            glTexParameteri(target, GL_TEXTURE_MIN_FILTER, mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
            glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            if (target == GL_TEXTURE_3D)
                glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        }

        // lightmap mips stop early: a chart shrunk much further bleeds into its neighbours
        constexpr int LIGHTMAP_LEVELS = 4;

        // HYPRWALK_lighting textures; returns the bytes used
        size_t uploadLighting(const SMapModel& model, auto& gl) {
            const auto& L = model.lighting;
            size_t      bytes = 0;
            gl.lightSets.assign(L.present ? L.sets.size() : 0, {});
            glActiveTexture(GL_TEXTURE0);
            for (size_t k = 0; k < gl.lightSets.size(); ++k) {
                const auto& s   = L.sets[k];
                auto&       out = gl.lightSets[k];
                out.average     = {s.average[0], s.average[1], s.average[2]};
                if (s.irradiance) {
                    // RGB9E5 HDR; mips are made on the loader thread (GL can't render into it to generate them)
                    glGenTextures(1, &out.irradiance);
                    glBindTexture(GL_TEXTURE_2D, out.irradiance);
                    glTexStorage2D(GL_TEXTURE_2D, s.irradiance.levels, GL_RGB9_E5, s.irradiance.w, s.irradiance.h);
                    const uint32_t* px = s.irradiance.texels.data();
                    for (int l = 0, w = s.irradiance.w, h = s.irradiance.h; l < s.irradiance.levels; ++l, w = std::max(1, w / 2), h = std::max(1, h / 2)) {
                        glTexSubImage2D(GL_TEXTURE_2D, l, 0, 0, w, h, GL_RGB, GL_UNSIGNED_INT_5_9_9_9_REV, px);
                        px += (size_t)w * h;
                    }
                    clampedLinear(GL_TEXTURE_2D, s.irradiance.levels > 1);
                    bytes += s.irradiance.texels.size() * 4;
                }
                if (!s.directional.rgba.empty()) {
                    const int levels = std::min(LIGHTMAP_LEVELS, 1 + (int)std::floor(std::log2((float)std::max(s.directional.w, s.directional.h))));
                    glGenTextures(1, &out.directional);
                    glBindTexture(GL_TEXTURE_2D, out.directional);
                    glTexStorage2D(GL_TEXTURE_2D, levels, GL_RGBA8, s.directional.w, s.directional.h);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s.directional.w, s.directional.h, GL_RGBA, GL_UNSIGNED_BYTE, s.directional.rgba.data());
                    glGenerateMipmap(GL_TEXTURE_2D);
                    clampedLinear(GL_TEXTURE_2D, levels > 1);
                    bytes += s.directional.rgba.size() * 4 / 3;
                }
                if (!s.shadows.rgba.empty()) {
                    // CS2 reads it at full size only
                    glGenTextures(1, &out.shadows);
                    glBindTexture(GL_TEXTURE_2D, out.shadows);
                    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, s.shadows.w, s.shadows.h);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s.shadows.w, s.shadows.h, GL_RED, GL_UNSIGNED_BYTE, s.shadows.rgba.data());
                    clampedLinear(GL_TEXTURE_2D, false);
                    bytes += s.shadows.rgba.size();
                }
                if (!s.probes.empty()) {
                    glGenTextures(1, &out.probes);
                    glBindTexture(GL_TEXTURE_3D, out.probes);
                    glTexStorage3D(GL_TEXTURE_3D, 1, GL_RGBA16F, s.probeDims[0], s.probeDims[1], s.probeDims[2] * 6);
                    glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, s.probeDims[0], s.probeDims[1], s.probeDims[2] * 6, GL_RGBA, GL_HALF_FLOAT, s.probes.data());
                    clampedLinear(GL_TEXTURE_3D, false);
                    glBindTexture(GL_TEXTURE_3D, 0);
                    for (int c = 0; c < 3; ++c)
                        out.probeDims[c] = (float)s.probeDims[c];
                    bytes += s.probes.size() * 2;
                }
            }
            // an empty 3D texture for the probe sampler when there are none
            const uint16_t zero[4] = {0, 0, 0, 0};
            glGenTextures(1, &gl.empty3D);
            glBindTexture(GL_TEXTURE_3D, gl.empty3D);
            glTexStorage3D(GL_TEXTURE_3D, 1, GL_RGBA16F, 1, 1, 1);
            glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_HALF_FLOAT, zero);
            clampedLinear(GL_TEXTURE_3D, false);
            glBindTexture(GL_TEXTURE_3D, 0);

            // sky lod for fog: CS2's fog cube is 7 mips down from a face about half the panorama wide
            if (L.skyImage >= 0 && (size_t)L.skyImage < model.images.size())
                gl.skyLod = std::max(0.f, std::log2((float)std::max(model.images[L.skyImage].w, 1)) - 5.f);
            return bytes;
        }

        // the game's tone curve, unnormalized: see gameCurve() in MAP_FS_BODY
        float curve(const float* k, float c) {
            const float num = c * (k[0] * c + k[1] * k[2]) + k[4] * k[3];
            const float den = c * (k[0] * c + k[1]) + k[5] * k[3];
            return num / den - k[4] / k[5];
        }
    }

    bool CRenderer::init(const SWorld& world) {
        destroyBase();
        if (m_map.model != world.model)
            destroyMap();

        gl::CStateGuard guard;

        m_progCapture    = gl::makeProgram("capture", shaders::CAPTURE_VS, shaders::CAPTURE_FS);
        m_progCaptureExt = gl::makeProgram("capture-ext", shaders::CAPTURE_VS, shaders::CAPTURE_EXT_FS);
        m_progLight      = gl::makeProgram("light", shaders::LIGHT_VS, shaders::LIGHT_FS);
        m_progSky        = gl::makeProgram("sky", shaders::SKY_VS, withCommon(shaders::SKY_FS_BODY, true));
        m_progWorld      = gl::makeProgram("world", shaders::WORLD_VS, withCommon(shaders::WORLD_FS_BODY, true, true));
        // dual-source blending for glass when available
        m_dualSource = dualSource && gl::hasExtension("GL_EXT_blend_func_extended");
        m_progMap    = m_dualSource ? gl::makeProgram("map", shaders::MAP_VS, withCommon(shaders::MAP_FS_BODY, true, true, true)) : 0;
        if (!m_progMap) {
            m_dualSource = false;
            m_progMap    = gl::makeProgram("map", shaders::MAP_VS, withCommon(shaders::MAP_FS_BODY, true, true));
        }
        m_progMapDepth   = gl::makeProgram("map-depth", shaders::MAP_DEPTH_VS, shaders::MAP_DEPTH_FS);
        m_progDepth      = gl::makeProgram("depth", shaders::DEPTH_VS, shaders::DEPTH_FS);
        m_progPanel      = gl::makeProgram("panel", shaders::PANEL_VS, shaders::PANEL_FS);
        m_progPanelDepth = gl::makeProgram("panel-depth", shaders::PANEL_VS, shaders::PANEL_DEPTH_FS);
        m_progCross      = gl::makeProgram("crosshair", shaders::CROSS_VS, shaders::CROSS_FS);
        m_progHud        = gl::makeProgram("hud", shaders::HUD_VS, shaders::HUD_FS);
        m_progAvatar     = gl::makeProgram("avatar", std::string(shaders::SKIN_GLSL) + shaders::AVATAR_VS_BODY, withCommon(shaders::MAP_FS_BODY, true, true));
        m_progAvatarDepth = gl::makeProgram("avatar-depth", std::string(shaders::SKIN_GLSL) + shaders::AVATAR_DEPTH_VS_BODY, shaders::MAP_DEPTH_FS);

        // the external-texture program is optional (only needed for some dmabufs)
        if (!m_progCapture || !m_progLight || !m_progSky || !m_progWorld || !m_progDepth || !m_progPanel || !m_progPanelDepth || !m_progCross || !m_progMap ||
            !m_progMapDepth || !m_progAvatar || !m_progAvatarDepth || !m_progHud) {
            logf("renderer init failed: a shader did not compile");
            destroyBase();
            destroyMap();
            return false;
        }

        // unit quad, triangle strip
        const float quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
        glGenVertexArrays(1, &m_quadVAO);
        glGenBuffers(1, &m_quadVBO);
        glBindVertexArray(m_quadVAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_quadVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

        glGenVertexArrays(1, &m_worldVAO);
        glGenBuffers(1, &m_worldVBO);
        glBindVertexArray(m_worldVAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_worldVBO);
        glBufferData(GL_ARRAY_BUFFER, world.vertices.size() * sizeof(SVertex), world.vertices.data(), GL_STATIC_DRAW);
        const GLsizei stride = sizeof(SVertex);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(SVertex, pos));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(SVertex, normal));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(SVertex, uv));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(SVertex, material));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(SVertex, ao));
        m_worldCount = (GLsizei)world.vertices.size();
        glBindVertexArray(0);

        if (world.model && !m_map.model && !uploadMap(world.model)) {
            destroyBase(); // the avatar stays, its CPU copy is gone
            destroyMap();
            return false;
        }

        m_sunDir = world.sunDir;
        GLint maxTex = 2048;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
        if (world.model) {
            const SAABB b      = world.bounds;
            const V3    size   = b.size();
            const float extent = std::max(size.x, size.z);
            m_sunCasters       = {b.min - V3{1, 1, 1}, b.max + V3{1, 1, 1}};
            m_sunFollow        = extent > 70.f;
            m_shadowSize       = std::min(extent > 40.f ? 4096 : 2048, (int)maxTex);
            m_fog              = 0.006f;
            m_sunFocus         = world.spawn;
            setupSun(m_sunCasters, m_sunFollow ? SAABB{{m_sunFocus.x - 35.f, b.min.y, m_sunFocus.z - 35.f}, {m_sunFocus.x + 35.f, b.max.y, m_sunFocus.z + 35.f}} :
                                                 m_sunCasters);
        } else {
            // the courtyard: everything that casts a shadow is below 7m
            const SAABB b{{world.bounds.min.x - 1.f, -0.5f, world.bounds.min.z - 1.f}, {world.bounds.max.x + 1.f, 7.f, world.bounds.max.z + 1.f}};
            m_sunFollow  = false;
            m_shadowSize = std::min(2048, (int)maxTex);
            m_fog        = 0.012f;
            setupSun(b, b);
            m_shadowBias   = 0.0015f;
            m_normalOffset = 0.03f;
        }

        auto makeShadowMap = [this](GLuint& tex, GLuint& fbo) {
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT24, m_shadowSize, m_shadowSize);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, tex, 0);
            const GLenum none = GL_NONE;
            glDrawBuffers(1, &none);
            glReadBuffer(GL_NONE);
            return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        };
        const bool shadowOk = makeShadowMap(m_shadowStaticTex, m_shadowStaticFBO);
        if (!makeShadowMap(m_shadowTex, m_shadowFBO) || !shadowOk) {
            logf("shadow framebuffer incomplete, shadows will be off");
            // an empty map means no shadows rather than garbage
            glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFBO);
            glClearDepthf(1.f);
            glClear(GL_DEPTH_BUFFER_BIT);
            glDeleteFramebuffers(1, &m_shadowStaticFBO);
            m_shadowStaticFBO = 0;
        } else
            bakeShadow();

        m_lightTarget.ensure(16, 1, false);

        GLint maxSamples = 4;
        glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
        m_samples = std::clamp(maxSamples, 1, 4);

        glGenFramebuffers(1, &m_resolveFBO);

        GLenum err = glGetError();
        if (err != GL_NO_ERROR)
            logf("GL error after renderer init: 0x{:x}", err);

        m_ready = true;
        logf("renderer ready: {} world vertices, {} map triangles, {}x MSAA, {}px shadows{}{}", m_worldCount, m_map.model ? m_map.model->triangles : 0, m_samples,
             m_shadowSize, m_sunFollow ? " (following)" : "", m_dualSource ? ", glass with a second source" : "");
        return true;
    }

    // light-space box around `region`, deep enough for `casters`; a fixed view and texel snapping stop edge crawl
    void CRenderer::setupSun(const SAABB& casters, const SAABB& region) {
        const V3 c    = casters.center();
        const M4 view = M4::lookAt(c + m_sunDir * (length(casters.size()) + 10.f), c, {0, 1, 0});
        auto     toLight = [&](const SAABB& b, V3& mn, V3& mx) {
            mn = {1e30f, 1e30f, 1e30f};
            mx = {-1e30f, -1e30f, -1e30f};
            for (int i = 0; i < 8; ++i) {
                const V3 p = view.point({(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z});
                mn         = vmin(mn, p);
                mx         = vmax(mx, p);
            }
        };
        V3 rmn, rmx, cmn, cmx;
        toLight(region, rmn, rmx);
        toLight(casters, cmn, cmx);
        const float hx = (rmx.x - rmn.x) * 0.5f, hy = (rmx.y - rmn.y) * 0.5f;
        float       cx = (rmx.x + rmn.x) * 0.5f, cy = (rmx.y + rmn.y) * 0.5f;
        const float tx = hx * 2.f / m_shadowSize, ty = hy * 2.f / m_shadowSize;
        cx             = std::round(cx / tx) * tx;
        cy             = std::round(cy / ty) * ty;
        const float zn = -cmx.z - 1.f, zf = -cmn.z + 1.f;
        m_sunViewProj  = M4::ortho(cx - hx, cx + hx, cy - hy, cy + hy, zn, zf) * view;
        if (m_map.model) {
            // a few centimeters, whatever the depth range and texel size
            const float texel = std::max(tx, ty);
            m_shadowBias      = std::max(0.03f, texel * 1.5f) / (zf - zn);
            m_normalOffset    = std::max(0.03f, texel * 1.5f);
        }
    }

    bool CRenderer::uploadMap(const std::shared_ptr<SMapModel>& model) {
        const auto start = std::chrono::steady_clock::now();
        if (model->vertices.empty() || model->indices.empty()) {
            logf("map {} has nothing to upload (already released?)", model->path);
            return false;
        }
        m_map.model = model;

        glGenVertexArrays(1, &m_map.vao);
        glGenBuffers(1, &m_map.vbo);
        glGenBuffers(1, &m_map.ibo);
        glBindVertexArray(m_map.vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_map.vbo);
        staticBuffer(GL_ARRAY_BUFFER, model->vertices.size() * sizeof(SMapVertex), model->vertices.data());
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_map.ibo);
        staticBuffer(GL_ELEMENT_ARRAY_BUFFER, model->indices.size() * sizeof(uint32_t), model->indices.data());
        const GLsizei stride = sizeof(SMapVertex);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(SMapVertex, pos));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(SMapVertex, normal));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(SMapVertex, uv));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(SMapVertex, uv1));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)offsetof(SMapVertex, color));
        glEnableVertexAttribArray(5);
        glVertexAttribPointer(5, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)offsetof(SMapVertex, ao));
        glEnableVertexAttribArray(6);
        glVertexAttribPointer(6, 4, GL_SHORT, GL_TRUE, stride, (void*)offsetof(SMapVertex, tangent));
        glEnableVertexAttribArray(7);
        glVertexAttribPointer(7, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(SMapVertex, light));
        glEnableVertexAttribArray(8);
        glVertexAttribIPointer(8, 4, GL_UNSIGNED_BYTE, stride, (void*)offsetof(SMapVertex, lighting));
        glBindVertexArray(0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        size_t bytes = 0, lightBytes = 0;
        {
            CUnpackGuard unpack;
            m_map.white = whiteTexture();
            bytes       = uploadImages(model->images, m_map.textures);
            lightBytes  = uploadLighting(*model, m_map);
        }

        const GLenum err = glGetError();
        logf("uploaded map {}: {} vertices, {} triangles, {} textures ({} MB){} in {} ms{}", model->path, model->vertices.size(), model->triangles, model->images.size(),
             bytes >> 20, lightBytes ? std::format(", its lighting ({} MB)", lightBytes >> 20) : std::string{},
             std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count(),
             err != GL_NO_ERROR ? std::format(", GL error 0x{:x}", err) : std::string{});
        if (err == GL_OUT_OF_MEMORY)
            return false;
        model->releaseCpuData();
        trimHeap();
        return true;
    }

    void CRenderer::destroyMap() {
        for (GLuint* b : {&m_map.vbo, &m_map.ibo}) {
            if (*b)
                glDeleteBuffers(1, b);
            *b = 0;
        }
        if (m_map.vao)
            glDeleteVertexArrays(1, &m_map.vao);
        m_map.vao = 0;
        for (GLuint& t : m_map.textures)
            if (t)
                glDeleteTextures(1, &t);
        for (auto& s : m_map.lightSets)
            for (GLuint* t : {&s.irradiance, &s.directional, &s.shadows, &s.probes})
                if (*t)
                    glDeleteTextures(1, t);
        for (GLuint* t : {&m_map.white, &m_map.empty3D})
            if (*t)
                glDeleteTextures(1, t);
        m_map = {};
    }

    bool CRenderer::uploadAvatar(const std::shared_ptr<SAvatarModel>& model) {
        const auto start = std::chrono::steady_clock::now();
        if (model->vertices.empty() || model->indices.empty() || model->joints.empty()) {
            logf("avatar {} has nothing to upload (already released?)", model->path);
            return false;
        }
        m_avatar.model = model;

        glGenVertexArrays(1, &m_avatar.vao);
        glGenBuffers(1, &m_avatar.vbo);
        glGenBuffers(1, &m_avatar.ibo);
        glBindVertexArray(m_avatar.vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_avatar.vbo);
        staticBuffer(GL_ARRAY_BUFFER, model->vertices.size() * sizeof(SAvatarVertex), model->vertices.data());
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_avatar.ibo);
        staticBuffer(GL_ELEMENT_ARRAY_BUFFER, model->indices.size() * sizeof(uint32_t), model->indices.data());
        const GLsizei stride = sizeof(SAvatarVertex);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(SAvatarVertex, pos));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(SAvatarVertex, normal));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(SAvatarVertex, uv));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(SAvatarVertex, uv1));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)offsetof(SAvatarVertex, color));
        glEnableVertexAttribArray(5);
        glVertexAttribIPointer(5, 4, GL_UNSIGNED_SHORT, stride, (void*)offsetof(SAvatarVertex, joints));
        glEnableVertexAttribArray(6);
        glVertexAttribPointer(6, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*)offsetof(SAvatarVertex, weights));
        // morph offsets (without morphs, attributes 7 and 8 read the zeros drawAvatar sets)
        if (!model->morphs.empty() && model->morphEnd > model->morphFirst) {
            glGenBuffers(1, &m_avatar.morphVBO);
            glBindBuffer(GL_ARRAY_BUFFER, m_avatar.morphVBO);
            const std::vector<float> zeros((size_t)model->morphEnd * 6, 0.f);
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(zeros.size() * sizeof(float)), zeros.data(), GL_DYNAMIC_DRAW);
            m_avatar.morphAcc.assign((size_t)(model->morphEnd - model->morphFirst) * 6, 0.f);
            glEnableVertexAttribArray(7);
            glVertexAttribPointer(7, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
            glEnableVertexAttribArray(8);
            glVertexAttribPointer(8, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
            m_avatar.morphW.assign(model->morphs.size(), 0.f);
            m_avatar.morphSpan.resize(model->morphs.size());
            for (size_t i = 0; i < model->morphs.size(); ++i) {
                const auto& mo = model->morphs[i];
                uint32_t    lo = UINT32_MAX, hi = 0;
                for (uint32_t k = mo.first; k < mo.first + mo.count; ++k) {
                    lo = std::min(lo, model->morphDeltas[k].vertex);
                    hi = std::max(hi, model->morphDeltas[k].vertex + 1);
                }
                m_avatar.morphSpan[i] = {lo, hi};
            }
        }
        glBindVertexArray(0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        // 64 joints a row, 3 texels each
        m_avatar.jointRows = (int)((model->joints.size() + 63) / 64);
        glActiveTexture(GL_TEXTURE5);
        glGenTextures(1, &m_avatar.jointTex);
        glBindTexture(GL_TEXTURE_2D, m_avatar.jointTex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, 64 * 3, m_avatar.jointRows);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m_avatar.staging.assign((size_t)m_avatar.jointRows * 64 * 3 * 4, 0.f);

        size_t bytes = 0;
        {
            CUnpackGuard unpack;
            m_avatar.white = whiteTexture();
            bytes          = uploadImages(model->images, m_avatar.textures);
        }

        const GLenum err = glGetError();
        logf("uploaded avatar {}: {} vertices, {} triangles, {} joints, {} textures ({} MB) in {} ms{}", model->path, model->vertices.size(), model->triangles,
             model->joints.size(), model->images.size(), bytes >> 20,
             std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count(),
             err != GL_NO_ERROR ? std::format(", GL error 0x{:x}", err) : std::string{});
        if (err == GL_OUT_OF_MEMORY)
            return false;
        model->releaseCpuData();
        trimHeap();
        return true;
    }

    void CRenderer::destroyAvatar() {
        for (GLuint* b : {&m_avatar.vbo, &m_avatar.ibo, &m_avatar.morphVBO}) {
            if (*b)
                glDeleteBuffers(1, b);
            *b = 0;
        }
        if (m_avatar.vao)
            glDeleteVertexArrays(1, &m_avatar.vao);
        m_avatar.vao = 0;
        for (GLuint& t : m_avatar.textures)
            if (t)
                glDeleteTextures(1, &t);
        for (GLuint* t : {&m_avatar.white, &m_avatar.jointTex})
            if (*t)
                glDeleteTextures(1, t);
        m_avatar = {};
    }

    bool CRenderer::updateAvatar(const SFrameParams& f) {
        m_avatar.live    = false;
        const auto& want = f.avatar.model;
        if (m_avatar.model != want) {
            destroyAvatar();
            if (!want || m_avatarFailed.lock() == want)
                return false;
            if (!uploadAvatar(want)) {
                destroyAvatar();
                m_avatarFailed = want;
                return false;
            }
        }
        if (!want || !f.avatar.joints || f.avatar.joints->size() != want->joints.size() * 12)
            return false;

        std::ranges::copy(*f.avatar.joints, m_avatar.staging.begin());
        CUnpackGuard unpack;
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, m_avatar.jointTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64 * 3, m_avatar.jointRows, GL_RGBA, GL_FLOAT, m_avatar.staging.data());
        glActiveTexture(GL_TEXTURE0);
        if (f.avatar.morphs)
            updateMorphs(*f.avatar.morphs);
        m_avatar.live = true;
        return true;
    }

    // adds the changes since last frame; a full rewrite every so often keeps rounding from piling up
    void CRenderer::updateMorphs(const std::vector<float>& w) {
        const auto& md = *m_avatar.model;
        auto&       a  = m_avatar;
        if (!a.morphVBO || w.size() != md.morphs.size())
            return;
        size_t i = 0;
        while (i < w.size() && w[i] == a.morphW[i])
            ++i;
        if (i == w.size())
            return;
        const bool full = ++a.morphUpdates % 2000 == 0;
        if (full) {
            std::ranges::fill(a.morphAcc, 0.f);
            std::ranges::fill(a.morphW, 0.f);
            i = 0;
        }
        uint32_t lo = full ? md.morphFirst : UINT32_MAX, hi = full ? md.morphEnd : 0;
        for (; i < w.size(); ++i) {
            const float dw = w[i] - a.morphW[i];
            if (dw == 0.f)
                continue;
            a.morphW[i]     = w[i];
            const auto& mo  = md.morphs[i];
            for (const SMorphDelta* d = md.morphDeltas.data() + mo.first, *e = d + mo.count; d < e; ++d) {
                float* acc = &a.morphAcc[(size_t)(d->vertex - md.morphFirst) * 6];
                for (int k = 0; k < 3; ++k) {
                    acc[k] += dw * d->pos[k];
                    acc[3 + k] += dw * d->normal[k];
                }
            }
            lo = std::min(lo, a.morphSpan[i].first);
            hi = std::max(hi, a.morphSpan[i].second);
        }
        if (lo >= hi)
            return;
        glBindBuffer(GL_ARRAY_BUFFER, a.morphVBO);
        glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)((size_t)lo * 6 * sizeof(float)), (GLsizeiptr)((size_t)(hi - lo) * 6 * sizeof(float)),
                        &a.morphAcc[(size_t)(lo - md.morphFirst) * 6]);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    const std::vector<SMapMaterial>& CRenderer::avatarMaterials(const SFrameParams& f) const {
        const auto& model = *m_avatar.model;
        return f.avatar.materials && f.avatar.materials->size() == model.materials.size() ? *f.avatar.materials : model.materials;
    }

    void CRenderer::drawAvatarDepth(const SFrameParams& f) {
        const auto&  model = *m_avatar.model;
        const GLuint prog  = m_progAvatarDepth;
        glUseProgram(prog);
        uMat(prog, "uViewProj", m_sunViewProj);
        uMat(prog, "uModel", f.avatar.transform * model.fix);
        glUniform1i(U(prog, "uJoints"), 5);
        glUniform1i(U(prog, "uBaseTex"), 0);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, m_avatar.jointTex);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(m_avatar.vao);
        if (!m_avatar.morphVBO) {
            glVertexAttrib4f(7, 0, 0, 0, 1);
            glVertexAttrib4f(8, 0, 0, 0, 1);
        }
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(2.f, 4.f);
        const auto& mats = avatarMaterials(f);
        for (size_t i = 0; i < model.batches.size(); ++i) {
            const auto& b = model.batches[i];
            if (!f.avatar.drawn(b))
                continue;
            const auto& m   = mats[f.avatar.material(b, i)];
            const bool  cut = m.alphaMode != ALPHA_OPAQUE && m.baseTex >= 0 && (size_t)m.baseTex < m_avatar.textures.size() && m_avatar.textures[m.baseTex];
            glBindTexture(GL_TEXTURE_2D, cut ? m_avatar.textures[m.baseTex] : m_avatar.white);
            glUniform4f(U(prog, "uBaseXf"), m.baseXf[0], m.baseXf[1], m.baseXf[2], m.baseXf[3]);
            glUniform2f(U(prog, "uBaseOffset"), m.baseXf[4], m.baseXf[5]);
            glUniform1f(U(prog, "uAlphaScale"), cut ? m.baseColor[3] : 1.f);
            // blended hair and cloth: shadow where mostly opaque
            glUniform1f(U(prog, "uCutoff"), !cut ? 0.f : m.alphaMode == ALPHA_MASK ? m.alphaCutoff : 0.5f);
            glDrawElements(GL_TRIANGLES, b.count, GL_UNSIGNED_INT, (void*)(b.first * sizeof(uint32_t)));
        }
        glDisable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(0.f, 0.f);
    }

    void CRenderer::bakeShadow() {
        glBindFramebuffer(GL_FRAMEBUFFER, m_shadowStaticFBO);
        glViewport(0, 0, m_shadowSize, m_shadowSize);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glClearDepthf(1.f);
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(2.f, 4.f);
        glUseProgram(m_progDepth);
        uMat(m_progDepth, "uViewProj", m_sunViewProj);
        glBindVertexArray(m_worldVAO);
        glDrawArrays(GL_TRIANGLES, 0, m_worldCount);

        if (m_map.model) {
            glBindVertexArray(m_map.vao);
            const auto& mats = m_map.model->materials;
            for (const auto& b : m_map.model->batches) {
                if (!b.castShadow)
                    continue;
                const auto& m = mats[b.material];
                if (m.alphaMode == ALPHA_MASK && m.baseTex >= 0 && m_map.textures[m.baseTex]) {
                    glUseProgram(m_progMapDepth);
                    uMat(m_progMapDepth, "uViewProj", m_sunViewProj);
                    glUniform4f(U(m_progMapDepth, "uBaseXf"), m.baseXf[0], m.baseXf[1], m.baseXf[2], m.baseXf[3]);
                    glUniform2f(U(m_progMapDepth, "uBaseOffset"), m.baseXf[4], m.baseXf[5]);
                    glUniform1f(U(m_progMapDepth, "uAlphaScale"), m.baseColor[3]);
                    glUniform1f(U(m_progMapDepth, "uCutoff"), m.alphaCutoff);
                    glUniform1i(U(m_progMapDepth, "uBaseTex"), 0);
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, m_map.textures[m.baseTex]);
                } else
                    glUseProgram(m_progDepth);
                glDrawElements(GL_TRIANGLES, b.count, GL_UNSIGNED_INT, (void*)(b.first * sizeof(uint32_t)));
            }
        }

        glDisable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(0.f, 0.f);
        m_shadowCasterHash = 0; // copy it over on the next frame
    }

    void CRenderer::updateShadow(const SFrameParams& f) {
        if (!m_shadowStaticFBO)
            return;

        // only redo it when something that casts a shadow moved
        uint64_t   hash = 1469598103934665603ull;
        const auto mix  = [&hash](const void* data, size_t n) {
            const auto* b = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < n; ++i) {
                hash ^= b[i];
                hash *= 1099511628211ull;
            }
        };
        std::vector<size_t> casters;
        for (size_t i = 0; i < f.panels->size(); ++i) {
            const auto& p = (*f.panels)[i];
            // the wallpaper lies flat on the wall: no shadow
            if (p.layer == 0 || p.alpha < 0.5f || p.clip.w < 1 || p.clip.h < 1 || !m_panelGL.contains(p.key))
                continue;
            casters.push_back(i);
            const float v[] = {p.pose.origin.x, p.pose.origin.y, p.pose.origin.z, p.pose.right.x, p.pose.right.y, p.pose.right.z, p.pose.down.x, p.pose.down.y,
                               p.pose.down.z, p.pose.scale, (float)p.box.w, (float)p.box.h, (float)p.clip.x, (float)p.clip.y, (float)p.clip.w, (float)p.clip.h};
            mix(&p.key, sizeof(p.key));
            mix(v, sizeof(v));
        }
        if (m_avatar.live) {
            mix(f.avatar.transform.m, sizeof(f.avatar.transform.m));
            mix(f.avatar.joints->data(), f.avatar.joints->size() * sizeof(float));
            if (f.avatar.morphs)
                mix(f.avatar.morphs->data(), f.avatar.morphs->size() * sizeof(float));
            if (f.avatar.materials) // atlas faces move the cutouts
                for (const auto& m : *f.avatar.materials) {
                    mix(m.baseXf, sizeof(m.baseXf));
                    mix(&m.baseColor[3], sizeof(float));
                }
            if (f.avatar.shown)
                mix(f.avatar.shown->data(), f.avatar.shown->size());
        }
        if (hash == m_shadowCasterHash)
            return;
        m_shadowCasterHash = hash;

        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_shadowStaticFBO);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_shadowFBO);
        glBlitFramebuffer(0, 0, m_shadowSize, m_shadowSize, 0, 0, m_shadowSize, m_shadowSize, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        if (casters.empty() && !m_avatar.live)
            return;

        glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFBO);
        glViewport(0, 0, m_shadowSize, m_shadowSize);
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glUseProgram(m_progPanelDepth);
        uMat(m_progPanelDepth, "uViewProj", m_sunViewProj);
        glUniform1i(U(m_progPanelDepth, "uTex"), 0);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(m_quadVAO);
        for (size_t i : casters) {
            const auto& p = (*f.panels)[i];
            setPanelUniforms(m_progPanelDepth, p, m_panelGL[p.key]);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        if (m_avatar.live)
            drawAvatarDepth(f);
    }

    void CRenderer::destroy() {
        destroyBase();
        destroyMap();
        destroyAvatar();
    }

    void CRenderer::destroyBase() {
        for (GLuint* p : {&m_progCapture, &m_progCaptureExt, &m_progLight, &m_progSky, &m_progWorld, &m_progDepth, &m_progPanel, &m_progPanelDepth, &m_progCross, &m_progMap,
                          &m_progMapDepth, &m_progAvatar, &m_progAvatarDepth, &m_progHud}) {
            if (*p) {
                g_uniforms.erase(*p);
                glDeleteProgram(*p);
            }
            *p = 0;
        }
        g_last = nullptr;
        for (GLuint* b : {&m_quadVBO, &m_worldVBO}) {
            if (*b)
                glDeleteBuffers(1, b);
            *b = 0;
        }
        for (GLuint* a : {&m_quadVAO, &m_worldVAO}) {
            if (*a)
                glDeleteVertexArrays(1, a);
            *a = 0;
        }
        for (GLuint* f : {&m_shadowFBO, &m_shadowStaticFBO, &m_msaaFBO, &m_resolveFBO}) {
            if (*f)
                glDeleteFramebuffers(1, f);
            *f = 0;
        }
        for (GLuint* r : {&m_msaaColor, &m_msaaDepth}) {
            if (*r)
                glDeleteRenderbuffers(1, r);
            *r = 0;
        }
        for (GLuint* t : {&m_shadowTex, &m_shadowStaticTex, &m_hudGL[0].tex, &m_hudGL[1].tex}) {
            if (*t)
                glDeleteTextures(1, t);
            *t = 0;
        }
        for (auto& g : m_hudGL)
            g = {};
        m_lightTarget.destroy();
        for (auto& [k, g] : m_panelGL)
            g.target.destroy();
        m_panelGL.clear();
        m_msaaW = m_msaaH = 0;
        m_ready           = false;
    }

    bool CRenderer::ensureMSAA(int w, int h) {
        if (m_msaaFBO && m_msaaW == w && m_msaaH == h)
            return true;

        if (!m_msaaFBO)
            glGenFramebuffers(1, &m_msaaFBO);
        if (!m_msaaColor)
            glGenRenderbuffers(1, &m_msaaColor);
        if (!m_msaaDepth)
            glGenRenderbuffers(1, &m_msaaDepth);

        glBindRenderbuffer(GL_RENDERBUFFER, m_msaaColor);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, m_samples, GL_RGBA8, w, h);
        glBindRenderbuffer(GL_RENDERBUFFER, m_msaaDepth);
        // with stencil for the avatar's Unity stencil masks (eyes showing through hair)
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, m_samples, GL_DEPTH24_STENCIL8, w, h);

        glBindFramebuffer(GL_FRAMEBUFFER, m_msaaFBO);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_msaaColor);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_msaaDepth);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            logf("MSAA framebuffer incomplete ({}x{}, {} samples)", w, h, m_samples);
            return false;
        }
        m_msaaW = w;
        m_msaaH = h;
        return true;
    }

    void CRenderer::capturePanels(const SFrameParams& f) {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glBindVertexArray(m_quadVAO);
        glActiveTexture(GL_TEXTURE0);

        for (auto& p : *f.panels) {
            auto& g    = m_panelGL[p.key];
            g.lastUsed = m_frame;

            const int pw = std::clamp((int)std::ceil(p.box.w * f.monScale), 1, 8192);
            const int ph = std::clamp((int)std::ceil(p.box.h * f.monScale), 1, 8192);
            // round up so small resizes don't reallocate every frame
            const int tw = std::min(8192, (pw + 127) / 128 * 128);
            const int th = std::min(8192, (ph + 127) / 128 * 128);
            g.target.ensure(tw, th, true);
            g.uvMax[0] = (float)pw / tw;
            g.uvMax[1] = (float)ph / th;

            glBindFramebuffer(GL_FRAMEBUFFER, g.target.fbo);
            glViewport(0, 0, tw, th);
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);

            for (const auto& s : p.surfaces) {
                const bool ext = s.tex->m_type == Render::TEXTURE_EXTERNAL;
                GLuint     prog = ext ? m_progCaptureExt : m_progCapture;
                if (!prog)
                    continue;

                glUseProgram(prog);
                s.tex->bind();
                s.tex->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                s.tex->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                s.tex->setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                s.tex->setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

                glUniform1i(U(prog, "uTex"), 0);
                glUniform4f(U(prog, "uBox"), s.box.x * f.monScale, s.box.y * f.monScale, s.box.w * f.monScale, s.box.h * f.monScale);
                glUniform2f(U(prog, "uTarget"), (float)tw, (float)th);
                glUniform4f(U(prog, "uUV"), s.uvTL.x, s.uvTL.y, s.uvBR.x, s.uvBR.y);
                glUniform1f(U(prog, "uOpaque"), s.tex->m_type == Render::TEXTURE_RGBX ? 1.f : 0.f);
                glUniform1f(U(prog, "uAlpha"), 1.f);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

                if (ext)
                    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
            }

            glBindTexture(GL_TEXTURE_2D, g.target.tex);
            glGenerateMipmap(GL_TEXTURE_2D);
        }

        // drop resources of panels that went away
        std::erase_if(m_panelGL, [this](auto& kv) {
            if (m_frame - kv.second.lastUsed > 30) {
                kv.second.target.destroy();
                return true;
            }
            return false;
        });
    }

    int CRenderer::updateLights(const SFrameParams& f, float* lights) {
        // biggest panels light the scene
        const auto area = [&](int i) {
            const auto& p = (*f.panels)[i];
            return (float)(p.clip.w * p.clip.h) * p.pose.scale * p.pose.scale;
        };
        std::vector<int> idx(f.panels->size());
        std::iota(idx.begin(), idx.end(), 0);
        std::ranges::sort(idx, [&](int a, int b) { return area(a) > area(b); });

        glBindFramebuffer(GL_FRAMEBUFFER, m_lightTarget.fbo);
        glViewport(0, 0, 16, 1);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_BLEND);
        glUseProgram(m_progLight);
        glBindVertexArray(m_quadVAO);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(U(m_progLight, "uTex"), 0);
        glUniform1f(U(m_progLight, "uCount"), 16.f);

        float* A     = lights;
        float* B     = lights + 64;
        float* C     = lights + 128;
        int    count = 0;
        for (int i : idx) {
            if (count >= 16)
                break;
            const auto& p = (*f.panels)[i];
            if (p.alpha < 0.05f || area(i) < 0.06f)
                continue;
            auto it = m_panelGL.find(p.key);
            if (it == m_panelGL.end() || !it->second.target.tex)
                continue;

            const auto& g   = it->second;
            const float pw  = g.uvMax[0] * g.target.w;
            const float ph  = g.uvMax[1] * g.target.h;
            const float lod = std::max(0.f, std::log2(std::max(pw, ph) / 4.f));

            glBindTexture(GL_TEXTURE_2D, g.target.tex);
            glUniform1f(U(m_progLight, "uIndex"), (float)count);
            glUniform2f(U(m_progLight, "uUVMax"), g.uvMax[0], g.uvMax[1]);
            glUniform1f(U(m_progLight, "uLod"), lod);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            const V3 c   = p.pose.at(p.clip.middle()) + p.pose.normal * 0.01f;
            const int k  = count * 4;
            A[k + 0]     = c.x;
            A[k + 1]     = c.y;
            A[k + 2]     = c.z;
            A[k + 3]     = 1.6f * p.alpha;
            B[k + 0]     = p.pose.right.x;
            B[k + 1]     = p.pose.right.y;
            B[k + 2]     = p.pose.right.z;
            B[k + 3]     = (float)p.clip.w * 0.5f * p.pose.scale;
            C[k + 0]     = p.pose.normal.x;
            C[k + 1]     = p.pose.normal.y;
            C[k + 2]     = p.pose.normal.z;
            C[k + 3]     = (float)p.clip.h * 0.5f * p.pose.scale;
            ++count;
        }
        return count;
    }

    void CRenderer::drawSky(const SFrameParams& f, const M4& viewProj) {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);
        glUseProgram(m_progSky);
        uMat(m_progSky, "uInvViewProj", viewProj.inverse());
        uVec3(m_progSky, "uEye", f.eye);
        uVec3(m_progSky, "uSunDir", m_sunDir);
        glUniform1f(U(m_progSky, "uTime"), f.time);
        glUniform1f(U(m_progSky, "uExposure"), f.exposure);
        glBindVertexArray(m_quadVAO);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    void CRenderer::setLighting(GLuint prog, const SFrameParams& f, const M4& viewProj, int lightCount, const float* lights) {
        glUseProgram(prog);
        uMat(prog, "uViewProj", viewProj);
        uMat(prog, "uSunViewProj", m_sunViewProj);
        uVec3(prog, "uSunDir", m_sunDir);
        uVec3(prog, "uEye", f.eye);
        glUniform1f(U(prog, "uShadowBias"), m_shadowBias);
        glUniform1f(U(prog, "uNormalOffset"), m_normalOffset);
        glUniform1f(U(prog, "uFog"), m_fog);
        glUniform1f(U(prog, "uExposure"), f.exposure);

        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, m_shadowTex);
        glUniform1i(U(prog, "uShadow"), 3);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_lightTarget.tex);
        glUniform1i(U(prog, "uLightTex"), 2);

        glUniform1i(U(prog, "uLightCount"), lightCount);
        if (lightCount > 0) {
            glUniform4fv(U(prog, "uLightA[0]"), lightCount, lights);
            glUniform4fv(U(prog, "uLightB[0]"), lightCount, lights + 64);
            glUniform4fv(U(prog, "uLightC[0]"), lightCount, lights + 128);
        }
        glActiveTexture(GL_TEXTURE0);
    }

    void CRenderer::drawWorld(const SFrameParams& f, const M4& viewProj, int lightCount, const float* lights) {
        if (m_worldCount == 0)
            return;
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        setLighting(m_progWorld, f, viewProj, lightCount, lights);
        glBindVertexArray(m_worldVAO);
        glDrawArrays(GL_TRIANGLES, 0, m_worldCount);
    }

    void CRenderer::drawMap(const SFrameParams& f, const M4& viewProj, int lightCount, const float* lights, eMapPass pass) {
        if (!m_map.model)
            return;
        const auto& model = *m_map.model;
        const auto  wanted = [&](const SMapBatch& b) {
            if (!b.render)
                return false;
            if (b.sky)
                return pass == MAP_PASS_SKY;
            const bool blend = model.materials[b.material].alphaMode == ALPHA_BLEND;
            if (b.backdrop)
                return pass == (blend ? MAP_PASS_BACKDROP_BLEND : MAP_PASS_BACKDROP);
            return pass == (blend ? MAP_PASS_BLEND : MAP_PASS_OPAQUE);
        };
        if (std::ranges::none_of(model.batches, wanted))
            return;

        switch (pass) {
            case MAP_PASS_SKY:
                // behind everything, like the procedural sky it covers
                glDisable(GL_DEPTH_TEST);
                glDepthMask(GL_FALSE);
                glEnable(GL_BLEND);
                break;
            case MAP_PASS_BACKDROP:
            case MAP_PASS_OPAQUE:
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LESS);
                glDepthMask(GL_TRUE);
                glDisable(GL_BLEND);
                break;
            case MAP_PASS_BACKDROP_BLEND:
            case MAP_PASS_BLEND:
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LEQUAL);
                glDepthMask(GL_FALSE);
                glEnable(GL_BLEND);
                break;
        }
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        const GLuint prog     = m_progMap;
        const bool   backdrop = pass == MAP_PASS_BACKDROP || pass == MAP_PASS_BACKDROP_BLEND;
        setLighting(prog, f, viewProj, lightCount, lights);
        if (backdrop)
            glUniform1f(U(prog, "uFog"), m_fog * BACKDROP_FOG);
        setSamplerUnits(prog);
        setBakedLighting(prog, f, backdrop ? 1 : 0);
        glUniform1f(U(prog, "uTime"), f.time);
        glBindVertexArray(m_map.vao);

        constexpr int GLASS_DUAL = 0x100;
        int           blending   = BLEND_NORMAL;
        for (const auto& b : model.batches) {
            if (!wanted(b))
                continue;
            const auto& m = model.materials[b.material];
            // mod2x decals, additive glows, and glass keeping the background per channel by its second color (fragKeep)
            const int want = m.alphaMode != ALPHA_BLEND ? BLEND_NORMAL : m.glass && m_dualSource ? GLASS_DUAL : m.blend;
            if (want != blending) {
                blending = want;
                if (want == BLEND_MOD2X)
                    glBlendFuncSeparate(GL_DST_COLOR, GL_SRC_COLOR, GL_ZERO, GL_ONE);
                else if (want == BLEND_ADD)
                    glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
                else if (want == GLASS_DUAL)
                    glBlendFuncSeparate(GL_ONE, GL_SRC1_COLOR_EXT, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                else
                    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            }
            setMaterial(prog, m, m_map.textures, model.images, m_map.white);
            glUniform1i(U(prog, "uMode"), b.sky ? 2 : m.unlit ? 1 : 0);
            // the 3D skybox's additive light (clouds, sun glow) goes over the sky, which the shader knows
            glUniform1i(U(prog, "uOverSky"), backdrop && want == BLEND_ADD ? 1 : 0);
            glDrawElements(GL_TRIANGLES, b.count, GL_UNSIGNED_INT, (void*)(b.first * sizeof(uint32_t)));
        }
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glActiveTexture(GL_TEXTURE0);
    }

    void CRenderer::setBakedLighting(GLuint prog, const SFrameParams& f, size_t set) {
        const bool baked = m_map.model && m_map.model->lighting.present && !m_map.lightSets.empty();
        glUniform1i(U(prog, "uBaked"), baked ? 1 : 0);
        // backdrop effects fade by distances in its own, smaller units; its fog is the map's
        if (set == 1 && m_map.model) {
            const M4&   b = m_map.model->backdropTransform;
            const float s = std::max(length(V3{b.m[0], b.m[1], b.m[2]}), 1e-3f);
            glUniform2f(U(prog, "uFogSpace"), 1.f / s, b.m[13]);
        } else
            glUniform2f(U(prog, "uFogSpace"), 1.f, 0.f);
        glActiveTexture(GL_TEXTURE0 + UNIT_PROBES);
        glBindTexture(GL_TEXTURE_3D, m_map.empty3D);
        glActiveTexture(GL_TEXTURE0);
        if (!baked)
            return;
        const auto& L  = m_map.model->lighting;
        // the backdrop's own when it has them, else the map's
        const auto& gl = m_map.lightSets[set < m_map.lightSets.size() ? set : 0];
        const auto  bind = [&](int unit, GLenum target, GLuint tex) {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(target, tex ? tex : target == GL_TEXTURE_3D ? m_map.empty3D : m_map.white);
        };
        bind(UNIT_IRRADIANCE, GL_TEXTURE_2D, gl.irradiance);
        bind(UNIT_DIRECTIONAL, GL_TEXTURE_2D, gl.directional);
        bind(UNIT_BAKED_SHADOW, GL_TEXTURE_2D, gl.shadows);
        // CS2: sun shadow = (1 - baked channel) * realtime shadow; a sun with no channel has only the realtime one
        glUniform1i(U(prog, "uBakedShadow"), gl.shadows ? 1 : 0);
        bind(UNIT_PROBES, GL_TEXTURE_3D, gl.probes);
        const bool sky = L.skyImage >= 0 && (size_t)L.skyImage < m_map.textures.size() && m_map.textures[L.skyImage];
        bind(UNIT_SKY, GL_TEXTURE_2D, sky ? m_map.textures[L.skyImage] : 0);
        glActiveTexture(GL_TEXTURE0);
        glUniform3f(U(prog, "uProbeDims"), gl.probeDims[0], gl.probeDims[1], gl.probeDims[2]);
        const V3 ambient = length(gl.average) > 0.f ? gl.average : m_map.lightSets[0].average;
        uVec3(prog, "uAmbient", ambient);
        uVec3(prog, "uSunColor", L.sunColor);
        glUniform1i(U(prog, "uHasSky"), sky ? 1 : 0);
        uVec3(prog, "uSkyColor", L.skyColor);
        uVec3(prog, "uSkyAverage", L.skyAverage);
        glUniform1f(U(prog, "uSkyLod"), m_map.skyLod);
        // CS2's cubemap fog
        if (L.fog && L.fogEnd > L.fogStart) {
            glUniform4f(U(prog, "uFogA"), L.fogStart, 1.f / (L.fogEnd - L.fogStart), L.fogExponent, L.fogMaxOpacity);
            if (L.fogHeightEnd > L.fogHeightStart) {
                const float scale = 1.f / (L.fogHeightStart - L.fogHeightEnd);
                glUniform4f(U(prog, "uFogB"), 1.f - L.fogHeightStart * scale, scale, L.fogHeightExponent, L.fogLodBias);
            } else
                glUniform4f(U(prog, "uFogB"), 1.f, 1e-6f, 0.f, L.fogLodBias);
        } else
            glUniform4f(U(prog, "uFogA"), 0.f, 0.f, 1.f, 0.f);
        // the tone curve, and its exposure bias on top of ours
        const float* k     = L.curve;
        const float  white = k[6] * 2.8f;
        const float  top   = curve(k, white);
        glUniform4f(U(prog, "uCurveA"), k[0], k[1], k[2], k[3]);
        glUniform4f(U(prog, "uCurveB"), k[4], k[5], white, std::abs(top) > 1e-9f ? 1.f / top : 1.f);
        glUniform1f(U(prog, "uExposure"), f.exposure * std::exp2(k[7]));
    }

    // a game's 3D skybox in its own depth range so the map keeps its precision; the map draws over it, as in Source
    void CRenderer::drawBackdrop(const SFrameParams& f, int lightCount, const float* lights) {
        if (!m_map.model || !std::ranges::any_of(m_map.model->batches, [](const SMapBatch& b) { return b.backdrop && b.render; }))
            return;
        const SAABB& b   = m_map.model->backdropBounds;
        float        far = 0;
        for (int i = 0; i < 8; ++i)
            far = std::max(far, length(V3{(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z} - f.eye));
        const float near = 1.f;
        far              = std::max(far * 1.01f, near * 2.f);
        M4 proj          = f.proj; // the same lens, other planes
        proj.m[10]       = (far + near) / (near - far);
        proj.m[14]       = 2.f * far * near / (near - far);
        const M4 viewProj = flipY() * proj * f.view;
        drawMap(f, viewProj, lightCount, lights, MAP_PASS_BACKDROP);
        drawMap(f, viewProj, lightCount, lights, MAP_PASS_BACKDROP_BLEND);
        glDepthMask(GL_TRUE);
        glClear(GL_DEPTH_BUFFER_BIT);
    }

    namespace {
        GLenum stencilFunc(eStencilComp c) {
            static constexpr GLenum FUNCS[] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
            return FUNCS[std::min<size_t>(c, 7)];
        }
        GLenum stencilOp(eStencilOp o) {
            static constexpr GLenum OPS[] = {GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP};
            return OPS[std::min<size_t>(o, 7)];
        }
    }

    void CRenderer::drawAvatar(const SFrameParams& f, const M4& viewProj, int lightCount, const float* lights, bool late) {
        if (!m_avatar.live || !f.avatar.visible)
            return;
        const auto& model = *m_avatar.model;
        // opaque and alpha-tested batches before the windows, blended or stencil-tested ones after them and the map's
        // glass (as the material variant says); by render queue as in Unity, stencil masks first
        const auto&                         mats = avatarMaterials(f);
        std::vector<std::pair<int, size_t>> order; // (queue, batch)
        for (size_t i = 0; i < model.batches.size(); ++i) {
            const auto& b = model.batches[i];
            if (!f.avatar.drawn(b) || !f.avatar.range(b).second)
                continue;
            const auto& m = mats[f.avatar.material(b, i)];
            if ((m.alphaMode == ALPHA_BLEND || m.stencil.reads()) == late)
                order.emplace_back(m.renderQueue(), i);
        }
        if (order.empty())
            return;
        std::ranges::stable_sort(order, {}, &std::pair<int, size_t>::first);

        glEnable(GL_DEPTH_TEST);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        // flipY() mirrors the output, so front faces are clockwise
        glFrontFace(GL_CW);
        glCullFace(GL_FRONT);

        const GLuint prog = m_progAvatar;
        setLighting(prog, f, viewProj, lightCount, lights);
        uMat(prog, "uModel", f.avatar.transform * model.fix);
        glUniform1f(U(prog, "uSky"), f.avatar.sky);
        glUniform1f(U(prog, "uBounce"), f.avatar.bounce);
        glUniform1i(U(prog, "uJoints"), 5);
        setSamplerUnits(prog);
        setBakedLighting(prog, f, 0);
        // with baked lighting, lit the way the game lights players: per pixel from the probe volume it's in
        int  mode = LIGHT_OWN;
        M4   probe = M4::identity();
        V3   lo, hi;
        if (m_map.model && m_map.model->lighting.present && !m_map.lightSets.empty()) {
            mode             = LIGHT_FLAT;
            const auto& set  = m_map.model->lighting.sets[0];
            const V3    feet{f.avatar.transform.m[12], f.avatar.transform.m[13], f.avatar.transform.m[14]};
            if (const auto* vol = m_map.lightSets[0].probes ? chooseLightProbe(set, feet + V3{0, 0.9f, 0}) : nullptr) {
                mode = LIGHT_PROBE;
                M4 toAtlas = M4::identity();
                for (int c = 0; c < 3; ++c) {
                    toAtlas.m[c * 5]  = (float)vol->atlasSize[c];
                    toAtlas.m[12 + c] = (float)vol->atlasOffset[c];
                }
                probe = toAtlas * vol->toBox;
                lo    = {vol->atlasOffset[0] + 0.5f, vol->atlasOffset[1] + 0.5f, vol->atlasOffset[2] + 0.5f};
                hi    = {std::max(lo.x, vol->atlasOffset[0] + vol->atlasSize[0] - 0.5f), std::max(lo.y, vol->atlasOffset[1] + vol->atlasSize[1] - 0.5f),
                         std::max(lo.z, vol->atlasOffset[2] + vol->atlasSize[2] - 0.5f)};
            }
        }
        glUniform1i(U(prog, "uLightMode"), mode);
        uMat(prog, "uProbeMatrix", probe);
        uVec3(prog, "uProbeMin", lo);
        uVec3(prog, "uProbeMax", hi);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, m_avatar.jointTex);
        glBindVertexArray(m_avatar.vao);
        if (!m_avatar.morphVBO) {
            glVertexAttrib4f(7, 0, 0, 0, 1);
            glVertexAttrib4f(8, 0, 0, 0, 1);
        }
        const auto loaded = [&](int image) { return image >= 0 && (size_t)image < m_avatar.textures.size() && m_avatar.textures[image]; };
        const M4    world = f.avatar.transform * model.fix;
        const float scale = length(V3{world.m[0], world.m[1], world.m[2]}); // the model's (lilToon's outlines scale with it)
        uVec3(prog, "uEye", f.eye);
        uVec3(prog, "uViewUp", V3{f.view.m[1], f.view.m[5], f.view.m[9]}); // (the view's second row)
        glUniform1f(U(prog, "uAspect"), (float)f.height / (float)std::max(f.width, 1));
        glUniform1i(U(prog, "uOutline"), 0);
        glUniform1f(U(prog, "uAgain"), 0.f);
        for (const auto& [queue, i] : order) {
            const auto& b     = model.batches[i];
            const auto& m     = mats[f.avatar.material(b, i)];
            const bool  blend = m.alphaMode == ALPHA_BLEND;
            const auto [first, count] = f.avatar.range(b);
            const auto  draw  = [&] { glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, (void*)(first * sizeof(uint32_t))); };
            if (blend) {
                glDepthFunc(GL_LEQUAL);
                glDepthMask(GL_FALSE);
                glEnable(GL_BLEND);
            } else {
                glDepthFunc(GL_LESS);
                glDepthMask(GL_TRUE);
                glDisable(GL_BLEND);
            }
            const auto& st = m.stencil;
            if (st.on) {
                glEnable(GL_STENCIL_TEST);
                glStencilFunc(stencilFunc(st.comp), st.ref, st.read);
                glStencilOp(stencilOp(st.fail), stencilOp(st.zfail), stencilOp(st.pass));
                glStencilMask(st.write);
            } else
                glDisable(GL_STENCIL_TEST);
            setMaterial(prog, m, m_avatar.textures, model.images, m_avatar.white);
            glUniform1i(U(prog, "uMode"), m.unlit ? 1 : 0);
            // UnlitWF's back faces and light clamp
            const int back = m.back == 2 && !loaded(m.backTex) ? 1 : m.back;
            glUniform1i(U(prog, "uBack"), back);
            if (back) {
                glUniform4f(U(prog, "uBackColor"), m.backColor[0], m.backColor[1], m.backColor[2], m.backColor[3]);
                glUniform4f(U(prog, "uBackXf"), m.backXf[0], m.backXf[1], m.backXf[2], m.backXf[3]);
                glUniform2f(U(prog, "uBackOffset"), m.backXf[4], m.backXf[5]);
                if (back == 2) {
                    glActiveTexture(GL_TEXTURE0 + UNIT_LAYER);
                    glBindTexture(GL_TEXTURE_2D, m_avatar.textures[m.backTex]);
                }
            }
            glUniform3f(U(prog, "uLightClamp"), m.lightClamp[0], m.lightClamp[1], m.lightClamp[2]);
            // toon shade and matcap textures use the layer mask and detail mask units (MAP_FS_BODY)
            const auto& tn = m.toon;
            glUniform1i(U(prog, "uToon"), !tn.on ? 0 : loaded(tn.shadeTex) ? 2 : 1);
            if (tn.on) {
                glUniform4f(U(prog, "uToonShade"), tn.shade[0], tn.shade[1], tn.shade[2], tn.shadeBase ? 1.f : 0.f);
                glUniform3f(U(prog, "uToonStep"), tn.lo, tn.hi, tn.strength);
                if (loaded(tn.shadeTex)) {
                    glActiveTexture(GL_TEXTURE0 + UNIT_LAYER_MASK);
                    glBindTexture(GL_TEXTURE_2D, m_avatar.textures[tn.shadeTex]);
                }
            }
            const bool matcap = loaded(tn.matcapTex);
            glUniform1i(U(prog, "uMatcap"), matcap ? (int)tn.matcapMode + 1 : 0);
            if (matcap) {
                glUniform4f(U(prog, "uMatcapColor"), tn.matcap[0], tn.matcap[1], tn.matcap[2], tn.matcap[3]);
                glUniform1f(U(prog, "uMatcapLit"), tn.matcapLit);
                glActiveTexture(GL_TEXTURE0 + UNIT_DETAIL_MASK);
                glBindTexture(GL_TEXTURE_2D, m_avatar.textures[tn.matcapTex]);
            }
            // the outline first, as Unity's toon shaders draw it: the mesh pushed out, its front faces culled
            if (const auto& ol = m.outline; ol.width > 0 && f.avatar.outlines) {
                glEnable(GL_CULL_FACE);
                glUniform1i(U(prog, "uOutline"), ol.space == OUTLINE_SCREEN ? 2 : 1);
                glUniform4f(U(prog, "uOutlineA"), ol.width * (ol.space == OUTLINE_OBJECT ? scale : 1.f), ol.shift, ol.fix, ol.fixMax);
                const bool mask = loaded(ol.maskTex);
                glUniform4f(U(prog, "uOutlineMask"), !mask || ol.maskChannel == 0 ? 1.f : 0.f, mask && ol.maskChannel == 1 ? 1.f : 0.f,
                            mask && ol.maskChannel == 2 ? 1.f : 0.f, mask && ol.maskChannel == 3 ? 1.f : 0.f);
                glUniform1f(U(prog, "uOutlineInvert"), mask && ol.maskInvert ? 1.f : 0.f);
                glUniform1f(U(prog, "uOutlineMaxW"), ol.maxW);
                glUniform4f(U(prog, "uOutlineColor"), ol.color[0], ol.color[1], ol.color[2], ol.color[3]);
                glUniform3f(U(prog, "uOutlineMix"), ol.base, ol.tint, ol.lit);
                glActiveTexture(GL_TEXTURE0 + UNIT_OUTLINE_MASK);
                glBindTexture(GL_TEXTURE_2D, mask ? m_avatar.textures[ol.maskTex] : m_avatar.white);
                // outline color texture in the detail unit (avatars have none; every other sampler is taken)
                const bool colorTex = loaded(ol.colorTex);
                glUniform2f(U(prog, "uOutlineTex"), colorTex && ol.colorBlend < 0 ? 1.f : 0.f, colorTex && ol.colorBlend >= 0 ? ol.colorBlend : 0.f);
                if (colorTex) {
                    glUniform4f(U(prog, "uOutlineTexXf"), ol.colorXf[0], ol.colorXf[1], ol.colorXf[2], ol.colorXf[3]);
                    glUniform2f(U(prog, "uOutlineTexOffset"), ol.colorXf[4], ol.colorXf[5]);
                    glActiveTexture(GL_TEXTURE0 + UNIT_DETAIL);
                    glBindTexture(GL_TEXTURE_2D, m_avatar.textures[ol.colorTex]);
                }
                draw();
                glUniform2f(U(prog, "uOutlineTex"), 0.f, 0.f);
                glUniform1i(U(prog, "uOutline"), 0);
                glDisable(GL_CULL_FACE);
            }
            draw();
            // UnlitWF's MaskOut_Blend: drawn again, fainter, where its stencil mask hid it
            if (st.on && st.again > 0) {
                glEnable(GL_BLEND);
                glDepthFunc(GL_LEQUAL);
                glDepthMask(GL_FALSE);
                glStencilFunc(stencilFunc(st.againComp), st.ref, st.read);
                glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
                glUniform1f(U(prog, "uAgain"), st.again);
                draw();
                glUniform1f(U(prog, "uAgain"), 0.f);
            }
        }
        glDisable(GL_STENCIL_TEST);
        glStencilMask(0xFF);
        glFrontFace(GL_CCW);
        glCullFace(GL_BACK);
        glActiveTexture(GL_TEXTURE0);
    }

    void CRenderer::setPanelUniforms(GLuint prog, const SPanel& p, const SPanelGL& g) {
        const float s = p.pose.scale;
        uVec3(prog, "uOrigin", p.pose.origin);
        uVec3(prog, "uRight", p.pose.right * (float)(p.box.w * s));
        uVec3(prog, "uDown", p.pose.down * (float)(p.box.h * s));
        glBindTexture(GL_TEXTURE_2D, g.target.tex);
        glUniform2f(U(prog, "uUVMax"), g.uvMax[0], g.uvMax[1]);
        glUniform2f(U(prog, "uSize"), p.box.w, p.box.h);
        glUniform1f(U(prog, "uRadius"), std::clamp(p.rounding, 0.f, (float)std::min(p.box.w, p.box.h) * 0.5f));
        glUniform4f(U(prog, "uClip"), p.clip.x, p.clip.y, p.clip.x + p.clip.w, p.clip.y + p.clip.h);
    }

    void CRenderer::drawPanels(const SFrameParams& f, const M4& viewProj, bool front) {
        if (front && std::ranges::none_of(*f.panels, [](const SPanel& p) { return p.front; }))
            return;
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glUseProgram(m_progPanel);
        uMat(m_progPanel, "uViewProj", viewProj);
        glUniform1i(U(m_progPanel, "uTex"), 0);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(m_quadVAO);

        const auto visible = [&](const SPanel& p) { return p.front == front && p.clip.w >= 1 && p.clip.h >= 1 && m_panelGL.contains(p.key); };
        const auto facing  = [&](const SPanel& p) { return dot(f.eye - p.pose.origin, p.pose.normal) > 0.f; };

        // opaque parts of world panels go into the depth buffer first so crossing panels occlude correctly; desktop
        // wall panels are parallel and drawn back to front; front panels (over everything) go last, in order
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glUniform1f(U(m_progPanel, "uMinAlpha"), 0.96f);
        glUniform4f(U(m_progPanel, "uOutline"), 0, 0, 0, 0);
        for (const auto& p : *f.panels) {
            if (front || !p.depthWrite || !visible(p))
                continue;
            setPanelUniforms(m_progPanel, p, m_panelGL[p.key]);
            glUniform1f(U(m_progPanel, "uAlpha"), p.alpha);
            glUniform1f(U(m_progPanel, "uFront"), facing(p) ? 1.f : 0.f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        if (front)
            glDisable(GL_DEPTH_TEST);

        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glUniform1f(U(m_progPanel, "uMinAlpha"), 0.002f);
        for (size_t i = 0; i < f.panels->size(); ++i) {
            const auto& p = (*f.panels)[i];
            if (!visible(p))
                continue;

            float outline[4] = {0, 0, 0, 0};
            if (p.held) {
                outline[0] = 0.45f;
                outline[1] = 0.85f;
                outline[2] = 1.0f;
                outline[3] = 3.f;
            } else if (f.hudAlpha > 0.f && (int)i == f.aimed && p.kind != PANEL_LAYER) {
                outline[0] = 1.0f;
                outline[1] = 0.82f;
                outline[2] = 0.4f;
                outline[3] = 2.5f * f.hudAlpha;
            }

            setPanelUniforms(m_progPanel, p, m_panelGL[p.key]);
            glUniform1f(U(m_progPanel, "uAlpha"), p.alpha);
            glUniform1f(U(m_progPanel, "uFront"), facing(p) ? 1.f : 0.f);
            glUniform4f(U(m_progPanel, "uOutline"), outline[0], outline[1], outline[2], outline[3]);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
    }

    void CRenderer::drawCrosshair(const SFrameParams& f) {
        if (!f.crosshair || f.hudAlpha <= 0.f)
            return;
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(m_progCross);
        glBindVertexArray(m_quadVAO);
        const float scale = std::max(1.f, f.monScale);
        glUniform2f(U(m_progCross, "uViewport"), (float)f.width, (float)f.height);
        glUniform1f(U(m_progCross, "uExtent"), (f.crosshairDot ? 3.5f : 16.f) * scale); // (a dot's quad cuts the arms off)
        glUniform1f(U(m_progCross, "uScale"), scale);
        glUniform1f(U(m_progCross, "uAlpha"), f.hudAlpha);
        if (f.typing) {
            glUniform4f(U(m_progCross, "uColor"), 0.45f, 0.85f, 1.0f, 1.f);
            glUniform1f(U(m_progCross, "uDot"), 1.f);
        } else if (f.aimed >= 0) {
            glUniform4f(U(m_progCross, "uColor"), 1.0f, 0.85f, 0.4f, 1.f);
            glUniform1f(U(m_progCross, "uDot"), 1.f);
        } else {
            glUniform4f(U(m_progCross, "uColor"), 0.35f, 1.0f, 0.4f, 1.f);
            glUniform1f(U(m_progCross, "uDot"), f.crosshairDot ? 1.f : 0.f);
        }
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    void CRenderer::drawHud(const SFrameParams& f) {
        drawHudImage(f, f.menu, m_hudGL[0]);
        drawHudImage(f, f.badge, m_hudGL[1]);
    }

    void CRenderer::drawHudImage(const SFrameParams& f, const SHudImage& m, SHudGL& g) {
        if (!m.pixels || m.w < 1 || m.h < 1 || m.pixels->size() < (size_t)m.w * m.h || m.alpha <= 0.f)
            return;

        glActiveTexture(GL_TEXTURE0);
        if (!g.tex) {
            glGenTextures(1, &g.tex);
            glBindTexture(GL_TEXTURE_2D, g.tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            g.serial = 0;
        } else
            glBindTexture(GL_TEXTURE_2D, g.tex);

        if (m.serial != g.serial || m.w != g.w || m.h != g.h) {
            CUnpackGuard unpack;
            if (m.w != g.w || m.h != g.h)
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m.w, m.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, m.pixels->data());
            else
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m.w, m.h, GL_RGBA, GL_UNSIGNED_BYTE, m.pixels->data());
            g.w      = m.w;
            g.h      = m.h;
            g.serial = m.serial;
        }

        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(m_progHud);
        glBindVertexArray(m_quadVAO);
        // on whole pixels, so at its own size the text stays sharp
        const float w = m.w * m.scale, h = m.h * m.scale;
        glUniform2f(U(m_progHud, "uViewport"), (float)f.width, (float)f.height);
        glUniform4f(U(m_progHud, "uRect"), std::round(m.x - w / 2.f), std::round(m.y - h / 2.f), w, h);
        glUniform1i(U(m_progHud, "uTex"), 0);
        glUniform2f(U(m_progHud, "uSize"), (float)m.w, (float)m.h);
        glUniform3f(U(m_progHud, "uCursor"), m.cursor[0], m.cursor[1], m.cursor[2]);
        glUniform1f(U(m_progHud, "uAlpha"), std::min(m.alpha, 1.f));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    void CRenderer::render(const SFrameParams& f, GLuint outTex) {
        if (!m_ready || !f.panels || f.width < 1 || f.height < 1)
            return;

        gl::CStateGuard guard;
        ++m_frame;

        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        capturePanels(f);
        updateAvatar(f);
        if (m_sunFollow && m_shadowStaticFBO) {
            const V3 d{f.eye.x - m_sunFocus.x, 0, f.eye.z - m_sunFocus.z};
            if (length(d) > 35.f * 0.3f) {
                m_sunFocus = f.eye;
                setupSun(m_sunCasters, {{f.eye.x - 35.f, m_sunCasters.min.y, f.eye.z - 35.f}, {f.eye.x + 35.f, m_sunCasters.max.y, f.eye.z + 35.f}});
                bakeShadow();
            }
        }
        updateShadow(f);

        float     lights[192] = {};
        const int lightCount  = updateLights(f, lights);

        if (!ensureMSAA(f.width, f.height))
            return;

        // our textures are sampled with row 0 = top, so render upside down
        const M4 viewProj = flipY() * f.proj * f.view;

        glBindFramebuffer(GL_FRAMEBUFFER, m_msaaFBO);
        glViewport(0, 0, f.width, f.height);
        glClearColor(0, 0, 0, 1);
        glClearDepthf(1.f);
        glClearStencil(0);
        glDepthMask(GL_TRUE);
        glStencilMask(0xFF);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        drawSky(f, viewProj);
        drawMap(f, viewProj, lightCount, lights, MAP_PASS_SKY);
        drawBackdrop(f, lightCount, lights);
        drawWorld(f, viewProj, lightCount, lights);
        drawMap(f, viewProj, lightCount, lights, MAP_PASS_OPAQUE);
        drawAvatar(f, viewProj, lightCount, lights, false);
        drawPanels(f, viewProj, false);
        // glass after the windows, which are mostly on walls behind it
        drawMap(f, viewProj, lightCount, lights, MAP_PASS_BLEND);
        drawAvatar(f, viewProj, lightCount, lights, true);
        drawPanels(f, viewProj, true);
        drawCrosshair(f);
        drawHud(f);

        // resolve into the output texture
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_resolveFBO);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outTex, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_msaaFBO);
        glBlitFramebuffer(0, 0, f.width, f.height, 0, 0, f.width, f.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
}
