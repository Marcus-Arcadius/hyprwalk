#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

namespace hyprwalk {

    // VRChat visemes: the vowels as the avatar's mouth presets (EX_AA .. EX_OH), then the consonants that show, where
    // the avatar has them: pp (m, b, p), ff (f, v), ss (s, z, ts), ch (sh, ch, j)
    enum eViseme : int { V_AA, V_IH, V_OU, V_EE, V_OH, V_PP, V_FF, V_SS, V_CH };
    constexpr int                VOWEL_COUNT = 5, VISEME_COUNT = 9;
    inline constexpr const char* VISEME_NAMES[VISEME_COUNT] = {"aa", "ih", "ou", "ee", "oh", "pp", "ff", "ss", "ch"};
    using SVisemes                                          = std::array<float, VISEME_COUNT>;

    // Real-time lip sync: loudness opens the mouth and the first two formants (LPC over a 30 ms window every 15 ms)
    // pick the vowel; fricative noise by band (6 kHz: s, 3 kHz: sh, flat: f) or a low murmur under the voice (m, the
    // lips closing) picks a consonant.
    //
    // The mouth opens from GATE to FULL dBFS plus a gain. The automatic gain lifts the voice's loud part (90th
    // percentile of voiced windows over HOLD seconds) to NOMINAL, by at most MAX_GAIN and never down: a quiet
    // microphone works, a whisper after speech stays shut. Nothing opens below MIN_LEVEL or the noise floor + 6 dB.
    class CLipSync {
      public:
        void            feed(const float* samples, size_t n, int rate); // mono, -1..1
        void            reset();
        const SVisemes& visemes() const { // 0..1 each, summing to the mouth's openness
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
        // last window's analysis (the harness's --lipsync-trace)
        struct SWindow {
            float    level = -120.f, gain = 0, crossings = 0, periodic = 0, f1 = 0, f2 = 0; // crossings: zero crossings a sample
            bool     voiced = false;
            SVisemes shape{};
            // consonants: mid and high are the 3 and 6 kHz shares of the energy above 1 kHz, low the share under 500
            // Hz, under the dB below the recent voice; consonant is V_PP .. V_CH, -1 = none
            float    mid = 0, high = 0, low = 0, under = 0;
            int      consonant = -1;
            float    lo = GATE, hi = FULL; // shut below lo, wide open from hi (dBFS)
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
        float gain() const { // current gain, dB
            return m_gain;
        }
        float reference() const { // recent loud voice level, dBFS (NOMINAL until heard)
            return m_ref;
        }
        float noiseFloor() const { // the quietest lately, dBFS
            return m_floor;
        }
        float room() const { // the noise in the last pause, dBFS (NAN: none yet)
            return m_room;
        }
        std::array<float, 2> marks() const { // last window's shut/open marks, dBFS as heard
            return {m_lo, m_hi};
        }

        static constexpr float GATE = -52.f, FULL = -20.f; // dBFS, at a gain of 0
        static constexpr float NOMINAL = -12.f;            // target for the voice's loud part (automatic gain)
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
        // consonant band energies per m_buf sample: above 1 kHz, ~3 kHz (sh), ~6 kHz (s) from the input before
        // decimating; under 500 Hz (murmur) from m_buf
        enum eBand { B_1K, B_3K, B_6K, B_LOW, BAND_COUNT };
        SBiquad                                    m_band[BAND_COUNT];
        float                                      m_acc[3] = {0, 0, 0}; // the input-rate bands since the last sample
        std::vector<std::array<float, BAND_COUNT>> m_bands;
        float                                      m_voice = -120.f; // recent voice level, dBFS
        float                                      m_floor = 0.f;    // recent quietest level (noise between words)
        // gain: set, or automatic from the voiced windows' (index, level) over the last HOLD seconds
        std::optional<float>                       m_gainSet;
        std::deque<std::pair<size_t, float>>       m_heard;
        std::vector<float>                         m_sorted; // (scratch)
        float                                      m_ref = NOMINAL, m_gain = 0.f, m_lo = GATE, m_hi = FULL;
        float                                      m_room = NAN; // the noise in a pause, dBFS (NAN: none heard yet)
        std::array<std::pair<float, bool>, 32>     m_recent{};   // the last windows: level, voiced
        float                                      m_fric[3] = {0, 0, 0}; // current fricative: summed mid, high; window count
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
