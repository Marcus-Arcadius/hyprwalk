#include "mic.hpp"

#include <mutex>

#ifdef H3D_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#endif

namespace h3d {

#ifdef H3D_PIPEWIRE
    struct CMicrophone::SImpl {
        pw_thread_loop*    loop   = nullptr;
        pw_stream*         stream = nullptr;
        std::mutex         mutex;
        std::vector<float> ring; // the last quarter second at most
        size_t             head = 0, count = 0;
        int                rate = 48000;

        static void onParam(void* data, uint32_t id, const spa_pod* param) {
            auto* m = (SImpl*)data;
            if (!param || id != SPA_PARAM_Format)
                return;
            spa_audio_info_raw info{};
            if (spa_format_audio_raw_parse(param, &info) >= 0 && info.rate > 0) {
                std::lock_guard lock(m->mutex);
                m->rate = (int)info.rate;
                m->ring.assign((size_t)info.rate / 4, 0.f);
                m->head = m->count = 0;
            }
        }

        static void onProcess(void* data) {
            auto*      m = (SImpl*)data;
            pw_buffer* b = pw_stream_dequeue_buffer(m->stream);
            if (!b)
                return;
            const spa_data& d = b->buffer->datas[0];
            if (d.data && d.chunk) {
                const auto*     s = (const float*)((const uint8_t*)d.data + d.chunk->offset);
                const uint32_t  n = d.chunk->size / sizeof(float);
                std::lock_guard lock(m->mutex);
                if (!m->ring.empty())
                    for (uint32_t i = 0; i < n; ++i) {
                        m->ring[(m->head + m->count) % m->ring.size()] = s[i];
                        if (m->count < m->ring.size())
                            ++m->count;
                        else
                            m->head = (m->head + 1) % m->ring.size(); // the oldest goes
                    }
            }
            pw_stream_queue_buffer(m->stream, b);
        }

        static const pw_stream_events& events() {
            static const pw_stream_events e = [] {
                pw_stream_events x{};
                x.version       = PW_VERSION_STREAM_EVENTS;
                x.param_changed = &onParam;
                x.process       = &onProcess;
                return x;
            }();
            return e;
        }
    };

    bool CMicrophone::available() {
        return true;
    }

    bool CMicrophone::start(std::string& error) {
        if (on())
            return true;
        static std::once_flag init;
        std::call_once(init, [] { pw_init(nullptr, nullptr); });
        m->loop = pw_thread_loop_new("hypr3d-lipsync", nullptr);
        if (!m->loop) {
            error = "couldn't start a PipeWire thread";
            return false;
        }
        pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Communication", PW_KEY_APP_NAME,
                                                 "hypr3d", PW_KEY_NODE_NAME, "hypr3d-lipsync", PW_KEY_NODE_DESCRIPTION, "hypr3d lip sync", nullptr);
        pw_thread_loop_lock(m->loop);
        {
            std::lock_guard lock(m->mutex);
            m->ring.assign((size_t)m->rate / 4, 0.f);
            m->head = m->count = 0;
        }
        m->stream = pw_stream_new_simple(pw_thread_loop_get_loop(m->loop), "hypr3d lip sync", props, &SImpl::events(), m.get());
        bool ok   = m->stream != nullptr;
        if (ok) {
            uint8_t            buffer[1024];
            spa_pod_builder    b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
            spa_audio_info_raw info{};
            info.format          = SPA_AUDIO_FORMAT_F32;
            info.rate            = 48000;
            info.channels        = 1;
            info.position[0]     = SPA_AUDIO_CHANNEL_MONO;
            const spa_pod* params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
            ok = pw_stream_connect(m->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                                   (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), params, 1) == 0;
        }
        pw_thread_loop_unlock(m->loop);
        if (!ok || pw_thread_loop_start(m->loop) != 0) {
            error = "couldn't open the microphone through PipeWire";
            stop();
            return false;
        }
        return true;
    }

    void CMicrophone::stop() {
        if (m->loop)
            pw_thread_loop_stop(m->loop);
        if (m->stream)
            pw_stream_destroy(m->stream);
        if (m->loop)
            pw_thread_loop_destroy(m->loop);
        m->stream = nullptr;
        m->loop   = nullptr;
        std::lock_guard lock(m->mutex);
        m->head = m->count = 0;
    }

    bool CMicrophone::on() const {
        return m->loop != nullptr;
    }

    void CMicrophone::read(std::vector<float>& out, int& rate) {
        std::lock_guard lock(m->mutex);
        rate = m->rate;
        for (size_t i = 0; i < m->count; ++i)
            out.push_back(m->ring[(m->head + i) % m->ring.size()]);
        m->head = m->count = 0;
    }
#else
    struct CMicrophone::SImpl {};

    bool CMicrophone::available() {
        return false;
    }

    bool CMicrophone::start(std::string& error) {
        error = "hypr3d was built without PipeWire (build.sh finds the one that runs)";
        return false;
    }

    void CMicrophone::stop() {}

    bool CMicrophone::on() const {
        return false;
    }

    void CMicrophone::read(std::vector<float>&, int& rate) {
        rate = 0;
    }
#endif

    CMicrophone::CMicrophone() : m(std::make_unique<SImpl>()) {}

    CMicrophone::~CMicrophone() {
        stop();
    }
}
