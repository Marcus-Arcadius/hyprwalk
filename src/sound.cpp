#include "sound.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include "third_party/stb_vorbis.c"

namespace h3d {

    namespace {
        const char* vorbisError(int e) {
            switch (e) {
                case VORBIS_outofmem: return "out of memory";
                case VORBIS_unexpected_eof: return "it ends too soon";
                case VORBIS_invalid_setup:
                case VORBIS_invalid_stream:
                case VORBIS_invalid_first_page:
                case VORBIS_bad_packet_type:
                case VORBIS_ogg_skeleton_not_supported:
                case VORBIS_cant_find_last_page: return "it isn't an Ogg Vorbis file, or it's damaged";
                case VORBIS_missing_capture_pattern:
                case VORBIS_invalid_stream_structure_version: return "it isn't an Ogg file";
                case VORBIS_feature_not_supported: return "it uses a Vorbis feature that isn't supported (floor 0, from before 2004)";
                case VORBIS_too_many_channels: return "it has too many channels";
                case VORBIS_continued_packet_flag_invalid:
                case VORBIS_incorrect_stream_serial_number:
                case VORBIS_seek_failed:
                case VORBIS_seek_invalid: return "it's damaged";
            }
            return "it can't be decoded";
        }
    }

    std::shared_ptr<const SSound> loadSound(const std::string& file, std::string& error) {
        std::error_code ec;
        const auto      size = std::filesystem::file_size(file, ec);
        std::ifstream   f(file, std::ios::binary);
        if (ec || !f) {
            error = std::format("sound {} can't be read", file);
            return nullptr;
        }
        if (size > SOUND_MAX_BYTES || size == 0) {
            error = size ? std::format("sound {} is too big ({} MB)", file, size >> 20) : std::format("sound {} is empty", file);
            return nullptr;
        }
        std::vector<uint8_t> bytes((size_t)size);
        if (!f.read((char*)bytes.data(), (std::streamsize)bytes.size())) {
            error = std::format("sound {} can't be read", file);
            return nullptr;
        }
        int         e = 0;
        stb_vorbis* v = stb_vorbis_open_memory(bytes.data(), (int)bytes.size(), &e, nullptr);
        if (!v) {
            // (an Ogg file of Opus, say, rather than Vorbis)
            const bool opus = bytes.size() > 36 && std::string_view((const char*)bytes.data() + 28, 8) == "OpusHead";
            error = std::format("sound {}: {}", file, opus ? "it's Opus, and only Ogg Vorbis is played" : vorbisError(e));
            return nullptr;
        }
        auto                   s    = std::make_shared<SSound>();
        const stb_vorbis_info  info = stb_vorbis_get_info(v);
        s->file                     = file;
        s->rate                     = (int)info.sample_rate;
        s->channels                 = info.channels;
        if (s->rate < 8000 || s->rate > 192000 || s->channels < 1 || s->channels > 2) {
            error = s->channels > 2 ? std::format("sound {} has {} channels, and only mono and stereo are played", file, s->channels)
                                    : std::format("sound {}: a rate of {} Hz can't be played", file, s->rate);
            stb_vorbis_close(v);
            return nullptr;
        }
        const size_t most = (size_t)(SOUND_MAX_SECONDS * s->rate);
        if (const unsigned n = stb_vorbis_stream_length_in_samples(v); n > 0)
            s->samples.reserve(std::min<size_t>(n, most) * s->channels);
        int16_t buffer[4096 * 2];
        for (;;) {
            const int n = stb_vorbis_get_samples_short_interleaved(v, s->channels, buffer, 4096 * s->channels);
            if (n <= 0)
                break;
            s->samples.insert(s->samples.end(), buffer, buffer + (size_t)n * s->channels);
            if (s->frames() > most) {
                error = std::format("sound {} is longer than {:.0f} minutes", file, SOUND_MAX_SECONDS / 60);
                stb_vorbis_close(v);
                return nullptr;
            }
        }
        stb_vorbis_close(v);
        if (s->samples.empty()) {
            error = std::format("sound {} is empty", file);
            return nullptr;
        }
        s->samples.shrink_to_fit();
        return s;
    }
}
