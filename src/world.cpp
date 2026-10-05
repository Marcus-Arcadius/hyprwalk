#include "world.hpp"

#include <cmath>
#include <map>
#include <tuple>

// The built-in courtyard: a small procedural desert yard (no copyrighted assets) made of axis-aligned boxes, which
// keeps collision and the baked ambient occlusion trivial.

namespace hyprwalk {

    enum eFace : uint8_t {
        FACE_NX = 1 << 0,
        FACE_PX = 1 << 1,
        FACE_NY = 1 << 2,
        FACE_PY = 1 << 3,
        FACE_NZ = 1 << 4,
        FACE_PZ = 1 << 5,
        FACE_ALL        = 0x3F,
        FACE_NO_BOTTOM  = FACE_ALL & ~FACE_NY,
    };

    struct SBox {
        SAABB     box;
        eMaterial mat;
        uint8_t   faces   = FACE_NO_BOTTOM;
        bool      collide = true;
    };

    namespace {
        struct SBuilder {
            std::vector<SBox> boxes;

            void add(V3 min, V3 max, eMaterial mat, uint8_t faces = FACE_NO_BOTTOM, bool collide = true) {
                boxes.push_back({{min, max}, mat, faces, collide});
            }

            // centered on x/z, standing on y
            void crate(float cx, float y, float cz, float size) {
                const float h = size * 0.5f;
                add({cx - h, y, cz - h}, {cx + h, y + size, cz + h}, MAT_CRATE);
            }
        };

        // cheap deterministic hemisphere directions (golden spiral, cosine weighted)
        std::vector<V3> hemisphereSamples(int n) {
            std::vector<V3> out;
            const float     golden = 2.39996323f;
            for (int i = 0; i < n; ++i) {
                const float u   = (i + 0.5f) / n;
                const float r   = std::sqrt(u);
                const float phi = i * golden;
                out.push_back({r * std::cos(phi), std::sqrt(std::max(0.f, 1.f - u)), r * std::sin(phi)});
            }
            return out;
        }

        float ambientOcclusion(const std::vector<SBox>& boxes, const std::vector<V3>& samples, const V3& p, const V3& n) {
            // tangent frame around n
            const V3 t = std::abs(n.y) > 0.9f ? V3{1, 0, 0} : V3{0, 1, 0};
            const V3 b1 = normalize(cross(n, t));
            const V3 b2 = cross(n, b1);

            const V3    origin  = p + n * 0.01f;
            const float maxDist = 1.4f;
            float       occl    = 0.f;

            for (const auto& s : samples) {
                const V3 dir  = b1 * s.x + n * s.y + b2 * s.z;
                float    best = maxDist;
                for (const auto& b : boxes) {
                    if (!b.collide)
                        continue;
                    const float d = rayAABB(origin, dir, b.box);
                    if (d >= 0.f && d < best)
                        best = d;
                }
                if (best < maxDist)
                    occl += 1.f - best / maxDist;
            }

            const float ao = 1.f - occl / samples.size();
            return std::clamp(0.25f + 0.75f * ao * ao, 0.f, 1.f);
        }

