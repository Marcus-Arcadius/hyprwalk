#include "gltf.hpp"

#include "third_party/cgltf.h"
#include "third_party/stb_image.h"
#include "third_party/stb_dxt.h"

#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string_view>
#include <tuple>

namespace h3d::gltf {

    namespace {
        constexpr int MAX_TEXTURE = 2048;

        bool base64Decode(std::string_view in, std::vector<uint8_t>& out) {
            auto val = [](char c) -> int {
                if (c >= 'A' && c <= 'Z')
                    return c - 'A';
                if (c >= 'a' && c <= 'z')
                    return c - 'a' + 26;
                if (c >= '0' && c <= '9')
                    return c - '0' + 52;
                if (c == '+' || c == '-')
                    return 62;
                if (c == '/' || c == '_')
                    return 63;
                return -1;
            };
            out.clear();
            uint32_t acc = 0;
            int      bits = 0;
            for (char c : in) {
                if (c == '=')
                    break;
                const int v = val(c);
                if (v < 0)
                    continue;
                acc = (acc << 6) | (uint32_t)v;
                bits += 6;
                if (bits >= 8) {
                    bits -= 8;
                    out.push_back((uint8_t)(acc >> bits));
                }
            }
            return !out.empty();
        }

        // halves the image until it fits MAX_TEXTURE (big maps have a lot of textures)
        void shrink(SMapImage& img) {
            while (img.w > MAX_TEXTURE || img.h > MAX_TEXTURE) {
                const int            nw = std::max(1, img.w / 2), nh = std::max(1, img.h / 2);
                std::vector<uint8_t> out((size_t)nw * nh * 4);
                for (int y = 0; y < nh; ++y) {
                    for (int x = 0; x < nw; ++x) {
                        const int x0 = std::min(x * 2, img.w - 1), x1 = std::min(x * 2 + 1, img.w - 1);
                        const int y0 = std::min(y * 2, img.h - 1), y1 = std::min(y * 2 + 1, img.h - 1);
                        for (int c = 0; c < 4; ++c) {
                            const int s = img.rgba[((size_t)y0 * img.w + x0) * 4 + c] + img.rgba[((size_t)y0 * img.w + x1) * 4 + c] +
                                img.rgba[((size_t)y1 * img.w + x0) * 4 + c] + img.rgba[((size_t)y1 * img.w + x1) * 4 + c];
                            out[((size_t)y * nw + x) * 4 + c] = (uint8_t)((s + 2) / 4);
                        }
                    }
                }
                img.rgba = std::move(out);
                img.w    = nw;
                img.h    = nh;
            }
        }

        // just enough JSON for the extensions cgltf hands over unparsed
        struct SJson {
            enum eType : uint8_t {
                NUL,
                BOOL,
                NUMBER,
                STRING,
                ARRAY,
                OBJECT
            } type = NUL;
            double                                     num = 0;
            std::string                                str;
            std::vector<SJson>                         items;
            std::vector<std::pair<std::string, SJson>> members;

            const SJson* get(std::string_view key) const {
                for (const auto& [k, v] : members)
                    if (k == key)
                        return &v;
                return nullptr;
            }
            double number(std::string_view key, double fallback) const {
                const SJson* v = get(key);
                return v && v->type == NUMBER ? v->num : fallback;
            }
            // reads up to n numbers of an array member
            void numbers(std::string_view key, float* out, size_t n) const {
                if (const SJson* v = get(key); v && v->type == ARRAY)
                    for (size_t i = 0; i < std::min(n, v->items.size()); ++i)
                        if (v->items[i].type == NUMBER)
                            out[i] = (float)v->items[i].num;
            }

            static std::optional<SJson> parse(std::string_view s) {
                size_t i = 0;
                auto   v = value(s, i, 0);
                return v;
            }

