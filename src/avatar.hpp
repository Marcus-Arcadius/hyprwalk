#pragma once

#include "lipsync.hpp"
#include "loader.hpp"
#include "map.hpp"
#include "math3d.hpp"
#include "sound.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The player's avatar: a skinned glTF / GLB / VRM model and its animator (named clips, else procedural for humanoids).

namespace hyprwalk {

    // skinned vertex in mesh space; joints index SAvatarModel::joints
    struct SAvatarVertex {
        float    pos[3];
        float    normal[3];
        float    uv[2];
        float    uv1[2];
        uint8_t  color[4];
        uint16_t joints[4];
        uint8_t  weights[4]; // sum to 255
    };

    struct SAvatarBatch {
        int      material = 0;
        int      part     = 0;
        uint32_t first = 0, count = 0; // in indices
        int      variants = 0;         // variantMaps index, 0 = none
        // first person copy without the head's triangles, after all batches' indices; fpCount UINT32_MAX = draw it all
        uint32_t fpFirst = 0, fpCount = UINT32_MAX;
    };

    // a mesh node: what outfit toggles show and hide
    struct SAvatarPart {
        std::string name;
        int         node = -1, gltfNode = -1;
        size_t      triangles = 0;
        bool        hidden    = false; // settings "hidden", until shown
    };

    struct STRS {
        V3   t;
        Quat r;
        V3   s{1, 1, 1};

        M4 matrix() const {
            return M4::trs(t, r, s);
        }
    };

    // node pose a toggle or slider sets (a converted VRChat Transform animation)
    struct SNodePose {
        enum : uint8_t {
            T = 1,
            R = 2,
            S = 4,
        };
        int     node = -1;
        uint8_t set  = 0;
        STRS    trs;
    };

    // VRCFury Smooth Loop: A to B and back every `seconds` while the toggle is on
    struct SAvatarLoop {
        float                              seconds = 0; // 0 = none
        std::vector<std::pair<int, float>> shapesA, shapesB;
        std::vector<SNodePose>             posesA, posesB;
    };

    // outfit toggle from the settings file (converted from VRChat)
    struct SAvatarToggle {
        std::string                        name, group; // group: the first of groups
        std::vector<std::string>           groups;      // exclusive with toggles sharing one
        bool                               on = false;
        std::vector<int>                   show, hide;  // parts; show ones stay hidden unless shown
        std::vector<std::pair<int, float>> shapes;      // morph weights while on
        std::vector<int>                   variants;
        std::vector<SNodePose>             poses;
        SAvatarLoop                        loop;
        std::vector<int>                   drop;        // nodes fixed in the world from turn-on
    };

    // radial puppet slider (settings file): morphs and poses lerp between keys, parts and variants step; 2D: -1..1 grid
    struct SAvatarSlider {
        struct SKey {
            float                              at = 0, atY = 0;
            std::vector<std::pair<int, float>> shapes;
            std::vector<int>                   show, hide, variants;
            std::vector<SNodePose>             poses;
        };
        std::string       name;
        float             value = 0, valueY = 0;
        int               grid  = 0;             // 2D: grid x grid keys, rows from the bottom; 0 = 1D
        std::vector<SKey> keys;                  // sorted by position
    };

    struct SAvatarNode {
        std::string name;
        int         parent = -1; // parents come before their children
        STRS        rest;
    };

    // skinning matrix = global(node) * inverseBind; unskinned meshes get their own joint with identity inverseBind
    struct SAvatarJoint {
        int node = 0;
        M4  inverseBind = M4::identity();
    };

    enum eAnimPath : uint8_t {
        PATH_T,
        PATH_R,
        PATH_S,
    };

    enum eInterp : uint8_t {
        INTERP_LINEAR,
        INTERP_STEP,
        INTERP_CUBIC,
    };

    struct SAnimChannel {
        int                node = 0;
        eAnimPath          path = PATH_T;
        eInterp            interp = INTERP_LINEAR;
        std::vector<float> times;
        std::vector<float> values; // 3 or 4 per key; cubic: in tangent, value, out tangent
    };

    struct SAnimClip {
        std::string               name;
        float                     duration = 0;
        float                     naturalSpeed = 0; // feet speed at rate 1 (m/s), 0 = unknown
        std::vector<SAnimChannel> channels;
    };

    enum eHumanBone : uint8_t {
        HB_HIPS,
        HB_SPINE,
        HB_CHEST,
        HB_UPPER_CHEST,
        HB_NECK,
        HB_HEAD,
        HB_L_UPPER_LEG,
        HB_L_LOWER_LEG,
        HB_L_FOOT,
        HB_R_UPPER_LEG,
        HB_R_LOWER_LEG,
        HB_R_FOOT,
        HB_L_SHOULDER,
        HB_L_UPPER_ARM,
        HB_L_LOWER_ARM,
        HB_L_HAND,
        HB_R_SHOULDER,
        HB_R_UPPER_ARM,
        HB_R_LOWER_ARM,
        HB_R_HAND,
        // optional; after the limb bones, which code takes by range
        HB_L_EYE,
        HB_R_EYE,
        HB_JAW,
        HB_L_TOES,
        HB_R_TOES,
        // then the fingers: see fingerBone()
        HB_FINGERS,
        HB_COUNT = HB_FINGERS + 30,
    };

    enum eFinger : uint8_t {
        FINGER_THUMB,
        FINGER_INDEX,
        FINGER_MIDDLE,
        FINGER_RING,
        FINGER_LITTLE,
        FINGER_COUNT,
    };
    // hand 0 left, 1 right; segment 0..2 from the palm (thumb 0 = metacarpal, VRM 0.x's proximal)
    constexpr int fingerBone(int hand, int finger, int segment) {
        return HB_FINGERS + (hand * FINGER_COUNT + finger) * 3 + segment;
    }

