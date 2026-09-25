#include "gl.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#ifndef GL_TEXTURE_BINDING_EXTERNAL_OES
#define GL_TEXTURE_BINDING_EXTERNAL_OES 0x8D67
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif

namespace h3d::gl {

    static GLuint compile(const char* name, GLenum type, const std::string& src) {
        GLuint      s   = glCreateShader(type);
        const char* ptr = src.c_str();
        glShaderSource(s, 1, &ptr, nullptr);
        glCompileShader(s);

        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            GLint len = 0;
            glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
            std::string msg(std::max(len, 1), '\0');
            glGetShaderInfoLog(s, len, nullptr, msg.data());
            logf("shader {} ({}) failed to compile: {}", name, type == GL_VERTEX_SHADER ? "vs" : "fs", msg);
            glDeleteShader(s);
            return 0;
        }
        return s;
    }

    GLuint makeProgram(const char* name, const std::string& vsSrc, const std::string& fsSrc) {
        GLuint vs = compile(name, GL_VERTEX_SHADER, vsSrc);
        if (!vs)
            return 0;
        GLuint fs = compile(name, GL_FRAGMENT_SHADER, fsSrc);
        if (!fs) {
            glDeleteShader(vs);
            return 0;
        }

        GLuint p = glCreateProgram();
        glAttachShader(p, vs);
        glAttachShader(p, fs);
        glLinkProgram(p);
        glDeleteShader(vs);
        glDeleteShader(fs);

        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) {
            GLint len = 0;
            glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
            std::string msg(std::max(len, 1), '\0');
            glGetProgramInfoLog(p, len, nullptr, msg.data());
            logf("program {} failed to link: {}", name, msg);
            glDeleteProgram(p);
            return 0;
        }
        return p;
    }

    int textureCompression() {
        const char* ext = (const char*)glGetString(GL_EXTENSIONS);
        if (!ext)
            return 0;
        const auto has = [&](const char* name) {
            for (const char* p = std::strstr(ext, name); p; p = std::strstr(p + 1, name))
                if ((p == ext || p[-1] == ' ') && (p[std::strlen(name)] == ' ' || p[std::strlen(name)] == 0))
                    return true;
            return false;
        };
        int out = 0;
        if (has("GL_EXT_texture_compression_s3tc"))
            out |= 1;
        if (has("GL_EXT_texture_compression_s3tc_srgb") || has("GL_NV_sRGB_formats"))
            out |= 2;
        if (has("GL_EXT_texture_compression_rgtc"))
            out |= 4;
        return out;
    }

    CStateGuard::CStateGuard() {
        glGetIntegerv(GL_CURRENT_PROGRAM, &m_program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &m_vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &m_arrayBuffer);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &m_drawFb);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &m_readFb);
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &m_renderbuffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &m_activeTex);
        glGetIntegerv(GL_VIEWPORT, m_viewport);
        glGetIntegerv(GL_SCISSOR_BOX, m_scissorBox);
        for (int i = 0; i < UNITS; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &m_tex2D[i]);
            glGetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &m_texExt[i]);
            glGetIntegerv(GL_TEXTURE_BINDING_3D, &m_tex3D[i]);
        }
        glGetIntegerv(GL_BLEND_SRC_RGB, &m_blendSrcRGB);
        glGetIntegerv(GL_BLEND_DST_RGB, &m_blendDstRGB);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &m_blendSrcA);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &m_blendDstA);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &m_blendEqRGB);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &m_blendEqA);
        glGetIntegerv(GL_DEPTH_FUNC, &m_depthFunc);
        static constexpr GLenum STENCIL[2][7] = {
            {GL_STENCIL_FUNC, GL_STENCIL_REF, GL_STENCIL_VALUE_MASK, GL_STENCIL_FAIL, GL_STENCIL_PASS_DEPTH_FAIL, GL_STENCIL_PASS_DEPTH_PASS, GL_STENCIL_WRITEMASK},
            {GL_STENCIL_BACK_FUNC, GL_STENCIL_BACK_REF, GL_STENCIL_BACK_VALUE_MASK, GL_STENCIL_BACK_FAIL, GL_STENCIL_BACK_PASS_DEPTH_FAIL,
             GL_STENCIL_BACK_PASS_DEPTH_PASS, GL_STENCIL_BACK_WRITEMASK}};
        for (int f = 0; f < 2; ++f)
            for (int k = 0; k < 7; ++k)
                glGetIntegerv(STENCIL[f][k], &m_stencilState[f][k]);
        glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &m_stencilClear);
        glGetIntegerv(GL_FRONT_FACE, &m_frontFace);
        glGetIntegerv(GL_CULL_FACE_MODE, &m_cullFace);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &m_unpackAlign);
        glGetIntegerv(GL_PACK_ALIGNMENT, &m_packAlign);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &m_depthMask);
        glGetBooleanv(GL_COLOR_WRITEMASK, m_colorMask);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, m_clearColor);
        glGetFloatv(GL_DEPTH_CLEAR_VALUE, &m_clearDepth);
        m_blend   = glIsEnabled(GL_BLEND);
        m_scissor = glIsEnabled(GL_SCISSOR_TEST);
        m_depth   = glIsEnabled(GL_DEPTH_TEST);
        m_cull    = glIsEnabled(GL_CULL_FACE);
        m_stencil = glIsEnabled(GL_STENCIL_TEST);
    }

    static void setCap(GLenum cap, bool on) {
        if (on)
            glEnable(cap);
        else
            glDisable(cap);
    }

    CStateGuard::~CStateGuard() {
        glUseProgram(m_program);
        glBindVertexArray(m_vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_arrayBuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_drawFb);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_readFb);
        glBindRenderbuffer(GL_RENDERBUFFER, m_renderbuffer);
        for (int i = 0; i < UNITS; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, m_tex2D[i]);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_texExt[i]);
            glBindTexture(GL_TEXTURE_3D, m_tex3D[i]);
        }
        glActiveTexture(m_activeTex);
        glViewport(m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3]);
        glScissor(m_scissorBox[0], m_scissorBox[1], m_scissorBox[2], m_scissorBox[3]);
        glBlendFuncSeparate(m_blendSrcRGB, m_blendDstRGB, m_blendSrcA, m_blendDstA);
        glBlendEquationSeparate(m_blendEqRGB, m_blendEqA);
        glDepthFunc(m_depthFunc);
        glDepthMask(m_depthMask);
        for (int f = 0; f < 2; ++f) {
            const GLenum face = f == 0 ? GL_FRONT : GL_BACK;
            glStencilFuncSeparate(face, m_stencilState[f][0], m_stencilState[f][1], (GLuint)m_stencilState[f][2]);
            glStencilOpSeparate(face, m_stencilState[f][3], m_stencilState[f][4], m_stencilState[f][5]);
            glStencilMaskSeparate(face, (GLuint)m_stencilState[f][6]);
        }
        glClearStencil(m_stencilClear);
        glFrontFace(m_frontFace);
        glCullFace(m_cullFace);
        glColorMask(m_colorMask[0], m_colorMask[1], m_colorMask[2], m_colorMask[3]);
        glClearColor(m_clearColor[0], m_clearColor[1], m_clearColor[2], m_clearColor[3]);
        glClearDepthf(m_clearDepth);
        glPixelStorei(GL_UNPACK_ALIGNMENT, m_unpackAlign);
        glPixelStorei(GL_PACK_ALIGNMENT, m_packAlign);
        setCap(GL_BLEND, m_blend);
        setCap(GL_SCISSOR_TEST, m_scissor);
        setCap(GL_DEPTH_TEST, m_depth);
        setCap(GL_CULL_FACE, m_cull);
        setCap(GL_STENCIL_TEST, m_stencil);
    }

    bool STarget::ensure(int w_, int h_, bool mipmapped) {
        w_ = std::clamp(w_, 1, 8192);
        h_ = std::clamp(h_, 1, 8192);
        const int wantLevels = mipmapped ? 1 + (int)std::floor(std::log2((float)std::max(w_, h_))) : 1;
        if (tex && w == w_ && h == h_ && levels == wantLevels)
            return false;

        destroy();
        w      = w_;
        h      = h_;
        levels = wantLevels;

        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexStorage2D(GL_TEXTURE_2D, levels, GL_RGBA8, w, h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmapped ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (mipmapped)
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 8.f);
        glGetError(); // anisotropy may be unsupported; that's fine

        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        return true;
    }

    void STarget::destroy() {
        if (fbo)
            glDeleteFramebuffers(1, &fbo);
        if (tex)
            glDeleteTextures(1, &tex);
        fbo = tex = 0;
        w = h = 0;
    }
}