          private:
            static void ws(std::string_view s, size_t& i) {
                while (i < s.size() && std::isspace((unsigned char)s[i]))
                    ++i;
            }
            static std::optional<std::string> string(std::string_view s, size_t& i) {
                if (i >= s.size() || s[i] != '"')
                    return std::nullopt;
                std::string out;
                for (++i; i < s.size(); ++i) {
                    if (s[i] == '"') {
                        ++i;
                        return out;
                    }
                    if (s[i] == '\\' && i + 1 < s.size()) {
                        ++i;
                        if (s[i] == 'u') { // keys and names we care about are ASCII
                            i += 4;
                            out += '?';
                            continue;
                        }
                        out += s[i] == 'n' ? '\n' : s[i] == 't' ? '\t' : s[i];
                    } else
                        out += s[i];
                }
                return std::nullopt;
            }
            static std::optional<SJson> value(std::string_view s, size_t& i, int depth) {
                ws(s, i);
                if (i >= s.size() || depth > 32)
                    return std::nullopt;
                SJson v;
                if (s[i] == '{' || s[i] == '[') {
                    const bool obj = s[i] == '{';
                    v.type         = obj ? OBJECT : ARRAY;
                    ++i;
                    ws(s, i);
                    if (i < s.size() && s[i] == (obj ? '}' : ']')) {
                        ++i;
                        return v;
                    }
                    for (;;) {
                        std::string key;
                        if (obj) {
                            ws(s, i);
                            auto k = string(s, i);
                            ws(s, i);
                            if (!k || i >= s.size() || s[i] != ':')
                                return std::nullopt;
                            ++i;
                            key = std::move(*k);
                        }
                        auto item = value(s, i, depth + 1);
                        if (!item)
                            return std::nullopt;
                        if (obj)
                            v.members.emplace_back(std::move(key), std::move(*item));
                        else
                            v.items.push_back(std::move(*item));
                        ws(s, i);
                        if (i < s.size() && s[i] == ',') {
                            ++i;
                            continue;
                        }
                        if (i < s.size() && s[i] == (obj ? '}' : ']')) {
                            ++i;
                            return v;
                        }
                        return std::nullopt;
                    }
                }
                if (s[i] == '"') {
                    auto str = string(s, i);
                    if (!str)
                        return std::nullopt;
                    v.type = STRING;
                    v.str  = std::move(*str);
                    return v;
                }
                for (auto [word, type, num] : {std::tuple{"true", BOOL, 1.0}, {"false", BOOL, 0.0}, {"null", NUL, 0.0}})
                    if (s.substr(i, std::strlen(word)) == word) {
                        i += std::strlen(word);
                        v.type = type;
                        v.num  = num;
                        return v;
                    }
                const size_t start = i;
                while (i < s.size() && (std::isdigit((unsigned char)s[i]) || std::strchr("+-.eE", s[i])))
                    ++i;
                if (i == start)
                    return std::nullopt;
                v.type = NUMBER;
                v.num  = std::strtod(std::string(s.substr(start, i - start)).c_str(), nullptr);
                return v;
            }
        };

        struct SMaterialReader {
            cgltf_data* data;
            SMaterials& out;
            bool        surfaceMaps = true; // normal and metallic-roughness textures

            int         texture(const cgltf_texture_view& tv, bool srgb) {
                return texture(tv.texture, srgb);
            }

            int texture(const cgltf_texture* t, bool srgb) {
                if (!t || !t->image)
                    return -1; // basisu / webp only: can't decode those
                const size_t ii = cgltf_image_index(data, t->image);
                if (ii >= out.imageSlot.size())
                    return -1;
                if (out.imageSlot[ii] < 0) {
                    out.imageSlot[ii] = (int)out.images.size();
                    SMapImage img;
                    const auto* ci = t->image;
                    img.name       = ci->name ? ci->name : ci->uri && std::strncmp(ci->uri, "data:", 5) != 0 ? ci->uri : std::format("image {}", ii);
                    if (const auto* s = t->sampler) {
                        img.wrapS   = s->wrap_s ? (int)s->wrap_s : 0x2901;
                        img.wrapT   = s->wrap_t ? (int)s->wrap_t : 0x2901;
                        img.nearest = s->mag_filter == cgltf_filter_type_nearest;
                    }
                    out.images.push_back(std::move(img));
                }
                out.images[out.imageSlot[ii]].srgb |= srgb;
                return out.imageSlot[ii];
            }

            static int texcoord(const cgltf_texture_view& v) {
                return std::min(v.has_transform && v.transform.has_texcoord ? v.transform.texcoord : v.texcoord, 1);
            }

            // KHR_texture_transform: T * R * S, as mat2 columns and the offset
            static void transform(const cgltf_texture_view& v, float* xf) {
                if (!v.has_transform)
                    return;
                const auto& t = v.transform;
                const float c = std::cos(t.rotation), s = std::sin(t.rotation);
                xf[0]         = c * t.scale[0];
                xf[1]         = -s * t.scale[0];
                xf[2]         = s * t.scale[1];
                xf[3]         = c * t.scale[1];
                xf[4]         = t.offset[0];
                xf[5]         = t.offset[1];
            }

            // a texture of the file by its index in "textures", as {"index": n}
            int textureRef(const SJson* ref, bool srgb) {
                if (!ref || ref->type != SJson::OBJECT)
                    return -1;
                const double i = ref->number("index", -1);
                return i >= 0 && i < (double)data->textures_count ? texture(&data->textures[(size_t)i], srgb) : -1;
            }

            static std::optional<SJson> extension(const cgltf_material& cm, const char* name) {
                for (size_t e = 0; e < cm.extensions_count; ++e) {
                    const auto& ext = cm.extensions[e];
                    if (ext.name && ext.data && std::strcmp(ext.name, name) == 0) {
                        auto j = SJson::parse(ext.data);
                        if (j && j->type == SJson::OBJECT)
                            return j;
                    }
                }
                return std::nullopt;
            }