    enum eAvatarClip : uint8_t {
        CLIP_IDLE,
        CLIP_WALK,
        CLIP_RUN,
        CLIP_JUMP,
        CLIP_FALL,
        CLIP_COUNT,
    };

    // humanoid foot at rest, from the ankle (avatar space, meters, facing -Z): heel and ball ground contacts, toe tip
    struct SFootShape {
        V3   heel{0, -0.07f, 0.05f}, ball{0, -0.07f, -0.12f}, toe{0, -0.07f, -0.17f};
        bool measured = false; // else guessed from the height
    };

    // arm clearance (rest skin, avatar space, meters): body radius by height and bearing round the hips joint
    struct SBodyClearance {
        float              y0 = 0, dy = 0.02f; // row i: heights y0 + dy * i .. + dy
        int                rows = 0, bearings = 48; // column k: atan2(z, x) from -π + 2π k / bearings
        std::vector<float> out;                     // rows * bearings, 0 = none
        // per arm and bone (upper arm, forearm, hand), per quarter from its joint: skin radius
        std::array<std::array<std::array<float, 4>, 3>, 2> arm{};
        std::array<float, 2>                                hand{}; // hand length past the wrist
        bool                                                measured = false;
    };

    // morph target (blend shape) of one mesh node, sparse deltas
    struct SAvatarMorph {
        std::string name;
        int         gltfNode = -1, gltfMesh = -1; // what VRM expressions refer to it by
        int         target = 0;                   // index in the mesh
        float       rest   = 0;                   // mesh default weight
        uint32_t    first = 0, count = 0;         // in SAvatarModel::morphDeltas
    };

    struct SMorphDelta {
        uint32_t vertex;
        float    pos[3], normal[3];
    };

    // VRM 1.0 presets (VRM 0.x joy, sorrow, fun, a..o = happy, sad, relaxed, aa..oh)
    enum eExpressionPreset : uint8_t {
        EX_HAPPY,
        EX_ANGRY,
        EX_SAD,
        EX_RELAXED,
        EX_SURPRISED,
        EX_AA,
        EX_IH,
        EX_OU,
        EX_EE,
        EX_OH,
        EX_BLINK,
        EX_BLINK_L,
        EX_BLINK_R,
        EX_LOOK_UP,
        EX_LOOK_DOWN,
        EX_LOOK_LEFT,
        EX_LOOK_RIGHT,
        EX_NEUTRAL,
        EX_COUNT,
    };
    const char* expressionPresetName(int preset);

    // an expression's override of automatic blinking, look-at and mouth
    enum eOverride : uint8_t {
        OVERRIDE_NONE,
        OVERRIDE_BLOCK, // off while it shows at all
        OVERRIDE_BLEND, // fades out as it fades in
    };

    struct SExpression {
        struct SMorphBind {
            int   morph = 0;
            float weight = 1;
        };
        enum eMaterialProp : uint8_t {
            MP_COLOR,    // base color factor, rgba
            MP_EMISSIVE, // emissive factor, rgb
            MP_UV,       // texture scale xy, offset zw (atlas faces)
        };
        struct SMaterialBind {
            int           material = 0;
            eMaterialProp prop     = MP_COLOR;
            float         value[4] = {1, 1, 0, 0};
        };
        std::string                name;
        int                        preset = -1; // eExpressionPreset
        std::vector<SMorphBind>    morphs;
        std::vector<SMaterialBind> materials;
        bool                       binary   = false; // all or nothing
        bool                       shapeKey = false; // raw morph, not a defined expression
        bool                       viseme   = false; // lip sync consonant (SAvatarModel::consonant)
        eOverride                  overrideBlink = OVERRIDE_NONE, overrideLookAt = OVERRIDE_NONE, overrideMouth = OVERRIDE_NONE;
    };

    // how the eyes follow what the avatar looks at
    struct SLookAt {
        enum eType : uint8_t {
            NONE,
            BONES,
            EXPRESSIONS, // lookUp/Down/Left/Right
        } type = NONE;
        // per direction (h inner, h outer, v down, v up): input degrees at full, and output (bone degrees or weight)
        float range[4][2] = {{90, 10}, {90, 10}, {90, 10}, {90, 10}};
    };

    // VRChat's hand gestures
    enum eGesture : uint8_t {
        GESTURE_NEUTRAL,
        GESTURE_FIST,
        GESTURE_OPEN,
        GESTURE_POINT,
        GESTURE_VICTORY,
        GESTURE_ROCK,
        GESTURE_GUN,
        GESTURE_THUMBS_UP,
        GESTURE_COUNT,
    };
    const char* gestureName(int gesture);
    int         gestureFromName(std::string_view name); // -1 = none

    // spring bones (VRM's, or converted VRChat PhysBones). Colliders: sphere or offset..tail capsule keeping bones out
    // (INSIDE: PhysBones' inside bounds); plane through offset, normal tail - offset; disc round offset, normal tail -
    // offset, 2 * radius thick, round-edged, pushing out its nearest side
    enum eColliderKind : uint8_t {
        COLLIDER_OUTSIDE,
        COLLIDER_INSIDE,
        COLLIDER_PLANE,
        COLLIDER_DISC,
    };
    struct SSpringCollider {
        int           node = -1;
        V3            offset, tail; // node space
        float         radius = 0;   // meters
        eColliderKind kind   = COLLIDER_OUTSIDE;
        uint16_t      body   = 0; // body collider bit (startsIn), else 0
        float         disc   = 0; // disc's flat radius, meters
    };

