#pragma once

#include <algorithm>
#include <cmath>

namespace h3d {

    struct V3 {
        float x = 0, y = 0, z = 0;

        constexpr V3() = default;
        constexpr V3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

        constexpr V3 operator+(const V3& o) const {
            return {x + o.x, y + o.y, z + o.z};
        }
        constexpr V3 operator-(const V3& o) const {
            return {x - o.x, y - o.y, z - o.z};
        }
        constexpr V3 operator*(float s) const {
            return {x * s, y * s, z * s};
        }
        constexpr V3 operator/(float s) const {
            return {x / s, y / s, z / s};
        }
        constexpr V3 operator-() const {
            return {-x, -y, -z};
        }
        V3& operator+=(const V3& o) {
            x += o.x, y += o.y, z += o.z;
            return *this;
        }
        V3& operator-=(const V3& o) {
            x -= o.x, y -= o.y, z -= o.z;
            return *this;
        }
        V3& operator*=(float s) {
            x *= s, y *= s, z *= s;
            return *this;
        }
    };

    constexpr float dot(const V3& a, const V3& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    constexpr V3 cross(const V3& a, const V3& b) {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }

    inline float length(const V3& v) {
        return std::sqrt(dot(v, v));
    }

    inline V3 normalize(const V3& v) {
        const float l = length(v);
        return l > 1e-8f ? v / l : V3{0, 0, 0};
    }

    constexpr V3 lerp(const V3& a, const V3& b, float t) {
        return a + (b - a) * t;
    }

    constexpr float lerpf(float a, float b, float t) {
        return a + (b - a) * t;
    }

    constexpr float smoothstep01(float t) {
        t = std::clamp(t, 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    inline V3 vmin(const V3& a, const V3& b) {
        return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
    }

    inline V3 vmax(const V3& a, const V3& b) {
        return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
    }

    // any unit vector perpendicular to n
    inline V3 perpendicular(const V3& n) {
        return normalize(std::abs(n.y) < 0.9f ? cross(n, V3{0, 1, 0}) : cross(n, V3{1, 0, 0}));
    }

    struct Quat {
        float x = 0, y = 0, z = 0, w = 1;

        static Quat axisAngle(const V3& axis, float a) {
            const V3    n = normalize(axis);
            const float s = std::sin(a * 0.5f);
            return {n.x * s, n.y * s, n.z * s, std::cos(a * 0.5f)};
        }

        // rotation whose local x, y, z axes end up at r, u, n (an orthonormal, right handed basis)
        static Quat fromBasis(const V3& r, const V3& u, const V3& n) {
            const float tr = r.x + u.y + n.z;
            Quat        q;
            if (tr > 0.f) {
                const float s = std::sqrt(tr + 1.f) * 2.f;
                q             = {(u.z - n.y) / s, (n.x - r.z) / s, (r.y - u.x) / s, 0.25f * s};
            } else if (r.x > u.y && r.x > n.z) {
                const float s = std::sqrt(1.f + r.x - u.y - n.z) * 2.f;
                q             = {0.25f * s, (u.x + r.y) / s, (n.x + r.z) / s, (u.z - n.y) / s};
            } else if (u.y > n.z) {
                const float s = std::sqrt(1.f + u.y - r.x - n.z) * 2.f;
                q             = {(u.x + r.y) / s, 0.25f * s, (n.y + u.z) / s, (n.x - r.z) / s};
            } else {
                const float s = std::sqrt(1.f + n.z - r.x - u.y) * 2.f;
                q             = {(n.x + r.z) / s, (n.y + u.z) / s, 0.25f * s, (r.y - u.x) / s};
            }
            return q.normalized();
        }

        Quat normalized() const {
            const float l = std::sqrt(x * x + y * y + z * z + w * w);
            return l > 1e-12f ? Quat{x / l, y / l, z / l, w / l} : Quat{};
        }

        Quat conj() const {
            return {-x, -y, -z, w};
        }

        Quat operator*(const Quat& o) const {
            return {w * o.x + x * o.w + y * o.z - z * o.y, w * o.y - x * o.z + y * o.w + z * o.x, w * o.z + x * o.y - y * o.x + z * o.w,
                    w * o.w - x * o.x - y * o.y - z * o.z};
        }

        V3 rotate(const V3& v) const {
            const V3 q{x, y, z};
            const V3 t = cross(q, v) * 2.f;
            return v + t * w + cross(q, t);
        }
    };

    inline Quat slerp(Quat a, Quat b, float t) {
        float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        if (d < 0.f) {
            b = {-b.x, -b.y, -b.z, -b.w};
            d = -d;
        }
        if (d > 0.9995f)
            return Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t}.normalized();
        const float th = std::acos(std::clamp(d, -1.f, 1.f));
        const float s  = std::sin(th);
        const float wa = std::sin((1.f - t) * th) / s;
        const float wb = std::sin(t * th) / s;
        return Quat{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb}.normalized();
    }

    // column-major 4x4, m[col * 4 + row], matches GL's expectations
    struct M4 {
        float m[16] = {};

        static M4 identity() {
            M4 r;
            r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f;
            return r;
        }

        static M4 perspective(float fovyRad, float aspect, float zNear, float zFar) {
            const float f = 1.f / std::tan(fovyRad * 0.5f);
            M4          r;
            r.m[0]  = f / aspect;
            r.m[5]  = f;
            r.m[10] = (zFar + zNear) / (zNear - zFar);
            r.m[11] = -1.f;
            r.m[14] = (2.f * zFar * zNear) / (zNear - zFar);
            return r;
        }

        static M4 ortho(float l, float r_, float b, float t, float n, float f) {
            M4 r = identity();
            r.m[0]  = 2.f / (r_ - l);
            r.m[5]  = 2.f / (t - b);
            r.m[10] = -2.f / (f - n);
            r.m[12] = -(r_ + l) / (r_ - l);
            r.m[13] = -(t + b) / (t - b);
            r.m[14] = -(f + n) / (f - n);
            return r;
        }

        static M4 lookAt(const V3& eye, const V3& center, const V3& upHint) {
            const V3 f = normalize(center - eye);
            const V3 s = normalize(cross(f, upHint));
            const V3 u = cross(s, f);
            M4       r = identity();
            r.m[0]     = s.x;
            r.m[4]     = s.y;
            r.m[8]     = s.z;
            r.m[1]     = u.x;
            r.m[5]     = u.y;
            r.m[9]     = u.z;
            r.m[2]     = -f.x;
            r.m[6]     = -f.y;
            r.m[10]    = -f.z;
            r.m[12]    = -dot(s, eye);
            r.m[13]    = -dot(u, eye);
            r.m[14]    = dot(f, eye);
            return r;
        }

        static M4 translation(const V3& t) {
            M4 r    = identity();
            r.m[12] = t.x;
            r.m[13] = t.y;
            r.m[14] = t.z;
            return r;
        }

        static M4 trs(const V3& t, const Quat& q, const V3& s) {
            const V3 x = q.rotate({1, 0, 0}) * s.x, y = q.rotate({0, 1, 0}) * s.y, z = q.rotate({0, 0, 1}) * s.z;
            M4       r;
            r.m[0]  = x.x;
            r.m[1]  = x.y;
            r.m[2]  = x.z;
            r.m[4]  = y.x;
            r.m[5]  = y.y;
            r.m[6]  = y.z;
            r.m[8]  = z.x;
            r.m[9]  = z.y;
            r.m[10] = z.z;
            r.m[12] = t.x;
            r.m[13] = t.y;
            r.m[14] = t.z;
            r.m[15] = 1.f;
            return r;
        }

        V3 point(const V3& p) const {
            return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13], m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
        }

        V3 dir(const V3& d) const {
            return {m[0] * d.x + m[4] * d.y + m[8] * d.z, m[1] * d.x + m[5] * d.y + m[9] * d.z, m[2] * d.x + m[6] * d.y + m[10] * d.z};
        }

        M4 transposed() const {
            M4 r;
            for (int c = 0; c < 4; ++c)
                for (int rr = 0; rr < 4; ++rr)
                    r.m[c * 4 + rr] = m[rr * 4 + c];
            return r;
        }

        float det3() const {
            return m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
        }

        M4 operator*(const M4& o) const {
            M4 r;
            for (int c = 0; c < 4; ++c) {
                for (int rr = 0; rr < 4; ++rr) {
                    float s = 0;
                    for (int k = 0; k < 4; ++k)
                        s += m[k * 4 + rr] * o.m[c * 4 + k];
                    r.m[c * 4 + rr] = s;
                }
            }
            return r;
        }

        // general inverse (cofactor expansion)
        M4 inverse() const {
            const float* a = m;
            M4           r;
            float*       inv = r.m;

            inv[0]  = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
            inv[4]  = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
            inv[8]  = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
            inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
            inv[1]  = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
            inv[5]  = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
            inv[9]  = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
            inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
            inv[2]  = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
            inv[6]  = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
            inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
            inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
            inv[3]  = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
            inv[7]  = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
            inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
            inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];

            float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
            if (std::abs(det) < 1e-12f)
                return identity();
            det = 1.f / det;
            for (float& v : r.m)
                v *= det;
            return r;
        }
    };