            // HYPR3D_materials_blend: {"texture": {"index": n}, "factor": [r, g, b, a], "uvScale": [s, t], "uvOffset": [s, t],
            // "maskTexture": {"index": n}, "softness": s, "maskChannel": 1|3, "normalTexture": {"index": n}}, see tools/cs2map.py
            void layer(const cgltf_material& cm, SMapMaterial& m) {
                const auto j = extension(cm, "HYPR3D_materials_blend");
                if (!j)
                    return;
                m.layerTex = textureRef(j->get("texture"), true);
                if (m.layerTex < 0)
                    return;
                m.layerMaskTex   = textureRef(j->get("maskTexture"), false);
                m.layerNormalTex = textureRef(j->get("normalTexture"), false);
                j->numbers("factor", m.layerColor, 4);
                float scale[2] = {1, 1}, offset[2] = {0, 0};
                j->numbers("uvScale", scale, 2);
                j->numbers("uvOffset", offset, 2);
                m.layerXf[0]       = scale[0];
                m.layerXf[3]       = scale[1];
                m.layerXf[4]       = offset[0];
                m.layerXf[5]       = offset[1];
                m.layerSoftness    = (float)j->number("softness", -1);
                m.layerMaskChannel = j->number("maskChannel", 1) == 3 ? 3 : 1;
            }

            // HYPR3D_materials_source2: what Source 2's shaders do that glTF doesn't say:
            // {"normalYDown": true, "specular": [direct, indirect], "blendMode": "mod2x", "selfIllumAlbedo": f,
            //  "detail": {"texture": {"index": n}, "maskTexture": {"index": n}, "mode": "mod2x"|"overlay", "blend": f,
            //             "blendToFull": f, "tint": [r, g, b], "transform": [mat2 columns, offset], "maskUV": 0|1}}
            void source2(const cgltf_material& cm, SMapMaterial& m) {
                const auto j = extension(cm, "HYPR3D_materials_source2");
                if (!j)
                    return;
                if (const SJson* v = j->get("normalYDown"); v && v->type == SJson::BOOL)
                    m.normalYDown = v->num != 0;
                if (const SJson* v = j->get("specular"); v && v->type == SJson::ARRAY)
                    for (size_t k = 0; k < std::min<size_t>(2, v->items.size()); ++k)
                        m.specular[k] = v->items[k].type == SJson::BOOL ? v->items[k].num != 0 : m.specular[k];
                if (const SJson* v = j->get("blendMode"); v && v->type == SJson::STRING && v->str == "mod2x")
                    m.blend = BLEND_MOD2X;
                m.selfIllumAlbedo = (float)j->number("selfIllumAlbedo", 0);
                if (const SJson* v = j->get("glass"); v && v->type == SJson::BOOL)
                    m.glass = v->num != 0;
                if (const SJson* v = j->get("vertexColor"); v && v->type == SJson::STRING)
                    m.vertexColor = v->str == "srgb" ? 1 : v->str == "none" ? 2 : v->str == "paint" ? 3 : 0;
                if (const SJson* e = j->get("effect"); e && e->type == SJson::OBJECT) {
                    m.effect = true;
                    if (const SJson* masks = e->get("masks"); masks && masks->type == SJson::ARRAY)
                        for (size_t k = 0; k < std::min<size_t>(3, masks->items.size()); ++k) {
                            const SJson& mk     = masks->items[k];
                            m.effectMaskTex[k] = textureRef(mk.get("texture"), false);
                            mk.numbers("scale", &m.effectMask[k][0], 2);
                            mk.numbers("pan", &m.effectMask[k][2], 2);
                        }
                    m.effectBoost   = (float)e->number("colorBoost", 1);
                    m.effectOpacity = (float)e->number("opacity", 1);
                    e->numbers("fade", m.effectFade, 4);
                    e->numbers("fresnel", m.effectFresnel, 4);
                    if (const SJson* v = e->get("additive"); v && v->type == SJson::BOOL && v->num != 0)
                        m.blend = BLEND_ADD;
                    if (const SJson* v = e->get("fog"); v && v->type == SJson::BOOL)
                        m.effectFog = v->num != 0;
                }
                const SJson* d = j->get("detail");
                if (!d || d->type != SJson::OBJECT)
                    return;
                m.detailTex = textureRef(d->get("texture"), false); // data: mod2x's neutral is 0.5, not its sRGB decoding
                if (m.detailTex < 0)
                    return;
                const SJson* mode = d->get("mode");
                m.detail          = mode && mode->type == SJson::STRING && mode->str == "overlay" ? DETAIL_OVERLAY : DETAIL_MOD2X;
                m.detailMaskTex   = textureRef(d->get("maskTexture"), false);
                m.detailBlend     = (float)d->number("blend", 1);
                m.detailBlendToFull = (float)d->number("blendToFull", 0);
                m.detailMaskUV    = d->number("maskUV", 0) >= 1 ? 1 : 0;
                d->numbers("tint", m.detailTint, 3);
                d->numbers("transform", m.detailXf, 6);
            }