    // spring rotation limit (VRMC_springBone_limit): cone round its y, hinge in its yz plane, or pitch and yaw
    enum eSpringLimit : uint8_t {
        LIMIT_NONE,
        LIMIT_CONE,
        LIMIT_HINGE,
        LIMIT_SPHERICAL,
    };

    struct SSpringJoint {
        int          node = -1, spring = 0;
        V3           tail;                 // node space: its child, or a made-up end
        float        length    = 0;        // meters, to the tail
        float        radius    = 0;        // meters, kept from colliders
        float        stiffness = 1;        // pull toward the animated pose
        float        drag      = 0.4f;     // 0..1, of its swing each step
        float        gravity   = 0;
        V3           gravityDir{0, -1, 0}; // in the world
        float        scale = 1;            // meters per unit of stiffness and gravity
        eSpringLimit limit = LIMIT_NONE;
        float        limitA = 0, limitB = 0; // radians: cone / hinge angle, or pitch and yaw
        Quat         limitFrame;             // in node space: y along the bone, then its turn
        uint16_t     startsIn = 0;           // limited: skip body colliders the tail starts in
    };

    struct SSpring {
        std::string      name;
        int              center = -1; // node the swing is relative to, -1 = the world
        std::vector<int> colliders;
        float            immobile = 0.9f; // no center: share of the air moving with the avatar
        // no center: 0..1 of the carrier's own motion the spring rides along with: PhysBones' Immobile (All Motion)
        float parentImmobile = 0;
        int   carrier        = -1;
    };

    // VRM 1.0 node constraint: a node turning with another
    struct SNodeConstraint {
        enum eType : uint8_t {
            ROLL,     // twists about its axis as the source does
            AIM,      // its axis points at the source
            ROTATION,
        } type = ROTATION;
        int   node = -1, source = -1;
        V3    axis{1, 0, 0}; // node space
        float weight = 1;
    };

    // emote retargeted to the avatar (built in, .vrma, or a model's clip), with its faces and hand gestures
    struct SAvatarEmote {
        std::string               name;
        std::string               from;   // "built in", "own clip", or its source file
        SAnimClip                 anim;   // on the avatar's nodes
        std::vector<SAnimChannel> faces;  // node = expression, value x = weight
        std::vector<SAnimChannel> eyes;   // look turn from +Z in head space, or none
        bool                      loop     = false;
        bool                      hold     = false; // last frame held until the avatar moves
        bool                      grounded = false; // feet stay on the ground (not a jump or fall)
        float                     speed    = 1;
        bool                      fingers  = false;
        std::array<int8_t, 2>     gesture{-1, -1};  // per hand, eGesture; -1 = the player's
        // settings file "sound" (a dance's song), looped with it; the dance follows its playback clock; null = none
        std::shared_ptr<const SSound> sound;
    };

    // attack: one arm's swing retargeted from a humanoid clip, adding trunk turns and setting the arms. Seconds: ready
    // = fists up (where a next swing starts), hit, next = the other arm may start, out = release begins
    struct SAvatarAttack {
        SAnimClip anim;
        float     ready = 0, hit = 0, next = 0, out = 0;
        // wrists (left, right) from the eyes in arm lengths, body frame (x left, y up, z ahead), at reachT: first
        // person puts the hands there for any build; empty = the clip's arms
        std::vector<float>             reachT;
        std::vector<std::array<V3, 2>> reach;
    };

    // walk / run body clip without legs (tools/blender/hyprwalk_walk.py): over a stride from the left heel strike, bone
    // turns from the T pose in the gait frame (+x left, +y up, +z ahead), the hips' move in hip heights, and means
    struct SGaitClip {
        std::string                             name;
        std::vector<std::array<Quat, HB_COUNT>> turn;
        std::vector<V3>                         move;
        std::array<bool, HB_COUNT>              has{};
        std::array<Quat, HB_COUNT>              mean{};
        V3                                      meanMove;
    };

    struct SAvatarModel {
        std::string                path, name;
        std::vector<SAvatarVertex> vertices;
        std::vector<uint32_t>      indices;
        std::vector<SMapImage>     images;
        std::vector<SMapMaterial>  materials;
        std::vector<SAvatarBatch>  batches; // opaque and alpha tested, then blended
        size_t                     blendFrom = 0;
        std::vector<SAvatarNode>   nodes;
        std::vector<SAvatarJoint>  joints;
        std::vector<SAnimClip>     clips;
        std::array<int, CLIP_COUNT> clipFor;  // -1 = none
        std::array<int, HB_COUNT>  human;     // node per bone, -1 = none
        bool                       humanoid = false;
        std::string                humanFrom; // "VRM", "VRM 1.0", "bone names" or the settings file
        int                        fingers = 0; // fingers with all three bones (of 10)
        // model space -> avatar space: feet at y = 0, centered, facing -Z, meters
        M4                         fix = M4::identity();
        float                      scale = 1;  // in fix
        float                      height = 0; // meters, standing
        V3                         forward{0, 0, 1}; // in model space
        size_t                     triangles = 0;
        std::array<SFootShape, 2>  feet;     // humanoid only; left, right
        SBodyClearance             clearance; // humanoid only
        // first person: the point between the eyes in head space (model units), its rest height (avatar space, meters)
        V3                         eyes;
        float                      eyeHeight = 0; // 0 = no first person body (needs a head and arms)

