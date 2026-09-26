#pragma once

#include "loader.hpp"
#include "map.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct cgltf_data;

// What loading maps and avatars have in common: opening a glTF file, reading
// its materials and decoding the textures they use.

namespace h3d::gltf {

    inline void check(const std::atomic<bool>& cancel) {
        if (cancel)
            throw SCancelled{};
    }

    std::string lower(std::string s);
    bool        readFile(const std::string& path, std::vector<uint8_t>& out);

    // a number from a file as an integer, held to lo..hi (NaN: the fallback). Casting a double an int can't hold is
    // undefined, and a file can say 1e999
    inline int fileInt(double v, int fallback, int lo = -(1 << 30), int hi = 1 << 30) {
        return v == v ? (int)std::clamp(v, (double)lo, (double)hi) : fallback;
    }
    // and as a float, finite (NaN and infinities: the fallback)
    inline float fileFloat(double v, float fallback) {
        return std::isfinite(v) ? (float)std::clamp(v, -1e30, 1e30) : fallback;
    }

    // runs fn(begin, end) over [0, n) on a few threads
    template <typename F>
    void parallelFor(size_t n, size_t chunk, const std::atomic<bool>& cancel, F&& fn) {
        const unsigned           threads = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
        std::atomic<size_t>      next    = 0;
        auto                     worker  = [&] {
            for (;;) {
                const size_t b = next.fetch_add(chunk);
                if (b >= n || cancel)
                    return;
                fn(b, std::min(n, b + chunk));
            }
        };
        std::vector<std::thread> pool;
        for (unsigned i = 1; i < threads; ++i)
            pool.emplace_back(worker);
        worker();
        for (auto& t : pool)
            t.join();
        check(cancel);
    }

    using DataPtr = std::unique_ptr<cgltf_data, void (*)(cgltf_data*)>;

    // parses `path` and loads its buffers; on failure returns null and says why
    // (`what` names the file in messages: "map", "avatar")
    DataPtr open(const std::string& path, const std::string& what, std::string& error);

    // the materials of a file in the renderer's terms. The images are only
    // described, decodeImages() fills in their pixels. Without surfaceMaps, normal
    // and metallic-roughness textures are left out (models without tangents).
    struct SMaterials {
        std::vector<SMapImage>    images;
        std::vector<SMapMaterial> materials;       // one per glTF material, then a default one
        std::vector<int>          imageSlot;       // glTF image -> index into images, -1 = unused
        std::vector<int>          baseUV;          // per material: which uv set its base color uses
        int                       defaultMaterial = 0;
    };
    SMaterials readMaterials(cgltf_data* data, bool surfaceMaps = true);

    // decodes every image in `images` (slots maps glTF images to them), on a few threads; with
    // `compress` (eTexCompression), block compresses what the GPU takes, with all their mip levels
    void decodeImages(cgltf_data* data, const std::string& dir, std::vector<SMapImage>& images, const std::vector<int>& slots, const std::atomic<bool>& cancel,
                      std::vector<std::string>& log, int compress = 0);

    // HYPR3D_lighting (see tools/cs2map.py): the lighting a game baked for the map, decoded. Returns
    // false when the file has none. out.skyImage is left as the file's image index.
    bool readLighting(cgltf_data* data, const std::string& dir, SMapLighting& out, const std::atomic<bool>& cancel, std::vector<std::string>& log);
}
