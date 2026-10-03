#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace h3d {

    // an emote's sound ("sound" in the settings file): an Ogg Vorbis file decoded whole to interleaved 16-bit mono or
    // stereo, played by speaker.hpp
    struct SSound {
        std::string          file;
        int                  rate = 0, channels = 0;
        std::vector<int16_t> samples; // frames x channels

        size_t frames() const {
            return channels > 0 ? samples.size() / channels : 0;
        }
        double duration() const {
            return rate > 0 ? (double)frames() / rate : 0;
        }
    };

    // limits on sound length and file size, so a file can't make decoding take any amount of memory
    constexpr double SOUND_MAX_SECONDS = 600;
    constexpr size_t SOUND_MAX_BYTES   = 128u << 20;

    // an Ogg Vorbis file, decoded; null (and why) if it can't be
    std::shared_ptr<const SSound> loadSound(const std::string& file, std::string& error);
}