        std::vector<SAvatarMorph>  morphs;
        std::vector<SMorphDelta>   morphDeltas;                 // kept: morphs are applied on the CPU
        uint32_t                   morphFirst = 0, morphEnd = 0; // vertex range the morphs touch
        std::vector<SExpression>   expressions;                 // presets, its own, then its shape keys
        std::array<int, EX_COUNT>  preset;                      // expression per preset, -1 = none
        std::array<int, VISEME_COUNT - VOWEL_COUNT> consonant{-1, -1, -1, -1}; // pp, ff, ss, ch visemes: expression, -1 = none
        std::string                expressionsFrom;             // "VRM", "VRM 1.0", "shape key names", "" = none
        SLookAt                    lookAt;
        // expression per hand (left, right) and gesture, -1 = none
        std::array<std::array<int, GESTURE_COUNT>, 2> gestureFace;
        // expression per gesture pair (left, right), over one-hand faces; -1 = none, -2 = no such combination
        std::array<std::array<int, GESTURE_COUNT>, GESTURE_COUNT> gestureCombo;
        // own hand poses (settings file "hands"): finger rotations [finger * 3 + segment]; unset = the preset curl
        std::array<std::array<std::array<Quat, 15>, GESTURE_COUNT>, 2> handPose{};
        std::array<std::array<bool, GESTURE_COUNT>, 2>                 handPoseSet{};

        std::vector<SAvatarPart>   parts;
        std::vector<SAvatarToggle> toggles;
        std::vector<SAvatarSlider> sliders;
        std::vector<int>           fixed; // MA World Fixed Objects: stay where spawned
        std::string                settings; // its "<name>.hyprwalk.json", "" = none
        // KHR_materials_variants: names; per SAvatarBatch::variants, (variant, material) pairs; [0] = none
        std::vector<std::string>                      variants;
        std::vector<std::vector<std::pair<int, int>>> variantMaps{{}};

        std::vector<SSpring>         springs;
        std::vector<SSpringJoint>    springJoints;
        std::vector<SSpringCollider> springColliders;
        std::string                  springsFrom; // "VRM", "VRM 1.0", settings, "bone names", "" = none
        std::vector<SNodeConstraint> constraints; // sources before what depends on them

        // built in (humanoids), own non-walking clips, the settings file's, the request's; later replace same-named
        std::vector<std::shared_ptr<const SAvatarEmote>> emotes;
        // attacks per arm (left mirrors right), third and first person (fist raised into view); no channels = none
        std::array<SAvatarAttack, 2> attacks, attacksFirst;
        std::string                  attacksFrom; // "built in", or the settings file
        // walk and run body clips (walking, running); null = the procedural gait's
        std::array<std::shared_ptr<const SGaitClip>, 2> gaitClips;
        std::string                                     gaitFrom; // "built in", the settings file's files, "none"

        // by VRM 1.0 preset name ("happy", "blinkLeft"), VRM 0.x name ("joy") or own name; -1 = none
        int              findExpression(std::string_view name) const;
        int              findToggle(std::string_view name) const;
        int              findSlider(std::string_view name) const;
        int              findVariant(std::string_view name) const;
        std::vector<int> findParts(std::string_view name) const;
        std::vector<int> findMorphs(std::string_view name) const; // all of that name, or "mesh node/name"

        SAvatarModel() {
            clipFor.fill(-1);
            human.fill(-1);
            preset.fill(-1);
            for (auto& h : gestureFace)
                h.fill(-1);
            for (auto& h : gestureCombo)
                h.fill(-2);
        }

        // call after the renderer's GPU upload; morph data stays
        void releaseCpuData() {
            std::vector<SAvatarVertex>().swap(vertices); // (= {} keeps the capacity)
            std::vector<uint32_t>().swap(indices);
            for (auto& i : images)
                std::vector<uint8_t>().swap(i.rgba);
        }
    };

    struct SAvatarRequest {
        std::string              path;
        float                    height = 0; // meters; 0 = as modeled, rescaled if implausible
        std::vector<std::string> emotes;     // emote files: .vrma, or glTF / VRM with clips
        std::string              attack, attackFirst; // .vrma attacks, over settings file's and built in
    };

    struct SAvatarResult {
        SAvatarRequest                req;
        std::shared_ptr<SAvatarModel> model;
        std::string                   error;
        std::vector<std::string>      log;
    };

    SAvatarResult loadAvatar(const SAvatarRequest& req, const std::atomic<bool>& cancel);

    class CAvatarLoader : public CBackgroundLoader<SAvatarRequest, SAvatarResult> {
      public:
        CAvatarLoader() : CBackgroundLoader(loadAvatar) {}
    };

    // emotes from files, retargeted to a loaded avatar
    struct SEmoteRequest {
        std::string                         path; // display name for messages
        std::vector<std::string>            files;
        std::shared_ptr<const SAvatarModel> model;
    };

    struct SEmoteResult {
        SEmoteRequest                                    req;
        std::vector<std::shared_ptr<const SAvatarEmote>> emotes;
        std::string                                      error;
        std::vector<std::string>                         log;
    };

    SEmoteResult loadEmotes(const SEmoteRequest& req, const std::atomic<bool>& cancel);
    bool         isEmoteFile(std::string_view path); // .vrma, .glb, .gltf, .vrm

    class CEmoteLoader : public CBackgroundLoader<SEmoteRequest, SEmoteResult> {
      public:
        CEmoteLoader() : CBackgroundLoader(loadEmotes) {}
    };

