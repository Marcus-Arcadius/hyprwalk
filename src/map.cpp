#include "map.hpp"
#include "gltf.hpp"

#include "third_party/cgltf.h"
#include "third_party/stb_image.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include <tuple>
#include <unordered_map>

// Loading user supplied maps: glTF 2.0 (.gltf with its files, or .glb).
// Everything is flattened into world space once, then split into one draw
// call per material; the same triangles feed the collision BVH, which is also
// used to bake some ambient occlusion and to find a spawn point and a wall
// for the desktop when the map doesn't say where they go.

namespace h3d {

    namespace {
        using gltf::check;
        using gltf::lower;

        constexpr float PLAYER_RADIUS = 0.3f;
        constexpr float PLAYER_HEIGHT = 1.8f;
        constexpr float PLAYER_EYE    = 1.65f;
        constexpr float HALF_FOV_Y    = 35.f * 3.14159265f / 180.f;

        bool contains(const std::string& s, std::initializer_list<const char*> needles) {
            return std::ranges::any_of(needles, [&](const char* n) { return s.find(n) != std::string::npos; });
        }

        // "textures/Sky.001" -> "sky"
        std::string baseName(const std::string& s) {
            std::string b = lower(s.substr(s.find_last_of("/\\") == std::string::npos ? 0 : s.find_last_of("/\\") + 1));
            if (const auto dot = b.find('.'); dot != std::string::npos)
                b.resize(dot);
            return b;
        }

        // what to do with a surface, going by its names: maps converted from
        // games keep their tool textures (clip brushes, triggers, the sky shell)
        struct SRules {
            bool render = true, collide = true, shadow = true, sky = false;
        };

        SRules rulesFor(const std::string& material, const std::string& object) {
            const std::string m = lower(material), o = lower(object), b = baseName(material);
            // Source
            if (contains(m, {"toolstrigger", "toolsskip", "toolshint", "toolsareaportal", "toolsoccluder", "toolsfog", "toolsblock_los", "toolsblockbullets", "toolsorigin"}))
                return {false, false, false, false};
            if (contains(m, {"toolsblocklight", "toolssolidblocklight"}))
                return {false, false, true, false};
            if (contains(m, {"toolsskybox"}))
                return {false, true, false, false};
            if (contains(m, {"toolsclip", "toolsplayerclip", "toolsnpcclip", "toolsinvisible"}))
                return {false, true, false, false};
            if (contains(m, {"toolsnodraw"}))
                return {false, true, true, false};
            // GoldSrc / Quake
            if (b == "aaatrigger" || b == "trigger" || b == "origin" || b == "hint" || b == "skip")
                return {false, false, false, false};
            if (b == "sky" || b == "clip")
                return {false, true, false, false};
            if (b == "null")
                return {false, true, true, false};
            // a sky mesh around the map
            if (contains(m, {"skybox", "skydome", "skysphere", "sky_box", "sky_dome", "sky_sphere"}) ||
                contains(o, {"skybox", "skydome", "skysphere", "sky_box", "sky_dome", "sky_sphere"}))
                return {true, false, false, true};
            return {};
        }

        std::vector<V3> hemisphere(int n) {
            std::vector<V3> out;
            for (int i = 0; i < n; ++i) {
                const float u = (i + 0.5f) / n, r = std::sqrt(u), phi = i * 2.39996323f;
                out.push_back({r * std::cos(phi), std::sqrt(std::max(0.f, 1.f - u)), r * std::sin(phi)});
            }
            return out;
        }

        V3 horizontal(const V3& v) {
            return normalize(V3{v.x, 0, v.z});
        }

        float yawFacing(const V3& dir) {
            return std::atan2(dir.x, -dir.z); // see forwardFrom() in main.cpp
        }

        // ---------------------------------------------------------- building

        struct SBatchKey {
            int  material = 0;
            bool render = true, shadow = true, sky = false, backdrop = false;
            auto operator<=>(const SBatchKey&) const = default;
        };

        struct SBuild {
            const SMapRequest&                        req;
            const std::atomic<bool>&                  cancel;
            cgltf_data*                               data = nullptr;
            SMapModel&                                model;
            std::vector<std::string>&                 log;
            std::map<SBatchKey, std::vector<uint32_t>> batches;
            std::vector<V3>                           colTris, occTris; // collision, and what blocks light
            std::vector<int>                          imageSlot;        // glTF image -> model image
            std::vector<int>                          baseUV;           // per material
            int                                       defaultMaterial = 0;
            size_t                                    skippedDraco = 0, skippedOther = 0, backdropTris = 0;

            // anchors found in the scene
            std::optional<V3>                         spawnNode, desktopNode;
            V3                                        spawnForward{0, 0, -1}, desktopNormal{0, 0, 1};
            bool                                      spawnNamed = false; // hypr3d_spawn rather than a game's player start
            std::optional<V3>                         sunNode;            // a directional light (KHR_lights_punctual), towards it

            void materials() {
                auto m          = gltf::readMaterials(data);
                model.images    = std::move(m.images);
                model.materials = std::move(m.materials);
                imageSlot       = std::move(m.imageSlot);
                baseUV          = std::move(m.baseUV);
                defaultMaterial = m.defaultMaterial;
            }

            // the game's lighting for the map, or for its backdrop (its own when it has one)
            const SMapLightSet* lightSet(bool backdrop) const {
                const auto& sets = model.lighting.sets;
                if (!model.lighting.present || sets.empty())
                    return nullptr;
                return backdrop ? (sets.size() > 1 ? &sets[1] : nullptr) : &sets[0];
            }

