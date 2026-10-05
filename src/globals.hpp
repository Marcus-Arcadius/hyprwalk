#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>

#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

#include <format>
#include <string>

inline HANDLE PHANDLE = nullptr;

namespace hyprwalk {
    void        log(const std::string& s);
    void        notify(const std::string& s, bool error = false);
    std::string logLines(size_t n); // the last n lines logged (and notified), oldest first

    template <typename... Args>
    void logf(std::format_string<Args...> fmt, Args&&... args) {
        log(std::format(fmt, std::forward<Args>(args)...));
    }
}
