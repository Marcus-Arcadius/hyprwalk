#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace h3d {

    // what the microphone is doing, to tell a muted or unlinked one from a quiet one
    struct SMicStatus {
        bool        on = false;
        std::string stream;         // the stream's state: "connecting", "paused", "streaming", "error", "unconnected"
        std::string error;          // why it's in error, "" = it isn't
        std::string coreError;      // the last error PipeWire gave the connection that didn't end it
        bool        linked = false; // a link feeds it from a source
        // the source it's linked to, id 0 = none
        uint32_t    sourceId = 0;
        std::string sourceName, sourceDescription, sourceNick; // node.name, node.description, node.nick
        std::string sourceState;    // PipeWire's: "running", "idle", "suspended", "creating", "error"
        std::string sourceError;
        int         muted  = -1;    // PipeWire's mute on the source (a device's own mute button isn't): 1, 0, -1 unknown
        float       volume = -1;    // its volume, linear (1 = as it comes), -1 unknown
        // what came
        uint64_t samples   = 0;     // since start()
        uint64_t buffers = 0, emptyBuffers = 0; // PipeWire's, and those it flagged empty (silence it made up itself)
        double   silentFor = 0;     // seconds of exact zeros to the last sample (a muted or silenced device sends them)
        double   sinceData = -1;    // seconds since the last buffer, -1 = none yet
        float    peak = -200, rms = -200; // the last whole second's, dBFS (a full scale sine: 0 both); -200 = nothing
        double   age  = 0;          // seconds since start()
        std::string target;         // the source asked for (node.name), "" = the default
        // the sources there are, to pick one from: node.name and node.description each
        std::vector<std::pair<std::string, std::string>> sources;
    };

    // The default microphone, through PipeWire, for lip sync: while it's on, its samples (mono floats) wait for the main
    // thread in a ring of a quarter of a second, and nothing is written anywhere or sent. PipeWire shows it as the
    // "hypr3d lip sync" stream. It watches PipeWire's graph too, for what it's linked to and whether that's muted.
    // Built without PipeWire, start() says so.
    class CMicrophone {
      public:
        CMicrophone();
        ~CMicrophone();
        CMicrophone(const CMicrophone&)            = delete;
        CMicrophone& operator=(const CMicrophone&) = delete;

        static bool available(); // built with PipeWire
        // target: a source's node.name, or its description or nick (as wpctl status lists it), "" = the default one. One
        // asked for and not there: WirePlumber links the default one instead
        bool        start(std::string& error, const std::string& target = "");
        void        stop();
        bool        on() const;
        void        read(std::vector<float>& out, int& rate); // what came since the last read, appended; its rate
        SMicStatus  status() const;

      private:
        struct SImpl;
        std::unique_ptr<SImpl> m;
    };
}