            void primitive(const cgltf_primitive& prim, const M4& xf, const std::string& object, bool backdrop) {
                if (prim.type != cgltf_primitive_type_triangles && prim.type != cgltf_primitive_type_triangle_strip && prim.type != cgltf_primitive_type_triangle_fan)
                    return;
                if (prim.has_draco_mesh_compression) {
                    ++skippedDraco;
                    return;
                }

                const cgltf_accessor *aPos = nullptr, *aNrm = nullptr, *aTan = nullptr, *aUV[2] = {nullptr, nullptr}, *aCol = nullptr, *aBlend = nullptr,
                                     *aLight = nullptr;
                for (size_t i = 0; i < prim.attributes_count; ++i) {
                    const auto& at = prim.attributes[i];
                    switch (at.type) {
                        case cgltf_attribute_type_position: aPos = at.data; break;
                        case cgltf_attribute_type_normal: aNrm = at.data; break;
                        case cgltf_attribute_type_tangent: aTan = at.data; break;
                        case cgltf_attribute_type_texcoord:
                            if (at.index < 2)
                                aUV[at.index] = at.data;
                            break;
                        case cgltf_attribute_type_color:
                            if (at.index == 0)
                                aCol = at.data;
                            break;
                        case cgltf_attribute_type_custom:
                            // how much of the material's second layer shows (HYPR3D_materials_blend)
                            if (at.name && !std::strcmp(at.name, "_BLEND"))
                                aBlend = at.data;
                            // where in the lightmap (HYPR3D_lighting)
                            if (at.name && !std::strcmp(at.name, "_LIGHTMAP_UV"))
                                aLight = at.data;
                            break;
                        default: break;
                    }
                }
                if (!aPos || aPos->count == 0 || (aPos->is_sparse && !aPos->buffer_view && !aPos->sparse.values_buffer_view)) {
                    ++skippedOther;
                    return;
                }

                const int     mat = prim.material ? (int)cgltf_material_index(data, prim.material) : defaultMaterial;
                const auto&   mm  = model.materials[mat];
                SRules        rules = rulesFor(mm.name, object);
                if (backdrop) // scenery far out: only seen, never reached, and its shadows wouldn't reach the map
                    rules = {rules.render, false, false, false};
                if (mm.effect) // dust and clouds: only seen
                    rules = {rules.render, false, false, rules.sky};
                if (!rules.render && !rules.collide && !rules.shadow)
                    return;

                const size_t n = aPos->count;

                // triangle list
                std::vector<uint32_t> idx;
                if (prim.indices) {
                    idx.resize(prim.indices->count);
                    for (size_t i = 0; i < idx.size(); ++i)
                        idx[i] = (uint32_t)cgltf_accessor_read_index(prim.indices, i);
                } else {
                    idx.resize(n);
                    std::iota(idx.begin(), idx.end(), 0u);
                }
                if (prim.type != cgltf_primitive_type_triangles) {
                    std::vector<uint32_t> list;
                    for (size_t i = 2; i < idx.size(); ++i) {
                        if (prim.type == cgltf_primitive_type_triangle_fan)
                            list.insert(list.end(), {idx[0], idx[i - 1], idx[i]});
                        else if (i % 2 == 0)
                            list.insert(list.end(), {idx[i - 2], idx[i - 1], idx[i]});
                        else
                            list.insert(list.end(), {idx[i - 1], idx[i - 2], idx[i]});
                    }
                    idx = std::move(list);
                }

                // only the vertices its triangles use: exporters often give every primitive of a
                // mesh the mesh's whole vertex buffer, which would be copied once per primitive
                std::vector<uint32_t> tris, used, remap(n, UINT32_MAX); // tris index into used
                tris.reserve(idx.size());
                for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                    if (idx[t] >= n || idx[t + 1] >= n || idx[t + 2] >= n)
                        continue;
                    for (int k = 0; k < 3; ++k) {
                        uint32_t& r = remap[idx[t + k]];
                        if (r == UINT32_MAX) {
                            r = (uint32_t)used.size();
                            used.push_back(idx[t + k]);
                        }
                        tris.push_back(r);
                    }
                }
                const size_t m    = used.size();
                const auto   read = [&](const cgltf_accessor* a, size_t comps, std::vector<float>& out) {
                    out.assign(m * comps, 0.f);
                    for (size_t i = 0; i < m; ++i)
                        cgltf_accessor_read_float(a, used[i], &out[i * comps], comps);
                };
                std::vector<float> pos, nrm, tan, uv[2], col, blend, lmuv;
                read(aPos, 3, pos);
                if (aNrm && aNrm->count == n)
                    read(aNrm, 3, nrm);
                if (aTan && aTan->count == n && aNrm)
                    read(aTan, 4, tan);
                const SMapLightSet* lset = lightSet(backdrop);
                if (aLight && aLight->count == n && lset && lset->hasLightmaps())
                    read(aLight, 2, lmuv);
                for (int k = 0; k < 2; ++k)
                    if (aUV[k] && aUV[k]->count == n)
                        read(aUV[k], 2, uv[k]);
                const size_t colComps = aCol && aCol->count == n ? cgltf_num_components(aCol->type) : 0;
                if (colComps)
                    read(aCol, colComps, col);
                if (aBlend && aBlend->count == n && mm.layerTex >= 0) { // the first component of _BLEND
                    const size_t comps = cgltf_num_components(aBlend->type);
                    read(aBlend, comps, blend);
                    for (size_t i = 0; i < m; ++i)
                        blend[i] = blend[i * comps];
                }

