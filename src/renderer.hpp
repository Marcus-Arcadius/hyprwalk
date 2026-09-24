#pragma once

#include "avatar.hpp"
#include "gl.hpp"
#include "map.hpp"
#include "math3d.hpp"
#include "panels.hpp"
#include "world.hpp"

#include <unordered_map>

namespace h3d {

    // where the 2D desktop sits in the world: a rectangle on a wall
    struct SScreenMapping {
        float    height = 2.4f;
        V3       center{0, 1.65f, 0};
        V3       right{1, 0, 0}, up{0, 1, 0}, normal{0, 0, 1};
        Vector2D logicalSize{1920, 1080}; // monitor size in logical px

        float    scale() const {
            return height / (float)logicalSize.y;
        }
        void setAnchor(const SDesktopAnchor& a) {
            height = a.height;
            center = a.center;
            normal = normalize(a.normal);
            right  = normalize(cross(V3{0, 1, 0}, normal));
            if (length(right) < 0.5f)
                right = {1, 0, 0};
            up = cross(normal, right);
        }
        // pose of a rectangle on the desktop, `depth` meters in front of the wall
        SPanelPose pose(const CBox& box, float depth) const {
            const float s = scale();
            SPanelPose  p;
            p.origin = center + right * (float)((box.x - logicalSize.x / 2.0) * s) + up * (float)((logicalSize.y / 2.0 - box.y) * s) + normal * depth;
            p.right  = right;
            p.down   = -up;
            p.normal = normal;
            p.scale  = s;
            return p;
        }
    };

    // the player's body, seen in third person
    struct SAvatarFrame {
        std::shared_ptr<SAvatarModel> model;            // null = none
        const std::vector<float>*     joints = nullptr; // CAvatarAnimator::joints()
        const std::vector<float>*     morphs = nullptr; // CAvatarAnimator::morphWeights(), null = at rest
        const std::vector<SMapMaterial>* materials = nullptr; // CAvatarAnimator::materials(), null = the model's
        const std::vector<uint8_t>*   shown = nullptr;  // CAvatarAnimator::partsShown(), null = all of it
        M4                            transform = M4::identity(); // avatar space -> world
        bool                          visible = false;  // drawn (third person); it casts its shadow either way
        float                         sky = 1, bounce = 0; // light around it, like the map's baked values

        bool                          drawn(const SAvatarBatch& b) const {
            return !shown || (size_t)b.part >= shown->size() || (*shown)[b.part];
        }
    };

    // a picture over everything: the Action Menu
    struct SHudImage {
        const std::vector<uint32_t>* pixels = nullptr; // premultiplied ARGB (cairo's), row 0 at the top; null = none
        int                          w = 0, h = 0;
        uint64_t                     serial = 0;   // changes with the pixels
        float                        x = 0, y = 0; // where its middle goes, output pixels from the top left
        float                        scale = 1, alpha = 1;
        float                        cursor[3] = {}; // a dot over it: x, y in its pixels from its top left, radius (0 = none)
    };

    struct SFrameParams {
        int                  width = 0, height = 0; // output pixels
        M4                   view, proj;
        V3                   eye;
        float                time      = 0;
        float                monScale  = 1;
        std::vector<SPanel>* panels    = nullptr; // with poses, in drawing order
        int                  aimed     = -1; // index into panels
        bool                 crosshair = true;
        bool                 typing    = false;
        float                hudAlpha  = 1;
        float                exposure  = 1; // brightens the world (not the windows) in dark places
        SAvatarFrame         avatar;
        SHudImage            menu;
    };

    class CRenderer {
      public:
        // keeps the GPU copy of a loaded map when it's the same one as before
        // (the map drops its CPU copy once uploaded)
        bool init(const SWorld& world);
        void destroy();
        bool ready() const {
            return m_ready;
        }

        // renders one frame into `outTex` (an immutable RGBA8 texture of the given size)
        void render(const SFrameParams& f, GLuint outTex);

      private:
        struct SPanelGL {
            gl::STarget target;
            float       uvMax[2] = {1, 1};
            uint64_t    lastUsed = 0;
        };

        bool                                     m_ready = false;
        uint64_t                                 m_frame = 0;

        GLuint                                   m_progCapture = 0, m_progCaptureExt = 0, m_progLight = 0, m_progSky = 0, m_progWorld = 0, m_progDepth = 0, m_progPanel = 0,
               m_progPanelDepth = 0, m_progCross = 0, m_progMap = 0, m_progMapDepth = 0, m_progAvatar = 0, m_progAvatarDepth = 0, m_progHud = 0;
        GLuint                                   m_quadVBO = 0, m_quadVAO = 0;
        GLuint                                   m_worldVBO = 0, m_worldVAO = 0;
        GLsizei                                  m_worldCount = 0;