            void read() {
                out.materials.resize(data->materials_count + 1);
                out.baseUV.assign(data->materials_count + 1, 0);
                out.defaultMaterial                           = (int)data->materials_count;
                out.materials[out.defaultMaterial].name       = "default";
                out.imageSlot.assign(data->images_count, -1);

                for (size_t i = 0; i < data->materials_count; ++i) {
                    const cgltf_material& cm = data->materials[i];
                    SMapMaterial&         m  = out.materials[i];
                    m.name                   = cm.name ? cm.name : "";

                    const cgltf_texture_view* base   = &cm.pbr_metallic_roughness.base_color_texture;
                    const float*              factor = cm.pbr_metallic_roughness.base_color_factor;
                    if (cm.has_pbr_specular_glossiness && !cm.has_pbr_metallic_roughness) {
                        base   = &cm.pbr_specular_glossiness.diffuse_texture;
                        factor = cm.pbr_specular_glossiness.diffuse_factor;
                    }
                    std::copy_n(factor, 4, m.baseColor);
                    m.baseTex     = texture(*base, true);
                    out.baseUV[i] = texcoord(*base);
                    transform(*base, m.baseXf);

                    std::copy_n(cm.emissive_factor, 3, m.emissive);
                    if (cm.has_emissive_strength)
                        for (float& e : m.emissive)
                            e *= cm.emissive_strength.emissive_strength;
                    // vertices carry two uv sets: the base color's first
                    m.emissiveTex = texture(cm.emissive_texture, true);
                    m.emissiveUV  = texcoord(cm.emissive_texture) == out.baseUV[i] ? 0 : 1;
                    transform(cm.emissive_texture, m.emissiveXf);

                    m.occlusionTex      = texture(cm.occlusion_texture, false);
                    m.occlusionUV       = cm.occlusion_texture.texcoord == out.baseUV[i] ? 0 : 1;
                    m.occlusionStrength = cm.occlusion_texture.texture ? cm.occlusion_texture.scale : 0.f;

                    m.normalTex   = surfaceMaps ? texture(cm.normal_texture, false) : -1;
                    if (m.normalTex >= 0)
                        out.images[m.normalTex].normal = true;
                    m.normalScale = cm.normal_texture.texture ? cm.normal_texture.scale : 1.f;
                    // roughness and metalness: read from the occlusion texture's g and b when they share
                    // an image (ORM), or from their own when there is no occlusion texture
                    const auto& pbr = cm.pbr_metallic_roughness;
                    m.roughness     = cm.has_pbr_metallic_roughness ? pbr.roughness_factor : 1.f;
                    m.metalness     = cm.has_pbr_metallic_roughness ? pbr.metallic_factor : 0.f;
                    if (surfaceMaps && cm.has_pbr_metallic_roughness && pbr.metallic_roughness_texture.texture) {
                        const int mr = texture(pbr.metallic_roughness_texture, false);
                        if (mr >= 0 && (m.occlusionTex < 0 || m.occlusionTex == mr)) {
                            if (m.occlusionTex < 0) {
                                m.occlusionTex      = mr;
                                m.occlusionUV       = pbr.metallic_roughness_texture.texcoord == out.baseUV[i] ? 0 : 1;
                                m.occlusionStrength = 0.f;
                            }
                            m.ormFromOcclusion = true;
                        }
                    }

                    m.alphaMode   = cm.alpha_mode == cgltf_alpha_mode_mask ? ALPHA_MASK : cm.alpha_mode == cgltf_alpha_mode_blend ? ALPHA_BLEND : ALPHA_OPAQUE;
                    m.alphaCutoff = cm.alpha_cutoff;
                    m.unlit       = cm.unlit;
                    layer(cm, m);
                    source2(cm, m);
                }
            }
        };
    }

