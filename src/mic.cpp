#include "mic.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#ifdef H3D_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/pod/iter.h>
#include <spa/utils/string.h>
#endif

namespace h3d {

#ifdef H3D_PIPEWIRE
    namespace {
        using Clock = std::chrono::steady_clock;

        const char* nodeState(pw_node_state s) {
            switch (s) {
                case PW_NODE_STATE_ERROR: return "error";
                case PW_NODE_STATE_CREATING: return "creating";
                case PW_NODE_STATE_SUSPENDED: return "suspended";
                case PW_NODE_STATE_IDLE: return "idle";
                case PW_NODE_STATE_RUNNING: return "running";
            }
            return "unknown";
        }

        float dB(double amplitude) { // -200: nothing at all
            return amplitude > 0 ? std::max(-200.f, (float)(20 * std::log10(amplitude))) : -200.f;
        }

        std::string lower(std::string s) {
            std::ranges::transform(s, s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return s;
        }
    }

    struct CMicrophone::SImpl {
        pw_thread_loop* loop     = nullptr;
        pw_context*     context  = nullptr;
        pw_core*        core     = nullptr;
        pw_registry*    registry = nullptr;
        pw_stream*      stream   = nullptr;
        pw_node*        source   = nullptr; // the source linked to the stream, bound for its state and its mute
        uint32_t        boundId  = 0;
        spa_hook        coreHook{}, registryHook{}, streamHook{}, sourceHook{};
        int             syncSeq = -1;
        bool            synced  = false;

        // the rest the loop's thread writes and the main thread reads
        mutable std::mutex mutex;
        std::vector<float> ring; // the last quarter second at most
        size_t             head = 0, count = 0;
        int                rate = 48000;
        // PipeWire's graph as the registry tells it: nodes and links (the node they come from, the one they go to)
        struct SNode {
            std::string name, description, nick, mediaClass;
        };
        std::unordered_map<uint32_t, SNode>                         nodes;
        std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> links;
        uint32_t    ownId = SPA_ID_INVALID; // the stream's node
        std::string streamState = "unconnected", streamError, target;
        std::string coreError; // the last PipeWire said that wasn't the stream's (a message to something gone)
        std::string sourceState, sourceError;
        int         mute = -1, softMute = -1;
        float       volume = -1, channelVolume = -1;
        // what came
        Clock::time_point started, lastData;
        bool              anyData = false;
        uint64_t          samples = 0, zeroRun = 0, buffers = 0, emptyBuffers = 0;
        double            secPeak = 0, secSum2 = 0;
        uint64_t          secN = 0;
        float             peak = -200, rms = -200;

        void clearSource() { // (with the mutex held)
            sourceState.clear();
            sourceError.clear();
            mute = softMute = -1;
            volume = channelVolume = -1;
        }

        uint32_t feeding() const { // the source a link feeds the stream from, 0 = none (with the mutex held)
            if (ownId == SPA_ID_INVALID)
                return 0;
            uint32_t best = 0;
            for (const auto& [id, l] : links)
                if (l.second == ownId && (!best || l.first < best))
                    best = l.first;
            return best;
        }

