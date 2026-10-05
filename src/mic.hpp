#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hyprwalk {

    // what the microphone is doing, to tell a muted or unlinked one from a quiet one
    struct SMicStatus {
        bool        on = false;
        std::string stream;         // stream state: "streaming", "paused", "error", ...
        std::string error;          // why it's in error, "" = it isn't
        std::string coreError;      // last non-fatal PipeWire core error
        bool        linked = false; // a link feeds it from a source
        // the source it's linked to, id 0 = none
        uint32_t    sourceId = 0;
        std::string sourceName, sourceDescription, sourceNick; // node.name, node.description, node.nick
        std::string sourceState;    // PipeWire node state: "running", "idle", ...
        std::string sourceError;
        int         muted  = -1;    // PipeWire mute (not the device's): 1, 0, -1 unknown
        float       volume = -1;    // linear (1 = unchanged), -1 unknown
        // received audio
        uint64_t samples   = 0;     // since start()
        uint64_t buffers = 0, emptyBuffers = 0; // PipeWire buffers; empty = silence it made up
        double   silentFor = 0;     // trailing exact zeros, s (muted devices send them)
        double   sinceData = -1;    // seconds since the last buffer, -1 = none yet
        float    peak = -200, rms = -200; // last second, dBFS (full-scale sine: 0); -200 none
        double   age  = 0;          // seconds since start()
        std::string target;         // the source asked for (node.name), "" = the default
        // available sources: (node.name, node.description)
        std::vector<std::pair<std::string, std::string>> sources;
    };

    // The microphone via PipeWire for lip sync (the "hyprwalk lip sync" stream): mono float samples wait for the main
    // thread in a quarter-second ring, and nothing is recorded or sent. Also watches the graph for its link and mute
    // state. Built without PipeWire, start() says so.
    class CMicrophone {
      public:
        CMicrophone();
        ~CMicrophone();
        CMicrophone(const CMicrophone&)            = delete;
        CMicrophone& operator=(const CMicrophone&) = delete;

        static bool available(); // built with PipeWire
        // target: a source's node.name, description or nick (as wpctl status lists them), "" = default; WirePlumber
        // links the default when it's missing
        bool        start(std::string& error, const std::string& target = "");
        void        stop();
        bool        on() const;
        void        read(std::vector<float>& out, int& rate); // appends samples since the last read; sets rate
        SMicStatus  status() const;

      private:
        struct SImpl;
        std::unique_ptr<SImpl> m;
    };
}
