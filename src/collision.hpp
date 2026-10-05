#pragma once

#include "math3d.hpp"

#include <cstdint>
#include <vector>

namespace hyprwalk {

    struct SRayHit {
        float t = 0;
        V3    normal; // facing the ray origin
    };

    // static collision geometry: boxes (the built-in courtyard) plus triangles (loaded maps) in a BVH
    class CCollision {
      public:
        void  clear();
        void  addBox(const SAABB& b);
        void  addTriangle(const V3& a, const V3& b, const V3& c);
        void  build();

        bool  overlaps(const SAABB& box) const;
        bool  raycast(const V3& origin, const V3& dir, float maxT, SRayHit& hit) const;
        // every hit along a ray, nearest first (used to find floors)
        void  raycastAll(const V3& origin, const V3& dir, float maxT, std::vector<SRayHit>& out) const;

        SAABB bounds() const {
            return m_bounds;
        }
        size_t triangleCount() const {
            return m_tris.size();
        }
        const std::vector<SAABB>& boxes() const {
            return m_boxes;
        }

      private:
        struct STri {
            V3 a, b, c;
        };
        struct SNode {
            SAABB    box;
            uint32_t first = 0, count = 0; // leaf when count > 0, else first = left child (right = first + 1)
        };

        std::vector<SAABB> m_boxes;
        std::vector<STri>  m_tris;
        std::vector<SNode> m_nodes;
        SAABB              m_bounds = SAABB::empty();

        void               buildNode(uint32_t node, uint32_t first, uint32_t count, std::vector<V3>& centroids, int depth);
        template <typename F>
        void forEachTriNear(const V3& o, const V3& d, float maxT, F&& fn) const;
    };
}
