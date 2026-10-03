// Hyprland symbols the plugin's headers use in static initializers; the harness isn't linked against Hyprland.
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/helpers/Color.hpp>

CHyprColor::CHyprColor(float r_, float g_, float b_, float a_) : r(r_), g(g_), b(b_), a(a_) {}
Log::CLogger::CLogger() {}

#include <hyprland/src/helpers/cm/ColorManagement.hpp>

const NColorManagement::SPCPRimaries& NColorManagement::getPrimaries(ePrimaries) {
    return NColorPrimaries::BT709;
}
WP<const NColorManagement::CImageDescription> NColorManagement::CImageDescription::from(const SImageDescription&) {
    return {};
}
