#include "collision.hpp"

#include <algorithm>
#include <numeric>

namespace hyprwalk {

    namespace {
        // separating axis test between a triangle and a box (center c, half size h)
        bool triBox(const V3& c, const V3& h, V3 v0, V3 v1, V3 v2) {
            v0 -= c;
            v1 -= c;
            v2 -= c;

            // box face normals, i.e. the triangle's bounding box against the box
            if (std::min({v0.x, v1.x, v2.x}) > h.x || std::max({v0.x, v1.x, v2.x}) < -h.x)
                return false;
            if (std::min({v0.y, v1.y, v2.y}) > h.y || std::max({v0.y, v1.y, v2.y}) < -h.y)
                return false;
            if (std::min({v0.z, v1.z, v2.z}) > h.z || std::max({v0.z, v1.z, v2.z}) < -h.z)
                return false;

            const V3 e[3] = {v1 - v0, v2 - v1, v0 - v2};

            // triangle normal
            const V3    n = cross(e[0], e[1]);
            const float d = dot(n, v0);
            if (std::abs(d) > h.x * std::abs(n.x) + h.y * std::abs(n.y) + h.z * std::abs(n.z))
                return false;

            // cross products of the edges with the box axes
            for (const V3& ed : e) {
                const V3 axes[3] = {{0, -ed.z, ed.y}, {ed.z, 0, -ed.x}, {-ed.y, ed.x, 0}};
                for (const V3& a : axes) {
                    const float p0 = dot(v0, a), p1 = dot(v1, a), p2 = dot(v2, a);
                    const float r  = h.x * std::abs(a.x) + h.y * std::abs(a.y) + h.z * std::abs(a.z);
                    if (std::min({p0, p1, p2}) > r || std::max({p0, p1, p2}) < -r)
                        return false;
                }
            }
            return true;
        }

        // slab test against precomputed 1/dir, returns the entry distance (or > maxT on a miss)
        float slab(const V3& o, const V3& inv, const SAABB& b, float maxT) {
            float t1 = (b.min.x - o.x) * inv.x, t2 = (b.max.x - o.x) * inv.x;
            float lo = std::min(t1, t2), hi = std::max(t1, t2);
            t1 = (b.min.y - o.y) * inv.y;
            t2 = (b.max.y - o.y) * inv.y;
            lo = std::max(lo, std::min(t1, t2));
            hi = std::min(hi, std::max(t1, t2));
            t1 = (b.min.z - o.z) * inv.z;
            t2 = (b.max.z - o.z) * inv.z;
            lo = std::max(lo, std::min(t1, t2));
            hi = std::min(hi, std::max(t1, t2));
            if (hi < std::max(lo, 0.f) || lo > maxT)
                return 1e30f;
            return std::max(lo, 0.f);
        }

        // Möller-Trumbore, both sides
        bool rayTri(const V3& o, const V3& d, const V3& a, const V3& b, const V3& c, float& t) {
            const V3    e1 = b - a, e2 = c - a;
            const V3    p   = cross(d, e2);
            const float det = dot(e1, p);
            if (std::abs(det) < 1e-12f)
                return false;
            const float inv = 1.f / det;
            const V3    s   = o - a;
            const float u   = dot(s, p) * inv;
            if (u < 0.f || u > 1.f)
                return false;
            const V3    q = cross(s, e1);
            const float v = dot(d, q) * inv;
            if (v < 0.f || u + v > 1.f)
                return false;
            t = dot(e2, q) * inv;
            return t > 1e-5f;
        }

