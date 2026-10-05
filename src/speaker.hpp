#pragma once

#include "sound.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace hyprwalk {

    // what the speaker is doing
    struct SSpeakerStatus {
        bool        on = false;     // a sound is playing, or fading out
        std::string name, file;     // emote name, sound file
        std::string stream;         // stream state: "streaming", "paused", "error", ...
        std::string error;          // why it's in error, "" = it isn't
        double      at       = -1;  // clock(): seconds heard, -1 = none
        double      position = 0;   // seconds into the sound, from 0 again on each loop
        double      start    = 0;   // seconds in when first heard (the dance's time then)
        double      duration = 0;
        double      latency  = 0;   // seconds from a frame given PipeWire till it's heard
        bool        loop     = false;
        float       volume   = 0;
        uint64_t    frames   = 0;   // given PipeWire since play()
    };

    // Plays an emote's sound through PipeWire to the default output (the "hyprwalk emote sound" stream, named after the
    // emote), one at a time, once or looping. It starts where the dance will be when it's heard; clock() then gives the
    // heard position so the dance keeps time despite dropped or slow frames. Built without PipeWire, play() says so.
    class CSpeaker {
      public:
        CSpeaker();
        ~CSpeaker();
        CSpeaker(const CSpeaker&)            = delete;
        CSpeaker& operator=(const CSpeaker&) = delete;

        static bool available(); // built with PipeWire
        // plays from `from` seconds in (wrapping when looping) at volume 0..1, stopping any current sound at once
        bool play(std::shared_ptr<const SSound> sound, bool loop, float volume, const std::string& name, std::string& error, double from = 0);
        void stop(float fade = 0.3f); // fade-out seconds; update() then closes the stream
        void stopNow();               // closes it now
        void setVolume(float volume);
        void update(); // per frame: closes a faded or finished stream
        bool on() const;
        // seconds into the sound heard now (wrapping when looping); -1 if not heard yet, finished, fading or in error
        double         clock() const;
        SSpeakerStatus status() const;

      private:
        struct SImpl;
        std::unique_ptr<SImpl> m;
    };
}
