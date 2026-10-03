#pragma once

#include "sound.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace h3d {

    // what the speaker is doing
    struct SSpeakerStatus {
        bool        on = false;     // a sound is playing, or fading out
        std::string name, file;     // what for (the emote), and the sound's file
        std::string stream;         // the stream's state: "connecting", "paused", "streaming", "error", "unconnected"
        std::string error;          // why it's in error, "" = it isn't
        double      at       = -1;  // clock(): seconds heard, -1 = none
        double      position = 0;   // where in the sound it is (seconds; going round again, from 0)
        double      start    = 0;   // where in it the first heard came (seconds): where the dance was by then
        double      duration = 0;
        double      latency  = 0;   // seconds from a frame given PipeWire till it's heard
        bool        loop     = false;
        float       volume   = 0;
        uint64_t    frames   = 0;   // given PipeWire since play()
    };

    // Plays an emote's sound (a dance's song) through PipeWire, to the default output, as the "hypr3d emote sound"
    // stream (the mixer shows the emote's name): one at a time, once or over and over. It starts where the dance is by
    // the time it's heard, and from then on clock() says how far into it is heard, for the dance to keep time with it
    // (a dropped frame, or a slow one, puts the dance behind the song otherwise). Built without PipeWire, play() says so.
    class CSpeaker {
      public:
        CSpeaker();
        ~CSpeaker();
        CSpeaker(const CSpeaker&)            = delete;
        CSpeaker& operator=(const CSpeaker&) = delete;

        static bool available(); // built with PipeWire
        // plays it from `from` seconds in (a looping one goes round), at volume 0..1; one playing stops at once
        bool play(std::shared_ptr<const SSound> sound, bool loop, float volume, const std::string& name, std::string& error, double from = 0);
        void stop(float fade = 0.3f); // fades out over fade seconds; update() then closes the stream
        void stopNow();               // closes it now
        void setVolume(float volume);
        void update(); // every frame: closes the stream once it's faded out or played to its end
        bool on() const;
        // seconds into the sound heard now: from where play() started it, going on round a looping one. -1 = none: not
        // heard yet (the stream hasn't started), played to its end, fading out, or in error
        double         clock() const;
        SSpeakerStatus status() const;

      private:
        struct SImpl;
        std::unique_ptr<SImpl> m;
    };
}