        // ray against a box, with the normal of the face it enters through
        bool rayBox(const V3& o, const V3& d, const SAABB& b, float& t, V3& n) {
            float       tmin = -1e30f, tmax = 1e30f;
            int         axis = -1;
            float       sign = 0;
            const float* oo = &o.x;
            const float* dd = &d.x;
            const float* mn = &b.min.x;
            const float* mx = &b.max.x;
            for (int i = 0; i < 3; ++i) {
                if (std::abs(dd[i]) < 1e-12f) {
                    if (oo[i] < mn[i] || oo[i] > mx[i])
                        return false;
                    continue;
                }
                float t1 = (mn[i] - oo[i]) / dd[i], t2 = (mx[i] - oo[i]) / dd[i];
                float s  = -1.f;
                if (t1 > t2) {
                    std::swap(t1, t2);
                    s = 1.f;
                }
                if (t1 > tmin) {
                    tmin = t1;
                    axis = i;
                    sign = s;
                }
                tmax = std::min(tmax, t2);
                if (tmin > tmax)
                    return false;
            }
            if (tmin < 0.f || axis < 0)
                return false; // starting inside: ignore, like back faces
            t              = tmin;
            n              = {0, 0, 0};
            (&n.x)[axis]   = sign;
            return true;
        }
    }

    void CCollision::clear() {
        m_boxes.clear();
        m_tris.clear();
        m_nodes.clear();
        m_bounds = SAABB::empty();
    }

    void CCollision::addBox(const SAABB& b) {
        m_boxes.push_back(b);
        m_bounds.grow(b.min);
        m_bounds.grow(b.max);
    }

    void CCollision::addTriangle(const V3& a, const V3& b, const V3& c) {
        if (length(cross(b - a, c - a)) < 1e-10f)
            return;
        m_tris.push_back({a, b, c});
        m_bounds.grow(a);
        m_bounds.grow(b);
        m_bounds.grow(c);
    }

    void CCollision::build() {
        m_nodes.clear();
        if (m_tris.empty())
            return;
        std::vector<V3> centroids(m_tris.size());
        for (size_t i = 0; i < m_tris.size(); ++i)
            centroids[i] = (m_tris[i].a + m_tris[i].b + m_tris[i].c) / 3.f;
        m_nodes.reserve(m_tris.size() / 2 + 1);
        m_nodes.emplace_back();
        buildNode(0, 0, (uint32_t)m_tris.size(), centroids, 0);
        // up to half of both is growth slack (de_dust2: ~190 MB)
        m_tris.shrink_to_fit();
        m_nodes.shrink_to_fit();
    }

    void CCollision::buildNode(uint32_t node, uint32_t first, uint32_t count, std::vector<V3>& centroids, int depth) {
        SAABB box = SAABB::empty(), cbox = SAABB::empty();
        for (uint32_t i = first; i < first + count; ++i) {
            box.grow(m_tris[i].a);
            box.grow(m_tris[i].b);
            box.grow(m_tris[i].c);
            cbox.grow(centroids[i]);
        }
        m_nodes[node].box = box;

        const V3 ext = cbox.size();
        if (count <= 4 || depth > 48 || std::max({ext.x, ext.y, ext.z}) < 1e-6f) {
            m_nodes[node].first = first;
            m_nodes[node].count = count;
            return;
        }

        // median split along the longest axis of the centroids
        const int axis = ext.x > ext.y && ext.x > ext.z ? 0 : (ext.y > ext.z ? 1 : 2);
        const uint32_t mid = first + count / 2;
        std::vector<uint32_t> idx(count);
        std::iota(idx.begin(), idx.end(), first);
        std::nth_element(idx.begin(), idx.begin() + count / 2, idx.end(), [&](uint32_t a, uint32_t b) { return (&centroids[a].x)[axis] < (&centroids[b].x)[axis]; });
        std::vector<STri> tris(count);
        std::vector<V3>   cents(count);
        for (uint32_t i = 0; i < count; ++i) {
            tris[i]  = m_tris[idx[i]];
            cents[i] = centroids[idx[i]];
        }
        std::ranges::copy(tris, m_tris.begin() + first);
        std::ranges::copy(cents, centroids.begin() + first);

        const uint32_t left = (uint32_t)m_nodes.size();
        m_nodes.emplace_back();
        m_nodes.emplace_back();
        m_nodes[node].first = left;
        m_nodes[node].count = 0;
        buildNode(left, first, mid - first, centroids, depth + 1);
        buildNode(left + 1, mid, first + count - mid, centroids, depth + 1);
    }

