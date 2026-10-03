// sound_test: emote sounds on their own for sound_check.sh: src/sound.cpp's decoding and src/speaker.cpp's PipeWire
// playback (to the PipeWire PIPEWIRE_REMOTE names).
//
//   sound_test decode FILE OUT    print FILE's rate, channels and frames (or the error); write its s16 samples to OUT
//   sound_test play FILE [--loop] [--from S] [--volume V] [--for S] [--fade S] [--wait S] [--log FILE]
//                                 once heard (clock() >= 0), play --for seconds (2), stop with --fade (0.3) and wait
//                                 for the stream to close, or stop after --wait seconds unheard (10). --log gets a line
//                                 every 5 ms: time, clock(), stream state, position, start, latency, frames. Prints the
//                                 last status as JSON and the timings; exit status 0 when it was heard and closed
#include "sound.hpp"
#include "speaker.hpp"

#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>
#include <string>
#include <thread>

using namespace h3d;

namespace {
    using Clock = std::chrono::steady_clock;

    int usage() {
        std::fputs("usage: sound_test decode FILE OUT | play FILE [--loop] [--from S] [--volume V] [--for S] [--fade S] [--wait S] [--log FILE]\n", stderr);
        return 2;
    }

    std::string json(const SSpeakerStatus& s) {
        return std::format(R"({{"on": {}, "name": "{}", "file": "{}", "stream": "{}", "error": "{}", "at": {:.6f}, "position": {:.6f}, "start": {:.9f}, )"
                           R"("duration": {:.4f}, "latency": {:.4f}, "loop": {}, "volume": {:.3f}, "frames": {}}})",
                           s.on, s.name, s.file, s.stream, s.error, s.at, s.position, s.start, s.duration, s.latency, s.loop, s.volume, s.frames);
    }
}

int main(int argc, char** argv) {
    if (argc < 3)
        return usage();
    const std::string mode = argv[1], file = argv[2];
    std::string       error;
    const auto        sound = loadSound(file, error);
    if (mode == "decode") {
        if (argc != 4)
            return usage();
        if (!sound) {
            std::printf("error: %s\n", error.c_str());
            return 1;
        }
        std::ofstream out(argv[3], std::ios::binary);
        out.write((const char*)sound->samples.data(), (std::streamsize)(sound->samples.size() * sizeof(int16_t)));
        std::printf("rate %d channels %d frames %zu\n", sound->rate, sound->channels, sound->frames());
        return out ? 0 : 1;
    }
    if (mode != "play")
        return usage();
    bool        loop = false;
    double      from = 0, length = 2, fade = 0.3, wait = 10;
    float       volume = 1;
    std::string logFile;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--loop")
            loop = true;
        else if (i + 1 < argc && a == "--from")
            from = std::stod(argv[++i]);
        else if (i + 1 < argc && a == "--volume")
            volume = std::stof(argv[++i]);
        else if (i + 1 < argc && a == "--for")
            length = std::stod(argv[++i]);
        else if (i + 1 < argc && a == "--fade")
            fade = std::stod(argv[++i]);
        else if (i + 1 < argc && a == "--wait")
            wait = std::stod(argv[++i]);
        else if (i + 1 < argc && a == "--log")
            logFile = argv[++i];
        else
            return usage();
    }
    if (!sound) {
        std::printf("error: %s\n", error.c_str());
        return 1;
    }
    std::FILE* log = logFile.empty() ? nullptr : std::fopen(logFile.c_str(), "w");
    CSpeaker   speaker;
    if (!speaker.play(sound, loop, volume, "sound_test", error, from)) {
        std::printf("error: %s\n", error.c_str());
        return 1;
    }
    const auto start = Clock::now();
    auto       since = [&] { return std::chrono::duration<double>(Clock::now() - start).count(); };
    double     heard = -1, stopped = -1;
    SSpeakerStatus last;
    while (speaker.on() && since() < 60) {
        speaker.update();
        const double         t = since();
        const SSpeakerStatus s = speaker.status();
        if (log)
            std::fprintf(log, "%.6f %.6f %s %.6f %.9f %.6f %llu\n", t, s.at, s.stream.c_str(), s.position, s.start, s.latency, (unsigned long long)s.frames);
        if (heard < 0 && s.at >= 0)
            heard = t;
        if (stopped < 0 && s.on)
            last = s; // status at the stop, or at the sound's end
        if (stopped < 0 && ((heard >= 0 && t - heard >= length) || (heard < 0 && t > wait))) {
            stopped = t;
            speaker.stop((float)fade);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (log)
        std::fclose(log);
    std::printf("%s\n", json(last).c_str());
    std::printf("heard after %.4f s, stopped at %.4f s, closed at %.4f s\n", heard, stopped, since());
    return heard >= 0 && !speaker.on() ? 0 : 1;
}
