#pragma once

#include <array>
#include <cstddef>
#include <vector>

namespace h3d {

    // VRChat's visemes for the vowels, as the avatar's mouth presets have them: aa, ih, ou, ee, oh (EX_AA ... EX_OH)
    constexpr int VISEME_COUNT = 5;
    using SVisemes             = std::array<float, VISEME_COUNT>;

    // Lip sync from a voice, on the fly: how loud it is opens the mouth, and the vowel's first two resonances (formants,
    // from linear prediction of a 30 ms window every 15 ms) say which shape: a, i, u, e or o. It keeps nothing but the
    // window it is looking at.
    class CLipSync {
      public:
        void            feed(const float* samples, size_t n, int rate); // mono, -1..1
        void            reset();
        const SVisemes& visemes() const { // 0..1 each, together as open as the mouth is
            return m_out;
        }
        float level() const { // the last window's loudness, dBFS (-120: silence)
            return m_level;
        }
        float f1() const { // the last voiced window's formants, Hz (0: none)
            return m_f1;
        }
        float f2() const {
            return m_f2;
        }

        float gate = -52.f, full = -20.f; // dBFS: the mouth shut below gate, wide open from full

      private:
        struct SBiquad {
            float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
            float run(float x);
        };
        int                m_rate = 0, m_decimate = 1, m_phase = 0;
        float              m_fs   = 0; // after decimating
        SBiquad            m_lp[2];    // before decimating
        std::vector<float> m_buf;      // the analysis window's samples, at m_fs
        size_t             m_hop = 0, m_size = 0;
        float              m_level = -120.f, m_f1 = 0, m_f2 = 0;
        float              m_unvoiced = 1; // seconds since the last voiced window
        SVisemes           m_shape{}, m_out{};

        void setRate(int rate);
        void window(); // analyse m_buf, then drop a hop of it
    };
}
