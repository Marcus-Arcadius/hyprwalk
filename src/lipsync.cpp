#include "lipsync.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace h3d {

    namespace {
        constexpr float PI = std::numbers::pi_v<float>;

        // the vowels' first two formants (Hz): Japanese a, i, u, e, o, between a man's and a woman's (the geometric
        // mean of the usual measurements), in VISEME order (aa, ih, ou, ee, oh)
        constexpr float VOWEL[VISEME_COUNT][2] = {{820, 1320}, {325, 2460}, {375, 1440}, {520, 2060}, {530, 920}};
        constexpr float SIGMA[2]               = {0.32f, 0.26f}; // how far off each may be, octaves
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
    }

    void CLipSync::feed(const float* samples, size_t n, int rate) {
        if (rate <= 0)
            return;
        if (rate != m_rate)
            setRate(rate);
        for (size_t i = 0; i < n; ++i) {
            float x = samples[i];
            if (m_decimate > 1) {
                x = m_lp[1].run(m_lp[0].run(x));
                if (++m_phase < m_decimate)
                    continue;
                m_phase = 0;
            }
            m_buf.push_back(x);
            if (m_buf.size() >= m_size)
                window();
        }
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
            const double gain = R[0] / std::max(E, 1e-300);
            voiced            = E > 0 && gain > 6.0 && crossings < (int)(N * 0.25);
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
                    for (int v = 0; v < VISEME_COUNT; ++v) {
                        const float d1 = std::log2(F1 / VOWEL[v][0]) / SIGMA[0], d2 = std::log2(F2 / VOWEL[v][1]) / SIGMA[1];
                        p[v]           = std::exp(-0.5f * (d1 * d1 + d2 * d2));
                        total += p[v];
                    }
                    for (int v = 0; v < VISEME_COUNT; ++v)
                        m_shape[v] = total > 1e-6f ? p[v] / total : 0.2f;
                }
            }
        }
        // a consonant, or noise: the last vowel's shape, not as open
        const float open = voiced ? op : op * 0.3f;
        const float dt   = m_hop / m_fs;
        for (int v = 0; v < VISEME_COUNT; ++v) {
            const float target = open * m_shape[v];
            const float tau    = target > m_out[v] ? 0.04f : 0.09f;
            m_out[v] += (target - m_out[v]) * (1.f - std::exp(-dt / tau));
        }
        m_buf.erase(m_buf.begin(), m_buf.begin() + (ptrdiff_t)m_hop);
    }
}
