#pragma once

#include <array>
#include <cstddef>
#include <vector>

namespace h3d {

    // VRChat's visemes lip sync drives: the vowels, as the avatar's mouth presets have them (aa, ih, ou, ee, oh: EX_AA
    // ... EX_OH), then the consonants that show, where the avatar has them: pp (lips shut: m, b, p), ff (lip on teeth: f,
    // v), ss (teeth: s, z, ts), ch (lips out: sh, ch, j)
    enum eViseme : int { V_AA, V_IH, V_OU, V_EE, V_OH, V_PP, V_FF, V_SS, V_CH };
    constexpr int                VOWEL_COUNT = 5, VISEME_COUNT = 9;
    inline constexpr const char* VISEME_NAMES[VISEME_COUNT] = {"aa", "ih", "ou", "ee", "oh", "pp", "ff", "ss", "ch"};
    using SVisemes                                          = std::array<float, VISEME_COUNT>;

    // Lip sync from a voice, on the fly: how loud it is opens the mouth, and the vowel's first two resonances (formants,
    // from linear prediction of a 30 ms window every 15 ms) say which shape: a, i, u, e or o. A fricative's noise, by
    // where it lies (around 6 kHz: s; around 3: sh; faint and flat: f), and a low, voiced murmur well under the voice (an
    // m, or the voice going: the lips close) show as consonants. It keeps nothing but the window it is looking at.
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
        // the last window, to look into (the harness's --lipsync-trace)
        struct SWindow {
            float    level = -120.f, gain = 0, crossings = 0, periodic = 0, f1 = 0, f2 = 0; // crossings: zero crossings a sample
            bool     voiced = false;
            SVisemes shape{};
            // for the consonants: of the energy above 1 kHz, the parts around 3 and 6 kHz; of it all, the part under
            // 500 Hz; how far under the voice lately (dB); the consonant (V_PP ... V_CH, -1: none)
            float    mid = 0, high = 0, low = 0, under = 0;
            int      consonant = -1;
        };
        const SWindow& last() const {
            return m_last;
        }
        size_t windows() const {
            return m_windows;
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
        // band energies for the consonants, per sample of m_buf: the input's above 1 kHz, around 3 kHz (sh) and around
        // 6 kHz (s), taken before decimating; m_buf's under 500 Hz (a murmur's)
        enum eBand { B_1K, B_3K, B_6K, B_LOW, BAND_COUNT };
        SBiquad                                    m_band[BAND_COUNT];
        float                                      m_acc[3] = {0, 0, 0}; // the input-rate bands since the last sample
        std::vector<std::array<float, BAND_COUNT>> m_bands;
        float                                      m_voice = -120.f; // how loud the voice has been lately, dBFS
        float                                      m_floor = 0.f;    // the quietest lately (the noise between words)
        float                                      m_fric[3] = {0, 0, 0}; // this fricative so far: its windows' mid, high, count
        size_t             m_hop = 0, m_size = 0;
        float              m_level = -120.f, m_f1 = 0, m_f2 = 0;
        float              m_unvoiced = 1; // seconds since the last voiced window
        SVisemes           m_shape{}, m_out{};
        SWindow            m_last;
        size_t             m_windows = 0;

        void           setRate(int rate);
        void           window(); // analyse m_buf, then drop a hop of it
        float          periodicity() const;
        static SBiquad biquad(int kind, float fc, float Q, float fs); // 0 low-pass, 1 high-pass, 2 band-pass
    };
}