    // first person with the body: camera in its eyes (CAvatarAnimator::eyes()), head hidden, hands in view
    enum eFirstHands : uint8_t {
        FPH_READY, // up front, low in view; gestures raised to show
        FPH_TOUCH, // right index finger reaching to the crosshair
        FPH_TYPE,  // lower, closer together, tapping on key presses
        FPH_HOLD,  // both out to a carried window, palms toward it
        FPH_DOWN,  // lowered out of view (playing a game in a window)
    };
    struct SFirstPerson {
        bool        on = false; // else the hands are the animation's
        V3          eye;        // camera position, world
        float       yaw = 0, pitch = 0; // yaw 0 = -z, right > 0; up > 0; kept while off
        eFirstHands hands = FPH_READY;
        float       room = 1e9f;  // meters ahead of the eye free for the hands
        int         tap  = -1;    // FPH_TYPE: hand to tap a key this frame (0 L, 1 R)
        bool        press = false; // FPH_TOUCH: a button went down this frame
    };

    // first person camera at the eyes (world): follows the feet and yaw at once, the eyes through a dead zone (no
    // wobble from breathing or bob), eased; within `reach` across and `low`..`high` up
    struct SFirstPersonEye {
        V3    off, v;  // offset from the feet in the yaw-0 frame; velocity
        V3    held;    // dead-zone eye target (same frame)
        bool  live  = false;
        float reach = 1e9f, low = 0.f, high = 1e9f;
        V3    update(const V3& feet, const V3& eyes, float yaw, float dt); // the camera (world)
        V3    at(const V3& feet, float yaw) const;                         // ... the camera now, no update
        void  reset() {
            live = false;
        }
    };

    // what the player is doing, for picking the animation
    struct SAvatarMotion {
        float dt        = 0;
        float speed     = 0; // horizontal, m/s
        V3    vel;           // world, horizontal; zero = along the body at `speed`
        V3    wish;          // keys' velocity, horizontal
        float accel = 10, decel = 14, turnBack = 7; // m/s² of vel -> wish: speeding, slowing, reversing
        float vy        = 0;
        bool  onGround  = true;
        bool  flying    = false;
        bool  crouched  = false;
        bool  run       = false; // Shift: run, else walk
        float lookPitch = 0; // camera relative to the body, radians, up > 0
        float lookYaw   = 0; // (right > 0)
        M4    world = M4::identity(); // avatar -> world, no lift(); springs react to it
        // ground height (world y) under (x, z) near `y`, for placing feet; empty or no hit = flat at the feet
        std::function<std::optional<float>(float x, float z, float y)> ground;
        SFirstPerson fp;
    };

    class CAvatarAnimator {
      public:
        void reset(std::shared_ptr<const SAvatarModel> model);
        void update(const SAvatarMotion& m);

        // 3 rows of each joint's 3x4 skinning matrix (12 floats per joint)
        const std::vector<float>& joints() const {
            return m_joints;
        }
        // meters to raise the avatar by so its feet touch the ground
        float lift() const {
            return m_lift;
        }
        // what's playing, for the status
        std::string playing() const;
        // gait status JSON: gait, phase, each foot planted or not; "null" when not walking procedurally
        std::string gaitStatus() const;
        // body yaw (radians, right > 0) turned toward `want` like a person: step-limited, eased so taps don't jerk it
        float turnBody(float yaw, float want, float dt, float speed);
        // third person: the yaw to face for m.wish (none if idle or not third person), the heading a moment ahead; on
        // quick back-and-forth taps it keeps its facing or faces keysYaw's W or S way and steps until one way is held
        std::optional<float> wayToFace(const SAvatarMotion& m, bool thirdPerson, float bodyYaw, float keysYaw);
        // turnBody's remaining turn (radians, right > 0)
        float turnLeft() const {
            return m_turnLeft;
        }
        // node transforms this frame, model space
        const std::vector<M4>& globals() const {
            return m_global;
        }
        // first person eye point this frame (avatar space, before lift()); none without a first person body
        std::optional<V3> eyes() const;
        // first person pose weight (trunk to the camera, hands in view), eased
        float firstPerson() const {
            return m_fpW;
        }
        // ... the arms' weight (less under emotes, FPH_DOWN, looking far down)
        float firstPersonArms() const {
            return m_fpArmsW;
        }

        // the face; see SAvatarModel::findExpression
        void setExpression(int expression, float weight = 1); // held until changed, -1 = none
        int  expression() const {
            return m_held;
        }
        void setGesture(int hand, int gesture); // hand 0 left, 1 right; eGesture
        // lip sync (CLipSync): vowel presets and the avatar's consonants, under the face's mouth override
        void setVisemes(const SVisemes& weights) {
            m_visemes = weights;
        }
        int  gesture(int hand) const {
            return m_gesture[hand & 1];
        }
        void setAutoBlink(bool on) {
            m_autoBlink = on;
        }
        // per SAvatarModel::morphs
        const std::vector<float>& morphWeights() const {
            return m_morphW;
        }
        // materials as expressions change them; null when none do
        const std::vector<SMapMaterial>* materials() const {
            return m_materials.empty() ? nullptr : &m_materials;
        }