        // in the loop's thread: bind the source the stream is linked to now, for its state and mute
        void relink() {
            uint32_t now;
            {
                std::lock_guard lock(mutex);
                now = feeding();
            }
            if (now == boundId)
                return;
            if (source) {
                spa_hook_remove(&sourceHook);
                sourceHook = {};
                pw_proxy_destroy((pw_proxy*)source);
                source = nullptr;
            }
            boundId = now;
            {
                std::lock_guard lock(mutex);
                clearSource();
            }
            if (!now || !registry)
                return;
            source = (pw_node*)pw_registry_bind(registry, now, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
            if (!source)
                return;
            pw_node_add_listener(source, &sourceHook, &nodeEvents(), this);
            uint32_t ids[] = {SPA_PARAM_Props};
            pw_node_subscribe_params(source, ids, 1);
            pw_node_enum_params(source, 0, SPA_PARAM_Props, 0, UINT32_MAX, nullptr);
        }

        // a source node's name for "target": its node.name as is, else the one whose description or nick it is (or
        // is part of), as wpctl status shows them; none: as it is (WirePlumber won't find it)
        std::string resolve(const std::string& want) const {
            std::lock_guard lock(mutex);
            const std::string w = lower(want);
            std::string       part;
            for (const auto& [id, n] : nodes) {
                if (!n.mediaClass.starts_with("Audio/Source"))
                    continue;
                if (n.name == want)
                    return n.name;
                const std::string d = lower(n.description), k = lower(n.nick);
                if (d == w || k == w)
                    return n.name;
                if (part.empty() && !w.empty() && (d.contains(w) || k.contains(w)))
                    part = n.name;
            }
            return part.empty() ? want : part;
        }

        // ---- the stream
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
                const uint32_t  n = std::min(d.chunk->size, d.maxsize - std::min(d.maxsize, d.chunk->offset)) / sizeof(float);
                std::lock_guard lock(m->mutex);
                ++m->buffers;
                if (d.chunk->flags & SPA_CHUNK_FLAG_EMPTY)
                    ++m->emptyBuffers;
                m->samples += n;
                m->lastData = Clock::now();
                m->anyData  = true;
                for (uint32_t i = 0; i < n; ++i) {
                    const float x = s[i];
                    m->zeroRun    = x != 0.f ? 0 : m->zeroRun + 1;
                    m->secPeak    = std::max(m->secPeak, (double)std::abs(x));
                    m->secSum2 += (double)x * x;
                    if (++m->secN >= (uint64_t)m->rate) { // a second's worth: what it was
                        m->peak    = dB(m->secPeak);
                        m->rms     = m->secSum2 > 0 ? std::max(-200.f, (float)(10 * std::log10(m->secSum2 / m->secN)) + 3.01f) : -200.f;
                        m->secPeak = m->secSum2 = 0;
                        m->secN                 = 0;
                    }
                }
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

        static void onStreamState(void* data, pw_stream_state, pw_stream_state state, const char* error) {
            auto* m = (SImpl*)data;
            {
                std::lock_guard lock(m->mutex);
                m->streamState = pw_stream_state_as_string(state);
                m->streamError = error ? error : "";
                if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING)
                    m->ownId = pw_stream_get_node_id(m->stream);
            }
            m->relink();
        }

        static const pw_stream_events& streamEvents() {
            static const pw_stream_events e = [] {
                pw_stream_events x{};
                x.version       = PW_VERSION_STREAM_EVENTS;
                x.state_changed = &onStreamState;
                x.param_changed = &onParam;
                x.process       = &onProcess;
                return x;
            }();
            return e;
        }

        // ---- the graph
        static void onGlobal(void* data, uint32_t id, uint32_t, const char* type, uint32_t, const spa_dict* props) {
            auto* m = (SImpl*)data;
            if (!props)
                return;
            auto get = [&](const char* key) {
                const char* v = spa_dict_lookup(props, key);
                return std::string(v ? v : "");
            };
            if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
                std::lock_guard lock(m->mutex);
                m->nodes[id] = {get(PW_KEY_NODE_NAME), get(PW_KEY_NODE_DESCRIPTION), get(PW_KEY_NODE_NICK), get(PW_KEY_MEDIA_CLASS)};
            } else if (spa_streq(type, PW_TYPE_INTERFACE_Link)) {
                {
                    std::lock_guard lock(m->mutex);
                    m->links[id] = {(uint32_t)std::strtoul(get(PW_KEY_LINK_OUTPUT_NODE).c_str(), nullptr, 10),
                                    (uint32_t)std::strtoul(get(PW_KEY_LINK_INPUT_NODE).c_str(), nullptr, 10)};
                }
                m->relink();
            }
        }

        static void onGlobalRemove(void* data, uint32_t id) {
            auto* m = (SImpl*)data;
            {
                std::lock_guard lock(m->mutex);
                m->nodes.erase(id);
                m->links.erase(id);
            }
            m->relink();
        }

        static const pw_registry_events& registryEvents() {
            static const pw_registry_events e = [] {
                pw_registry_events x{};
                x.version       = PW_VERSION_REGISTRY_EVENTS;
                x.global        = &onGlobal;
                x.global_remove = &onGlobalRemove;
                return x;
            }();
            return e;
        }

        // ---- the source
        static void onSourceInfo(void* data, const pw_node_info* info) {
            auto* m = (SImpl*)data;
            if (!info || !(info->change_mask & PW_NODE_CHANGE_MASK_STATE))
                return;
            std::lock_guard lock(m->mutex);
            m->sourceState = nodeState(info->state);
            m->sourceError = info->error ? info->error : "";
        }

