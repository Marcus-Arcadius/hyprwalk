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

// glTF loading shared by maps and avatars: opening a file, reading its materials and decoding their textures.

namespace hyprwalk::gltf {

    inline void check(const std::atomic<bool>& cancel) {
        if (cancel)
            throw SCancelled{};
    }

    std::string lower(std::string s);
    bool        readFile(const std::string& path, std::vector<uint8_t>& out);

    // a file's number as an int clamped to lo..hi, NaN -> fallback (casting an out-of-range double to int is UB)
    inline int fileInt(double v, int fallback, int lo = -(1 << 30), int hi = 1 << 30) {
        return v == v ? (int)std::clamp(v, (double)lo, (double)hi) : fallback;
    }
    // as a finite float; NaN and infinities -> fallback
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

    // parses `path` and loads its buffers; null on failure, with `error` set (`what` names the file: "map", "avatar")
    DataPtr open(const std::string& path, const std::string& what, std::string& error);
    // the same for a built-in GLB in memory, which can't reference external buffers
    DataPtr openMemory(const void* bytes, size_t size, const std::string& what, std::string& error);

    // a file's materials in the renderer's terms; decodeImages() fills in the images' pixels. Without surfaceMaps,
    // normal and metallic-roughness textures are skipped (models without tangents)
    struct SMaterials {
        std::vector<SMapImage>    images;
        std::vector<SMapMaterial> materials;       // one per glTF material, then a default one
        std::vector<int>          imageSlot;       // glTF image -> index into images, -1 = unused
        std::vector<int>          baseUV;          // per material: which uv set its base color uses
        int                       defaultMaterial = 0;
    };
    SMaterials readMaterials(cgltf_data* data, bool surfaceMaps = true);

    // decodes `images` on a few threads (`slots` maps glTF images to them); with `compress` (eTexCompression), block
    // compresses what the GPU supports, with all mip levels
    void decodeImages(cgltf_data* data, const std::string& dir, std::vector<SMapImage>& images, const std::vector<int>& slots, const std::atomic<bool>& cancel,
                      std::vector<std::string>& log, int compress = 0);

    // decodes HYPRWALK_lighting (tools/cs2map.py); false if the file has none. out.skyImage stays a file image index
    bool readLighting(cgltf_data* data, const std::string& dir, SMapLighting& out, const std::atomic<bool>& cancel, std::vector<std::string>& log);
}