        // outfit: toggles, and parts and shape keys set by hand over them
        void setToggle(int toggle, bool on); // turns the others of its group off
        bool toggle(int toggle) const {
            return toggle >= 0 && toggle < (int)m_toggles.size() && m_toggles[toggle];
        }
        void setPart(int part, int shown);        // 1 shown, 0 hidden, -1 as the toggles say
        void resetOutfit();                       // as the settings file has it
        void setShape(int morph, float weight);   // NaN = as the toggles say
        float shape(int morph) const;             // weight without expressions
        void  setSlider(int slider, float value, float valueY = NAN); // 0..1 (2D: -1..1 each), NaN = where it starts
        float slider(int slider) const;
        float sliderY(int slider) const; // a 2D slider's second axis
        // per SAvatarModel::parts, 1 = drawn
        const std::vector<uint8_t>& partsShown() const {
            return m_shown;
        }
        // material per batch; null without material variants
        const std::vector<int>* batchMaterials() const {
            return m_batchMat.empty() ? nullptr : &m_batchMat;
        }
        bool variant(int v) const { // a material variant in effect
            return v >= 0 && v < (int)m_variantOn.size() && m_variantOn[v];
        }

        // gait style: walk / run body from SGaitClip, else procedural; physics off: spring bones hang as animated
        void setGaitStyle(bool on) {
            m_gaitStyle = on;
        }
        bool gaitStyle() const {
            return m_gaitStyle;
        }
        void setPhysics(bool on);
        bool physics() const {
            return m_physics;
        }

        // emotes: the model's and added ones; one plays over the rest until done or the avatar moves
        const std::vector<std::shared_ptr<const SAvatarEmote>>& emotes() const {
            return m_emotes;
        }
        int  findEmote(std::string_view name) const;               // loose name match or number from 1; -1 = none
        int  addEmote(std::shared_ptr<const SAvatarEmote> emote); // made for this model; replaces one of that name
        void playEmote(int emote, int loop = -1);                  // 1 loops, 0 once, -1 = the emote's own
        void stopEmote();
        int  emote() const { // playing, -1 = none
            return m_emoteOut ? -1 : m_emote;
        }
        // start count (replays count), looping, and time into it (seconds)
        uint32_t emoteStarts() const {
            return m_emoteStarts;
        }
        bool emoteLooping() const {
            return m_emoteLoop;
        }
        float emoteTime() const {
            return m_emoteTime;
        }
        // next update only: the emote's sound position (seconds, CSpeaker::clock()) for the dance to keep time with
        void setEmoteClock(double seconds) {
            m_emoteClock = seconds;
        }

        // punch ahead (body facing, or camera in first person) over the legs' motion; asked mid-swing, the other arm
        // follows the hit. hand: 0 left, 1 right, -1 next; stops an emote; false without arms to swing
        bool        attack(int hand = -1);
        // JSON: per arm swing time and body weight, next and last arm, swing count, chest turn from the hips (radians)
        std::string attackStatus() const;

      private:
        enum eState : uint8_t {
            ST_IDLE,
            ST_WALK,
            ST_RUN,
            ST_AIR,
        };
        enum eSource : uint8_t {
            SRC_REST,
            SRC_CLIP,
            SRC_PROC,
        };
        struct SSource {
            eSource kind   = SRC_REST;
            int     clip   = -1;
            bool    frozen = false; // hold the first frame (an idle made from a walk)
            bool    loop   = true;
            bool    operator==(const SSource&) const = default;
        };

        SSource choose(eState st, const SAvatarMotion& m) const;
        void    gait(const SAvatarMotion& m, bool air, std::vector<STRS>& pose); // procedural walk (humanoids)
        void    fingerAxes(); // m_fingerAxes, m_thumbPose, m_handRig, m_clipFingers
        void    hands(float dt, std::vector<STRS>& pose);
        void    look(const SAvatarMotion& m, std::vector<STRS>& pose, float weight, float yawOff = 0) const;
        // first person: hand state (firstPersonState), trunk turn toward the camera (returns it), arms to the hands
        void    firstPersonState(const SAvatarMotion& m, float emoteW);
        float   firstPersonTrunk(const SAvatarMotion& m, std::vector<STRS>& pose) const;
        void    firstPersonArms(const SAvatarMotion& m, std::vector<STRS>& pose);
        // arm s (0 left) to `wrist`, elbow toward `elbow`, hand along `along`, palm to `palm` (model space), by w
        void    reachArm(std::vector<STRS>& pose, int s, const V3& wrist, const V3& elbow, const V3& along, const V3& palm, float w);
        // attacks: swing state and m_attackPose; trunk turn; arms (first person: within the room ahead)
        void    attackState(const SAvatarMotion& m);
        void    attackTrunk(std::vector<STRS>& pose) const;
        void    attackArms(const SAvatarMotion& m, std::vector<STRS>& pose);
        // two-bone IK: arm s's wrist w of the way to `wrist` (model space), elbow in its bend plane, hand rotation kept
        void    armTo(std::vector<STRS>& pose, int s, const V3& wrist, float w) const;
        void    emotePose(float dt, std::vector<STRS>& pose); // the emote over the pose, m_emoteFace
        void    face(const SAvatarMotion& m, std::vector<STRS>& pose);
        float   random(); // 0..1
        void    rotateBone(std::vector<STRS>& pose, int bone, const Quat& d) const;
        void    globals(const std::vector<STRS>& pose, std::vector<M4>& out) const;
        float   lowestFoot(const std::vector<M4>& g) const;
        void    outfit(); // m_shown, m_shapeBase from toggles and overrides
        void    constrain(std::vector<STRS>& pose) const;
        M4      globalOf(const std::vector<STRS>& pose, int node) const;
        void    springs(const SAvatarMotion& m);
        enum eSpringPass : uint8_t {
            SP_START, // tails where the bones point
            SP_STEP,  // a step of t seconds
            SP_SHOW,  // bones turned to the tails, t into the next step
        };
        // world: model space -> world; pose: m_global at that time
        void                   springPass(eSpringPass pass, const M4& world, float t, const std::vector<M4>& pose);
        const std::vector<M4>& springPoseAt(float u);

