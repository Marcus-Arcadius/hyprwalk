#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <deque>
#include <optional>
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
    // m, or the voice going: the lips close) show as consonants. It keeps nothing but the window it is looking at, and
    // the voice's loudness lately.
    //
    // How loud is loud depends on the microphone: the mouth opens from GATE to FULL dBFS (-52, -20: a voice about
    // -12 dBFS at its loudest, as a microphone set up for speech gives it) raised by a gain. Automatic, the gain makes
    // up for a quieter microphone: the loud part of the voice lately (the 90th percentile of its voiced windows over
    // the last 15 s) is brought to -12 dBFS, never turned down, 50 dB at most. So a voice 40 dB quieter opens the
    // mouth as wide once it has said a syllable, while something quiet right after it (a whisper, the room) stays
    // shut. Nothing opens below -80 dBFS, nor in the noise between words (6 dB over the quietest lately), whatever
    // the gain.
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
            float    lo = GATE, hi = FULL; // the marks: shut below, wide open from (dBFS)
        };
        const SWindow& last() const {
            return m_last;
        }
        size_t windows() const {
            return m_windows;
        }
        // the gain in dB, none = automatic
        void                 setGain(std::optional<float> dB);
        std::optional<float> gainSetting() const {
            return m_gainSet;
        }
        float gain() const { // what it is now, dB
            return m_gain;
        }
        float reference() const { // the voice's loud part lately, dBFS (none heard yet: NOMINAL)
            return m_ref;
        }
        float noiseFloor() const { // the quietest lately, dBFS
            return m_floor;
        }
        float room() const { // the noise in the last pause, dBFS (NAN: none yet)
            return m_room;
        }
        std::array<float, 2> marks() const { // the last window's: shut below, wide open from (dBFS, as heard)
            return {m_lo, m_hi};
        }

        static constexpr float GATE = -52.f, FULL = -20.f; // dBFS, at a gain of 0
        static constexpr float NOMINAL = -12.f;            // the voice's loud part the automatic gain brings it to
        static constexpr float MAX_GAIN = 50.f, MIN_LEVEL = -80.f;
        static constexpr float HOLD     = 15.f; // seconds of voice the automatic gain goes by

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
        // the gain: set, or automatic from the voiced windows' levels (and when, in windows) over the last HOLD seconds
        std::optional<float>                       m_gainSet;
        std::deque<std::pair<size_t, float>>       m_heard;
        std::vector<float>                         m_sorted; // (scratch)
        float                                      m_ref = NOMINAL, m_gain = 0.f, m_lo = GATE, m_hi = FULL;
        float                                      m_room = NAN; // the noise in a pause, dBFS (NAN: none heard yet)
        std::array<std::pair<float, bool>, 32>     m_recent{};   // the last windows: level, voiced
        float                                      m_fric[3] = {0, 0, 0}; // this fricative so far: its windows' mid, high, count
        size_t             m_hop = 0, m_size = 0;
        float              m_level = -120.f, m_f1 = 0, m_f2 = 0;
        float              m_unvoiced = 1; // seconds since the last voiced window
        SVisemes           m_shape{}, m_out{};
        SWindow            m_last;
        size_t             m_windows = 0;

        void           setRate(int rate);
        void           window(); // analyse m_buf, then drop a hop of it
        void           levelMarks(bool voiced); // m_gain, m_lo and m_hi for this window
        float          periodicity() const;
        static SBiquad biquad(int kind, float fc, float Q, float fs); // 0 low-pass, 1 high-pass, 2 band-pass
    };
}
