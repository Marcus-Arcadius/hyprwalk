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

// The body seen in third person: a skinned glTF / GLB / VRM the user brings.
// Its own clips play when their names say what they are (idle, walk, run,
// jump, fall); humanoids without them walk procedurally: steps planted where
// the feet land, the body carried over them as people walk and run.

namespace h3d {

    // skinned vertex: pos and normal are in the space of the mesh, joints index SAvatarModel::joints
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
        int      variants = 0;         // SAvatarModel::variantMaps: the materials it has in material variants, 0 = none
        // first person, the camera in the eyes: its indices but the head's (and its hair's), a copy of them after all the
        // batches' own (theirs stay as they were, in order); fpCount UINT32_MAX: it has none of the head's, all of it
        uint32_t fpFirst = 0, fpCount = UINT32_MAX;
    };

    // a mesh node: what outfit toggles show and hide
    struct SAvatarPart {
        std::string name;
        int         node = -1, gltfNode = -1;
        size_t      triangles = 0;
        bool        hidden    = false; // until shown (the settings file's "hidden")
    };

    struct STRS {
        V3   t;
        Quat r;
        V3   s{1, 1, 1};

        M4 matrix() const {
            return M4::trs(t, r, s);
        }
    };

    // a node a toggle or a slider puts elsewhere (a VRChat animation of its Transform, converted): what it sets of
    // the node's own translation, rotation and scale
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

    // a toggle's smooth loop (VRCFury's Smooth Loop, like breathing): from a to b and back every `seconds` while it's on
    struct SAvatarLoop {
        float                              seconds = 0; // 0 = none
        std::vector<std::pair<int, float>> shapesA, shapesB;
        std::vector<SNodePose>             posesA, posesB;
    };

    // an outfit toggle from the avatar's settings file (VRChat's, converted)
    struct SAvatarToggle {
        std::string                        name, group; // group: the first of groups
        std::vector<std::string>           groups;      // exclusive with every toggle that shares one of these
        bool                               on = false;  // at first
        std::vector<int>                   show, hide;  // parts: hidden unless a toggle that shows them is on; hidden while on
        std::vector<std::pair<int, float>> shapes;      // morph weights while on
        std::vector<int>                   variants;    // material variants in effect while on
        std::vector<SNodePose>             poses;       // nodes put elsewhere while on
        SAvatarLoop                        loop;        // plays while on
        std::vector<int>                   drop;        // nodes that stay in the world where they were when it turned on
    };

    // a slider from the settings file (a VRChat radial puppet, converted): 0..1 through its keys. Morph weights and
    // node poses go in straight lines from key to key; parts and material variants are as the last key at or below the
    // value has them. A 2D one (a puppet's two axes) goes -1..1 each way over a grid of keys, blended between the four
    // around the value; parts and variants as the nearest has them
    struct SAvatarSlider {
        struct SKey {
            float                              at = 0, atY = 0;
            std::vector<std::pair<int, float>> shapes;
            std::vector<int>                   show, hide, variants;
            std::vector<SNodePose>             poses;
        };
        std::string       name;
        float             value = 0, valueY = 0; // at first
        int               grid  = 0;             // 2D: grid x grid keys, row by row from the bottom; 0 = 1D
        std::vector<SKey> keys;                  // by where they are
    };

    struct SAvatarNode {
        std::string name;
        int         parent = -1; // parents come before their children
        STRS        rest;
    };

    // skinning matrix = global(node) * inverseBind. Meshes that aren't skinned
    // hang off a joint of their own with an identity inverseBind.
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
        float                     naturalSpeed = 0; // m/s the feet move at when played at rate 1, 0 = unknown
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
        // optional, after the ones the limbs count along
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
    // hand 0 left, 1 right; segment 0..2 out from the palm (the thumb's 0 is its metacarpal, VRM 0.x's proximal)
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

    // a humanoid's foot as it stands at rest, in avatar space (meters, facing -Z) from its ankle: where the heel and
    // the ball of the foot touch the ground, and the tips of the toes. Walking rolls the foot over the heel as it
    // lands and over the ball as it pushes off
    struct SFootShape {
        V3   heel{0, -0.07f, 0.05f}, ball{0, -0.07f, -0.12f}, toe{0, -0.07f, -0.17f};
        bool measured = false; // from the mesh, else a guess from the height
    };

    // What a humanoid's arms keep clear of, from its skin at rest (avatar space, meters): how far out the body goes (a
    // skirt, a coat, the hips) round a line up through the hips' joint, by height over that joint and bearing; and how
    // thick each arm's skin is (a sleeve, a glove) round its bones
    struct SBodyClearance {
        float              y0 = 0, dy = 0.02f; // row i is the heights y0 + dy * i .. + dy
        int                rows = 0, bearings = 48; // column k: atan2(z, x) from -π + 2π k / bearings
        std::vector<float> out;                     // rows * bearings, 0 = nothing there
        // per arm (left, right), for the upper arm, the forearm and the hand, by quarter along it from its joint: how far
        // its skin goes out round the line through it
        std::array<std::array<std::array<float, 4>, 3>, 2> arm{};
        std::array<float, 2>                                hand{}; // how far the hand's skin goes on from the wrist
        bool                                                measured = false;
    };

    // a morph target (blend shape) of one mesh node; its deltas are sparse
    struct SAvatarMorph {
        std::string name;
        int         gltfNode = -1, gltfMesh = -1; // what VRM expressions refer to it by
        int         target = 0;                   // index in the mesh
        float       rest   = 0;                   // the mesh's default weight
        uint32_t    first = 0, count = 0;         // in SAvatarModel::morphDeltas
    };

    struct SMorphDelta {
        uint32_t vertex;
        float    pos[3], normal[3];
    };

    // VRM 1.0's preset expressions (VRM 0.x's joy, sorrow, fun and a..o are happy, sad, relaxed and aa..oh)
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

    // what an expression does to the automatic blinking, looking around and mouth while it shows
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
        bool                       shapeKey = false; // one of the model's morphs by its own name, not an expression it defines
        bool                       viseme   = false; // a consonant's mouth, for lip sync (SAvatarModel::consonant)
        eOverride                  overrideBlink = OVERRIDE_NONE, overrideLookAt = OVERRIDE_NONE, overrideMouth = OVERRIDE_NONE;
    };

    // how the eyes follow what the avatar looks at
    struct SLookAt {
        enum eType : uint8_t {
            NONE,
            BONES,       // the eye bones turn
            EXPRESSIONS, // lookUp/Down/Left/Right
        } type = NONE;
        // horizontal inner, horizontal outer, vertical down, vertical up: the angle (degrees)
        // at which it's all the way, and how far that is (degrees of the eye bone, or a weight)
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

    // spring bones (VRM's, or VRChat's PhysBones converted): hair, skirts, tails and accessories
    // that swing. A collider is a sphere, or a capsule from offset to tail, that keeps the bones out (or in: PhysBones'
    // inside bounds), or a plane through offset they keep to the side of that tail - offset points to, or a flat round
    // disc round offset facing that way, as thick as twice its radius, round at its edge (a tutu, a hat's brim): what
    // gets on it is pushed off its nearest side, never further in, as a ring of capsules round it would
    enum eColliderKind : uint8_t {
        COLLIDER_OUTSIDE,
        COLLIDER_INSIDE,
        COLLIDER_PLANE,
        COLLIDER_DISC,
    };
    struct SSpringCollider {
        int           node = -1;
        V3            offset, tail; // in the node's space
        float         radius = 0;   // meters
        eColliderKind kind   = COLLIDER_OUTSIDE;
        uint16_t      body   = 0; // one made up for the humanoid's body: its bit (for SSpringJoint::startsIn), else 0
        float         disc   = 0; // a disc's: meters from its middle to where its edge starts rounding
    };

    // how far a spring's bone may turn (VRMC_springBone_limit's, as VRChat's PhysBones have them): within a cone round
    // the limit's y, in its yz plane up to an angle from its y (a hinge), or a pitch and yaw from its y (spherical)
    enum eSpringLimit : uint8_t {
        LIMIT_NONE,
        LIMIT_CONE,
        LIMIT_HINGE,
        LIMIT_SPHERICAL,
    };

    struct SSpringJoint {
        int          node = -1, spring = 0;
        V3           tail;                 // what it points at, in the node's space: its child, or a made-up end
        float        length    = 0;        // meters, to the tail
        float        radius    = 0;        // meters, how far it keeps from the colliders
        float        stiffness = 1;        // pull back to where the animation has it
        float        drag      = 0.4f;     // 0..1, of its swing each step
        float        gravity   = 0;        // pull along gravityDir
        V3           gravityDir{0, -1, 0}; // in the world
        float        scale = 1;            // meters per unit of stiffness and gravity
        eSpringLimit limit = LIMIT_NONE;
        float        limitA = 0, limitB = 0; // radians: the cone's or the hinge's angle; or the pitch, and the yaw
        Quat         limitFrame;             // the limit's frame in the node's space: y along the bone, then the limit's own turn
        uint16_t     startsIn = 0;           // with a limit: the body's colliders (SSpringCollider::body) its tail starts inside of, left out
    };

    struct SSpring {
        std::string      name;
        int              center = -1; // the node it moves with (the swing is relative to it), -1 = the world
        std::vector<int> colliders;
        float            immobile = 0.9f; // with no center, 0..1: how much of the air it drags in moves along with the avatar
        // with no center, 0..1: how much of what the node it hangs from (carrier) does beyond where the avatar goes (the walk's
        // bob and sway, a turn, a dance) it's carried along with, not swung by: VRChat PhysBones' Immobile ("All Motion")
        float parentImmobile = 0;
        int   carrier        = -1;
    };

    // VRM 1.0's node constraints: a node that turns along with another one
    struct SNodeConstraint {
        enum eType : uint8_t {
            ROLL,     // turns around its axis as far as the source does around it
            AIM,      // its axis points at the source
            ROTATION, // turns as the source does
        } type = ROTATION;
        int   node = -1, source = -1;
        V3    axis{1, 0, 0}; // in the node's space
        float weight = 1;
    };

    // an emote: a clip made for the avatar (from a built in one, a VRM animation, or a clip of its own or
    // another model's), and the faces and hand gestures it makes
    struct SAvatarEmote {
        std::string               name;
        std::string               from;   // "built in", "own clip", or the file it came from
        SAnimClip                 anim;   // on the avatar's nodes
        std::vector<SAnimChannel> faces;  // node = an expression, the value's x = its weight
        std::vector<SAnimChannel> eyes;   // none, or where they look: a turn from straight ahead (+Z) in the head
        bool                      loop     = false; // over and over (else once)
        bool                      hold     = false; // its last frame stays till the avatar moves
        bool                      grounded = false; // the feet stay on the ground (not a jump, a fall)
        float                     speed    = 1;     // how fast it plays
        bool                      fingers  = false; // it moves the fingers
        std::array<int8_t, 2>     gesture{-1, -1};  // per hand, eGesture; -1 = the player's
        // what it plays while it plays (the settings file's "sound": a dance's song), from its start and round with it
        // as it loops; the dance keeps time with what's heard of it. Null = none
        std::shared_ptr<const SSound> sound;
    };

    // an attack (CAvatarAnimator::attack()): an arm's swing, a clip made for the avatar from one made for any humanoid
    // (built in, or the settings file's "attack"), on the trunk (spine to head), which it turns as it turns them from
    // rest over what the body does, and the arms (collarbones to hands), which it has as it has them. When the fists
    // are up (a swing after another starts there), when it strikes, when the other arm's may start, and when it starts
    // letting go of the body (seconds)
    struct SAvatarAttack {
        SAnimClip anim;
        float     ready = 0, hit = 0, next = 0, out = 0;
        // where each wrist (left, right) was from the eyes on the avatar it was made on, in its arm's lengths, the way the
        // body faced (x its left, y up, z ahead), at times through the clip: first person puts the hands there in the view
        // whatever the avatar's build (none: as the clip has the arms)
        std::vector<float>             reachT;
        std::vector<std::array<V3, 2>> reach;
    };

    // the walk's or the run's body (but the legs: the walking steps the feet itself), a clip made in Blender
    // (tools/blender/h3d_walk.py): through one stride (0: the left heel lands, 0.5: the right), every humanoid bone's
    // turn from the T pose in the frame the walking poses in (+x the body's left, +y up, +z ahead) and the hips' move
    // (in hips heights), at even steps of it; and the mean of each over the stride
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
        size_t                     blendFrom = 0; // first blended batch
        std::vector<SAvatarNode>   nodes;
        std::vector<SAvatarJoint>  joints;
        std::vector<SAnimClip>     clips;
        std::array<int, CLIP_COUNT> clipFor;  // -1 = none
        std::array<int, HB_COUNT>  human;     // node per bone, -1 = none
        bool                       humanoid = false;
        std::string                humanFrom; // "VRM", "VRM 1.0", "bone names", the settings file's name
        int                        fingers = 0; // of the 10, how many have all three bones
        // model space -> avatar space: feet at y = 0, centered, facing -Z, meters
        M4                         fix = M4::identity();
        float                      scale = 1;  // in fix
        float                      height = 0; // meters, standing
        V3                         forward{0, 0, 1}; // in model space
        size_t                     triangles = 0;
        std::array<SFootShape, 2>  feet;     // a humanoid's, left and right
        SBodyClearance             clearance; // a humanoid's
        // first person (a humanoid's): the eyes, between them, in the head's space (model units), and how far up they
        // are at rest (avatar space, meters); its batches without the head's triangles: SAvatarBatch::fpFirst
        V3                         eyes;
        float                      eyeHeight = 0; // 0: it can't be seen from inside (not a humanoid with a head and arms)

        std::vector<SAvatarMorph>  morphs;
        std::vector<SMorphDelta>   morphDeltas;                 // kept: the renderer adds up the morphs on the CPU
        uint32_t                   morphFirst = 0, morphEnd = 0; // the vertices morphs move
        std::vector<SExpression>   expressions;                 // the presets it has, its own, then its shape keys
        std::array<int, EX_COUNT>  preset;                      // expression per preset, -1 = none
        std::array<int, VISEME_COUNT - VOWEL_COUNT> consonant{-1, -1, -1, -1}; // lip sync's pp, ff, ss, ch: expression, -1 = none
        std::string                expressionsFrom;             // "VRM", "VRM 1.0", "shape key names", "" = none
        SLookAt                    lookAt;
        // the face each hand's gesture makes (left, right): an expression, -1 = none
        std::array<std::array<int, GESTURE_COUNT>, 2> gestureFace;
        // the face both hands' gestures make together (left, right), over the one hand's: an expression, -1 = none,
        // -2 = no such combination
        std::array<std::array<int, GESTURE_COUNT>, GESTURE_COUNT> gestureCombo;
        // hand poses of its own (the settings file's "hands": a Gesture layer's, converted): per hand and gesture, each
        // finger bone's local rotation (finger * 3 + segment); where none is set the fingers curl as the gesture's preset
        std::array<std::array<std::array<Quat, 15>, GESTURE_COUNT>, 2> handPose{};
        std::array<std::array<bool, GESTURE_COUNT>, 2>                 handPoseSet{};

        std::vector<SAvatarPart>   parts;
        std::vector<SAvatarToggle> toggles;
        std::vector<SAvatarSlider> sliders;
        std::vector<int>           fixed; // nodes held in the world (MA's World Fixed Object): where their rest pose was
                                          // when the avatar appeared
        std::string                settings; // the settings file it came with ("<name>.hypr3d.json"), "" = none
        // material variants (KHR_materials_variants): their names, and per batch (SAvatarBatch::variants) the material
        // it takes in some of them, (variant, material); [0] is none
        std::vector<std::string>                      variants;
        std::vector<std::vector<std::pair<int, int>>> variantMaps{{}};

        std::vector<SSpring>         springs;
        std::vector<SSpringJoint>    springJoints;
        std::vector<SSpringCollider> springColliders;
        std::string                  springsFrom; // "VRM", "VRM 1.0", the settings file's name, "bone names", "" = none
        std::vector<SNodeConstraint> constraints; // sources before what depends on them

        // the built in ones (humanoids), its own clips that aren't for walking about, the settings file's and
        // those the request brings; a later one of the same name replaces an earlier one
        std::vector<std::shared_ptr<const SAvatarEmote>> emotes;
        // a humanoid's attacks, per arm (left, right: the left's the right's mirror image), third person's and first
        // person's (seen from the eyes: the fist up where it shows); no channels: none
        std::array<SAvatarAttack, 2> attacks, attacksFirst;
        std::string                  attacksFrom; // "built in", or the settings file's file
        // a humanoid's walk and run (walking, running): how its body goes over the steps; none = the walking's own
        std::array<std::shared_ptr<const SGaitClip>, 2> gaitClips;
        std::string                                     gaitFrom; // "built in", the settings file's files, "none"

        // expressions by VRM 1.0's preset names ("happy", "aa", "blinkLeft"), VRM 0.x's ("joy", "a"),
        // or their own; -1 = none
        int              findExpression(std::string_view name) const;
        int              findToggle(std::string_view name) const;
        int              findSlider(std::string_view name) const;
        int              findVariant(std::string_view name) const;
        std::vector<int> findParts(std::string_view name) const;  // all of that name
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

        // the renderer copied everything it needs to the GPU (but the morphs)
        void releaseCpuData() {
            vertices = {};
            indices  = {};
            for (auto& i : images)
                i.rgba = {};
        }
    };

    struct SAvatarRequest {
        std::string              path;
        float                    height = 0; // meters, 0 = as it comes (or a sensible size if it's far off)
        std::vector<std::string> emotes;     // files to make emotes of: VRM animations (.vrma), glTF / VRM with clips
        std::string              attack, attackFirst; // VRM animations for its attacks, over the settings file's and the built in ones
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

    // emotes from files, made for an avatar that's loaded
    struct SEmoteRequest {
        std::string                         path; // the files, for the messages
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
    bool         isEmoteFile(std::string_view path); // by its extension: .vrma, .glb, .gltf, .vrm

    class CEmoteLoader : public CBackgroundLoader<SEmoteRequest, SEmoteResult> {
      public:
        CEmoteLoader() : CBackgroundLoader(loadEmotes) {}
    };

    // First person with the body: the camera in its eyes (CAvatarAnimator::eyes()), the head not drawn, and the hands
    // in view as a first person game has them, doing what the player does
    enum eFirstHands : uint8_t {
        FPH_READY, // held up in front, low in the view; a gesture held up where it shows
        FPH_TOUCH, // the right one's finger reaching to where the crosshair is (a window clicked, held while pressed)
        FPH_TYPE,  // lower and nearer together, tapping as keys go down
        FPH_HOLD,  // both out toward what's carried (a window), the palms to it
        FPH_DOWN,  // let down, out of the view (playing a game in a window)
    };
    struct SFirstPerson {
        bool        on = false; // first person with the body: else the hands are the animation's
        V3          eye;        // the camera in the eyes (world), and the way it looks (the plugin's: yaw 0 looks -z,
        float       yaw = 0, pitch = 0; // right > 0; up > 0); while it's off, as it was last (the arms going back)
        eFirstHands hands = FPH_READY;
        float       room = 1e9f;  // how far ahead of the eye there's room for the hands (a wall, a window), meters
        int         tap  = -1;    // FPH_TYPE: a key went down this frame for that hand to tap (0 left, 1 right)
        bool        press = false; // FPH_TOUCH: a button went down this frame (the finger pokes)
    };

    // First person's camera in the eyes (world: CAvatarAnimator::eyes() where the avatar's drawn), going with the body (its
    // feet) and the camera's yaw at once (the head turns with it), and after the eyes: held still through what moves them
    // only a little (breathing, a step's bob: the windows in view don't wobble), else eased after them. No further out
    // from the feet than `reach` across, and `low` to `high` up
    struct SFirstPersonEye {
        V3    off, v;  // from the feet, turned back by the camera's yaw (as at yaw 0), and how fast that changes
        V3    held;    // where the eyes are held (the same frame)
        bool  live  = false;
        float reach = 1e9f, low = 0.f, high = 1e9f;
        V3    update(const V3& feet, const V3& eyes, float yaw, float dt); // the camera (world)
        V3    at(const V3& feet, float yaw) const;                         // ... where it is now
        void  reset() {
            live = false;
        }
    };

    // what the player is doing, for picking the animation
    struct SAvatarMotion {
        float dt        = 0;
        float speed     = 0; // horizontal, m/s
        V3    vel;           // the same as a velocity in the world (horizontal); none = along the body at `speed`
        V3    wish;          // the velocity the player's keys ask for (horizontal): going on, or back the other way
        float accel = 10, decel = 14, turnBack = 7; // how quickly `vel` goes to `wish`, m/s² (slower: decel; back the other way: turnBack)
        float vy        = 0;
        bool  onGround  = true;
        bool  flying    = false;
        bool  crouched  = false;
        bool  run       = false; // the player runs (Shift), else walks
        float lookPitch = 0; // where the camera looks relative to the body (radians, up > 0)
        float lookYaw   = 0; // (right > 0)
        M4    world = M4::identity(); // avatar space -> world, without lift(): the springs swing as it moves
        // the ground's height (world y) under a point of the world, looked for near `y`: where a foot can land (a
        // stair, a slope); none, or a function that finds nothing, is flat at the body's feet
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
        // the walking: the gait, its phase, each foot planted or in the air (JSON; "null" when it isn't walked)
        std::string gaitStatus() const;
        // the body's yaw (radians, the plugin's: right turns are positive) turned toward `want` over dt, as people turn:
        // quickly, but no faster than a few steps allow (and slower running), speeding up into a turn and slowing out of
        // it (not all at once: a tap of a key doesn't jerk the body round); turning right round while walking, the way
        // the feet make it without crossing the legs
        float turnBody(float yaw, float want, float dt, float speed);
        // third person, the yaw the body turns to for where the keys take it (m.wish; none when they ask for nothing, or
        // not in third person): the way it will be going a moment on (walking, right round as soon as they go back the
        // other way, turning as it slows; running, as it slows down enough to turn); but going back and forth (a key back
        // the other way again within half a second) the body faces the way it has been going if the taps only slow it
        // down, else the way W walks (keysYaw), or S if it's turned more that way, and steps from side to side or back
        // and forth, as people do, till one way is held. Called every frame
        std::optional<float> wayToFace(const SAvatarMotion& m, bool thirdPerson, float bodyYaw, float keysYaw);
        // how far the body has still to turn (turnBody's, radians, right > 0)
        float turnLeft() const {
            return m_turnLeft;
        }
        // every node's transform this frame, in the model's space
        const std::vector<M4>& globals() const {
            return m_global;
        }
        // first person: where the eyes are this frame, between them (avatar space, before lift()); none when the model
        // can't be seen from inside (SAvatarModel::eyeHeight)
        std::optional<V3> eyes() const;
        // first person: how much of the pose is its (the trunk turned to the camera, the hands in view), eased in and out
        float firstPerson() const {
            return m_fpW;
        }
        // ... how much of the arms is its (less under an emote, let down, looking far down)
        float firstPersonArms() const {
            return m_fpArmsW;
        }

        // the face; see SAvatarModel::findExpression
        void setExpression(int expression, float weight = 1); // held until changed, -1 = none
        int  expression() const {
            return m_held;
        }
        void setGesture(int hand, int gesture); // hand 0 left, 1 right; eGesture
        // lip sync (CLipSync's): the mouth's aa, ih, ou, ee, oh presets this far, and its consonants where the avatar has
        // them, under what the face blocks of the mouth
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
        // the model's materials as expressions change them (colors, atlas faces); null when none do
        const std::vector<SMapMaterial>* materials() const {
            return m_materials.empty() ? nullptr : &m_materials;
        }

        // the outfit: toggles, and parts and shape keys set by hand over what the toggles say
        void setToggle(int toggle, bool on); // turns the others of its group off
        bool toggle(int toggle) const {
            return toggle >= 0 && toggle < (int)m_toggles.size() && m_toggles[toggle];
        }
        void setPart(int part, int shown);        // 1 shown, 0 hidden, -1 as the toggles say
        void resetOutfit();                       // as the settings file has it
        void setShape(int morph, float weight);   // NaN = as the toggles say
        float shape(int morph) const;             // the weight it has without expressions
        void  setSlider(int slider, float value, float valueY = NAN); // 0..1 (2D: -1..1 each), NaN = where it starts
        float slider(int slider) const;
        float sliderY(int slider) const; // a 2D slider's second axis
        // per SAvatarModel::parts, 1 = drawn
        const std::vector<uint8_t>& partsShown() const {
            return m_shown;
        }
        // per SAvatarModel::batches, the material it's drawn with; null when the model has no material variants
        const std::vector<int>* batchMaterials() const {
            return m_batchMat.empty() ? nullptr : &m_batchMat;
        }
        bool variant(int v) const { // a material variant in effect
            return v >= 0 && v < (int)m_variantOn.size() && m_variantOn[v];
        }

        // spring bones; off, they hang as the animation has them
        // the walk's and the run's body from the avatar's clips (SGaitClip), or the walking's own
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

        // emotes: the model's, and those added since; one plays over the rest till it's done or the avatar moves
        const std::vector<std::shared_ptr<const SAvatarEmote>>& emotes() const {
            return m_emotes;
        }
        int  findEmote(std::string_view name) const;               // by name, loosely, or its number from 1; -1 = none
        int  addEmote(std::shared_ptr<const SAvatarEmote> emote); // made for this model; replaces one of that name
        void playEmote(int emote, int loop = -1);                  // loop: 1 over and over, 0 once, -1 as the emote has it
        void stopEmote();
        int  emote() const { // playing, -1 = none
            return m_emoteOut ? -1 : m_emote;
        }
        // how many have started (one played again counts), whether the one playing goes over and over, and how far into
        // it it is (seconds of its own)
        uint32_t emoteStarts() const {
            return m_emoteStarts;
        }
        bool emoteLooping() const {
            return m_emoteLoop;
        }
        float emoteTime() const {
            return m_emoteTime;
        }
        // for the next update only: how far into its sound is heard (seconds, CSpeaker::clock()'s), for the emote
        // playing to be there in it (at its speed, round as it loops), as a dance keeps time with its song
        void setEmoteClock(double seconds) {
            m_emoteClock = seconds;
        }

        // an attack: a punch at what's ahead (SAvatarAttack: the fists up, one struck, the body turned into it, back),
        // over whatever the legs do (standing, walking, in the air); the way the body faces, and in first person the way
        // the camera looks. Asked for while one is going, it's the other arm's, as soon as that one has struck (one,
        // two); after a pause, the right's again. hand: 0 left, 1 right, -1 whichever is next. An emote stops. False: no
        // arms to swing (not a humanoid with a head, arms and hands)
        bool        attack(int hand = -1);
        // (JSON) each arm's swing: how far into it (seconds) and how much of the body is its; the one asked for next, the
        // last one, how many there have been; how far the chest is turned from the hips (radians, right > 0)
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
        void    gait(const SAvatarMotion& m, bool air, std::vector<STRS>& pose); // walked procedurally (a humanoid)
        void    fingerAxes(); // m_fingerAxes, m_thumbPose, m_handRig, m_clipFingers
        void    hands(float dt, std::vector<STRS>& pose);
        void    look(const SAvatarMotion& m, std::vector<STRS>& pose, float weight, float yawOff = 0) const;
        // first person (SFirstPerson): what the hands do and how much it's in, before the fingers (firstPersonState); the
        // trunk turned toward where the camera looks (the turn given, radians, right > 0); the arms reaching to where
        // the hands go in view
        void    firstPersonState(const SAvatarMotion& m, float emoteW);
        float   firstPersonTrunk(const SAvatarMotion& m, std::vector<STRS>& pose) const;
        void    firstPersonArms(const SAvatarMotion& m, std::vector<STRS>& pose);
        // an arm's bones (s: 0 left, 1 right) reaching from its shoulder as posed for a wrist, the elbow toward `elbow`, the
        // hand pointing `along` with its palm to `palm` (model space), over the pose by w
        void    reachArm(std::vector<STRS>& pose, int s, const V3& wrist, const V3& elbow, const V3& along, const V3& palm, float w);
        // attacks (attack()): the swings on (the fists, the one asked for next) and their pose (m_attackPose); the
        // trunk turned as they turn it; the arms as they have them (first person: no further ahead than there's room)
        void    attackState(const SAvatarMotion& m);
        void    attackTrunk(std::vector<STRS>& pose) const;
        void    attackArms(const SAvatarMotion& m, std::vector<STRS>& pose);
        // an arm's wrist (s: 0 left, 1 right) moved to `wrist` (model space): the upper arm and forearm turned as little as
        // takes, in the plane the elbow bends in, the hand turned as it was; w of the way
        void    armTo(std::vector<STRS>& pose, int s, const V3& wrist, float w) const;
        void    emotePose(float dt, std::vector<STRS>& pose); // the emote over the pose, m_emoteFace
        void    face(const SAvatarMotion& m, std::vector<STRS>& pose);
        float   random(); // 0..1
        void    rotateBone(std::vector<STRS>& pose, int bone, const Quat& d) const;
        void    globals(const std::vector<STRS>& pose, std::vector<M4>& out) const;
        float   lowestFoot(const std::vector<M4>& g) const;
        void    outfit(); // m_shown and m_shapeBase from the toggles and what's set by hand
        void    constrain(std::vector<STRS>& pose) const;
        M4      globalOf(const std::vector<STRS>& pose, int node) const;
        void    springs(const SAvatarMotion& m);
        enum eSpringPass : uint8_t {
            SP_START, // the tails where the bones point
            SP_STEP,  // a step of t seconds
            SP_SHOW,  // the bones turned to the tails, t of the way into the next step
        };
        // world: model space -> the world; pose: m_global, as it was then
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
        // the procedural walking's state (avatar.cpp's SGait), made for the model
        std::shared_ptr<void>               m_gait;
        bool                                m_gaitUsed = false; // last frame
        bool                                m_flying   = false; // last frame
        bool                                m_flewOff  = false; // flew since it was last on the ground: in the air at once
        // Where the pose changes all at once (the walking leaving the ground, landing, taking to the air), what was shown
        // goes on as it went and settles into the new pose: per node, the turn (a rotation vector) and the move from the
        // new pose to the one shown, and how fast they change, dying away critically damped at m_settleRate a second
        // (0: nothing to settle). The gait asks for it with m_settleAsk (the rate), the frame the pose jumps
        std::vector<V3>                     m_settleR, m_settleRV, m_settleT, m_settleTV;
        std::vector<STRS>                   m_settleNow, m_settleWas; // the pose shown (before the look and hands) the last two frames
        float                               m_settleRate = 0, m_settleAsk = 0;
        float                               m_settleLift = 0, m_settleLiftWas = 0; // (asked with it: the body jumped off or landed, the hips going on as they went that much)
        V3                                  m_settleRoot, m_settleRootV; // where the avatar was last frame (world), and how fast it went
        bool                                m_settleRootSet = false;
        bool                                m_settleJust = false, m_settleFresh = false; // it started this frame; last frame
        std::vector<STRS>                   m_settleNew; // the new pose the frame it started (how fast it goes, the frame after)
        float                               m_settleDt   = 0; // the frame before's
        int                                 m_turnSide = 0;     // turning right round: 1 right, -1 left
        float                               m_turnRate = 0;     // how fast the body turns, radians a second
        float                               m_turnYaw  = 0;     // the yaw turnBody gave last (else it was set: turning afresh)
        float                               m_turnLeft = 0;     // how far it has still to turn, radians (the way it turns)
        bool                                m_turnKnown = false; // turnBody ran since the walking last looked
        // wayToFace's: the way the keys asked last, the time, when they last went back the other way, since when they ask
        // for this way, whether the body holds its facing
        V3                                  m_wayLast;
        float                               m_wayClock = 0, m_wayBack = -1e9f, m_waySince = 0;
        bool                                m_wayHold = false;
        float                               m_wayYaw  = 0;
        V3                                  m_wayMean; // the body's velocity, the last moment (see WAY_MEAN)

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
            std::array<Quat, 3>             thumb{};          // the thumb's bones' turns, at rest
        };
        struct SFingerAxes {
            V3 curl, spread; // model space, at rest: turning the bone toward the palm, toward the thumb
        };
        std::array<std::array<SFingerAxes, FINGER_COUNT * 3>, 2>       m_fingerAxes{};
        std::array<std::array<std::array<Quat, 3>, GESTURE_COUNT>, 2> m_thumbPose{}; // per hand and gesture, as thumb
        std::array<bool, 2>                 m_handRig{};
        std::array<SHandPose, 2>            m_hand{};
        std::array<float, 2>                m_handW{}; // how much of the gesture's pose is over the animation's
        std::array<std::array<Quat, 15>, 2> m_handQ{};      // the hand's own pose (SAvatarModel::handPose), eased in
        std::array<float, 2>                m_handCustom{}; // how much of that is over the preset's
        std::vector<uint8_t>                m_clipFingers; // per clip: moves the fingers

        // outfit
        std::vector<uint8_t>                m_toggles;   // per toggle, on
        std::vector<float>                  m_sliders;   // per slider, 0..1 (2D: x, -1..1)
        std::vector<float>                  m_slidersY;  // per slider: a 2D one's y
        std::vector<int8_t>                 m_partSet;   // per part: 1 shown, 0 hidden, -1 as the toggles say
        std::vector<float>                  m_shapeSet;  // per morph, NaN = as the toggles say
        std::vector<uint8_t>                m_shown;     // per part
        std::vector<float>                  m_shapeBase; // per morph: rest, toggles, sliders, set by hand
        std::vector<uint8_t>                m_variantOn; // per material variant
        std::vector<int>                    m_batchMat;  // per batch, when the model has material variants
        std::vector<STRS>                   m_nodePose;  // per node, its rest as the toggles and sliders have it; empty = the rest
        std::vector<int>                    m_loops;     // the toggles that are on and have a loop
        std::vector<int>                    m_dropBy;    // per node dropped in the world: the toggle, else -1
        std::vector<M4>                     m_dropAt;    // per node dropped: where in the world
        std::vector<uint8_t>                m_dropTake;  // per node: dropped just now, take where it is
        void                                loopPoses(std::vector<STRS>& pose) const; // the loops' node poses
        void                                drops(const SAvatarMotion& m);          // nodes left in the world

        // emotes
        std::vector<std::shared_ptr<const SAvatarEmote>> m_emotes;
        int                                 m_emote     = -1;
        float                               m_emoteTime = 0;
        bool                                m_emoteLoop = false;
        bool                                m_emoteOut  = false; // fading out: done, stopped, or the avatar moved
        float                               m_emoteW    = 0;     // how much of it is over the rest
        float                               m_emoteSwap = 1;     // 0 -> 1 from m_emoteFrom to it (one after another)
        uint32_t                            m_emoteStarts = 0;
        double                              m_emoteClock  = -1; // setEmoteClock()'s, -1 = none
        std::vector<STRS>                   m_emoteNow, m_emoteFrom;
        std::vector<float>                  m_emoteFace; // per expression, its weight in
        float                               m_emoteEyes = 0, m_emoteYaw = 0, m_emotePitch = 0; // how much its eyes are in; degrees

        // first person: how much it's in (eased; under an emote, which has the arms), and each hand: where its wrist
        // goes in the camera's frame (x right, y up, -z ahead; in arm lengths, shoulder to wrist), eased as a spring
        // (and how fast it goes), the way it points and its palm faces (the same frame, eased), a key's tap
        struct SFpHand {
            V3    at, v;
            V3    along, palm;
            float pitch = 0; // its frame's (the camera's pitched less, but touching and holding as it is), eased
            float tap   = 0; // seconds since a key went down for it (typing), large: none
        };
        float                  m_fpW = 0, m_fpArmsW = 0; // in at all (the camera); the arms (less: emotes, FPH_DOWN)
        float                  m_fpTrunkW = 0;           // the trunk turned to the camera (less: emotes)
        std::array<SFpHand, 2> m_fpHand{};
        bool                   m_fpLive = false;    // the hands have been placed (else they start where they go)
        float                  m_fpYaw = 0, m_fpPitch = 0; // the camera last frame
        V3                     m_fpEye;             // ... where it was (world)
        // the camera the hands and the trunk go by this frame (SFirstPerson's; while it's off, as it was last: its eye
        // kept in avatar space and its yaw from the body's, so they go back from where they were as the body moves)
        V3                     m_fpViewEye, m_fpEyeAt;
        float                  m_fpViewYaw = 0, m_fpViewPitch = 0, m_fpYawFrom = 0;
        V3                     m_fpLag, m_fpLagV;   // the hands behind the camera's turns and moves (its frame, arm lengths)
        float                  m_fpPoke = 1;        // seconds since a button went down (touching): the finger pokes
        float                  m_fpPhase = 0;       // a stride's phase for the hands' bob, when the walking isn't procedural
        std::array<int8_t, 2>  m_fpGesture{-1, -1}; // the fingers first person gives the hands (pointing, open), -1 = the player's

        // attacks: the swing on (seconds into its clip, < 0: none; whose: its clip's arm; first person's clips), the
        // one before it, going on under it as this one takes over (m_swingCross of the way, eased, over ATTACK_CROSS);
        // how much of the body they have (eased in as one starts, out as the last lets go); per node, what of it the
        // attacks have (ATTACK_TRUNK, ATTACK_ARM) and its pose, theirs this frame
        struct SSwing {
            float t    = -1;
            int   hand = 1;
            bool  fp   = false;
        };
        SSwing                m_swing, m_swingWas;
        float                 m_swingCross = 1, m_attackW = 0;
        int                   m_swingNext = -1; // asked for while one winds up: this arm's, once that one has struck
        int                   m_swingLast = 0;  // the arm that swung last
        float                 m_swingAgo  = 1e9f; // seconds since a swing last started
        int                   m_swings    = 0;
        std::array<bool, 2>   m_swingFist{};    // the hands made fists for it
        std::vector<uint8_t>  m_attackPart;
        std::vector<STRS>     m_attackPose, m_attackWas;
        void                  swingStart(int hand, bool fp);
        const SAvatarAttack&  attackOf(const SSwing& sw) const;

        // springs, stepped at a fixed rate
        struct SColliderAt {
            V3            a, b; // the ends of the capsule, in the world (a plane or a disc: a point on it, and that plus its normal)
            float         radius;
            eColliderKind kind;
            float         disc; // (a disc's flat part's radius)
        };
        bool                                m_physics = true;
        bool                                m_gaitStyle = true;
        std::vector<int>                    m_springOf;   // per node: its joint, -1 = none, -2 = below one
        int                                 m_springFrom = 0; // the first node the springs move
        std::vector<V3>                     m_tail, m_tailPrev; // per joint, in its spring's center's space
        std::vector<V3>                     m_carried;          // per joint: how far its spring's carrier carried it the last step (the world)
        std::vector<V3>                     m_again;            // per joint: what pushing it again would do where the last step left it (its parent's frame)
        std::vector<M4>                     m_carrierAt, m_carrierMove; // per spring: its carrier in the world at the last step; its move over this one
        std::vector<M4>                     m_springGlobal;     // per node the springs move: in the world
        std::vector<M4>                     m_centerAt, m_centerInv; // per spring: center -> world
        std::vector<SColliderAt>            m_colliderAt; // per collider, in the world
        std::vector<int>                    m_springUp;   // the nodes the springs go by of the pose, and all above them
        std::vector<STRS>                   m_springWas;  // the pose (m_pose) last frame
        std::vector<M4>                     m_springPose; // m_global between the two (springPoseAt())
        float                               m_springAcc  = 0; // time into the next step
        bool                                m_springLive = false;
        STRS                                m_springBody;                    // model space (feet on the ground) -> the world, the last frame
        M4                                  m_springStepAt = M4::identity(); // the same at the last step
        M4                                  m_springMove   = M4::identity(); // the avatar's move in the world over this step
    };
}