        std::shared_ptr<const SAvatarModel> m_model;
        std::vector<STRS>                   m_pose, m_from, m_target;
        std::vector<M4>                     m_global, m_restGlobal;
        std::vector<float>                  m_joints;
        std::vector<Quat>                   m_restGlobalRot; // per node
        SSource                             m_source;
        eState                              m_state    = ST_IDLE;
        float                               m_fade     = 1; // 0 -> 1 from m_from to the new source
        float                               m_clipTime = 0;
        float                               m_time     = 0;
        float                               m_airTime  = 0;
        float                               m_lift = 0, m_restFootY = 0;
        bool                                m_first = true;
        // procedural gait state (avatar.cpp's SGait)
        std::shared_ptr<void>               m_gait;
        bool                                m_gaitUsed = false; // last frame
        bool                                m_flying   = false; // last frame
        bool                                m_flewOff  = false; // flew since last grounded: airborne at once
        // pose jumps (leaving the ground, landing, taking off): per node, the shown pose's rotation and offset from the
        // new one, with rates, decaying critically damped at m_settleRate (0 = none); asked via m_settleAsk
        std::vector<V3>                     m_settleR, m_settleRV, m_settleT, m_settleTV;
        std::vector<STRS>                   m_settleNow, m_settleWas; // shown pose (before look and hands), last two frames
        float                               m_settleRate = 0, m_settleAsk = 0;
        float                               m_settleLift = 0, m_settleLiftWas = 0; // jump-off / landing: share of hip motion kept
        V3                                  m_settleRoot, m_settleRootV; // avatar position last frame (world), velocity
        bool                                m_settleRootSet = false;
        bool                                m_settleJust = false, m_settleFresh = false; // started this frame; last frame
        std::vector<STRS>                   m_settleNew; // new pose when it started (for its rate next frame)
        float                               m_settleDt   = 0; // previous frame's dt
        int                                 m_turnSide = 0;     // turning round: 1 right, -1 left
        float                               m_turnRate = 0;     // body turn rate, radians/s
        float                               m_turnYaw  = 0;     // last turnBody result; a different yaw restarts
        float                               m_turnLeft = 0;     // remaining turn, radians, in its direction
        bool                                m_turnKnown = false; // turnBody ran since the gait last checked
        // wayToFace: last key way, clock, last reversal, since when this way, holding the facing
        V3                                  m_wayLast;
        float                               m_wayClock = 0, m_wayBack = -1e9f, m_waySince = 0;
        bool                                m_wayHold = false;
        float                               m_wayYaw  = 0;
        V3                                  m_wayMean; // body velocity averaged over WAY_MEAN

        // face
        std::vector<float>                  m_exprW;   // per expression: held and gesture faces, fading
        std::vector<float>                  m_exprOut; // what's applied, blinking and looking included
        std::vector<float>                  m_matOut;  // m_exprOut when the materials were last made
        std::vector<float>                  m_morphW;
        std::vector<SMapMaterial>           m_materials;
        int                                 m_held = -1;
        float                               m_heldWeight = 1;
        SVisemes                            m_visemes{};
        std::array<uint8_t, 2>              m_gesture{};
        int                                 m_lastHand  = 1;
        bool                                m_autoBlink = true;
        float                               m_blinkIn = 2, m_blinkT = -1; // till the next blink; into this one (-1 = none)
        bool                                m_blinkAgain = false;
        float                               m_saccadeIn = 1, m_saccadeYaw = 0, m_saccadePitch = 0; // of how far the eyes turn
        uint32_t                            m_rng = 0x2545F491;

        // fingers: the gesture's pose over the animation's
        struct SHandPose {
            std::array<float, FINGER_COUNT> curl{}, spread{}; // 1 = a fist; spread toward the thumb
            std::array<Quat, 3>             thumb{};          // thumb bone rotations at rest
        };
        struct SFingerAxes {
            V3 curl, spread; // rest model space: toward palm, toward thumb
        };
        std::array<std::array<SFingerAxes, FINGER_COUNT * 3>, 2>       m_fingerAxes{};
        std::array<std::array<std::array<Quat, 3>, GESTURE_COUNT>, 2> m_thumbPose{}; // per hand and gesture, as thumb
        std::array<bool, 2>                 m_handRig{};
        std::array<SHandPose, 2>            m_hand{};
        std::array<float, 2>                m_handW{}; // gesture pose weight over the animation
        std::array<std::array<Quat, 15>, 2> m_handQ{};      // own hand pose (SAvatarModel::handPose), eased in
        std::array<float, 2>                m_handCustom{}; // its weight over the preset
        std::vector<uint8_t>                m_clipFingers; // per clip: animates the fingers

        // outfit
        std::vector<uint8_t>                m_toggles;
        std::vector<float>                  m_sliders;   // per slider, 0..1 (2D: x, -1..1)
        std::vector<float>                  m_slidersY;  // per slider: a 2D one's y
        std::vector<int8_t>                 m_partSet;   // per part: 1 shown, 0 hidden, -1 as the toggles say
        std::vector<float>                  m_shapeSet;  // per morph, NaN = as the toggles say
        std::vector<uint8_t>                m_shown;
        std::vector<float>                  m_shapeBase; // per morph: rest, toggles, sliders, set by hand
        std::vector<uint8_t>                m_variantOn;
        std::vector<int>                    m_batchMat;  // per batch, when the model has material variants
        std::vector<STRS>                   m_nodePose;  // per node rest from toggles, sliders; empty = rest
        std::vector<int>                    m_loops;     // toggles on with a loop
        std::vector<int>                    m_dropBy;    // per node dropped in the world: the toggle, else -1
        std::vector<M4>                     m_dropAt;    // per dropped node: its world placement
        std::vector<uint8_t>                m_dropTake;  // per node: just dropped, take its current place
        void                                loopPoses(std::vector<STRS>& pose) const;
        void                                drops(const SAvatarMotion& m);          // nodes left in the world

