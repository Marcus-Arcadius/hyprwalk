#include "speaker.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <mutex>

#ifdef H3D_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#endif

namespace h3d {

#ifdef H3D_PIPEWIRE
    namespace {
        using Clock = std::chrono::steady_clock;

        constexpr float RAMP = 0.02f; // gain ramp, s: no clicks on start or volume change
    }

    struct CSpeaker::SImpl {
        pw_thread_loop*               loop    = nullptr;
        pw_context*                   context = nullptr;
        pw_core*                      core    = nullptr;
        pw_stream*                    stream  = nullptr;
        spa_hook                      coreHook{}, streamHook{};
        std::shared_ptr<const SSound> sound; // held so the stream's thread never frees it
        int                           rate = 48000, channels = 2; // the stream's, as the sound's

        // the rest the stream's thread and the main thread share
        mutable std::mutex mutex;
        const SSound*      playing = nullptr;
        std::string        name;
        bool               looping = false, started = false, fading = false, done = false;
        Clock::time_point  asked;     // when play() was called
        // heardBy: when the last of a finished or faded sound is heard, so closing doesn't cut it short; giveUp: close
        // then regardless (a stream no longer asked for data never fades)
        Clock::time_point  heardBy, giveUp = Clock::time_point::max();
        double             from = 0;  // start position, seconds
        uint64_t           given = 0; // next frame on the sound's timeline, counting loops
        uint64_t           first = 0; // the first given
        float              gain = 0, volume = 1, fadeStep = 0;
        // clock: frames given before and after the last buffer, when that buffer was asked for, and its latency
        uint64_t           clockFrames = 0, clockEnd = 0;
        Clock::time_point  clockAt;
        double             latency   = 0;
        mutable double     lastClock = -1; // never goes back
        std::string        streamState = "unconnected", streamError;

        static void onProcess(void* data) {
            auto*      m = (SImpl*)data;
            pw_buffer* b = pw_stream_dequeue_buffer(m->stream);
            if (!b)
                return;
            spa_data&      d      = b->buffer->datas[0];
            const int      ch     = m->channels;
            const uint32_t stride = sizeof(float) * ch;
            auto*          out    = (float*)d.data;
            uint32_t       n      = d.maxsize / stride;
            if (b->requested)
                n = std::min<uint32_t>(n, (uint32_t)b->requested);
            if (!out || !d.chunk) {
                pw_stream_queue_buffer(m->stream, b);
                return;
            }
            // latency of what's given now: the graph's delay to the device plus what's queued before it
            pw_time t{};
            double  late = 0;
            if (pw_stream_get_time_n(m->stream, &t, sizeof(t)) == 0 && t.rate.denom > 0)
                late = std::clamp((double)t.delay * t.rate.num / t.rate.denom + (double)(t.queued + t.buffered) / m->rate, 0.0, 1.0);
            const auto      now = Clock::now();
            std::lock_guard lock(m->mutex);
            const SSound*   s      = m->playing;
            const size_t    frames = s ? s->frames() : 0;
            if (!m->started && !m->done) {
                // start where the dance will be when this is heard, so the song comes in with it
                m->started      = true;
                const double at = m->from + std::chrono::duration<double>(now - m->asked).count() + late;
                m->given        = (uint64_t)std::llround(std::max(0.0, at) * m->rate);
                m->first        = m->given;
            }
            m->clockFrames  = m->given;
            m->clockAt      = now;
            m->latency      = late;
            size_t      pos = frames ? (m->looping ? m->given % frames : m->given) : 0;
            const float ramp = 1.f / (RAMP * m->rate);
            const bool  wasDone = m->done;
            uint32_t    sounded = 0;
            for (uint32_t i = 0; i < n; ++i) {
                float* o = out + (size_t)i * ch;
                if (!m->done && pos >= frames) {
                    if (m->looping && frames)
                        pos = 0;
                    else
                        m->done = true;
                }
                if (!m->done && m->fading) {
                    m->gain -= m->fadeStep;
                    if (m->gain <= 0) {
                        m->gain = 0;
                        m->done = true;
                    }
                }
                if (m->done) {
                    std::fill(o, o + ch, 0.f);
                    continue;
                }
                if (!m->fading)
                    m->gain = m->gain < m->volume ? std::min(m->volume, m->gain + ramp) : std::max(m->volume, m->gain - ramp);
                const int16_t* f = s->samples.data() + pos * ch;
                const float    k = m->gain / 32768.f;
                for (int c = 0; c < ch; ++c)
                    o[c] = f[c] * k;
                ++pos;
                ++m->given;
                ++sounded;
            }
            if (m->done && !wasDone)
                m->heardBy = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(late + (double)sounded / m->rate));
            m->clockEnd     = m->given;
            d.chunk->offset = 0;
            d.chunk->stride = stride;
            d.chunk->size   = n * stride;
            b->size         = n;
            pw_stream_queue_buffer(m->stream, b);
        }

