#pragma once

#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <format>
#include <mutex>
#include <optional>
#include <thread>

namespace h3d {

    // thrown from inside a load when it gets cancelled
    struct SCancelled {};

    // Runs `load(request, cancel)` on a worker thread. `fd()` becomes readable
    // when a result is waiting, which `take()` then hands over (on the main
    // thread). Results need `req` and `error` members, requests a `path`.
    template <typename Req, typename Res>
    class CBackgroundLoader {
      public:
        using LoadFn = Res (*)(const Req&, const std::atomic<bool>&);

        explicit CBackgroundLoader(LoadFn fn) : m_fn(fn) {
            m_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        }
        ~CBackgroundLoader() {
            cancel();
            if (m_fd >= 0)
                close(m_fd);
        }
        CBackgroundLoader(const CBackgroundLoader&)            = delete;
        CBackgroundLoader& operator=(const CBackgroundLoader&) = delete;

        int fd() const {
            return m_fd;
        }
        bool busy() const {
            return m_busy;
        }

        void load(const Req& req) {
            cancel();
            m_busy   = true;
            m_thread = std::thread([this, req] {
                Res r;
                try {
                    r = m_fn(req, m_cancel);
                } catch (SCancelled&) {
                    return;
                } catch (std::exception& e) {
                    r       = Res{};
                    r.req   = req;
                    r.error = std::format("loading {} failed: {}", req.path, e.what());
                }
                {
                    std::lock_guard lk(m_mutex);
                    m_result = std::move(r);
                }
                m_busy             = false;
                const uint64_t one = 1;
                if (m_fd >= 0)
                    (void)!write(m_fd, &one, sizeof(one));
            });
        }

        void cancel() {
            if (m_thread.joinable()) {
                m_cancel = true;
                m_thread.join();
            }
            m_cancel = false;
            m_busy   = false;
            std::lock_guard lk(m_mutex);
            m_result.reset();
        }

        std::optional<Res> take() {
            uint64_t v = 0;
            if (m_fd >= 0)
                (void)!read(m_fd, &v, sizeof(v));
            std::lock_guard lk(m_mutex);
            auto            r = std::move(m_result);
            m_result.reset();
            if (r && m_thread.joinable())
                m_thread.join(); // done anyway, it only had to write the eventfd
            return r;
        }

      private:
        LoadFn             m_fn;
        std::thread        m_thread;
        std::atomic<bool>  m_cancel = false, m_busy = false;
        std::mutex         m_mutex;
        std::optional<Res> m_result;
        int                m_fd = -1;
    };
}
