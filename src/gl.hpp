#pragma once

#include "globals.hpp"

#include <string>

namespace h3d::gl {

    // compiles and links a program, returns 0 on failure (and logs why)
    GLuint makeProgram(const char* name, const std::string& vs, const std::string& fs);

    // the block compressed texture formats the current context takes (eTexCompression in map.hpp);
    // 0 without a context
    int textureCompression();

    // the current context has this extension (false without a context)
    bool hasExtension(const char* name);

    // Hyprland caches parts of the GL state (current program, blend, viewport,
    // bound framebuffer...). Everything we touch while rendering the 3D scene is
    // saved here and put back afterwards, so Hyprland never notices.
    class CStateGuard {
      public:
        CStateGuard();
        ~CStateGuard();

        CStateGuard(const CStateGuard&)            = delete;
        CStateGuard& operator=(const CStateGuard&) = delete;

      private:
        GLint     m_program = 0, m_vao = 0, m_arrayBuffer = 0, m_drawFb = 0, m_readFb = 0, m_renderbuffer = 0, m_activeTex = 0;
        GLint     m_viewport[4] = {}, m_scissorBox[4] = {};
        static constexpr int UNITS = 18; // the texture units we use (see setMaterial() in renderer.cpp)
        GLint     m_tex2D[UNITS] = {}, m_texExt[UNITS] = {}, m_tex3D[UNITS] = {};
        GLint     m_blendSrcRGB = 0, m_blendDstRGB = 0, m_blendSrcA = 0, m_blendDstA = 0, m_blendEqRGB = 0, m_blendEqA = 0;
        GLint     m_depthFunc = 0, m_unpackAlign = 4, m_packAlign = 4;
        GLint     m_stencilState[2][7] = {}; // front, back: func, ref, value mask, fail, depth fail, pass, write mask
        GLint     m_stencilClear = 0, m_frontFace = 0x0901, m_cullFace = 0x0405; // GL_CCW, GL_BACK
        GLboolean m_depthMask = GL_TRUE, m_colorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
        GLfloat   m_clearColor[4] = {}, m_clearDepth = 1.f;
        bool      m_blend = false, m_scissor = false, m_depth = false, m_cull = false, m_stencil = false;
    };

    // a single-sampled color texture with a framebuffer object pointed at it
    struct STarget {
        GLuint tex = 0, fbo = 0;
        int    w = 0, h = 0, levels = 1;

        // (re)creates when the size differs; returns true if storage changed
        bool ensure(int w, int h, bool mipmapped);
        void destroy();
    };
}