                // to world space
                const M4        nm = xf.inverse().transposed();
                std::vector<V3> P(m), N(m, V3{0, 0, 0});
                for (size_t i = 0; i < m; ++i)
                    P[i] = xf.point({pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]});
                if (!nrm.empty()) {
                    for (size_t i = 0; i < m; ++i)
                        N[i] = normalize(nm.dir({nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]}));
                } else {
                    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
                        const uint32_t a = tris[t], b = tris[t + 1], c = tris[t + 2];
                        const V3       fn = cross(P[b] - P[a], P[c] - P[a]); // area weighted
                        N[a] += fn;
                        N[b] += fn;
                        N[c] += fn;
                    }
                    for (auto& v : N)
                        v = normalize(v);
                }

                // tangents turn with the node; a mirroring transform flips the bitangent
                std::vector<V3> T;
                float           handedness = 1.f;
                if (!tan.empty()) {
                    T.resize(m);
                    for (size_t i = 0; i < m; ++i)
                        T[i] = normalize(xf.dir({tan[i * 4], tan[i * 4 + 1], tan[i * 4 + 2]}));
                    const V3 X = xf.dir({1, 0, 0}), Y = xf.dir({0, 1, 0}), Z = xf.dir({0, 0, 1});
                    handedness = dot(cross(X, Y), Z) < 0.f ? -1.f : 1.f;
                }

                // the game's lighting: its lightmap, or the light probe volume the object is in (by its
                // middle). Merged props are many objects in one: each connected piece gets its own.
                eMapLight                                 mode = model.lighting.present ? LIGHT_FLAT : LIGHT_OWN;
                std::vector<const SMapLightSet::SVolume*> vol;
                if (mode != LIGHT_OWN && !rules.sky && !mm.unlit) {
                    if (!lmuv.empty())
                        mode = LIGHT_MAP;
                    else if (lset && !lset->probes.empty()) {
                        std::vector<uint32_t> piece(m);
                        std::iota(piece.begin(), piece.end(), 0u);
                        const auto find = [&](uint32_t i) {
                            while (piece[i] != i)
                                i = piece[i] = piece[piece[i]];
                            return i;
                        };
                        for (size_t t = 0; t + 2 < tris.size(); t += 3)
                            for (int k = 1; k < 3; ++k)
                                piece[find(tris[t + k])] = find(tris[t]);
                        std::unordered_map<uint32_t, SAABB> boxes;
                        for (uint32_t i = 0; i < m; ++i)
                            boxes.try_emplace(find(i), SAABB::empty()).first->second.grow(P[i]);
                        std::unordered_map<uint32_t, const SMapLightSet::SVolume*> chosen;
                        for (const auto& [root, box] : boxes)
                            chosen[root] = chooseLightProbe(*lset, box.center());
                        vol.resize(m);
                        for (uint32_t i = 0; i < m; ++i)
                            vol[i] = chosen[find(i)];
                        if (std::ranges::all_of(vol, [](const auto* v) { return v != nullptr; }))
                            mode = LIGHT_PROBE;
                    }
                }

                if (rules.collide || (rules.shadow && mm.alphaMode != ALPHA_BLEND)) {
                    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
                        const V3 &a = P[tris[t]], &b = P[tris[t + 1]], &c = P[tris[t + 2]];
                        if (rules.collide)
                            colTris.insert(colTris.end(), {a, b, c});
                        if (rules.shadow && mm.alphaMode != ALPHA_BLEND)
                            occTris.insert(occTris.end(), {a, b, c});
                    }
                }
                if (backdrop)
                    for (const auto& p : P)
                        model.backdropBounds.grow(p);
                else if (!rules.sky && !mm.effect)
                    for (const auto& p : P)
                        model.geometryBounds.grow(p);

                if (!rules.render && !rules.shadow)
                    return;

                const uint32_t base = (uint32_t)model.vertices.size();
                const int      bu   = baseUV[mat];
                for (size_t i = 0; i < m; ++i) {
                    SMapVertex v{};
                    v.pos[0]    = P[i].x;
                    v.pos[1]    = P[i].y;
                    v.pos[2]    = P[i].z;
                    v.normal[0] = N[i].x;
                    v.normal[1] = N[i].y;
                    v.normal[2] = N[i].z;
                    // uv holds the base color's set, uv1 the other one
                    const auto& u0 = uv[bu], &u1 = uv[1 - bu];
                    if (!u0.empty()) {
                        v.uv[0] = u0[i * 2];
                        v.uv[1] = u0[i * 2 + 1];
                    }
                    if (!u1.empty()) {
                        v.uv1[0] = u1[i * 2];
                        v.uv1[1] = u1[i * 2 + 1];
                    }
                    for (size_t c = 0; c < 4; ++c) {
                        const float f = c < colComps ? col[i * colComps + c] : 1.f;
                        v.color[c]    = (uint8_t)std::lround(std::clamp(f, 0.f, 1.f) * 255.f);
                    }
                    // Hammer's vertex paint in a tint: all 0 is unpainted (CS2's vertex shader leaves those alone)
                    if (mm.vertexColor == 4 && !(v.color[0] | v.color[1] | v.color[2] | v.color[3]))
                        v.color[0] = v.color[1] = v.color[2] = v.color[3] = 255;
                    v.ao[0] = v.ao[1] = 255;
                    v.ao[2] = 0;
                    v.ao[3] = blend.empty() ? 0 : (uint8_t)std::lround(std::clamp(blend[i], 0.f, 1.f) * 255.f);
                    if (!T.empty()) {
                        const auto sn = [](float f) { return (int16_t)std::lround(std::clamp(f, -1.f, 1.f) * 32767.f); };
                        v.tangent[0]  = sn(T[i].x);
                        v.tangent[1]  = sn(T[i].y);
                        v.tangent[2]  = sn(T[i].z);
                        v.tangent[3]  = sn((tan[i * 4 + 3] < 0.f ? -1.f : 1.f) * handedness);
                    }
                    v.lighting[0] = mode;
                    if (mode == LIGHT_MAP) {
                        v.light[0] = lmuv[i * 2];
                        v.light[1] = lmuv[i * 2 + 1];
                    } else if (mode == LIGHT_PROBE) {
                        // texels of the atlas, kept half a texel inside the volume's own block so filtering
                        // never reaches into its neighbours'
                        const auto&  vl    = *vol[i];
                        const V3     q     = vl.toBox.point(P[i]);
                        const float  qc[3] = {q.x, q.y, q.z};
                        for (int c = 0; c < 3; ++c) {
                            const float lo = vl.atlasOffset[c] + 0.5f, hi = vl.atlasOffset[c] + vl.atlasSize[c] - 0.5f;
                            v.light[c]     = std::clamp(qc[c] * vl.atlasSize[c] + vl.atlasOffset[c], lo, std::max(lo, hi));
                        }
                    }
                    model.vertices.push_back(v);
                }
                auto& out = batches[{mat, rules.render, rules.shadow && mm.alphaMode != ALPHA_BLEND, rules.sky, backdrop}];
                for (const uint32_t t : tris)
                    out.push_back(base + t);
                if (backdrop)
                    backdropTris += tris.size() / 3;
            }

            // backdrop: under a hypr3d_backdrop node
            void node(const cgltf_node* nd, bool backdrop, const M4& xf) {
                const std::string name = nd->name ? nd->name : "";
                const std::string ln   = lower(name);

                if (ln.starts_with("hypr3d_spawn")) {
                    spawnNode    = xf.point({0, 0, 0});
                    spawnForward = xf.dir({0, 0, -1});
                    spawnNamed   = true;
                } else if (ln.starts_with("hypr3d_desktop")) {
                    desktopNode   = xf.point({0, 0, 0});
                    desktopNormal = xf.dir({0, 0, 1});
                } else if (!spawnNamed && !spawnNode && ln.starts_with("info_player_"))
                    spawnNode = xf.point({0, 0, 0});

                // lights shine down their -Z
                if (nd->light && nd->light->type == cgltf_light_type_directional && !sunNode)
                    if (const V3 d = normalize(xf.dir({0, 0, 1})); d.y > 0.1f)
                        sunNode = d;

                if (!nd->mesh)
                    return;
                const std::string object = name + " " + (nd->mesh->name ? nd->mesh->name : "");

                std::vector<M4> instances;
                if (nd->has_mesh_gpu_instancing) {
                    const cgltf_accessor *t = nullptr, *r = nullptr, *s = nullptr;
                    for (size_t i = 0; i < nd->mesh_gpu_instancing.attributes_count; ++i) {
                        const auto& at = nd->mesh_gpu_instancing.attributes[i];
                        if (!at.name)
                            continue;
                        if (!std::strcmp(at.name, "TRANSLATION"))
                            t = at.data;
                        else if (!std::strcmp(at.name, "ROTATION"))
                            r = at.data;
                        else if (!std::strcmp(at.name, "SCALE"))
                            s = at.data;
                    }
                    const size_t count = t ? t->count : r ? r->count : s ? s->count : 0;
                    for (size_t i = 0; i < count; ++i) {
                        float tv[3] = {0, 0, 0}, rv[4] = {0, 0, 0, 1}, sv[3] = {1, 1, 1};
                        if (t)
                            cgltf_accessor_read_float(t, i, tv, 3);
                        if (r)
                            cgltf_accessor_read_float(r, i, rv, 4);
                        if (s)
                            cgltf_accessor_read_float(s, i, sv, 3);
                        instances.push_back(xf * M4::trs({tv[0], tv[1], tv[2]}, Quat{rv[0], rv[1], rv[2], rv[3]}.normalized(), {sv[0], sv[1], sv[2]}));
                    }
                } else
                    instances.push_back(xf);

                for (const auto& m : instances) {
                    for (size_t p = 0; p < nd->mesh->primitives_count; ++p)
                        primitive(nd->mesh->primitives[p], m, object, backdrop);
                    check(cancel);
                }
            }
        };

        // The map's own sun if it has one. Otherwise high in the sky and along
        // the map's longest side, so light reaches down into courtyards and
        // streets rather than only the rooftops.
        V3 mapSun(const SAABB& bounds, const std::optional<V3>& fromMap) {
            V3 sun = fromMap.value_or(V3{});
            if (!fromMap) {
                const V3    size  = bounds.size();
                const float along = size.x >= size.z ? 0.f : 3.14159265f * 0.5f;
                const float az = along + 0.35f, elev = 65.f * 3.14159265f / 180.f;
                sun = {-std::cos(az) * std::cos(elev), std::sin(elev), std::sin(az) * std::cos(elev)};
            } else if (sun.y > 0.97f)
                sun = sun + V3{-0.2f, 0, 0.1f}; // straight down leaves no shadows to see shapes by
            return normalize(sun);
        }

        // per vertex: how enclosed it is nearby, how much of the sky it sees, and
        // how much sunlight the surroundings bounce onto it
        void bakeAO(SMapModel& model, const CCollision& occ, const V3& sun, const std::atomic<bool>& cancel) {
            const size_t n = model.vertices.size();
            if (n == 0 || n > 3'000'000)
                return;
            const auto  dirs  = hemisphere(n > 800'000 ? 8 : 16);
            const float RANGE = 1.5f, SKY = 60.f;
            gltf::parallelFor(n, 512, cancel, [&](size_t b, size_t e) {
                for (size_t i = b; i < e; ++i) {
                    auto&    v = model.vertices[i];
                    const V3 nrm{v.normal[0], v.normal[1], v.normal[2]};
                    if (length(nrm) < 0.5f)
                        continue;
                    const V3 t  = perpendicular(nrm);
                    const V3 bt = cross(nrm, t);
                    const V3 p  = V3{v.pos[0], v.pos[1], v.pos[2]} + nrm * 0.03f;
                    float    local = 0, sky = 0, bounce = 0;
                    for (const auto& s : dirs) {
                        const V3 d = t * s.x + nrm * s.y + bt * s.z;
                        SRayHit  h;
                        if (occ.raycast(p, d, SKY, h)) {
                            local += smoothstep01(h.t / RANGE);
                            // one bounce: is the surface this ray hit in the sun?
                            // (the directions are cosine weighted, so a plain average is the irradiance)
                            const float lit = dot(h.normal, sun);
                            if (SRayHit sh; lit > 0.f && !occ.raycast(p + d * h.t + h.normal * 0.02f, sun, 300.f, sh))
                                bounce += lit;
                        } else {
                            local += 1.f;
                            sky += 1.f;
                        }
                    }
                    v.ao[0] = (uint8_t)std::lround(local / dirs.size() * 255.f);
                    v.ao[1] = (uint8_t)std::lround(sky / dirs.size() * 255.f);
                    v.ao[2] = (uint8_t)std::lround(std::min(bounce / dirs.size(), 1.f) * 255.f);
                }
            });
        }

        // ------------------------------------------------------------ anchors

        bool fits(const CCollision& col, const V3& feet) {
            return !col.overlaps({{feet.x - PLAYER_RADIUS, feet.y + 0.05f, feet.z - PLAYER_RADIUS}, {feet.x + PLAYER_RADIUS, feet.y + PLAYER_HEIGHT, feet.z + PLAYER_RADIUS}});
        }

        // the floor under a point, if there is one within `depth`
        std::optional<V3> floorBelow(const CCollision& col, const V3& p, float depth) {
            std::vector<SRayHit> hits;
            col.raycastAll(p, {0, -1, 0}, depth, hits);
            for (const auto& h : hits)
                if (h.normal.y > 0.6f)
                    return p + V3{0, -h.t + 0.02f, 0};
            return std::nullopt;
        }

        struct SSpot {
            V3    feet;
            float score = 0;
        };

        // good places to stand, best first: on a floor, inside the map (walls
        // around at eye height rather than open void, which is where roofs are),
        // with some room, and a few meters apart
        std::vector<SSpot> findSpawns(const CCollision& col, const CCollision& occ, const SAABB& bounds, size_t count) {
            const int            G    = 32;
            const V3             size = bounds.size();
            std::vector<SSpot>   all;
            std::vector<SRayHit> hits;
            for (int gz = 0; gz < G; ++gz) {
                for (int gx = 0; gx < G; ++gx) {
                    const V3 top{bounds.min.x + (gx + 0.5f) / G * size.x, bounds.max.y + 1.f, bounds.min.z + (gz + 0.5f) / G * size.z};
                    col.raycastAll(top, {0, -1, 0}, size.y + 2.f, hits);
                    for (const auto& h : hits) {
                        if (h.normal.y < 0.7f)
                            continue;
                        const V3 feet = top + V3{0, -h.t + 0.02f, 0};
                        if (!fits(col, feet))
                            continue;
                        const V3 eye = feet + V3{0, PLAYER_EYE, 0};
                        int                   enclosed = 0;
                        std::array<float, 16> dist;
                        for (int k = 0; k < 16; ++k) {
                            const float a = k * 3.14159265f / 8.f;
                            SRayHit     w;
                            dist[k] = 15.f;
                            if (occ.raycast(eye, {std::cos(a), 0, std::sin(a)}, 80.f, w)) {
                                ++enclosed;
                                dist[k] = std::min(w.t, 15.f);
                            }
                        }
                        // how much room there is: the narrowest way through tells an alley
                        // from a courtyard, the median distance a hall from a corridor
                        float width = 30.f;
                        for (int k = 0; k < 8; ++k)
                            width = std::min(width, dist[k] + dist[k + 8]);
                        std::ranges::sort(dist);
                        const float room = (dist[7] + dist[8]) * 0.5f;
                        SRayHit     up;
                        const bool  lowCeiling = occ.raycast(eye, {0, 1, 0}, 2.5f, up);
                        float       score = enclosed / 16.f + room / 15.f * 2.f + std::min(width, 8.f) / 8.f * 2.f + (lowCeiling ? 0.f : 0.3f);
                        if (enclosed < 12)
                            score *= 0.25f; // most likely on a roof, or outside the map
                        all.push_back({feet, score});
                    }
                }
            }
            std::ranges::sort(all, [](const SSpot& a, const SSpot& b) { return a.score > b.score; });
            std::vector<SSpot> out;
            for (const auto& s : all) {
                if (out.size() >= count || s.score < all.front().score * 0.75f)
                    break;
                if (std::ranges::none_of(out, [&](const SSpot& o) { return length(o.feet - s.feet) < 3.f; }))
                    out.push_back(s);
            }
            return out;
        }

        // stand in front of the desktop, far enough to see all of it
        std::optional<V3> spawnFacing(const CCollision& col, const SDesktopAnchor& d) {
            const float want = std::max(d.height * 0.5f / std::tan(HALF_FOV_Y) * 1.8f, 3.f);
            SRayHit     h;
            const float room = col.raycast(d.center + d.normal * 0.05f, d.normal, 14.f, h) ? h.t - 0.8f : 14.f;
            for (float dist : {want, want * 0.75f, want * 0.5f, 2.f}) {
                if (dist > room)
                    continue;
                const V3 at = d.center + d.normal * dist;
                const auto f = floorBelow(col, at, 6.f);
                if (!f || !fits(col, *f) || at.y - f->y > 3.5f)
                    continue;
                // and a clear way up to it (no planter or railing in between)
                if (SRayHit k; col.raycast(*f + V3{0, 0.5f, 0}, -d.normal, dist - 0.3f, k))
                    continue;
                return f;
            }
            return std::nullopt;
        }

        struct SWallFit {
            SDesktopAnchor desktop;
            V3             stand; // where to see it from
            float          score = 0;
        };

        // a flat stretch of wall the desktop fits on, seen from `feet`: every part
        // of it on the wall, nothing in front of it (pillars, planters, shelves,
        // an arch) and all of it in sight from where the player will stand
        std::optional<SWallFit> findWall(const CCollision& col, const CCollision& occ, const V3& feet, float aspect, float height) {
            const V3                eye = feet + V3{0, PLAYER_EYE, 0};
            const V3                up{0, 1, 0};
            std::optional<SWallFit> best;
            float                   bestScore = -1e9f;

            const auto grid = [&](const V3& c, const V3& right, float W, float H, int nx, int ny, auto&& fn) {
                for (int iy = 0; iy < ny; ++iy)
                    for (int ix = 0; ix < nx; ++ix)
                        if (!fn(c + right * (W * (ix / (nx - 1.f) - 0.5f)) + up * (H * (iy / (ny - 1.f) - 0.5f))))
                            return false;
                return true;
            };

            for (int k = 0; k < 48; ++k) {
                const float a = k * 3.14159265f / 24.f;
                const V3    dir{std::cos(a), 0, std::sin(a)};
                SRayHit     h;
                if (!occ.raycast(eye, dir, 18.f, h) || h.t < 2.2f || std::abs(h.normal.y) > 0.25f)
                    continue;
                const V3 n     = horizontal(h.normal);
                const V3 right = normalize(cross(up, n));
                const V3 hit   = eye + dir * h.t;
                for (float H : {height, height * 0.8f, height * 0.62f, height * 0.48f}) {
                    const float W = H * aspect;
                    const V3    center{hit.x, feet.y + std::max(PLAYER_EYE, H * 0.5f + 0.25f), hit.z};
                    const V3    c = center + n * 0.01f;
                    // on the wall, and nothing standing or hanging in front of it
                    const bool  onWall = grid(c, right, W, H, 9, 5, [&](const V3& q) {
                        SRayHit w;
                        if (!occ.raycast(q + n * 0.3f, -n, 0.6f, w) || std::abs(w.t - 0.31f) > 0.08f || dot(w.normal, n) < 0.9f)
                            return false;
                        return !occ.raycast(q + n * 0.03f, n, 1.5f, w);
                    });
                    if (!onWall)
                        continue;
                    // room to stand back and see all of it (not the back of a curtain in a corridor)
                    SRayHit     front;
                    const float room = col.raycast(c + n * 0.05f, n, 14.f, front) ? front.t : 14.f;
                    if (room < 2.6f)
                        break;
                    const SDesktopAnchor d{c, n, H};
                    const V3             stand = spawnFacing(col, d).value_or(feet);
                    const V3             from  = stand + V3{0, PLAYER_EYE, 0};
                    const bool           seen  = grid(c + n * 0.02f, right, W, H, 7, 5, [&](const V3& q) {
                        const V3    to  = q - from;
                        const float len = length(to);
                        SRayHit     w;
                        return len < 1e-3f || !occ.raycast(from, to / len, len - 0.03f, w);
                    });
                    if (!seen)
                        continue;
                    const float score = H * 2.f + std::min(room, 6.f) * 0.3f - std::abs(h.t - 5.f) * 0.15f;
                    if (score > bestScore) {
                        bestScore = score;
                        best      = SWallFit{d, stand, score};
                    }
                    break;
                }
            }
            return best;
        }

        // ------------------------------------------------------- state file

        struct SState {
            std::optional<float>          scale;
            std::optional<std::pair<V3, float>> spawn;
            std::optional<SDesktopAnchor> desktop;
        };

        SState readState(const std::string& mapPath) {
            SState        st;
            std::ifstream f(mapStatePath(mapPath));
            std::string   line;
            while (std::getline(f, line)) {
                std::istringstream in(line);
                std::string        key;
                in >> key;
                if (key == "scale") {
                    float s = 0;
                    if (in >> s && s > 0)
                        st.scale = s;
                } else if (key == "spawn") {
                    V3    p;
                    float yaw = 0;
                    if (in >> p.x >> p.y >> p.z >> yaw)
                        st.spawn = {p, yaw};
                } else if (key == "desktop") {
                    SDesktopAnchor d;
                    if (in >> d.center.x >> d.center.y >> d.center.z >> d.normal.x >> d.normal.y >> d.normal.z >> d.height && d.height > 0.1f)
                        st.desktop = d;
                }
            }
            return st;
        }
    }

    const SMapLightSet::SVolume* chooseLightProbe(const SMapLightSet& set, const V3& p) {
        const SMapLightSet::SVolume* best         = nullptr;
        int                          bestPriority = std::numeric_limits<int>::min();
        float                        bestScore    = 1e30f;
        const auto                   consider     = [&](const SMapLightSet::SVolume& v, float score) {
            if (v.priority < bestPriority || (v.priority == bestPriority && score >= bestScore))
                return;
            best         = &v;
            bestPriority = v.priority;
            bestScore    = score;
        };
        for (const auto& v : set.volumes) {
            const V3 q = v.toBox.point(p);
            if (q.x >= 0.f && q.x <= 1.f && q.y >= 0.f && q.y <= 1.f && q.z >= 0.f && q.z <= 1.f)
                consider(v, length(p - v.bounds.center()));
        }
        if (!best)
            for (const auto& v : set.volumes) {
                const V3 d = vmax(vmax(v.bounds.min - p, p - v.bounds.max), V3{0, 0, 0});
                consider(v, length(d));
            }
        return best;
    }

    // ------------------------------------------------------------ exposure

    namespace {
        float luma(const V3& c) {
            return c.x * 0.2125f + c.y * 0.7154f + c.z * 0.0721f;
        }

        const SMapLighting* bakedLighting(const SWorld& world) {
            return world.model && world.model->lighting.present ? &world.model->lighting : nullptr;
        }

        // the most hypr3d's own lighting brightens a dark place
        constexpr float OWN_MAX_EXPOSURE = 1.6f;
    }

    float exposureSample(const SWorld& world, const V3& eye, const V3& dir) {
        const SMapLighting* L = bakedLighting(world);
        SRayHit             h;
        if (!world.collision.raycast(eye, dir, 250.f, h))
            return L ? luma(L->skyAverage) : 0.8f; // sky
        const V3    p   = eye + dir * h.t + h.normal * 0.02f;
        const float ndl = dot(h.normal, world.sunDir);
        SRayHit     s;
        const bool  lit = ndl > 0.f && !world.collision.raycast(p, world.sunDir, 300.f, s);
        if (!L)
            return 0.45f * ((lit ? 2.78f * ndl : 0.f) + 0.3f);
        // the game's lighting: its sun on a surface of middling albedo, and the rest as its light
        // probes have it there
        // (half a meter out: a probe right at the wall may be inside it)
        const auto& set     = L->sets[0];
        float       ambient = luma(V3{set.average[0], set.average[1], set.average[2]});
        const V3    at      = p + h.normal * 0.5f;
        if (const auto* vol = set.probeLuma.empty() ? nullptr : chooseLightProbe(set, at)) {
            const V3    q     = vol->toBox.point(at);
            const float qc[3] = {q.x, q.y, q.z};
            int         t[3];
            for (int c = 0; c < 3; ++c)
                t[c] = std::clamp((int)(qc[c] * vol->atlasSize[c]) + vol->atlasOffset[c], vol->atlasOffset[c], vol->atlasOffset[c] + vol->atlasSize[c] - 1);
            if (t[0] < set.probeDims[0] && t[1] < set.probeDims[1] && t[2] < set.probeDims[2]) {
                // the ambient cube along the normal, in Source's axes (blocks +x +y +z -x -y -z)
                const uint8_t* cube = &set.probeLuma[(((size_t)t[2] * set.probeDims[1] + t[1]) * set.probeDims[0] + t[0]) * 6];
                const float    n[3] = {h.normal.z, h.normal.x, h.normal.y};
                ambient             = 0.f;
                for (int c = 0; c < 3; ++c)
                    ambient += n[c] * n[c] * probeLumaValue(cube[n[c] >= 0.f ? c : c + 3]);
            }
        }
        return 0.45f * ((lit ? luma(L->sunColor) * ndl : 0.f) + ambient);
    }

    V3 exposureDirection(int i) {
        // evenly over the sphere: a golden angle spiral
        const float y = 1.f - (i + 0.5f) * 2.f / EXPOSURE_SAMPLES;
        const float r = std::sqrt(std::max(0.f, 1.f - y * y));
        const float a = i * 2.39996323f;
        return {r * std::cos(a), y, r * std::sin(a)};
    }

    float exposureFor(const SWorld& world, const float* samples, const V3& view) {
        const SMapLighting* L = bakedLighting(world);
        if (!L) {
            // hypr3d's own lighting: all around, as eyes adjust to a place
            float sum = 0;
            for (int i = 0; i < EXPOSURE_SAMPLES; ++i)
                sum += samples[i];
            return std::clamp(0.7f / std::max(sum / EXPOSURE_SAMPLES, 1e-3f), 1.f, OWN_MAX_EXPOSURE);
        }
        if (!L->exposureAuto)
            return 1.f;
        // CS2 meters the log average of what's on screen: mostly what's in front, then
        // aims what its tone curve shows as middle grey at it, within the map's range
        float sum = 0, weights = 0;
        for (int i = 0; i < EXPOSURE_SAMPLES; ++i) {
            const float w = 0.05f + smoothstep01(std::clamp((dot(exposureDirection(i), view) - 0.5f) / 0.45f, 0.f, 1.f));
            sum += w * std::log(std::max(samples[i], 0.005f));
            weights += w;
        }
        const float  average = std::exp(sum / weights);
        const float* k       = L->curve;
        const float  white   = k[6] * 2.8f;
        const auto   raw     = [&](float c) {
            c = std::min(c * 2.8f, white);
            return (c * (k[0] * c + k[1] * k[2]) + k[4] * k[3]) / (c * (k[0] * c + k[1]) + k[5] * k[3]) - k[4] / k[5];
        };
        const float top = raw(white / 2.8f);
        float       lo = 0.f, hi = white / 2.8f;
        for (int i = 0; i < 32 && std::abs(top) > 1e-9f; ++i) {
            const float mid = (lo + hi) * 0.5f;
            (raw(mid) / top < 0.18f ? lo : hi) = mid;
        }
        const float grey = std::abs(top) > 1e-9f ? (lo + hi) * 0.5f : 0.18f;
        return std::clamp(grey / average, std::min(L->exposureMin, L->exposureMax), std::max(L->exposureMin, L->exposureMax));
    }

    float exposureStep(const SWorld& world, float current, float target, float dt) {
        const SMapLighting* L = bakedLighting(world);
        current               = std::max(current, 1e-3f);
        if (!L)
            return std::exp(std::lerp(std::log(current), std::log(target), 1.f - std::exp(-dt / 0.6f)));
        // CS2's: so many stops a second
        const float from = std::log2(current), to = std::log2(target);
        const float rate = (to > from ? L->exposureSpeedUp : L->exposureSpeedDown) * dt;
        return std::exp2(to > from ? std::min(from + rate, to) : std::max(from - rate, to));
    }

    // ------------------------------------------------------------ loading

    SMapResult loadMap(const SMapRequest& req, const std::atomic<bool>& cancel) {
        SMapResult res;
        res.req          = req;
        const auto start = std::chrono::steady_clock::now();
        auto&      log   = res.log;

        std::error_code ec;
        const auto      abs = std::filesystem::absolute(req.path, ec);
        if (!std::filesystem::is_regular_file(abs, ec)) {
            res.error = std::format("map {} doesn't exist", req.path);
            return res;
        }
        const std::string path = abs.string();

        auto guard = gltf::open(path, "map", res.error);
        if (!guard)
            return res;
        cgltf_data* data = guard.get();

        auto world   = std::make_unique<SWorld>();
        world->model = std::make_shared<SMapModel>();
        auto& model  = *world->model;
        model.path   = path;
        world->name  = abs.stem().string();

        SBuild b{req, cancel, data, model, log};
        b.materials();
        // the game's own lighting, when the file has it (cs2map's HYPR3D_lighting): before the
        // geometry, which gets its lightmap uvs and light probe coordinates as it's read
        const bool lit = gltf::readLighting(data, abs.parent_path().string(), model.lighting, cancel, log);
        // (the sky's image as the model has them, lighting or not: it counts the materials' images only)
        auto& L    = model.lighting;
        L.skyImage = L.skyImage >= 0 && (size_t)L.skyImage < b.imageSlot.size() ? b.imageSlot[L.skyImage] : -1;
        if (lit) {
            size_t volumes = 0;
            for (const auto& s : L.sets)
                volumes += s.volumes.size();
            log.push_back(std::format("the map's own lighting: {} lightmap set(s), {} light probe volumes{}{}", L.sets.size(), volumes, L.fog ? ", fog" : "",
                                      L.exposureAuto ? std::format(", exposure {}-{}", L.exposureMin, L.exposureMax) : ""));
        }

        // each node with whether it's in the backdrop and its parent's world transform (worked out on the way down:
        // cgltf_node_transform_world walks up to the root for each node, and a file can nest them 100000 deep)
        struct SVisit {
            const cgltf_node* nd;
            bool              backdrop;
            M4                parent;
        };
        std::vector<SVisit> stack;
        const cgltf_scene*  scene = data->scene ? data->scene : data->scenes_count ? &data->scenes[0] : nullptr;
        if (scene)
            for (size_t i = 0; i < scene->nodes_count; ++i)
                stack.push_back({scene->nodes[i], false, M4::identity()});
        else
            for (size_t i = 0; i < data->nodes_count; ++i)
                if (!data->nodes[i].parent)
                    stack.push_back({&data->nodes[i], false, M4::identity()});
        while (!stack.empty()) {
            auto [nd, backdrop, parent] = stack.back();
            stack.pop_back();
            gltf::check(cancel);
            float lm[16];
            cgltf_node_transform_local(nd, lm);
            M4 local;
            std::memcpy(local.m, lm, sizeof(lm));
            const M4 xf = parent * local;
            if (nd->name && lower(nd->name).starts_with("hypr3d_backdrop")) {
                backdrop                = true;
                model.backdropTransform = xf;
            }
            b.node(nd, backdrop, xf);
            for (size_t i = 0; i < nd->children_count; ++i)
                stack.push_back({nd->children[i], backdrop, xf});
        }
        if (b.skippedDraco)
            log.push_back(std::format("skipped {} draco compressed meshes (not supported)", b.skippedDraco));
        if (b.backdropTris)
            log.push_back(std::format("backdrop (hypr3d_backdrop): {} triangles", b.backdropTris));
        if (model.vertices.empty() && b.colTris.empty()) {
            res.error = std::format("{} has no triangles", req.path);
            return res;
        }

        // units: glTF is meters, but converted game maps often come in their
        // own units (inches for Source and GoldSrc maps)
        const SState st      = readState(path);
        const V3     rawSize = model.geometryBounds.size();
        const float  extent  = std::max(rawSize.x, rawSize.z);
        float        scale   = req.scale;
        if (scale <= 0.f && st.scale && *st.scale > 0.f)
            scale = *st.scale; // what it was last used at
        if (scale <= 0.f) {
            scale = extent > 600.f ? 0.0254f : 1.f;
            if (scale != 1.f)
                log.push_back(std::format("the map is {:.0f} units across, taking them as inches (scale {}); set plugin:hypr3d:map_scale to override", extent, scale));
        }
        if (scale != 1.f) {
            for (auto& v : model.vertices)
                for (float& c : v.pos)
                    c *= scale;
            for (auto& p : b.colTris)
                p *= scale;
            for (auto& p : b.occTris)
                p *= scale;
            model.geometryBounds.min *= scale;
            model.geometryBounds.max *= scale;
            model.backdropBounds.min *= scale;
            model.backdropBounds.max *= scale;
            if (b.spawnNode)
                *b.spawnNode *= scale;
            if (b.desktopNode)
                *b.desktopNode *= scale;
        }
        check(cancel);

        for (size_t i = 0; i + 2 < b.colTris.size(); i += 3)
            world->collision.addTriangle(b.colTris[i], b.colTris[i + 1], b.colTris[i + 2]);
        world->collision.build();
        CCollision occ;
        for (size_t i = 0; i + 2 < b.occTris.size(); i += 3)
            occ.addTriangle(b.occTris[i], b.occTris[i + 1], b.occTris[i + 2]);
        occ.build();
        b.colTris = {};
        b.occTris = {};
        check(cancel);

        // one index range per material: opaque, alpha tested, sky, blended, shadow only
        auto order = [&](const SBatchKey& k) {
            const auto mode = model.materials[k.material].alphaMode;
            if (!k.render)
                return 4;
            if (k.sky)
                return 2;
            return mode == ALPHA_BLEND ? 3 : mode == ALPHA_MASK ? 1 : 0;
        };
        std::vector<std::pair<SBatchKey, std::vector<uint32_t>*>> sorted;
        for (auto& [k, v] : b.batches)
            if (!v.empty())
                sorted.emplace_back(k, &v);
        std::ranges::stable_sort(sorted, [&](const auto& x, const auto& y) { return order(x.first) < order(y.first); });
        for (auto& [k, v] : sorted) {
            model.batches.push_back({k.material, (uint32_t)model.indices.size(), (uint32_t)v->size(), k.render, k.shadow, k.sky, k.backdrop});
            model.indices.insert(model.indices.end(), v->begin(), v->end());
        }
        model.triangles = model.indices.size() / 3;
        b.batches.clear();

        // the sky's gradients would band in compressed blocks
        for (const auto& bt : model.batches)
            if (const int t = model.materials[bt.material].baseTex; bt.sky && t >= 0)
                model.images[t].plain = true;
        if (L.skyImage >= 0 && (size_t)L.skyImage < model.images.size())
            model.images[L.skyImage].plain = true;
        gltf::decodeImages(data, abs.parent_path().string(), model.images, b.imageSlot, cancel, log, req.compress);
        if (const V3 d = model.lighting.sunDir; model.lighting.present && length(d) > 0.5f && d.y > 0.1f)
            b.sunNode = normalize(d);
        world->sunDir = mapSun(model.geometryBounds, b.sunNode);
        if (b.sunNode)
            log.push_back("using the map's sun");
        // the game's lightmaps have all of that and more
        if (!model.lighting.present)
            bakeAO(model, occ, world->sunDir, cancel);
        else if (auto& L = model.lighting; L.skyImage >= 0 && (size_t)L.skyImage < model.images.size() && !model.images[L.skyImage].rgba.empty()) {
            // the sky's light, from its upper half (area weighted)
            const auto& img    = model.images[L.skyImage];
            double      sum[3] = {0, 0, 0}, wsum = 0;
            for (int y = 0; y < img.h / 2; ++y) {
                const double w = std::sin((y + 0.5) / img.h * 3.14159265358979);
                for (int x = 0; x < img.w; x += 4) {
                    for (int c = 0; c < 3; ++c) {
                        const double v = img.rgba[((size_t)y * img.w + x) * 4 + c] / 255.0;
                        sum[c] += w * (v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4));
                    }
                    wsum += w;
                }
            }
            if (wsum > 0)
                L.skyAverage = {(float)(sum[0] / wsum) * L.skyColor.x, (float)(sum[1] / wsum) * L.skyColor.y, (float)(sum[2] / wsum) * L.skyColor.z};
        }

        // where to stand and where the desktop hangs: saved by hand, marked
        // in the map, or guessed
        const SAABB& bounds = model.geometryBounds;
        const float  fixState = st.scale ? scale / *st.scale : 1.f;
        std::optional<V3> spawn;
        float             spawnYaw = 0;
        bool              yawKnown = false;
        if (st.spawn) {
            spawn    = st.spawn->first * fixState;
            spawnYaw = st.spawn->second;
            yawKnown = true;
        } else if (b.spawnNode) {
            spawn = floorBelow(world->collision, *b.spawnNode + V3{0, 0.5f, 0}, 3.f).value_or(*b.spawnNode);
            if (b.spawnNamed && length(horizontal(b.spawnForward)) > 0.5f) {
                spawnYaw = yawFacing(horizontal(b.spawnForward));
                yawKnown = true;
            }
        }

        std::optional<SDesktopAnchor> desktop;
        if (st.desktop) {
            desktop = *st.desktop;
            desktop->center *= fixState;
        } else if (b.desktopNode) {
            SDesktopAnchor d;
            d.center = *b.desktopNode;
            d.normal = length(horizontal(b.desktopNormal)) > 0.5f ? horizontal(b.desktopNormal) : V3{0, 0, 1};
            d.height = req.desktopHeight;
            desktop  = d;
        }

        std::vector<SSpot> spots;
        if (!spawn && desktop)
            spawn = spawnFacing(world->collision, *desktop);
        if (!spawn) {
            spots = findSpawns(world->collision, occ, bounds, 8);
            if (!spots.empty())
                spawn = spots.front().feet;
        }
        if (!spawn) {
            spawn = bounds.center();
            log.push_back("couldn't find a floor to start on, starting in the middle (fly with F)");
        }
        check(cancel);

        if (!desktop) {
            // look for a wall from each good spot, and take the best wall
            std::optional<SWallFit> wall;
            if (spots.empty())
                wall = findWall(world->collision, occ, *spawn, req.aspect, req.desktopHeight);
            for (const auto& s : spots) {
                check(cancel);
                auto w = findWall(world->collision, occ, s.feet, req.aspect, req.desktopHeight);
                if (!w)
                    continue;
                w->score += s.score * 2.f;
                if (!wall || w->score > wall->score)
                    wall = w;
            }
            if (wall) {
                desktop = wall->desktop;
                // step back in front of it, if nothing says where to start
                if (!st.spawn && !b.spawnNode)
                    spawn = wall->stand;
            } else {
                // no wall in sight: hang it in the air in front of the start,
                // towards the most open side if nothing says which way to look
                const V3 eye = *spawn + V3{0, PLAYER_EYE, 0};
                V3       fwd = yawKnown ? V3{std::sin(spawnYaw), 0, -std::cos(spawnYaw)} : V3{0, 0, -1};
                if (!yawKnown) {
                    float most = 0;
                    for (int k = 0; k < 16; ++k) {
                        const V3 dir{std::cos(k * 3.14159265f / 8.f), 0, std::sin(k * 3.14159265f / 8.f)};
                        SRayHit  h;
                        if (const float t = occ.raycast(eye, dir, 30.f, h) ? h.t : 30.f; t > most) {
                            most = t;
                            fwd  = dir;
                        }
                    }
                }
                SRayHit        ahead;
                SDesktopAnchor d;
                d.center           = eye + fwd * std::clamp(occ.raycast(eye, fwd, 5.f, ahead) ? ahead.t - 1.f : 4.f, 1.5f, 4.f);
                d.normal           = -fwd;
                d.height           = req.desktopHeight;
                desktop            = d;
                log.push_back("no free wall for the desktop found, it floats in front of the start; aim at a wall and run `hyprctl hypr3d desktop here`");
            }
        }
        if (!yawKnown) {
            const V3 to = desktop->center - (*spawn + V3{0, PLAYER_EYE, 0});
            spawnYaw    = length(horizontal(to)) > 0.1f ? yawFacing(horizontal(to)) : 0.f;
        }

        log.push_back(std::format("start at {:.2f} {:.2f} {:.2f} facing {:.0f}°, desktop at {:.2f} {:.2f} {:.2f} ({:.2f} m tall)", spawn->x, spawn->y, spawn->z,
                                  spawnYaw * 180.f / 3.14159265f, desktop->center.x, desktop->center.y, desktop->center.z, desktop->height));
        world->spawn    = *spawn;
        world->spawnYaw = spawnYaw;
        world->desktop  = *desktop;
        world->bounds   = bounds;
        res.req.scale   = scale;

        log.push_back(std::format("{}: {} triangles, {} materials, {} textures, {:.1f} x {:.1f} x {:.1f} m, loaded in {} ms", world->name, model.triangles,
                                  model.materials.size() - 1, model.images.size(), bounds.size().x, bounds.size().y, bounds.size().z,
                                  std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()));
        res.world = std::move(world);
        return res;
    }

    // ------------------------------------------------------------ state

    std::string mapStatePath(const std::string& mapPath) {
        const char* xdg  = std::getenv("XDG_STATE_HOME");
        const char* home = std::getenv("HOME");
        std::string dir  = xdg && *xdg ? std::string(xdg) : std::string(home ? home : "/tmp") + "/.local/state";
        uint32_t    h    = 2166136261u;
        for (unsigned char c : mapPath) {
            h ^= c;
            h *= 16777619u;
        }
        return std::format("{}/hypr3d/maps/{}-{:08x}.conf", dir, std::filesystem::path(mapPath).stem().string(), h);
    }

    bool saveMapState(const std::string& mapPath, const SWorld& world, float scale) {
        const auto      file = mapStatePath(mapPath);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(file).parent_path(), ec);
        std::ofstream f(file, std::ios::trunc);
        if (!f)
            return false;
        const auto& d = world.desktop;
        f << std::format("# hypr3d anchors for {}\n", mapPath);
        f << std::format("scale {}\n", scale);
        f << std::format("spawn {} {} {} {}\n", world.spawn.x, world.spawn.y, world.spawn.z, world.spawnYaw);
        f << std::format("desktop {} {} {} {} {} {} {}\n", d.center.x, d.center.y, d.center.z, d.normal.x, d.normal.y, d.normal.z, d.height);
        return (bool)f;
    }
}