        static void onSourceParam(void* data, int, uint32_t id, uint32_t, uint32_t, const spa_pod* param) {
            auto* m = (SImpl*)data;
            if (id != SPA_PARAM_Props || !param || !spa_pod_is_object_type(param, SPA_TYPE_OBJECT_Props))
                return;
            std::lock_guard      lock(m->mutex);
            const spa_pod_prop*  prop;
            const spa_pod_object* obj = (const spa_pod_object*)param;
            SPA_POD_OBJECT_FOREACH(obj, prop) {
                switch (prop->key) {
                    case SPA_PROP_mute:
                    case SPA_PROP_softMute: {
                        bool b = false;
                        if (spa_pod_get_bool(&prop->value, &b) >= 0)
                            (prop->key == SPA_PROP_mute ? m->mute : m->softMute) = b;
                        break;
                    }
                    case SPA_PROP_volume: {
                        float f = 0;
                        if (spa_pod_get_float(&prop->value, &f) >= 0)
                            m->volume = f;
                        break;
                    }
                    case SPA_PROP_channelVolumes: {
                        float          v[64];
                        const uint32_t n = spa_pod_copy_array(&prop->value, SPA_TYPE_Float, v, 64);
                        if (n > 0) {
                            float sum = 0;
                            for (uint32_t i = 0; i < n; ++i)
                                sum += v[i];
                            m->channelVolume = sum / n;
                        }
                        break;
                    }
                    default: break;
                }
            }
        }

        static const pw_node_events& nodeEvents() {
            static const pw_node_events e = [] {
                pw_node_events x{};
                x.version = PW_VERSION_NODE_EVENTS;
                x.info    = &onSourceInfo;
                x.param   = &onSourceParam;
                return x;
            }();
            return e;
        }

        // ---- the connection
        static void onDone(void* data, uint32_t id, int seq) {
            auto* m = (SImpl*)data;
            if (id == PW_ID_CORE && seq == m->syncSeq) {
                m->synced = true;
                pw_thread_loop_signal(m->loop, false);
            }
        }

        static void onCoreError(void* data, uint32_t id, int, int res, const char* message) {
            auto* m = (SImpl*)data;
            if (id != PW_ID_CORE)
                return; // (the stream's own come as its state)
            std::lock_guard lock(m->mutex);
            if (res == -EPIPE) { // gone; anything else is only said (as the stream itself takes it)
                m->streamState = "error";
                m->streamError = "PipeWire went away";
            } else
                m->coreError = message ? message : "PipeWire error";
        }

        static const pw_core_events& coreEvents() {
            static const pw_core_events e = [] {
                pw_core_events x{};
                x.version = PW_VERSION_CORE_EVENTS;
                x.done    = &onDone;
                x.error   = &onCoreError;
                return x;
            }();
            return e;
        }
    };

    bool CMicrophone::available() {
        return true;
    }

    bool CMicrophone::start(std::string& error, const std::string& target) {
        if (on())
            return true;
        static std::once_flag init;
        std::call_once(init, [] { pw_init(nullptr, nullptr); });
        m->loop = pw_thread_loop_new("hypr3d-lipsync", nullptr);
        if (!m->loop || pw_thread_loop_start(m->loop) != 0) {
            error = "couldn't start a PipeWire thread";
            stop();
            return false;
        }
        pw_thread_loop_lock(m->loop);
        {
            std::lock_guard lock(m->mutex);
            m->ring.assign((size_t)m->rate / 4, 0.f);
            m->head = m->count = 0;
            m->nodes.clear();
            m->links.clear();
            m->ownId       = SPA_ID_INVALID;
            m->streamState = "connecting";
            m->streamError.clear();
            m->coreError.clear();
            m->target = target;
            m->clearSource();
            m->started = m->lastData = Clock::now();
            m->anyData                = false;
            m->samples = m->zeroRun = m->buffers = m->emptyBuffers = 0;
            m->secPeak = m->secSum2 = 0;
            m->secN                 = 0;
            m->peak = m->rms = -200;
        }
        m->synced  = false;
        m->context = pw_context_new(pw_thread_loop_get_loop(m->loop), nullptr, 0);
        m->core    = m->context ? pw_context_connect(m->context, nullptr, 0) : nullptr;
        bool ok    = m->core != nullptr;
        if (ok) {
            pw_core_add_listener(m->core, &m->coreHook, &SImpl::coreEvents(), m.get());
            m->registry = pw_core_get_registry(m->core, PW_VERSION_REGISTRY, 0);
            if (m->registry)
                pw_registry_add_listener(m->registry, &m->registryHook, &SImpl::registryEvents(), m.get());
            // a source asked for: which it is (the registry's nodes, a round trip away; a second at most)
            std::string name = target;
            if (!target.empty() && m->registry) {
                m->syncSeq = pw_core_sync(m->core, PW_ID_CORE, 0);
                while (!m->synced)
                    if (pw_thread_loop_timed_wait(m->loop, 1) != 0)
                        break;
                name = m->resolve(target);
                std::lock_guard lock(m->mutex);
                m->target = name;
            }
            pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Communication", PW_KEY_APP_NAME,
                                                     "hypr3d", PW_KEY_NODE_NAME, "hypr3d-lipsync", PW_KEY_NODE_DESCRIPTION, "hypr3d lip sync", nullptr);
            // that one; not there, WirePlumber gives it the default one (status() says which it got). (Not with
            // node.dont-fallback: WirePlumber 0.5.17 sets a stream linked to a target that is the default to follow
            // the default from then on, and then ends one with dont-fallback, its target "not found")
            if (!name.empty())
                pw_properties_set(props, PW_KEY_TARGET_OBJECT, name.c_str());
            m->stream = pw_stream_new(m->core, "hypr3d lip sync", props);
            ok        = m->stream != nullptr;
        }
        if (ok) {
            pw_stream_add_listener(m->stream, &m->streamHook, &SImpl::streamEvents(), m.get());
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
        if (!ok) {
            error = "couldn't open the microphone through PipeWire";
            stop();
            return false;
        }
        return true;
    }

