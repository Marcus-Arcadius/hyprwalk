#pragma once

#include <memory>
#include <string>
#include <vector>

namespace h3d {

    // The default microphone, through PipeWire, for lip sync: while it's on, its samples (mono floats) wait for the main
    // thread in a ring of a quarter of a second, and nothing is written anywhere or sent. PipeWire shows it as the
    // "hypr3d lip sync" stream. Built without PipeWire, start() says so.
    class CMicrophone {
      public:
        CMicrophone();
        ~CMicrophone();
        CMicrophone(const CMicrophone&)            = delete;
        CMicrophone& operator=(const CMicrophone&) = delete;

        static bool available(); // built with PipeWire
        bool        start(std::string& error);
        void        stop();
        bool        on() const;
        void        read(std::vector<float>& out, int& rate); // what came since the last read, appended; its rate

      private:
        struct SImpl;
        std::unique_ptr<SImpl> m;
    };
}
