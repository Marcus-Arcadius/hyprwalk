#include "lipsync.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace h3d {

    namespace {
        constexpr float PI = std::numbers::pi_v<float>;

        // the vowels' first two formants (Hz): Japanese a, i, u, e, o, between a man's and a woman's, in VISEME order
        // (aa, ih, ou, ee, oh). Between Tokyo speakers' (Yazawa and Kondo 2019, the geometric mean of 8 men's and 8
        // women's) and what this code measures on 28 recordings of 11 speakers: u's second is higher than the older
        // tables have it (Tokyo's u is hardly rounded), e's first lower
        constexpr float VOWEL[VOWEL_COUNT][2] = {{800, 1380}, {325, 2460}, {375, 1560}, {490, 2120}, {520, 930}};
        constexpr float SIGMA[2]               = {0.32f, 0.26f}; // how far off each may be, octaves
    }

    // RBJ's cookbook biquads: a low-pass, a high-pass and a band-pass (0 dB at its peak) at fc Hz, for rate fs
    CLipSync::SBiquad CLipSync::biquad(int kind, float fc, float Q, float fs) {
        fc            = std::min(fc, 0.45f * fs);
        const float w = 2 * PI * fc / fs, c = std::cos(w), al = std::sin(w) / (2 * Q), a0 = 1 + al;
        if (kind == 0)
            return {(1 - c) / 2 / a0, (1 - c) / a0, (1 - c) / 2 / a0, -2 * c / a0, (1 - al) / a0, 0, 0};
        if (kind == 1)
            return {(1 + c) / 2 / a0, -(1 + c) / a0, (1 + c) / 2 / a0, -2 * c / a0, (1 - al) / a0, 0, 0};
        return {al / a0, 0, -al / a0, -2 * c / a0, (1 - al) / a0, 0, 0};
    }

    float CLipSync::SBiquad::run(float x) {
        const float y = b0 * x + z1;
        z1            = b1 * x - a1 * y + z2;
        z2            = b2 * x - a2 * y;
        return y;
    }

    void CLipSync::reset() {
        m_rate = 0;
        m_buf.clear();
        m_shape.fill(0);
        m_out.fill(0);
        m_level = -120.f;
        m_f1 = m_f2 = 0;
        m_unvoiced = 1;
        m_last     = {};
        m_windows  = 0;
        m_bands.clear();
        m_voice = -120.f;
        m_floor = 0.f;
        m_fric[0] = m_fric[1] = m_fric[2] = 0;
    }

    void CLipSync::setRate(int rate) {
        m_rate     = rate;
        m_decimate = std::max(1, (int)std::lround(rate / 11025.0)); // formants are below 4 kHz: about 11 kHz is plenty
        m_fs       = (float)rate / m_decimate;
        m_phase    = 0;
        // a fourth-order Butterworth low-pass before decimating (two biquads, RBJ's)
        const float fc = std::min(0.45f * m_fs, 5000.f);
        for (int k = 0; k < 2; ++k) {
            const float Q = k == 0 ? 0.5412f : 1.3066f, w = 2 * PI * fc / rate, c = std::cos(w), al = std::sin(w) / (2 * Q), a0 = 1 + al;
            m_lp[k]       = {(1 - c) / 2 / a0, (1 - c) / a0, (1 - c) / 2 / a0, -2 * c / a0, (1 - al) / a0, 0, 0};
        }
        m_size = (size_t)std::lround(0.03f * m_fs);
        m_hop  = m_size / 2;
        m_buf.clear();
        m_bands.clear();
        m_band[B_1K]   = biquad(1, 1000, 0.7071f, (float)rate);
        m_band[B_3K]   = biquad(2, 3000, 1.2f, (float)rate);
        m_band[B_6K]   = biquad(2, 6000, 0.9f, (float)rate);
        m_band[B_LOW]  = biquad(0, 500, 0.7071f, m_fs);
        m_acc[0] = m_acc[1] = m_acc[2] = 0;
    }

    void CLipSync::feed(const float* samples, size_t n, int rate) {
        if (rate <= 0)
            return;
        if (rate != m_rate)
            setRate(rate);
        for (size_t i = 0; i < n; ++i) {
            float x = samples[i];
            for (int b = B_1K; b <= B_6K; ++b) {
                const float y = m_band[b].run(x);
                m_acc[b] += y * y;
            }
            if (m_decimate > 1) {
                x = m_lp[1].run(m_lp[0].run(x));
                if (++m_phase < m_decimate)
                    continue;
                m_phase = 0;
            }
            m_buf.push_back(x);
            auto& e = m_bands.emplace_back();
            for (int b = B_1K; b <= B_6K; ++b) {
                e[b]     = m_acc[b];
                m_acc[b] = 0;
            }
            for (int b = B_LOW; b < BAND_COUNT; ++b) {
                const float y = m_band[b].run(x);
                e[b]          = y * y;
            }
            if (m_buf.size() >= m_size)
                window();
        }
    }

    float CLipSync::periodicity() const {
        // the normalised autocorrelation's highest peak over lags of a voice's pitch, 60 to 500 Hz: near 1 for a vowel,
        // low for noise
        const size_t N    = m_size;
        double       mean = 0;
        for (size_t i = 0; i < N; ++i)
            mean += m_buf[i];
        mean /= (double)N;
        const size_t lo = std::max<size_t>(2, (size_t)(m_fs / 500)), hi = std::min<size_t>(N / 2, (size_t)(m_fs / 60));
        double       best = 0;
        for (size_t lag = lo; lag <= hi; ++lag) {
            double xy = 0, xx = 0, yy = 0;
            for (size_t i = 0; i + lag < N; ++i) {
                const double a = m_buf[i] - mean, b = m_buf[i + lag] - mean;
                xy += a * b;
                xx += a * a;
                yy += b * b;
            }
            if (xx > 0 && yy > 0)
                best = std::max(best, xy / std::sqrt(xx * yy));
        }
        return (float)best;
    }

    void CLipSync::window() {
        const size_t N = m_size;
        const int    P = std::clamp((int)std::lround(m_fs / 1000.f) + 2, 10, 18); // the prediction's order
        double       sum2 = 0;
        for (size_t i = 0; i < N; ++i)
            sum2 += (double)m_buf[i] * m_buf[i];
        m_level        = 20.f * std::log10((float)std::sqrt(sum2 / N) + 1e-9f) + 3.01f; // a full scale sine: 0 dBFS
        const float op = std::clamp((m_level - gate) / (full - gate), 0.f, 1.f);

        // linear prediction of the window, pre-emphasised and Hamming windowed
        std::vector<double> x(N);
        for (size_t i = 0; i < N; ++i)
            x[i] = ((double)m_buf[i] - (i ? 0.97 * m_buf[i - 1] : 0.0)) * (0.54 - 0.46 * std::cos(2 * std::numbers::pi * i / (N - 1)));
        std::vector<double> R(P + 1, 0.0), a(P + 1, 0.0), t(P + 1);
        for (int k = 0; k <= P; ++k) {
            for (size_t i = k; i < N; ++i)
                R[k] += x[i] * x[i - k];
            const double bw = 2 * std::numbers::pi * 40.0 * k / m_fs; // widen the peaks a little: steadier
            R[k] *= std::exp(-0.5 * bw * bw);
        }
        R[0] *= 1.0001;
        bool voiced = false;
        m_last      = {m_level};
        if (R[0] > 1e-12) {
            // Levinson-Durbin
            double E = R[0];
            a[0]     = 1;
            for (int i = 1; i <= P; ++i) {
                double acc = R[i];
                for (int j = 1; j < i; ++j)
                    acc += a[j] * R[i - j];
                const double k = -acc / E;
                t              = a;
                for (int j = 1; j < i; ++j)
                    a[j] = t[j] + k * t[i - j];
                a[i] = k;
                E *= 1 - k * k;
                if (E <= 0)
                    break;
            }
            int crossings = 0;
            for (size_t i = 1; i < N; ++i)
                crossings += (m_buf[i] >= 0) != (m_buf[i - 1] >= 0);
            // voiced: with the formants' peaks well above the rest (a gain of the prediction over 6), or periodic
            // (as a vowel from a lossy recording is, or an u, whose one low peak barely rises from the rest), and not
            // as noisy as an s
            const double gain     = R[0] / std::max(E, 1e-300);
            const float  periodic = periodicity();
            voiced                = E > 0 && ((gain > 6.0 && crossings < (int)(N * 0.25)) || (periodic > 0.6f && crossings < (int)(N * 0.35)));
            m_last.gain           = (float)gain;
            m_last.crossings      = (float)crossings / (float)N;
            m_last.periodic       = periodic;
            if (voiced) {
                // the formants: the prediction polynomial's roots (Durand-Kerner), those narrower than 400 Hz above
                // 150 Hz, by frequency
                std::vector<std::complex<double>> z(P);
                for (int k = 0; k < P; ++k)
                    z[k] = std::pow(std::complex<double>(0.4, 0.9), k);
                for (int it = 0; it < 200; ++it) {
                    double moved = 0;
                    for (int k = 0; k < P; ++k) {
                        std::complex<double> num = 1.0, den = 1.0;
                        for (int j = 1; j <= P; ++j)
                            num = num * z[k] + a[j];
                        for (int j = 0; j < P; ++j)
                            if (j != k)
                                den *= z[k] - z[j];
                        const std::complex<double> step = std::abs(den) > 1e-300 ? num / den : 0.0;
                        z[k] -= step;
                        moved = std::max(moved, std::abs(step));
                    }
                    if (moved < 1e-9)
                        break;
                }
                std::vector<float> fs;
                for (const auto& r : z) {
                    const double ang = std::arg(r), mag = std::abs(r);
                    if (ang <= 0 || mag <= 0 || mag >= 1)
                        continue;
                    const double hz = ang * m_fs / (2 * std::numbers::pi), bw = -std::log(mag) * m_fs / std::numbers::pi;
                    if (hz > 150 && hz < m_fs / 2 - 100 && bw < 400)
                        fs.push_back((float)hz);
                }
                std::ranges::sort(fs);
                const float F1 = fs.size() > 0 ? fs[0] : 0, F2 = fs.size() > 1 ? fs[1] : 0;
                voiced = F1 > 0 && F2 > 0;
                if (voiced) {
                    m_f1 = F1;
                    m_f2 = F2;
                    SVisemes p{};
                    float    total = 0;
                    for (int v = 0; v < VOWEL_COUNT; ++v) {
                        const float d1 = std::log2(F1 / VOWEL[v][0]) / SIGMA[0], d2 = std::log2(F2 / VOWEL[v][1]) / SIGMA[1];
                        p[v]           = std::exp(-0.5f * (d1 * d1 + d2 * d2));
                        total += p[v];
                    }
                    for (int v = 0; v < VOWEL_COUNT; ++v)
                        m_shape[v] = total > 1e-6f ? p[v] / total : 0.2f;
                }
            }
        }
        m_last.voiced = voiced;
        if (voiced) {
            m_last.f1    = m_f1;
            m_last.f2    = m_f2;
            m_last.shape = m_shape;
        }
        // for the consonants: the window's energy by band
        {
            double e[BAND_COUNT] = {};
            for (size_t i = 0; i < N; ++i)
                for (int b = 0; b < BAND_COUNT; ++b)
                    e[b] += m_bands[i][b];
            const double above = std::max(e[B_1K], 1e-20);
            m_last.mid         = (float)(e[B_3K] / above);
            m_last.high        = (float)(e[B_6K] / above);
            m_last.low         = (float)(e[B_LOW] / std::max(sum2, 1e-20));
            m_voice = std::max(m_voice - 6.f * (m_hop / m_fs), voiced ? m_level : -120.f); // (it forgets 6 dB a second)
            m_floor = std::min(m_floor + 1.f * (m_hop / m_fs), m_level);                  // (it rises 1 dB a second)
            m_last.under = m_voice - m_level;
        }
        // the consonant, where it stands out of the noise between words: noise mostly above 1 kHz is a fricative's, by
        // its bands an s (little around 3 kHz: its peak is higher), an sh (much around 3 kHz) or an f (as much here as
        // there: flat); and a voiced window with its energy low, a low first formant and well under the voice a
        // murmur's (an m; or the voice fading out, as the lips close)
        int        cons = -1;
        const bool out  = m_level > gate && m_level > m_floor + 10.f;
        if (out && !voiced && m_last.high + m_last.mid > 0.6f) {
            // (by its windows so far, from its second: its edges pass through the others' bands, and its first
            // window has some of the vowel)
            m_fric[0] += m_last.mid;
            m_fric[1] += m_last.high;
            m_fric[2] += 1;
            const float mid = m_fric[0] / m_fric[2];
            cons            = m_fric[2] < 2 ? -1 : mid < 0.3f ? V_SS : mid > 0.45f ? V_CH : V_FF;
        } else {
            m_fric[0] = m_fric[1] = m_fric[2] = 0;
            if (out && voiced && m_last.low > 0.85f && m_f1 < 400.f && m_last.under > 8.f)
                cons = V_PP;
        }
        m_last.consonant = cons;
        ++m_windows;
        // a consonant: the last vowel's shape, not as open, for as long as a consonant lasts. Noise that goes on (a
        // hiss, a fan, breath) shuts it
        const float dt   = m_hop / m_fs;
        m_unvoiced       = voiced ? 0.f : m_unvoiced + dt;
        const float open = voiced ? op : op * 0.3f * std::max(0.f, 1.f - m_unvoiced / 0.2f);
        // (a consonant as clearly as the voice around it is loud: none in noise with no voice before it, and noise that
        // goes on shuts it too)
        const float strength = std::clamp((m_voice - gate) / (full - gate), 0.f, 1.f) * std::max(0.f, 1.f - m_unvoiced / 0.25f);
        for (int c = VOWEL_COUNT; c < VISEME_COUNT; ++c) {
            const float target = c == cons ? strength : 0.f;
            const float tau    = target > m_out[c] ? 0.03f : 0.06f;
            m_out[c] += (target - m_out[c]) * (1.f - std::exp(-dt / tau));
        }
        for (int v = 0; v < VOWEL_COUNT; ++v) {
            const float target = open * m_shape[v];
            const float tau    = target > m_out[v] ? 0.04f : 0.09f;
            m_out[v] += (target - m_out[v]) * (1.f - std::exp(-dt / tau));
        }
        m_buf.erase(m_buf.begin(), m_buf.begin() + (ptrdiff_t)m_hop);
        m_bands.erase(m_bands.begin(), m_bands.begin() + (ptrdiff_t)m_hop);
    }
}