    bool CCollision::overlaps(const SAABB& box) const {
        for (const auto& b : m_boxes) {
            if (box.min.x < b.max.x - 1e-4f && box.max.x > b.min.x + 1e-4f && box.min.y < b.max.y - 1e-4f && box.max.y > b.min.y + 1e-4f && box.min.z < b.max.z - 1e-4f &&
                box.max.z > b.min.z + 1e-4f)
                return true;
        }
        if (m_nodes.empty())
            return false;

        // touching isn't overlapping, so standing on a floor doesn't count
        const V3 c = box.center();
        const V3 h = box.size() * 0.5f - V3{1e-4f, 1e-4f, 1e-4f};
        const SAABB q{c - h, c + h};

        uint32_t stack[128];
        int      sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const SNode& n = m_nodes[stack[--sp]];
            if (!n.box.overlaps(q))
                continue;
            if (n.count > 0) {
                for (uint32_t i = n.first; i < n.first + n.count; ++i)
                    if (triBox(c, h, m_tris[i].a, m_tris[i].b, m_tris[i].c))
                        return true;
            } else if (sp < 126) {
                stack[sp++] = n.first;
                stack[sp++] = n.first + 1;
            }
        }
        return false;
    }

    template <typename F>
    void CCollision::forEachTriNear(const V3& o, const V3& d, float maxT, F&& fn) const {
        if (m_nodes.empty())
            return;
        const V3 inv{1.f / (std::abs(d.x) > 1e-12f ? d.x : 1e-12f), 1.f / (std::abs(d.y) > 1e-12f ? d.y : 1e-12f), 1.f / (std::abs(d.z) > 1e-12f ? d.z : 1e-12f)};
        uint32_t stack[128];
        int      sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const SNode& n = m_nodes[stack[--sp]];
            // fn returns the current search distance, so far subtrees get skipped
            if (slab(o, inv, n.box, maxT) > maxT)
                continue;
            if (n.count > 0) {
                for (uint32_t i = n.first; i < n.first + n.count; ++i)
                    maxT = fn(m_tris[i], maxT);
            } else if (sp < 126) {
                stack[sp++] = n.first;
                stack[sp++] = n.first + 1;
            }
        }
    }

    bool CCollision::raycast(const V3& o, const V3& d, float maxT, SRayHit& hit) const {
        bool found = false;
        for (const auto& b : m_boxes) {
            float t = 0;
            V3    n;
            if (rayBox(o, d, b, t, n) && t < maxT) {
                maxT       = t;
                hit.t      = t;
                hit.normal = n;
                found      = true;
            }
        }
        forEachTriNear(o, d, maxT, [&](const STri& tri, float best) {
            float t = 0;
            if (rayTri(o, d, tri.a, tri.b, tri.c, t) && t < best) {
                V3 n = normalize(cross(tri.b - tri.a, tri.c - tri.a));
                if (dot(n, d) > 0.f)
                    n = -n;
                hit.t      = t;
                hit.normal = n;
                found      = true;
                return t;
            }
            return best;
        });
        return found;
    }

    void CCollision::raycastAll(const V3& o, const V3& d, float maxT, std::vector<SRayHit>& out) const {
        out.clear();
        for (const auto& b : m_boxes) {
            float t = 0;
            V3    n;
            if (rayBox(o, d, b, t, n) && t < maxT)
                out.push_back({t, n});
        }
        forEachTriNear(o, d, maxT, [&](const STri& tri, float best) {
            float t = 0;
            if (rayTri(o, d, tri.a, tri.b, tri.c, t)) {
                V3 n = normalize(cross(tri.b - tri.a, tri.c - tri.a));
                if (dot(n, d) > 0.f)
                    n = -n;
                out.push_back({t, n});
            }
            return best;
        });
        std::ranges::sort(out, [](const SRayHit& a, const SRayHit& b) { return a.t < b.t; });
    }
}
