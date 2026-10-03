#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace h3d {

    // A sound an emote plays (a dance's song, "sound" in the settings file): an Ogg Vorbis file decoded whole, 16 bits
    // a sample, interleaved, mono or stereo. The speaker (speaker.hpp) plays it
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

    // the longest a sound can be, and its file's biggest (what decoding one would take otherwise is a file's say)
    constexpr double SOUND_MAX_SECONDS = 600;
    constexpr size_t SOUND_MAX_BYTES   = 128u << 20;

    // an Ogg Vorbis file, decoded; null (and why) if it can't be
    std::shared_ptr<const SSound> loadSound(const std::string& file, std::string& error);
}