        void emitFace(std::vector<SVertex>& out, const SBox& b, int axis, bool positive, const std::vector<SBox>& all, const std::vector<V3>& samples,
                      std::map<std::tuple<int, int, int, int>, float>& aoCache) {
            const V3& mn = b.box.min;
            const V3& mx = b.box.max;

            // the two in-plane axes (u, v) and the fixed coordinate
            int ua = 0, va = 0;
            if (axis == 0) {
                ua = 2;
                va = 1;
            } else if (axis == 1) {
                ua = 0;
                va = 2;
            } else {
                ua = 0;
                va = 1;
            }

            auto get = [](const V3& v, int a) { return a == 0 ? v.x : (a == 1 ? v.y : v.z); };
            auto set = [](V3& v, int a, float val) {
                if (a == 0)
                    v.x = val;
                else if (a == 1)
                    v.y = val;
                else
                    v.z = val;
            };

            const float fixedC = positive ? get(mx, axis) : get(mn, axis);
            V3          normal{0, 0, 0};
            set(normal, axis, positive ? 1.f : -1.f);

            const float u0 = get(mn, ua), u1 = get(mx, ua);
            const float v0 = get(mn, va), v1 = get(mx, va);

            const float cell = 0.5f;
            const int   nu   = std::max(1, (int)std::ceil((u1 - u0) / cell - 1e-4f));
            const int   nv   = std::max(1, (int)std::ceil((v1 - v0) / cell - 1e-4f));

            auto vertexAt = [&](int iu, int iv) {
                const float uu = u0 + (u1 - u0) * iu / nu;
                const float vv = v0 + (v1 - v0) * iv / nv;
                V3          p;
                set(p, axis, fixedC);
                set(p, ua, uu);
                set(p, va, vv);

                // share AO between coincident vertices with the same normal
                const auto key = std::make_tuple((int)std::lround(p.x * 100), (int)std::lround(p.y * 100), (int)std::lround(p.z * 100), axis * 2 + (positive ? 1 : 0));
                float      ao  = 1.f;
                if (auto it = aoCache.find(key); it != aoCache.end())
                    ao = it->second;
                else {
                    ao = ambientOcclusion(all, samples, p, normal);
                    aoCache[key] = ao;
                }

                SVertex vtx{};
                vtx.pos[0]    = p.x;
                vtx.pos[1]    = p.y;
                vtx.pos[2]    = p.z;
                vtx.normal[0] = normal.x;
                vtx.normal[1] = normal.y;
                vtx.normal[2] = normal.z;
                if (b.mat == MAT_CRATE) {
                    // local 0..1 per face so the planks line up with the edges
                    vtx.uv[0] = (uu - u0) / std::max(1e-4f, u1 - u0);
                    vtx.uv[1] = (vv - v0) / std::max(1e-4f, v1 - v0);
                } else {
                    vtx.uv[0] = uu;
                    vtx.uv[1] = vv;
                }
                vtx.material = (float)b.mat;
                vtx.ao       = ao;
                return vtx;
            };

            // winding doesn't matter (no culling), but keep it consistent anyway
            for (int iv = 0; iv < nv; ++iv) {
                for (int iu = 0; iu < nu; ++iu) {
                    const SVertex a = vertexAt(iu, iv), bb = vertexAt(iu + 1, iv), c = vertexAt(iu + 1, iv + 1), d = vertexAt(iu, iv + 1);
                    // split along the diagonal with the smaller AO difference to avoid streaks
                    if (std::abs(a.ao - c.ao) <= std::abs(bb.ao - d.ao)) {
                        out.insert(out.end(), {a, bb, c, a, c, d});
                    } else {
                        out.insert(out.end(), {a, bb, d, bb, c, d});
                    }
                }
            }
        }
    }