    void CMicrophone::stop() {
        if (m->loop)
            pw_thread_loop_stop(m->loop);
        // (the loop's thread is gone: nothing else touches these)
        if (m->source) {
            spa_hook_remove(&m->sourceHook);
            pw_proxy_destroy((pw_proxy*)m->source);
        }
        if (m->stream) {
            spa_hook_remove(&m->streamHook);
            pw_stream_destroy(m->stream);
        }
        if (m->registry) {
            spa_hook_remove(&m->registryHook);
            pw_proxy_destroy((pw_proxy*)m->registry);
        }
        if (m->core) {
            spa_hook_remove(&m->coreHook);
            pw_core_disconnect(m->core);
        }
        if (m->context)
            pw_context_destroy(m->context);
        if (m->loop)
            pw_thread_loop_destroy(m->loop);
        m->sourceHook = m->streamHook = m->registryHook = m->coreHook = {};
        m->source                                                     = nullptr;
        m->boundId                                                    = 0;
        m->stream                                                     = nullptr;
        m->registry                                                   = nullptr;
        m->core                                                       = nullptr;
        m->context                                                    = nullptr;
        m->loop                                                       = nullptr;
        std::lock_guard lock(m->mutex);
        m->head = m->count = 0;
        m->nodes.clear();
        m->links.clear();
        m->ownId       = SPA_ID_INVALID;
        m->streamState = "unconnected";
        m->streamError.clear();
        m->clearSource();
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

    SMicStatus CMicrophone::status() const {
        SMicStatus      s;
        const auto      now = Clock::now();
        std::lock_guard lock(m->mutex);
        s.on     = m->loop != nullptr;
        s.stream    = m->streamState;
        s.error     = m->streamError;
        s.coreError = m->coreError;
        s.target = m->target;
        if (!s.on)
            return s;
        s.sourceId = m->feeding();
        s.linked   = s.sourceId != 0;
        if (const auto it = m->nodes.find(s.sourceId); it != m->nodes.end()) {
            s.sourceName        = it->second.name;
            s.sourceDescription = it->second.description;
            s.sourceNick        = it->second.nick;
        }
        s.sourceState = m->sourceState;
        s.sourceError = m->sourceError;
        s.muted       = m->mute == 1 || m->softMute == 1 ? 1 : m->mute == 0 || m->softMute == 0 ? 0 : -1;
        s.volume      = m->channelVolume >= 0 ? (m->volume >= 0 ? m->volume : 1.f) * m->channelVolume : m->volume;
        s.samples      = m->samples;
        s.buffers      = m->buffers;
        s.emptyBuffers = m->emptyBuffers;
        s.silentFor    = m->rate > 0 ? (double)m->zeroRun / m->rate : 0;
        s.sinceData    = m->anyData ? std::chrono::duration<double>(now - m->lastData).count() : -1;
        s.age          = std::chrono::duration<double>(now - m->started).count();
        if (s.sinceData >= 0 && s.sinceData < 1.5) { // (a second that ended long ago says nothing about now)
            s.peak = m->peak;
            s.rms  = m->rms;
        }
        for (const auto& [id, n] : m->nodes)
            if (n.mediaClass.starts_with("Audio/Source"))
                s.sources.emplace_back(n.name, n.description);
        std::ranges::sort(s.sources);
        return s;
    }
#else
    struct CMicrophone::SImpl {};

    bool CMicrophone::available() {
        return false;
    }

    bool CMicrophone::start(std::string& error, const std::string&) {
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

    SMicStatus CMicrophone::status() const {
        return {};
    }
#endif

    CMicrophone::CMicrophone() : m(std::make_unique<SImpl>()) {}

    CMicrophone::~CMicrophone() {
        stop();
    }
}