    struct SAABB {
        V3 min, max;

        bool contains(const V3& p) const {
            return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z;
        }
        bool overlaps(const SAABB& o) const {
            return min.x <= o.max.x && max.x >= o.min.x && min.y <= o.max.y && max.y >= o.min.y && min.z <= o.max.z && max.z >= o.min.z;
        }
        V3 center() const {
            return (min + max) * 0.5f;
        }
        V3 size() const {
            return max - min;
        }
        void grow(const V3& p) {
            min = vmin(min, p);
            max = vmax(max, p);
        }
        static SAABB empty() {
            return {{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
        }
    };

    // slab test, returns distance along the ray or a negative value on miss
    inline float rayAABB(const V3& o, const V3& d, const SAABB& b) {
        float tmin = -1e30f, tmax = 1e30f;
        const float* oo = &o.x;
        const float* dd = &d.x;
        const float* bmin = &b.min.x;
        const float* bmax = &b.max.x;
        for (int i = 0; i < 3; ++i) {
            if (std::abs(dd[i]) < 1e-9f) {
                if (oo[i] < bmin[i] || oo[i] > bmax[i])
                    return -1.f;
                continue;
            }
            float t1 = (bmin[i] - oo[i]) / dd[i];
            float t2 = (bmax[i] - oo[i]) / dd[i];
            if (t1 > t2)
                std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax)
                return -1.f;
        }
        if (tmax < 0)
            return -1.f;
        return tmin >= 0 ? tmin : tmax;
    }
}