    SWorld buildWorld(const SScreenSpec& screen) {
        SBuilder    w;

        const float screenW = screen.height * screen.aspect;
        const float HW      = std::max(10.f, screenW * 0.5f + 3.5f); // half width of the yard
        const float DEPTH   = 22.f;
        const float WALL_H  = 5.f;
        const float THICK   = 0.5f;
        const float FRONT   = -0.05f; // the wall face the desktop hangs on

        // floor (only the top is ever visible)
        w.add({-HW - THICK, -1.f, FRONT - THICK}, {HW + THICK, 0.f, DEPTH + THICK}, MAT_SAND, FACE_PY);

        // outer walls
        w.add({-HW - THICK, 0, FRONT - THICK}, {HW + THICK, WALL_H, FRONT}, MAT_STONE, FACE_PZ | FACE_PY);   // north (screen wall)
        w.add({-HW - THICK, 0, DEPTH}, {HW + THICK, WALL_H, DEPTH + THICK}, MAT_STONE, FACE_NZ | FACE_PY);   // south
        w.add({-HW - THICK, 0, FRONT}, {-HW, WALL_H, DEPTH}, MAT_STONE, FACE_PX | FACE_PY);                  // west
        w.add({HW, 0, FRONT}, {HW + THICK, WALL_H, DEPTH}, MAT_STONE, FACE_NX | FACE_PY);                    // east

        // crenellations along the top of every wall
        for (float x = -HW; x < HW - 0.4f; x += 1.6f) {
            w.add({x, WALL_H, FRONT - THICK}, {x + 0.8f, WALL_H + 0.55f, FRONT}, MAT_STONE, FACE_NO_BOTTOM, false);
            w.add({x, WALL_H, DEPTH}, {x + 0.8f, WALL_H + 0.55f, DEPTH + THICK}, MAT_STONE, FACE_NO_BOTTOM, false);
        }
        for (float z = FRONT + 0.8f; z < DEPTH - 0.4f; z += 1.6f) {
            w.add({-HW - THICK, WALL_H, z}, {-HW, WALL_H + 0.55f, z + 0.8f}, MAT_STONE, FACE_NO_BOTTOM, false);
            w.add({HW, WALL_H, z}, {HW + THICK, WALL_H + 0.55f, z + 0.8f}, MAT_STONE, FACE_NO_BOTTOM, false);
        }

        // plaster trim along the bottom of the walls
        w.add({-HW, 0, FRONT}, {HW, 0.25f, FRONT + 0.06f}, MAT_PLASTER, FACE_PZ | FACE_PY);
        w.add({-HW, 0, DEPTH - 0.06f}, {HW, 0.25f, DEPTH}, MAT_PLASTER, FACE_NZ | FACE_PY);
        w.add({-HW, 0, FRONT + 0.06f}, {-HW + 0.06f, 0.25f, DEPTH - 0.06f}, MAT_PLASTER, FACE_PX | FACE_PY);
        w.add({HW - 0.06f, 0, FRONT + 0.06f}, {HW, 0.25f, DEPTH - 0.06f}, MAT_PLASTER, FACE_NX | FACE_PY);

        // pillars flanking the screen
        for (float side : {-1.f, 1.f}) {
            const float px = side * (screenW / 2 + 0.95f);
            w.add({px - 0.4f, 0, FRONT}, {px + 0.4f, 0.4f, FRONT + 0.8f}, MAT_STONE);
            w.add({px - 0.32f, 0.4f, FRONT}, {px + 0.32f, 3.6f, FRONT + 0.64f}, MAT_PLASTER);
            w.add({px - 0.45f, 3.6f, FRONT}, {px + 0.45f, 3.9f, FRONT + 0.9f}, MAT_STONE, FACE_ALL);
        }

        // south gate: stone frame and wooden doors
        w.add({-2.1f, 0, DEPTH - 0.4f}, {-1.5f, 3.6f, DEPTH}, MAT_STONE);
        w.add({1.5f, 0, DEPTH - 0.4f}, {2.1f, 3.6f, DEPTH}, MAT_STONE);
        w.add({-2.1f, 3.0f, DEPTH - 0.4f}, {2.1f, 3.6f, DEPTH}, MAT_STONE, FACE_ALL);
        w.add({-1.5f, 0, DEPTH - 0.12f}, {0, 3.0f, DEPTH - 0.06f}, MAT_CRATE, FACE_NZ);
        w.add({0, 0, DEPTH - 0.12f}, {1.5f, 3.0f, DEPTH - 0.06f}, MAT_CRATE, FACE_NZ);

        // crates
        const float L = -HW + 2.5f;
        w.crate(L, 0, 8.0f, 0.9f);
        w.crate(L, 0, 8.95f, 0.9f);
        w.crate(L, 0.9f, 8.45f, 0.9f);
        w.crate(L + 1.0f, 0, 8.5f, 0.9f);
        w.crate(4.8f, 0, 6.5f, 0.9f);
        w.crate(HW - 3.0f, 0, 10.5f, 1.3f);
        w.crate(HW - 3.0f, 1.3f, 10.5f, 0.8f);
        w.crate(-3.5f, 0, 14.5f, 0.9f);

        // low cover wall
        w.add({1.5f, 0, 15.0f}, {5.0f, 1.1f, 15.5f}, MAT_PLASTER);

        // raised platform with a step in the east corner
        w.add({HW - 3.2f, 0, 16.0f}, {HW, 0.9f, DEPTH}, MAT_STONE);
        w.add({HW - 3.8f, 0, 18.0f}, {HW - 3.2f, 0.45f, 20.0f}, MAT_STONE);

        // west arcade: a row of pillars
        for (float z = 5.0f; z < DEPTH - 3.f; z += 4.0f) {
            w.add({-HW + 0.8f, 0, z}, {-HW + 1.4f, 3.2f, z + 0.6f}, MAT_PLASTER);
            w.add({-HW + 0.7f, 3.2f, z - 0.1f}, {-HW + 1.5f, 3.45f, z + 0.7f}, MAT_STONE, FACE_ALL);
        }

        SWorld world;
        world.spawn          = {0.f, 0.f, 4.0f};
        world.bounds         = {{-HW, -1.f, FRONT}, {HW, 50.f, DEPTH}};
        world.desktop.center = {0.f, screen.centerY, FRONT + 0.01f};
        world.desktop.normal = {0, 0, 1};
        world.desktop.height = screen.height;

        const auto                                      samples = hemisphereSamples(24);
        std::map<std::tuple<int, int, int, int>, float> aoCache;

        for (const auto& b : w.boxes) {
            if (b.collide)
                world.collision.addBox(b.box);

            for (int axis = 0; axis < 3; ++axis) {
                for (int pos = 0; pos < 2; ++pos) {
                    const uint8_t bit = 1 << (axis * 2 + pos);
                    if (!(b.faces & bit))
                        continue;
                    emitFace(world.vertices, b, axis, pos == 1, w.boxes, samples, aoCache);
                }
            }
        }

        world.collision.build();
        return world;
    }
}