    std::string lower(std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    bool readFile(const std::string& path, std::vector<uint8_t>& out) {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return false;
        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return true;
    }

    DataPtr open(const std::string& path, const std::string& what, std::string& error) {
        cgltf_options opt{};
        cgltf_data*   data = nullptr;
        if (const auto r = cgltf_parse_file(&opt, path.c_str(), &data); r != cgltf_result_success) {
            error = std::format("{} isn't a glTF/GLB file cgltf can read (error {})", path, (int)r);
            return {nullptr, cgltf_free};
        }
        DataPtr guard(data, cgltf_free);
        if (cgltf_load_buffers(&opt, data, path.c_str()) != cgltf_result_success) {
            error = std::format("couldn't load the buffers of the {} (missing .bin next to it?)", what);
            return {nullptr, cgltf_free};
        }
        for (size_t i = 0; i < data->extensions_required_count; ++i) {
            const std::string ext = data->extensions_required[i];
            if (ext == "KHR_draco_mesh_compression" || ext == "EXT_meshopt_compression" || ext == "KHR_texture_basisu") {
                error = std::format("the {} needs {}, which isn't supported: re-export it uncompressed (e.g. `gltf-transform copy in.glb out.glb`)", what, ext);
                return {nullptr, cgltf_free};
            }
        }
        return guard;
    }

    SMaterials readMaterials(cgltf_data* data, bool surfaceMaps) {
        SMaterials out;
        SMaterialReader{data, out, surfaceMaps}.read();
        return out;
    }

    namespace {
        // an image's encoded bytes: in the file, a data: uri, or a file next to it
        const uint8_t* imageBytes(const cgltf_image* ci, const std::string& dir, std::vector<uint8_t>& file, size_t& size, std::string& error) {
            size = 0;
            if (ci->buffer_view) {
                size = ci->buffer_view->size;
                return cgltf_buffer_view_data(ci->buffer_view);
            }
            if (ci->uri && std::strncmp(ci->uri, "data:", 5) == 0) {
                const char* comma = std::strchr(ci->uri, ',');
                if (comma && base64Decode(comma + 1, file)) {
                    size = file.size();
                    return file.data();
                }
                return nullptr;
            }
            if (ci->uri) {
                std::string uri = ci->uri;
                uri.resize(cgltf_decode_uri(uri.data()));
                if (readFile((std::filesystem::path(dir) / uri).string(), file)) {
                    size = file.size();
                    return file.data();
                }
                error = std::format("texture {} not found", uri);
            }
            return nullptr;
        }

        // ---- block compression

        const float* srgbToLinearTable() {
            static const auto table = [] {
                std::array<float, 256> t{};
                for (int i = 0; i < 256; ++i) {
                    const float v = i / 255.f;
                    t[i]          = v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
                }
                return t;
            }();
            return table.data();
        }

        uint8_t linearToSrgbByte(float v) {
            static const auto table = [] {
                std::array<uint8_t, 4096> t{};
                for (int i = 0; i < 4096; ++i) {
                    const float l = i / 4095.f;
                    const float s = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.f / 2.4f) - 0.055f;
                    t[i]          = (uint8_t)std::lround(std::clamp(s, 0.f, 1.f) * 255.f);
                }
                return t;
            }();
            return table[(size_t)std::lround(std::clamp(v, 0.f, 1.f) * 4095.f)];
        }

        // the next mip level down, 2x2 box filtered (colors in linear light)
        std::vector<uint8_t> halve(const uint8_t* src, int w, int h, bool srgb, int& nw, int& nh) {
            nw = std::max(1, w / 2);
            nh = std::max(1, h / 2);
            std::vector<uint8_t> out((size_t)nw * nh * 4);
            const float*         lin = srgbToLinearTable();
            for (int y = 0; y < nh; ++y)
                for (int x = 0; x < nw; ++x) {
                    const int     x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1), y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
                    const uint8_t* p[4] = {src + ((size_t)y0 * w + x0) * 4, src + ((size_t)y0 * w + x1) * 4, src + ((size_t)y1 * w + x0) * 4, src + ((size_t)y1 * w + x1) * 4};
                    uint8_t*       o    = &out[((size_t)y * nw + x) * 4];
                    for (int c = 0; c < 4; ++c) {
                        if (srgb && c < 3)
                            o[c] = linearToSrgbByte(0.25f * (lin[p[0][c]] + lin[p[1][c]] + lin[p[2][c]] + lin[p[3][c]]));
                        else
                            o[c] = (uint8_t)((p[0][c] + p[1][c] + p[2][c] + p[3][c] + 2) / 4);
                    }
                }
            return out;
        }

        // one mip level into 4x4 blocks: BC1 (8 bytes a block), BC3 or BC5 (16)
        void encodeLevel(const uint8_t* px, int w, int h, eTexFormat format, std::vector<uint8_t>& out) {
            uint8_t block[64], rg[32];
            for (int by = 0; by < h; by += 4)
                for (int bx = 0; bx < w; bx += 4) {
                    for (int y = 0; y < 4; ++y)
                        for (int x = 0; x < 4; ++x) {
                            const uint8_t* s = px + ((size_t)std::min(by + y, h - 1) * w + std::min(bx + x, w - 1)) * 4;
                            std::memcpy(block + (y * 4 + x) * 4, s, 4);
                            rg[(y * 4 + x) * 2]     = s[0];
                            rg[(y * 4 + x) * 2 + 1] = s[1];
                        }
                    const size_t at = out.size();
                    out.resize(at + (format == TEX_BC1 ? 8 : 16));
                    if (format == TEX_BC5)
                        stb_compress_bc5_block(&out[at], rg);
                    else
                        stb_compress_dxt_block(&out[at], block, format == TEX_BC3 ? 1 : 0, STB_DXT_NORMAL);
                }
        }

        // replaces the pixels with block compressed ones, every mip level, when the GPU takes them
        void compressImage(SMapImage& img, int compress) {
            if (img.plain || img.w < 8 || img.h < 8 || img.w % 4 || img.h % 4)
                return;
            const bool  alpha = [&] {
                for (size_t i = 3; i < img.rgba.size(); i += 4)
                    if (img.rgba[i] != 255)
                        return true;
                return false;
            }();
            eTexFormat format = alpha ? TEX_BC3 : TEX_BC1;
            if (img.normal && !alpha && (compress & COMPRESS_RGTC))
                format = TEX_BC5;
            if (!(compress & COMPRESS_S3TC) || (img.srgb && !(compress & COMPRESS_S3TC_SRGB)))
                return;
            std::vector<uint8_t> out, level = std::move(img.rgba);
            int                  w = img.w, h = img.h, levels = 0;
            for (;;) {
                encodeLevel(level.data(), w, h, format, out);
                ++levels;
                if (w == 1 && h == 1)
                    break;
                int nw = 0, nh = 0;
                level = halve(level.data(), w, h, img.srgb, nw, nh);
                w     = nw;
                h     = nh;
            }
            img.rgba   = std::move(out);
            img.format = format;
            img.levels = levels;
        }