        // emotes
        std::vector<std::shared_ptr<const SAvatarEmote>> m_emotes;
        int                                 m_emote     = -1;
        float                               m_emoteTime = 0;
        bool                                m_emoteLoop = false;
        bool                                m_emoteOut  = false; // fading out: done, stopped, or the avatar moved
        float                               m_emoteW    = 0;     // its weight over the rest
        float                               m_emoteSwap = 1;     // 0 -> 1 from m_emoteFrom (chained emotes)
        uint32_t                            m_emoteStarts = 0;
        double                              m_emoteClock  = -1; // setEmoteClock()'s, -1 = none
        std::vector<STRS>                   m_emoteNow, m_emoteFrom;
        std::vector<float>                  m_emoteFace; // per expression weight
        float                               m_emoteEyes = 0, m_emoteYaw = 0, m_emotePitch = 0; // its eyes' weight; degrees

        // first person weights; per hand, a sprung wrist target in camera space (x right, y up, -z ahead; arm lengths)
        struct SFpHand {
            V3    at, v;
            V3    along, palm;
            float pitch = 0; // frame pitch: the camera's for touch / hold, else less
            float tap   = 0; // seconds since its last key tap; large = none
        };
        float                  m_fpW = 0, m_fpArmsW = 0; // overall (camera); arms (less: emotes, FPH_DOWN)
        float                  m_fpTrunkW = 0;           // trunk turn to the camera (less: emotes)
        std::array<SFpHand, 2> m_fpHand{};
        bool                   m_fpLive = false;    // hands placed (else they start at their targets)
        float                  m_fpYaw = 0, m_fpPitch = 0; // the camera last frame
        V3                     m_fpEye;             // ... where it was (world)
        // camera the hands and trunk follow; while off, the last one (eye in avatar space, yaw relative to the body)
        V3                     m_fpViewEye, m_fpEyeAt;
        float                  m_fpViewYaw = 0, m_fpViewPitch = 0, m_fpYawFrom = 0;
        V3                     m_fpLag, m_fpLagV;   // hand lag behind camera turns and moves (arm lengths)
        float                  m_fpPoke = 1;        // seconds since a button press (touch): finger poke
        float                  m_fpPhase = 0;       // stride phase for hand bob without procedural gait
        std::array<int8_t, 2>  m_fpGesture{-1, -1}; // first person finger gestures; -1 = the player's

        // attacks: current swing (seconds into its clip, < 0 = none) and the previous one under it (m_swingCross, eased
        // over ATTACK_CROSS); body weight; per node what attacks set (ATTACK_TRUNK, ATTACK_ARM), their pose
        struct SSwing {
            float t    = -1;
            int   hand = 1;
            bool  fp   = false;
        };
        SSwing                m_swing, m_swingWas;
        float                 m_swingCross = 1, m_attackW = 0;
        int                   m_swingNext = -1; // arm queued during a swing, after its hit
        int                   m_swingLast = 0;
        float                 m_swingAgo  = 1e9f; // seconds since a swing last started
        int                   m_swings    = 0;
        std::array<bool, 2>   m_swingFist{};    // the hands made fists for it
        std::vector<uint8_t>  m_attackPart;
        std::vector<STRS>     m_attackPose, m_attackWas;
        void                  swingStart(int hand, bool fp);
        const SAvatarAttack&  attackOf(const SSwing& sw) const;

        // springs, stepped at a fixed rate
        struct SColliderAt {
            V3            a, b; // world capsule ends; plane / disc: point, + normal
            float         radius;
            eColliderKind kind;
            float         disc; // disc's flat radius
        };
        bool                                m_physics = true;
        bool                                m_gaitStyle = true;
        std::vector<int>                    m_springOf;   // per node: its joint, -1 = none, -2 = below one
        int                                 m_springFrom = 0; // the first node the springs move
        std::vector<V3>                     m_tail, m_tailPrev; // per joint, in its spring center's space
        std::vector<V3>                     m_carried;          // per joint: carrier's carry last step (world)
        std::vector<V3>                     m_again;            // per joint: what re-pushing it would do (parent frame)
        std::vector<M4>                     m_carrierAt, m_carrierMove; // per spring: carrier at last step (world); its move
        std::vector<M4>                     m_springGlobal;     // per spring-moved node, world
        std::vector<M4>                     m_centerAt, m_centerInv; // per spring: center -> world
        std::vector<SColliderAt>            m_colliderAt; // per collider, in the world
        std::vector<int>                    m_springUp;   // nodes the springs read, and their ancestors
        std::vector<STRS>                   m_springWas;  // the pose (m_pose) last frame
        std::vector<M4>                     m_springPose; // m_global between the two (springPoseAt())
        float                               m_springAcc  = 0; // time into the next step
        bool                                m_springLive = false;
        STRS                                m_springBody;                    // model space (feet on ground) -> world, last frame
        M4                                  m_springStepAt = M4::identity(); // the same at the last step
        M4                                  m_springMove   = M4::identity(); // the avatar's move in the world over this step
    };
}