        // the game's lighting of the map, or of its backdrop (SMapLightSet)
        struct SLightSetGL {
            GLuint irradiance = 0, directional = 0, shadows = 0, probes = 0; // 0: none
            float  probeDims[3] = {1, 1, 1};
            V3     average;
        };
        struct SMapGL {
            std::shared_ptr<SMapModel> model;
            GLuint                     vao = 0, vbo = 0, ibo = 0, white = 0, empty3D = 0;
            std::vector<GLuint>        textures; // per image, 0 when it didn't load
            std::vector<SLightSetGL>   lightSets;
            float                      skyLod = 0; // the sky texture's coarsest useful mip
        };
        SMapGL                                   m_map;

        // uploaded when a frame first shows it; kept like the map's
        struct SAvatarGL {
            std::shared_ptr<SAvatarModel> model;
            GLuint                        vao = 0, vbo = 0, ibo = 0, white = 0, jointTex = 0;
            int                           jointRows = 0;
            std::vector<GLuint>           textures;
            std::vector<float>            staging; // joint rows, padded
            bool                          live = false; // uploaded, posed this frame
            // the morphs, added up on the CPU: dpos, dnormal per vertex in [morphFirst, morphEnd)
            GLuint                        morphVBO = 0;
            std::vector<float>            morphAcc;
            std::vector<float>            morphW; // the weights morphAcc holds
            std::vector<std::pair<uint32_t, uint32_t>> morphSpan; // per morph: the vertices it moves
            uint32_t                      morphUpdates = 0;
        };
        SAvatarGL                                m_avatar;
        std::weak_ptr<SAvatarModel>              m_avatarFailed; // don't try that one again

        // the world's shadow is baked once into the static map; each frame it is
        // copied into the sampled one and whatever moves is drawn on top
        GLuint                                   m_shadowTex = 0, m_shadowFBO = 0, m_shadowStaticTex = 0, m_shadowStaticFBO = 0;
        int                                      m_shadowSize = 2048;
        uint64_t                                 m_shadowCasterHash = 1;
        M4                                       m_sunViewProj;
        V3                                       m_sunDir;
        float                                    m_shadowBias = 0.0015f, m_normalOffset = 0.03f, m_fog = 0.012f;
        // big maps: the shadow map covers the area around the player and moves along
        bool                                     m_sunFollow = false;
        V3                                       m_sunFocus;
        SAABB                                    m_sunCasters;

        gl::STarget                              m_lightTarget;

        GLuint                                   m_msaaFBO = 0, m_msaaColor = 0, m_msaaDepth = 0, m_resolveFBO = 0;
        int                                      m_msaaW = 0, m_msaaH = 0, m_samples = 4;

        std::unordered_map<uintptr_t, SPanelGL> m_panelGL;

        GLuint                                   m_hudTex = 0;
        int                                      m_hudW = 0, m_hudH = 0;
        uint64_t                                 m_hudSerial = 0; // what m_hudTex holds

        void                                     destroyBase();
        void                                     destroyMap();
        bool                                     uploadMap(const std::shared_ptr<SMapModel>& model);
        void                                     destroyAvatar();
        bool                                     uploadAvatar(const std::shared_ptr<SAvatarModel>& model);
        bool                                     updateAvatar(const SFrameParams& f); // joints; false = nothing to draw
        void                                     updateMorphs(const std::vector<float>& weights);
        const std::vector<SMapMaterial>&         avatarMaterials(const SFrameParams& f) const;
        void                                     setupSun(const SAABB& casters, const SAABB& region);
        void                                     bakeShadow();
        void                                     updateShadow(const SFrameParams& f);
        bool                                     ensureMSAA(int w, int h);
        void                                     capturePanels(const SFrameParams& f);
        int                                      updateLights(const SFrameParams& f, float* lights);
        void                                     drawSky(const SFrameParams& f, const M4& viewProj);
        void                                     setLighting(GLuint prog, const SFrameParams& f, const M4& viewProj, int lightCount, const float* lights);
        void                                     drawWorld(const SFrameParams& f, const M4& viewProj, int lightCount, const float* lights);
        enum eMapPass {
            MAP_PASS_SKY,
            MAP_PASS_BACKDROP,
            MAP_PASS_BACKDROP_BLEND,
            MAP_PASS_OPAQUE,
            MAP_PASS_BLEND,
        };
        void                                     drawMap(const SFrameParams& f, const M4& viewProj, int lightCount, const float* lights, eMapPass pass);
        // binds the game's lighting (set 0: the map, 1: its backdrop) and sets its uniforms, or turns it off
        void                                     setBakedLighting(GLuint prog, const SFrameParams& f, size_t set);
        void                                     drawBackdrop(const SFrameParams& f, int lightCount, const float* lights);
        void                                     drawAvatar(const SFrameParams& f, const M4& viewProj, int lightCount, const float* lights, bool blended);
        void                                     drawAvatarDepth(const SFrameParams& f);
        void                                     drawPanels(const SFrameParams& f, const M4& viewProj);
        void                                     drawCrosshair(const SFrameParams& f);
        void                                     drawHud(const SFrameParams& f);
        void                                     setPanelUniforms(GLuint prog, const SPanel& p, const SPanelGL& g);
    };
}