        // a texture that is one color all over (Source 2 Viewer writes a lot of those) needs one texel
        void flatten(SMapImage& img) {
            const size_t n = (size_t)img.w * img.h;
            if (n <= 1)
                return;
            const uint32_t* px = reinterpret_cast<const uint32_t*>(img.rgba.data());
            for (size_t i = 1; i < n; ++i)
                if (px[i] != px[0])
                    return;
            img.rgba.resize(4);
            img.rgba.shrink_to_fit();
            img.w = img.h = 1;
        }
    }

    void decodeImages(cgltf_data* data, const std::string& dir, std::vector<SMapImage>& images, const std::vector<int>& slots, const std::atomic<bool>& cancel,
                      std::vector<std::string>& log, int compress) {
        std::vector<const cgltf_image*> source(images.size(), nullptr);
        for (size_t i = 0; i < slots.size(); ++i)
            if (slots[i] >= 0)
                source[slots[i]] = &data->images[i];

        std::vector<std::string> errors(images.size());
        parallelFor(images.size(), 1, cancel, [&](size_t b, size_t e) {
            for (size_t i = b; i < e; ++i) {
                std::vector<uint8_t> file;
                size_t               size  = 0;
                const uint8_t*       bytes = imageBytes(source[i], dir, file, size, errors[i]);
                if (!errors[i].empty())
                    continue;
                if (!bytes || size == 0) {
                    errors[i] = std::format("texture {} has no data", images[i].name);
                    continue;
                }
                int      w = 0, h = 0, comps = 0;
                uint8_t* px = stbi_load_from_memory(bytes, (int)size, &w, &h, &comps, 4);
                if (!px) {
                    errors[i] = std::format("texture {}: {}", images[i].name, stbi_failure_reason());
                    continue;
                }
                auto& img = images[i];
                img.w     = w;
                img.h     = h;
                img.rgba.assign(px, px + (size_t)w * h * 4);
                stbi_image_free(px);
                flatten(img);
                shrink(img);
                if (compress)
                    compressImage(img, compress);
            }
        });
        size_t bad = 0;
        for (const auto& e : errors) {
            if (e.empty())
                continue;
            if (++bad <= 5)
                log.push_back(e);
        }
        if (bad > 5)
            log.push_back(std::format("... and {} more textures that didn't load", bad - 5));
    }
    // ------------------------------------------------------------ HYPR3D_lighting

    namespace {
        // RGB9E5: three 9 bit mantissas sharing a 5 bit exponent (EXT_texture_shared_exponent)
        uint32_t packRGB9E5(float r, float g, float b) {
            constexpr float MAXV = 65408.f; // (511 / 512) * 2^16
            r = std::fmin(std::fmax(r, 0.f), MAXV);
            g = std::fmin(std::fmax(g, 0.f), MAXV);
            b = std::fmin(std::fmax(b, 0.f), MAXV);
            const float m = std::fmax(r, std::fmax(g, b));
            if (m < 1e-9f)
                return 0;
            int e = 0;
            std::frexp(m, &e); // m = f * 2^e, f in [0.5, 1): floor(log2 m) = e - 1
            int   shared = std::max(-16, e - 1) + 16;
            float scale  = std::ldexp(1.f, shared - 24);
            if (std::floor(m / scale + 0.5f) >= 512.f) {
                ++shared;
                scale *= 2.f;
            }
            const auto q = [&](float v) { return std::min(511u, (uint32_t)std::floor(v / scale + 0.5f)); };
            return q(r) | q(g) << 9 | q(b) << 18 | (uint32_t)shared << 27;
        }

        uint16_t toHalf(float f) {
            uint32_t x;
            std::memcpy(&x, &f, 4);
            const uint32_t sign = (x >> 16) & 0x8000u;
            const int      e    = (int)((x >> 23) & 0xffu) - 127 + 15;
            uint32_t       mant = x & 0x7fffffu;
            if (e <= 0) { // too small: subnormal or 0
                if (e < -10)
                    return (uint16_t)sign;
                mant |= 0x800000u;
                const int shift = 14 - e;
                return (uint16_t)(sign | ((mant + (1u << (shift - 1))) >> shift));
            }
            if (e >= 31)
                return (uint16_t)(sign | 0x7c00u);
            return (uint16_t)((sign | (uint32_t)e << 10 | mant >> 13) + ((mant >> 12) & 1u));
        }

        // the RGBE PNGs cs2map writes: mantissas in rgb, the exponent + 128 in alpha
        inline float rgbe(uint8_t m, uint8_t e) {
            return e ? std::ldexp(m + 0.5f, (int)e - 136) : 0.f;
        }

        // an RGBE image -> RGB9E5 with `maxLevels` mip levels (box filtered, in linear light)
        SHdrImage hdrImage(const std::vector<uint8_t>& px, int w, int h, int maxLevels) {
            SHdrImage out;
            out.w      = w;
            out.h      = h;
            out.levels = std::min(maxLevels, 1 + (int)std::floor(std::log2((float)std::max(w, h))));
            std::vector<float> level((size_t)w * h * 3), next;
            for (size_t i = 0; i < (size_t)w * h; ++i)
                for (int c = 0; c < 3; ++c)
                    level[i * 3 + c] = rgbe(px[i * 4 + c], px[i * 4 + 3]);
            int lw = w, lh = h;
            for (int l = 0; l < out.levels; ++l) {
                for (size_t i = 0; i < (size_t)lw * lh; ++i)
                    out.texels.push_back(packRGB9E5(level[i * 3], level[i * 3 + 1], level[i * 3 + 2]));
                if (l + 1 == out.levels)
                    break;
                const int nw = std::max(1, lw / 2), nh = std::max(1, lh / 2);
                next.assign((size_t)nw * nh * 3, 0.f);
                for (int y = 0; y < nh; ++y)
                    for (int x = 0; x < nw; ++x)
                        for (int c = 0; c < 3; ++c) {
                            const int x0 = std::min(x * 2, lw - 1), x1 = std::min(x * 2 + 1, lw - 1);
                            const int y0 = std::min(y * 2, lh - 1), y1 = std::min(y * 2 + 1, lh - 1);
                            next[((size_t)y * nw + x) * 3 + c] = 0.25f * (level[((size_t)y0 * lw + x0) * 3 + c] + level[((size_t)y0 * lw + x1) * 3 + c] +
                                                                          level[((size_t)y1 * lw + x0) * 3 + c] + level[((size_t)y1 * lw + x1) * 3 + c]);
                        }
                level.swap(next);
                lw = nw;
                lh = nh;
            }
            return out;
        }
    }

    bool readLighting(cgltf_data* data, const std::string& dir, SMapLighting& out, const std::atomic<bool>& cancel, std::vector<std::string>& log) {
        std::optional<SJson> j;
        for (size_t i = 0; i < data->data_extensions_count; ++i) {
            const auto& e = data->data_extensions[i];
            if (e.name && e.data && std::strcmp(e.name, "HYPR3D_lighting") == 0)
                j = SJson::parse(e.data);
        }
        if (!j || j->type != SJson::OBJECT)
            return false;

        std::mutex errorLock;
        // one of the file's images, decoded to `comps` channels
        const auto decode = [&](const SJson* ref, int comps, int& w, int& h, std::vector<uint8_t>& px) {
            const double i = ref && ref->type == SJson::OBJECT ? ref->number("image", -1) : -1;
            if (i < 0 || i >= (double)data->images_count)
                return false;
            std::vector<uint8_t> file;
            size_t               size = 0;
            std::string          error;
            const uint8_t*       bytes = imageBytes(&data->images[(size_t)i], dir, file, size, error);
            int                  c     = 0;
            uint8_t*             p     = bytes ? stbi_load_from_memory(bytes, (int)size, &w, &h, &c, comps) : nullptr;
            if (!p) {
                std::lock_guard lock(errorLock);
                log.push_back(std::format("lighting image {}: {}", i, bytes ? stbi_failure_reason() : error.empty() ? "no data" : error));
                return false;
            }
            px.assign(p, p + (size_t)w * h * comps);
            stbi_image_free(p);
            return true;
        };

        // what to decode, done on a few threads: the big lightmaps take a second each
        std::vector<std::function<void()>> jobs;
        const SJson*                       sets = j->get("sets");
        const size_t                       n    = sets && sets->type == SJson::ARRAY ? sets->items.size() : 0;
        out.sets.resize(n);
        for (size_t k = 0; k < n; ++k) {
            const SJson& sj  = sets->items[k];
            SMapLightSet& ls = out.sets[k];
            if (const SJson* lm = sj.get("lightmaps"); lm && lm->type == SJson::OBJECT) {
                jobs.push_back([&, lm] {
                    int                  w = 0, h = 0;
                    std::vector<uint8_t> px;
                    if (decode(lm->get("irradiance"), 4, w, h, px))
                        ls.irradiance = hdrImage(px, w, h, 4);
                });
                jobs.push_back([&, lm] {
                    auto& img = ls.directional;
                    if (!decode(lm->get("directional"), 4, img.w, img.h, img.rgba))
                        img = {};
                });
                jobs.push_back([&, lm] {
                    auto& img = ls.shadows; // one byte a texel
                    if (!decode(lm->get("shadows"), 1, img.w, img.h, img.rgba))
                        img = {};
                });
            }
            const SJson* pj = sj.get("probes");
            if (!pj || pj->type != SJson::OBJECT)
                continue;
            float dims[3] = {0, 0, 0};
            pj->numbers("size", dims, 3);
            const int cols = (int)pj->number("columns", 16);
            for (int c = 0; c < 3; ++c)
                ls.probeDims[c] = (int)dims[c];
            if (const SJson* vols = pj->get("volumes"); vols && vols->type == SJson::ARRAY)
                for (const auto& v : vols->items) {
                    SMapLightSet::SVolume vol;
                    float                 m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, a[3] = {0, 0, 0}, b[3] = {1, 1, 1}, lo[3] = {0, 0, 0},
                          hi[3] = {0, 0, 0};
                    v.numbers("matrix", m, 16);
                    v.numbers("atlasOffset", a, 3);
                    v.numbers("atlasSize", b, 3);
                    v.numbers("min", lo, 3);
                    v.numbers("max", hi, 3);
                    std::memcpy(vol.toBox.m, m, sizeof(m));
                    vol.bounds = {{lo[0], lo[1], lo[2]}, {hi[0], hi[1], hi[2]}};
                    for (int c = 0; c < 3; ++c) {
                        vol.atlasOffset[c] = (int)a[c];
                        vol.atlasSize[c]   = std::max(1, (int)b[c]);
                    }
                    vol.priority = (int)v.number("priority", 0);
                    ls.volumes.push_back(vol);
                }
            // the atlas: its slices in a grid, `cols` wide; six blocks of probeDims[2] slices each
            jobs.push_back([&, pj, cols] {
                int                  w = 0, h = 0, sw = 0, sh = 0;
                std::vector<uint8_t> irr, shd;
                const int            W = ls.probeDims[0], H = ls.probeDims[1], D = ls.probeDims[2];
                if (W < 1 || H < 1 || D < 1 || !decode(pj->get("irradiance"), 4, w, h, irr))
                    return;
                if (!decode(pj->get("shadows"), 1, sw, sh, shd))
                    shd.clear();
                const auto at = [&](int slice, int x, int y, int iw) { return ((size_t)(slice / cols * H + y) * iw + (size_t)(slice % cols * W + x)); };
                if (w < cols * W || h < (6 * D + cols - 1) / cols * H)
                    return;
                ls.probes.assign((size_t)W * H * D * 6 * 4, 0);
                ls.probeLuma.assign((size_t)W * H * D * 6, 0);
                double sum[3] = {0, 0, 0};
                size_t lit    = 0;
                for (int z = 0; z < 6 * D; ++z)
                    for (int y = 0; y < H; ++y)
                        for (int x = 0; x < W; ++x) {
                            const size_t s   = at(z, x, y, w) * 4;
                            uint16_t*    o   = &ls.probes[(((size_t)z * H + y) * W + x) * 4];
                            float        rgb[3];
                            for (int c = 0; c < 3; ++c) {
                                rgb[c] = rgbe(irr[s + c], irr[s + 3]);
                                o[c]   = toHalf(rgb[c]);
                            }
                            ls.probeLuma[(((size_t)(z % D) * H + y) * W + x) * 6 + z / D] = probeLumaByte(rgb[0] * 0.2125f + rgb[1] * 0.7154f + rgb[2] * 0.0721f);
                            if (rgb[0] + rgb[1] + rgb[2] > 0.f) {
                                for (int c = 0; c < 3; ++c)
                                    sum[c] += rgb[c];
                                ++lit;
                            }
                            // the sun's shadow goes with the first block
                            const bool hasShadow = z < D && !shd.empty() && sw >= cols * W;
                            o[3]                 = toHalf(hasShadow ? shd[at(z, x, y, sw)] / 255.f : 0.f);
                        }
                for (int c = 0; c < 3; ++c)
                    ls.average[c] = lit ? (float)(sum[c] / (double)lit) : 0.f;
            });
        }
        parallelFor(jobs.size(), 1, cancel, [&](size_t b, size_t e) {
            for (size_t i = b; i < e; ++i)
                jobs[i]();
        });

        if (const SJson* sun = j->get("sun"); sun && sun->type == SJson::OBJECT) {
            float c[3] = {out.sunColor.x, out.sunColor.y, out.sunColor.z}, d[3] = {0, 0, 0};
            sun->numbers("color", c, 3);
            sun->numbers("direction", d, 3);
            out.sunColor = {c[0], c[1], c[2]};
            out.sunDir   = V3{d[0], d[1], d[2]};
        }
        if (const SJson* f = j->get("fog"); f && f->type == SJson::OBJECT) {
            out.fog               = true;
            out.fogStart          = (float)f->number("start", 0);
            out.fogEnd            = (float)f->number("end", 100);
            out.fogExponent       = (float)f->number("exponent", 1);
            out.fogMaxOpacity     = (float)f->number("maxOpacity", 1);
            out.fogLodBias        = (float)f->number("lodBias", 0);
            out.fogHeightStart    = (float)f->number("heightStart", 0);
            out.fogHeightEnd      = (float)f->number("heightEnd", 0);
            out.fogHeightExponent = (float)f->number("heightExponent", 1);
        }
        if (const SJson* sky = j->get("sky"); sky && sky->type == SJson::OBJECT) {
            out.skyImage = (int)sky->number("image", -1);
            float c[3]   = {1, 1, 1};
            sky->numbers("color", c, 3);
            out.skyColor = {c[0], c[1], c[2]};
        }
        if (const SJson* e = j->get("exposure"); e && e->type == SJson::OBJECT) {
            out.exposureAuto      = true;
            out.exposureMin       = (float)e->number("min", 1);
            out.exposureMax       = (float)e->number("max", 1);
            out.exposureSpeedUp   = (float)e->number("speedUp", 1);
            out.exposureSpeedDown = (float)e->number("speedDown", 1);
        }
        if (const SJson* t = j->get("tonemap"); t && t->type == SJson::OBJECT) {
            static constexpr const char* KEYS[8] = {"shoulderStrength", "linearStrength", "linearAngle", "toeStrength", "toeNum", "toeDenom", "whitePoint", "exposureBias"};
            for (int k = 0; k < 8; ++k)
                out.curve[k] = (float)t->number(KEYS[k], out.curve[k]);
        }
        out.present = !out.sets.empty() && out.sets[0].hasLightmaps();
        return out.present;
    }
}