        static void onStreamState(void* data, pw_stream_state, pw_stream_state state, const char* error) {
            auto*           m = (SImpl*)data;
            std::lock_guard lock(m->mutex);
            m->streamState = pw_stream_state_as_string(state);
            if (state == PW_STREAM_STATE_ERROR)
                m->streamError = error ? error : "PipeWire error";
        }

        static const pw_stream_events& streamEvents() {
            static const pw_stream_events e = [] {
                pw_stream_events x{};
                x.version       = PW_VERSION_STREAM_EVENTS;
                x.state_changed = &onStreamState;
                x.process       = &onProcess;
                return x;
            }();
            return e;
        }

        static void onCoreError(void* data, uint32_t id, int, int res, const char* message) {
            auto* m = (SImpl*)data;
            if (id != PW_ID_CORE || res != -EPIPE)
                return; // the stream's errors arrive as its state
            std::lock_guard lock(m->mutex);
            m->streamState = "error";
            m->streamError = "PipeWire went away";
        }

        static const pw_core_events& coreEvents() {
            static const pw_core_events e = [] {
                pw_core_events x{};
                x.version = PW_VERSION_CORE_EVENTS;
                x.error   = &onCoreError;
                return x;
            }();
            return e;
        }
    };

    bool CSpeaker::available() {
        return true;
    }

    bool CSpeaker::play(std::shared_ptr<const SSound> sound, bool loop, float volume, const std::string& name, std::string& error, double from) {
        stopNow();
        if (!sound || !sound->frames() || sound->channels < 1 || sound->channels > 2 || sound->rate <= 0) {
            error = "nothing to play";
            return false;
        }
        static std::once_flag init;
        std::call_once(init, [] { pw_init(nullptr, nullptr); });
        m->sound    = std::move(sound);
        m->rate     = m->sound->rate;
        m->channels = m->sound->channels;
        {
            std::lock_guard lock(m->mutex);
            m->playing = m->sound.get();
            m->name    = name;
            m->looping = loop;
            m->started = m->fading = m->done = false;
            m->asked                         = Clock::now();
            m->heardBy                       = m->asked;
            m->giveUp                        = Clock::time_point::max();
            m->from                          = std::isfinite(from) ? std::max(0.0, from) : 0.0;
            m->given = m->first = m->clockFrames = m->clockEnd = 0;
            m->latency                              = 0;
            m->lastClock                            = -1;
            m->gain = m->fadeStep = 0;
            m->volume                = std::clamp(volume, 0.f, 1.f);
            m->streamState           = "connecting";
            m->streamError.clear();
        }
        m->loop = pw_thread_loop_new("hypr3d-emote-sound", nullptr);
        if (!m->loop || pw_thread_loop_start(m->loop) != 0) {
            error = "couldn't start a PipeWire thread";
            stopNow();
            return false;
        }
        pw_thread_loop_lock(m->loop);
        m->context = pw_context_new(pw_thread_loop_get_loop(m->loop), nullptr, 0);
        m->core    = m->context ? pw_context_connect(m->context, nullptr, 0) : nullptr;
        bool ok    = m->core != nullptr;
        if (ok) {
            pw_core_add_listener(m->core, &m->coreHook, &SImpl::coreEvents(), m.get());
            // (the mixer shows the emote's name)
            pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Game", PW_KEY_APP_NAME, "hypr3d",
                                                     PW_KEY_NODE_NAME, "hypr3d-emote-sound", PW_KEY_NODE_DESCRIPTION, "hypr3d emote sound", PW_KEY_MEDIA_NAME,
                                                     name.empty() ? "emote" : name.c_str(), nullptr);
            m->stream = pw_stream_new(m->core, "hypr3d emote sound", props);
            ok        = m->stream != nullptr;
        }
        if (ok) {
            pw_stream_add_listener(m->stream, &m->streamHook, &SImpl::streamEvents(), m.get());
            uint8_t            buffer[1024];
            spa_pod_builder    b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
            spa_audio_info_raw info{};
            info.format   = SPA_AUDIO_FORMAT_F32;
            info.rate     = (uint32_t)m->rate;
            info.channels = (uint32_t)m->channels;
            if (m->channels == 1)
                info.position[0] = SPA_AUDIO_CHANNEL_MONO;
            else {
                info.position[0] = SPA_AUDIO_CHANNEL_FL;
                info.position[1] = SPA_AUDIO_CHANNEL_FR;
            }
            const spa_pod* params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
            ok = pw_stream_connect(m->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                                   (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), params, 1) == 0;
        }
        pw_thread_loop_unlock(m->loop);
        if (!ok) {
            error = "couldn't play it through PipeWire";
            stopNow();
            return false;
        }
        return true;
    }

    void CSpeaker::stop(float fade) {
        if (!m->loop)
            return;
        const auto      now = Clock::now();
        std::lock_guard lock(m->mutex);
        if (m->done)
            return;
        if (!m->started || !(fade > 0)) {
            m->done    = true;
            m->heardBy = now;
        } else if (!m->fading) {
            m->fading   = true;
            m->fadeStep = std::max(m->gain, 1e-4f) / std::max(1.f, fade * m->rate);
            m->giveUp   = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(fade + 1.0));
        }
    }

    void CSpeaker::stopNow() {
        if (m->loop)
            pw_thread_loop_stop(m->loop);
        // the loop's thread is stopped and the stream's goes with the stream: nothing else touches these
        if (m->stream) {
            spa_hook_remove(&m->streamHook);
            pw_stream_destroy(m->stream);
        }
        if (m->core) {
            spa_hook_remove(&m->coreHook);
            pw_core_disconnect(m->core);
        }
        if (m->context)
            pw_context_destroy(m->context);
        if (m->loop)
            pw_thread_loop_destroy(m->loop);
        m->streamHook = m->coreHook = {};
        m->stream                   = nullptr;
        m->core                     = nullptr;
        m->context                  = nullptr;
        m->loop                     = nullptr;
        {
            std::lock_guard lock(m->mutex);
            m->playing = nullptr;
            m->started = m->fading = m->done = false;
            m->streamState                   = m->streamError.empty() ? "unconnected" : "error"; // the error stays till the next
        }
        m->sound.reset();
    }

    void CSpeaker::setVolume(float volume) {
        std::lock_guard lock(m->mutex);
        m->volume = std::clamp(volume, 0.f, 1.f);
    }

    void CSpeaker::update() {
        if (!m->loop)
            return;
        bool over;
        {
            const auto      now = Clock::now();
            std::lock_guard lock(m->mutex);
            over = (m->done && now >= m->heardBy) || now >= m->giveUp || m->streamState == "error";
        }
        if (over)
            stopNow();
    }

    bool CSpeaker::on() const {
        return m->loop != nullptr;
    }

    double CSpeaker::clock() const {
        std::lock_guard lock(m->mutex);
        if (!m->playing || !m->started || m->done || m->fading || m->streamState == "error")
            return -1;
        // frames given before the last buffer are heard after its latency, then time runs on, capped at what has been
        // given (a late buffer holds it) and never going back
        const double since = std::chrono::duration<double>(Clock::now() - m->clockAt).count();
        const double t     = std::min((double)m->clockFrames / m->rate - m->latency + since, (double)m->clockEnd / m->rate - m->latency);
        m->lastClock       = std::max(m->lastClock, t);
        return m->lastClock;
    }

    SSpeakerStatus CSpeaker::status() const {
        SSpeakerStatus s;
        s.at = clock();
        std::lock_guard lock(m->mutex);
        s.on     = m->loop != nullptr;
        s.stream = m->streamState;
        s.error  = m->streamError;
        if (!m->playing)
            return s;
        const size_t frames = m->playing->frames();
        s.name              = m->name;
        s.file              = std::filesystem::path(m->playing->file).filename().string();
        s.duration          = m->playing->duration();
        s.position          = frames ? (double)(m->looping ? m->given % frames : std::min<uint64_t>(m->given, frames)) / m->rate : 0;
        s.start             = m->started ? (double)m->first / m->rate : 0;
        s.latency           = m->latency;
        s.loop              = m->looping;
        s.volume            = m->volume;
        s.frames            = m->started ? m->given - m->first : 0;
        return s;
    }
#else
    struct CSpeaker::SImpl {};

    bool CSpeaker::available() {
        return false;
    }

    bool CSpeaker::play(std::shared_ptr<const SSound>, bool, float, const std::string&, std::string& error, double) {
        error = "hypr3d was built without PipeWire (build.sh finds the one that runs)";
        return false;
    }

    void CSpeaker::stop(float) {}

    void CSpeaker::stopNow() {}

    void CSpeaker::setVolume(float) {}

    void CSpeaker::update() {}

    bool CSpeaker::on() const {
        return false;
    }

    double CSpeaker::clock() const {
        return -1;
    }

    SSpeakerStatus CSpeaker::status() const {
        return {};
    }
#endif

    CSpeaker::CSpeaker() : m(std::make_unique<SImpl>()) {}

    CSpeaker::~CSpeaker() {
        stopNow();
    }
}
