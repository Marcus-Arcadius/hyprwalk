#include "avatar.hpp"
#include "gltf.hpp"

#include "third_party/cgltf.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <numeric>
#include <ranges>
#include <set>
#include <string_view>

namespace h3d {

    namespace {
        using gltf::check;
        using gltf::lower;

        constexpr float TAU = 6.28318530718f;
        constexpr V3    UP{0, 1, 0};
        constexpr float MAX_CLIP = 900.f; // seconds: the longest clip (an emote) taken
        constexpr int   DROP_FIXED = 1 << 30; // CAvatarAnimator::m_dropBy of a node held in the world by no toggle

        // --- a small JSON reader, for the VRM extensions (cgltf leaves them as text)

        struct SJson {
            enum eType : uint8_t {
                J_NULL,
                J_BOOL,
                J_NUM,
                J_STR,
                J_ARR,
                J_OBJ,
            } type = J_NULL;
            double                                     num = 0;
            std::string                                str;
            std::vector<SJson>                         arr;
            std::vector<std::pair<std::string, SJson>> obj;

            const SJson* get(std::string_view k) const {
                if (type == J_OBJ)
                    for (const auto& [n, v] : obj)
                        if (n == k)
                            return &v;
                return nullptr;
            }
        };

        class CJsonReader {
          public:
            explicit CJsonReader(std::string_view s) : m_begin(s.data()), m_p(s.data()), m_end(s.data() + s.size()) {}

            bool read(SJson& out) {
                if (!value(out, 0))
                    return false;
                ws();
                return m_p == m_end;
            }

            // the line it got to, for saying where it went wrong
            int line() const {
                return 1 + (int)std::count(m_begin, m_p, '\n');
            }

          private:
            const char* m_begin;
            const char* m_p;
            const char* m_end;

            void ws() {
                while (m_p < m_end && (*m_p == ' ' || *m_p == '\n' || *m_p == '\r' || *m_p == '\t'))
                    ++m_p;
            }

            bool literal(std::string_view w) {
                if ((size_t)(m_end - m_p) < w.size() || std::string_view(m_p, w.size()) != w)
                    return false;
                m_p += w.size();
                return true;
            }

            bool string(std::string& out) {
                if (m_p >= m_end || *m_p != '"')
                    return false;
                ++m_p;
                while (m_p < m_end && *m_p != '"') {
                    char c = *m_p++;
                    if (c != '\\') {
                        out += c;
                        continue;
                    }
                    if (m_p >= m_end)
                        return false;
                    switch (c = *m_p++) {
                        case 'n': out += '\n'; break;
                        case 't': out += '\t'; break;
                        case 'r': out += '\r'; break;
                        case 'b': out += '\b'; break;
                        case 'f': out += '\f'; break;
                        case 'u': {
                            if (m_end - m_p < 4)
                                return false;
                            unsigned cp = 0;
                            for (int i = 0; i < 4; ++i) {
                                const char h = *m_p++;
                                cp           = cp * 16 + (h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0);
                            }
                            // names are ASCII or Japanese, surrogate pairs can stay broken
                            if (cp < 0x80)
                                out += (char)cp;
                            else if (cp < 0x800) {
                                out += (char)(0xC0 | cp >> 6);
                                out += (char)(0x80 | (cp & 0x3F));
                            } else {
                                out += (char)(0xE0 | cp >> 12);
                                out += (char)(0x80 | (cp >> 6 & 0x3F));
                                out += (char)(0x80 | (cp & 0x3F));
                            }
                            break;
                        }
                        default: out += c; break; // " \ /
                    }
                }
                if (m_p >= m_end)
                    return false;
                ++m_p;
                return true;
            }

            bool value(SJson& v, int depth) {
                if (depth > 64)
                    return false;
                ws();
                if (m_p >= m_end)
                    return false;
                switch (*m_p) {
                    case '{': {
                        ++m_p;
                        v.type = SJson::J_OBJ;
                        ws();
                        if (m_p < m_end && *m_p == '}') {
                            ++m_p;
                            return true;
                        }
                        for (;;) {
                            ws();
                            std::string k;
                            if (!string(k))
                                return false;
                            ws();
                            if (m_p >= m_end || *m_p++ != ':')
                                return false;
                            SJson child;
                            if (!value(child, depth + 1))
                                return false;
                            v.obj.emplace_back(std::move(k), std::move(child));
                            ws();
                            if (m_p >= m_end)
                                return false;
                            const char c = *m_p++;
                            if (c == '}')
                                return true;
                            if (c != ',')
                                return false;
                        }
                    }
                    case '[': {
                        ++m_p;
                        v.type = SJson::J_ARR;
                        ws();
                        if (m_p < m_end && *m_p == ']') {
                            ++m_p;
                            return true;
                        }
                        for (;;) {
                            SJson child;
                            if (!value(child, depth + 1))
                                return false;
                            v.arr.push_back(std::move(child));
                            ws();
                            if (m_p >= m_end)
                                return false;
                            const char c = *m_p++;
                            if (c == ']')
                                return true;
                            if (c != ',')
                                return false;
                        }
                    }
                    case '"': v.type = SJson::J_STR; return string(v.str);
                    case 't': v.type = SJson::J_BOOL; v.num = 1; return literal("true");
                    case 'f': v.type = SJson::J_BOOL; return literal("false");
                    case 'n': return literal("null");
                    default: {
                        std::string num;
                        while (m_p < m_end && std::string_view("0123456789+-.eE").find(*m_p) != std::string_view::npos)
                            num += *m_p++;
                        if (num.empty())
                            return false;
                        char* e = nullptr;
                        v.type  = SJson::J_NUM;
                        v.num   = std::strtod(num.c_str(), &e);
                        return e && *e == 0;
                    }
                }
            }
        };

        // --- math

        // shortest rotation taking direction a to b
        Quat arc(const V3& a, const V3& b) {
            const float d = dot(a, b);
            if (d < -0.9999f)
                return Quat::axisAngle(perpendicular(a), TAU * 0.5f);
            const V3 c = cross(a, b);
            return Quat{c.x, c.y, c.z, 1.f + d}.normalized();
        }

        // rotation of a transform that may carry scale (and a mirror)
        Quat rotationOf(const M4& m) {
            V3 x{m.m[0], m.m[1], m.m[2]};
            const V3 y{m.m[4], m.m[5], m.m[6]}, z{m.m[8], m.m[9], m.m[10]};
            if (m.det3() < 0)
                x = -x;
            return Quat::fromBasis(normalize(x), normalize(y), normalize(z));
        }

        // where a spring's bone at O points at p, turned back within its limit (frame L: its limitFrame in the world), as
        // far from O; VRMC_springBone_limit's reference implementations
        V3 springLimit(const SSpringJoint& J, const Quat& L, const V3& O, const V3& p) {
            V3 d = L.conj().rotate(normalize(p - O));
            switch (J.limit) {
                case LIMIT_NONE: return p;
                case LIMIT_CONE: {
                    // no further from y than the angle
                    if (const float c = std::cos(J.limitA); d.y < c) {
                        const float side = d.x * d.x + d.z * d.z, s = std::sqrt(std::max(0.f, 1.f - c * c));
                        d                = side <= 1e-8f ? V3{0, c, s} : V3{d.x * s / std::sqrt(side), c, d.z * s / std::sqrt(side)}; // (straight back: to +z)
                    }
                    break;
                }
                case LIMIT_HINGE: {
                    // in the yz plane, no further from y than the angle
                    const float l = std::sqrt(d.y * d.y + d.z * d.z);
                    d             = l <= 1e-4f ? V3{0, 1, 0} : V3{0, d.y / l, d.z / l};
                    if (const float c = std::cos(J.limitA); d.y < c)
                        d = {0, c, (d.z < 0 ? -1.f : 1.f) * std::sqrt(std::max(0.f, 1.f - c * c))};
                    break;
                }
                case LIMIT_SPHERICAL: {
                    // a pitch round x (in the yz plane) and a yaw toward x, each within its own
                    const float pitch = d.y <= -1.f + 1e-6f ? TAU * 0.5f : std::abs(d.x) >= 1.f - 1e-6f ? 0.f : std::atan2(d.z, d.y);
                    const float yaw = std::clamp(std::asin(std::clamp(d.x, -1.f, 1.f)), -J.limitB, J.limitB), p2 = std::clamp(pitch, -J.limitA, J.limitA);
                    d               = {std::sin(yaw), std::cos(yaw) * std::cos(p2), std::cos(yaw) * std::sin(p2)};
                    break;
                }
            }
            return O + L.rotate(d) * length(p - O);
        }

        V3 origin(const M4& m) {
            return {m.m[12], m.m[13], m.m[14]};
        }

        STRS decompose(const M4& m) {
            STRS r;
            r.t = origin(m);
            V3 x{m.m[0], m.m[1], m.m[2]};
            const V3 y{m.m[4], m.m[5], m.m[6]}, z{m.m[8], m.m[9], m.m[10]};
            r.s = {length(x), length(y), length(z)};
            if (m.det3() < 0) {
                x     = -x;
                r.s.x = -r.s.x;
            }
            if (std::abs(r.s.x) > 1e-12f && r.s.y > 1e-12f && r.s.z > 1e-12f)
                r.r = Quat::fromBasis(normalize(x), normalize(y), normalize(z));
            return r;
        }

        // --- which bone is which, from the names

        std::vector<std::string> tokens(const std::string& name) {
            std::vector<std::string> out;
            std::string              cur;
            auto                     flush = [&] {
                if (!cur.empty())
                    out.push_back(lower(cur));
                cur.clear();
            };
            for (const char ch : name) {
                const unsigned char c = ch;
                if (!std::isalnum(c)) {
                    flush();
                    continue;
                }
                if (!cur.empty()) {
                    const unsigned char p = cur.back();
                    if ((bool)std::isdigit(c) != (bool)std::isdigit(p) || (std::isupper(c) && std::islower(p)))
                        flush();
                    else if (std::islower(c) && std::isupper(p) && cur.size() >= 2 && std::isupper((unsigned char)cur[cur.size() - 2])) {
                        // "LShoulder": a run of capitals, then a word
                        cur.pop_back();
                        flush();
                        cur += (char)p;
                    }
                }
                cur += (char)c;
            }
            flush();
            return out;
        }

        struct SBoneName {
            int         side = 0; // 1 left, 2 right
            std::string key;      // what's left once sides, numbers and rig prefixes are gone
        };

        SBoneName boneName(const std::string& name) {
            static constexpr std::string_view DROP[] = {"mixamorig", "j", "bip", "def", "org", "mch", "jnt", "joint", "bone", "b", "c", "skeleton", "armature",
                                                        "character", "rig", "cc", "base", "sk", "bn", "valve", "biped", "ctrl", "bind", "hlp"};
            SBoneName                         out;
            for (const auto& t : tokens(name)) {
                if (t == "l" || t == "left")
                    out.side = 1;
                else if (t == "r" || t == "right")
                    out.side = 2;
                else if (!std::isdigit((unsigned char)t[0]) && std::ranges::find(DROP, t) == std::end(DROP))
                    out.key += t;
            }
            return out;
        }

        // bone of a key: centre bones directly, sided ones as an offset from the left upper leg / shoulder
        enum eKeyKind : uint8_t {
            K_NONE,
            K_CENTER,
            K_LEG,
            K_ARM,
            K_SPINE,
            K_EYE,
            K_TOES,
        };
        struct SKey {
            eKeyKind kind;
            int      bone; // eHumanBone for K_CENTER, else 0.. along the limb
        };
        SKey keyOf(const std::string& k, int side) {
            static const std::map<std::string_view, SKey> KEYS = {
                {"hips", {K_CENTER, HB_HIPS}},  {"hip", {K_CENTER, HB_HIPS}},       {"pelvis", {K_CENTER, HB_HIPS}},    {"neck", {K_CENTER, HB_NECK}},
                {"head", {K_CENTER, HB_HEAD}},  {"spine", {K_SPINE, 0}},            {"chest", {K_SPINE, 0}},            {"upperchest", {K_SPINE, 0}},
                {"torso", {K_SPINE, 0}},        {"abdomen", {K_SPINE, 0}},          {"upleg", {K_LEG, 0}},              {"thigh", {K_LEG, 0}},
                {"upperleg", {K_LEG, 0}},       {"leg", {K_LEG, 1}},                {"lowerleg", {K_LEG, 1}},           {"calf", {K_LEG, 1}},
                {"shin", {K_LEG, 1}},           {"knee", {K_LEG, 1}},               {"foot", {K_LEG, 2}},               {"ankle", {K_LEG, 2}},
                {"shoulder", {K_ARM, 0}},       {"clavicle", {K_ARM, 0}},           {"collar", {K_ARM, 0}},             {"arm", {K_ARM, 1}},
                {"upperarm", {K_ARM, 1}},       {"uparm", {K_ARM, 1}},              {"shldr", {K_ARM, 1}},              {"forearm", {K_ARM, 2}},
                {"lowerarm", {K_ARM, 2}},       {"elbow", {K_ARM, 2}},              {"hand", {K_ARM, 3}},               {"wrist", {K_ARM, 3}},
                {"eye", {K_EYE, 0}},            {"eyeball", {K_EYE, 0}},            {"toe", {K_TOES, 0}},               {"toes", {K_TOES, 0}},
                {"jaw", {K_CENTER, HB_JAW}},
            };
            const auto it = KEYS.find(k);
            if (it == KEYS.end())
                return {K_NONE, 0};
            SKey r = it->second;
            if (side && r.kind == K_CENTER && r.bone == HB_HIPS)
                return {K_LEG, 0}; // "LeftHip" is a thigh
            if ((r.kind == K_LEG || r.kind == K_ARM || r.kind == K_EYE || r.kind == K_TOES) != (side != 0))
                return {K_NONE, 0};
            return r;
        }

        // a humanoid bone by its VRM name ("leftUpperArm") or Unity's ("Left Thumb Proximal"). VRM 1.0's thumb
        // starts at its metacarpal; VRM 0.x's and Unity's call that one the proximal
        int humanBoneOf(std::string_view name, bool vrm1) {
            std::string n;
            for (const char c : name)
                if (std::isalnum((unsigned char)c))
                    n += (char)std::tolower((unsigned char)c);
            static const std::map<std::string, int, std::less<>> BONES = {
                {"hips", HB_HIPS},
                {"spine", HB_SPINE},
                {"chest", HB_CHEST},
                {"upperchest", HB_UPPER_CHEST},
                {"neck", HB_NECK},
                {"head", HB_HEAD},
                {"leftupperleg", HB_L_UPPER_LEG},
                {"leftlowerleg", HB_L_LOWER_LEG},
                {"leftfoot", HB_L_FOOT},
                {"rightupperleg", HB_R_UPPER_LEG},
                {"rightlowerleg", HB_R_LOWER_LEG},
                {"rightfoot", HB_R_FOOT},
                {"leftshoulder", HB_L_SHOULDER},
                {"leftupperarm", HB_L_UPPER_ARM},
                {"leftlowerarm", HB_L_LOWER_ARM},
                {"lefthand", HB_L_HAND},
                {"rightshoulder", HB_R_SHOULDER},
                {"rightupperarm", HB_R_UPPER_ARM},
                {"rightlowerarm", HB_R_LOWER_ARM},
                {"righthand", HB_R_HAND},
                {"lefteye", HB_L_EYE},
                {"righteye", HB_R_EYE},
                {"jaw", HB_JAW},
                {"lefttoes", HB_L_TOES},
                {"righttoes", HB_R_TOES},
            };
            if (const auto it = BONES.find(n); it != BONES.end())
                return it->second;
            const int hand = n.starts_with("left") ? 0 : n.starts_with("right") ? 1 : -1;
            if (hand < 0)
                return -1;
            static constexpr std::string_view FINGERS[] = {"thumb", "index", "middle", "ring", "little"};
            const std::string_view            rest      = std::string_view(n).substr(hand == 0 ? 4 : 5);
            for (int f = 0; f < FINGER_COUNT; ++f) {
                if (!rest.starts_with(FINGERS[f]))
                    continue;
                const std::string_view seg = rest.substr(FINGERS[f].size());
                int                    s   = seg == "proximal" ? 0 : seg == "intermediate" ? 1 : seg == "distal" ? 2 : -1;
                if (f == FINGER_THUMB && (vrm1 || seg == "metacarpal"))
                    s = seg == "metacarpal" ? 0 : seg == "proximal" ? 1 : seg == "distal" ? 2 : -1;
                return s < 0 ? -1 : fingerBone(hand, f, s);
            }
            return -1;
        }

        // which finger a bone's name says: eFinger, -1 = none, -2 = the palm (a metacarpal, not the thumb's)
        int fingerOf(const std::string& name) {
            const std::string key = boneName(name).key;
            auto              has = [&](std::initializer_list<std::string_view> words) {
                return std::ranges::any_of(words, [&](std::string_view w) { return (unsigned char)w[0] >= 0x80 ? name.contains(w) : key.contains(w); });
            };
            if (has({"thumb", "pollex", "親指"}))
                return FINGER_THUMB;
            if (has({"metacarpal", "palm", "carpal"}))
                return -2;
            if (has({"index", "pointer", "人指", "人差"}))
                return FINGER_INDEX;
            if (has({"middle", "中指"}) || key == "mid" || key.ends_with("mid"))
                return FINGER_MIDDLE;
            if (has({"ring", "薬指"}))
                return FINGER_RING;
            if (has({"little", "pinky", "pinkie", "small", "小指"}))
                return FINGER_LITTLE;
            return -1;
        }

        // clip names: which one is which
        int clipKind(const std::string& name) {
            const auto toks = tokens(name);
            auto       has  = [&](std::initializer_list<std::string_view> words, bool exact = false) {
                for (const auto& t : toks)
                    for (auto w : words)
                        if (exact ? t == w : t.starts_with(w))
                            return true;
                return false;
            };
            if (has({"jump"}))
                return CLIP_JUMP;
            if (has({"fall"}) || has({"air", "inair", "midair"}, true))
                return CLIP_FALL;
            if (has({"run", "sprint", "jog"}))
                return CLIP_RUN;
            if (has({"walk"}))
                return CLIP_WALK;
            if (has({"idle", "survey", "stand", "breath", "wait"}) || has({"rest"}, true))
                return CLIP_IDLE;
            return -1;
        }

        // --- faces

        double jnum(const SJson* j, double def) {
            return j && j->type == SJson::J_NUM ? j->num : def;
        }

        bool jbool(const SJson* j) {
            return j && j->type == SJson::J_BOOL && j->num != 0;
        }

        std::string_view jstr(const SJson* j) {
            return j && j->type == SJson::J_STR ? std::string_view(j->str) : std::string_view();
        }

        const std::vector<SJson>* jarr(const SJson* j) {
            return j && j->type == SJson::J_ARR ? &j->arr : nullptr;
        }

        float srgbToLinear(float c) {
            return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }

        // shape key and expression names, compared loosely: "vrc.v_aa" and "V_AA" are both "vaa"
        std::string normName(std::string_view s) {
            if (const size_t dot = s.rfind('.');
                dot != std::string_view::npos && s.size() - dot > 2 && !std::ranges::all_of(s.substr(dot + 1), [](char c) { return c >= '0' && c <= '9'; }))
                s = s.substr(dot + 1);
            std::string out;
            for (const char c : s)
                if (c != '_' && c != '-' && c != '.' && c != ' ')
                    out += c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
            return out;
        }

        constexpr std::string_view PRESET_NAMES[EX_COUNT] = {"happy", "angry",    "sad",        "relaxed", "surprised", "aa",       "ih",       "ou",        "ee",
                                                             "oh",    "blink",    "blinkLeft",  "blinkRight", "lookUp",  "lookDown", "lookLeft", "lookRight", "neutral"};

        // VRM 1.0's preset names, VRM 0.x's (joy, a, blink_l...), -1 = none of them
        int presetOf(std::string_view name) {
            const std::string n = normName(name);
            for (int p = 0; p < EX_COUNT; ++p)
                if (n == normName(PRESET_NAMES[p]))
                    return p;
            static const std::map<std::string_view, int> OLD = {
                {"joy", EX_HAPPY}, {"sorrow", EX_SAD}, {"fun", EX_RELAXED}, {"surprise", EX_SURPRISED}, {"a", EX_AA},         {"i", EX_IH},
                {"u", EX_OU},      {"e", EX_EE},       {"o", EX_OH},        {"blinkl", EX_BLINK_L},     {"blinkr", EX_BLINK_R},
            };
            const auto it = OLD.find(n);
            return it == OLD.end() ? -1 : it->second;
        }

        // Shape keys that make a preset, for models that don't say (VRoid, VRChat, MMD, ARKit and plain
        // names). Per preset the alternatives in order; each is the shape keys it's made of, as normName()
        // has them, with their weights.
        const std::array<std::vector<std::string_view>, EX_COUNT>& shapeKeyGuesses() {
            static const std::array<std::vector<std::string_view>, EX_COUNT> G = {{
                /* happy */ {"fclalljoy", "joy", "happy", "smile", "笑い にっこり にこり",
                             "mouthsmileleft mouthsmileright cheeksquintleft:0.5 cheeksquintright:0.5 eyesquintleft:0.5 eyesquintright:0.5"},
                /* angry */ {"fclallangry", "angry", "anger", "mad", "怒り", "browdownleft browdownright nosesneerleft:0.5 nosesneerright:0.5 mouthfrownleft:0.5 mouthfrownright:0.5"},
                /* sad */ {"fclallsorrow", "sad", "sorrow", "sadness", "trouble", "困る 口角下げ", "browinnerup mouthfrownleft mouthfrownright"},
                /* relaxed */ {"fclallfun", "relaxed", "relax", "fun", "calm", "なごみ にこり:0.5", "mouthsmileleft:0.6 mouthsmileright:0.6 eyesquintleft:0.4 eyesquintright:0.4"},
                /* surprised */ {"fclallsurprised", "surprised", "surprise", "shock", "shocked", "びっくり",
                                 "eyewideleft eyewideright browinnerup browouterupleft browouterupright jawopen:0.3"},
                /* aa */ {"fclmtha", "vaa", "aa", "a", "moutha", "mtha", "あ", "jawopen:0.8"},
                /* ih */ {"fclmthi", "vih", "ih", "i", "mouthi", "mthi", "い", "jawopen:0.25 mouthstretchleft:0.5 mouthstretchright:0.5"},
                /* ou */ {"fclmthu", "vou", "ou", "u", "mouthu", "mthu", "う", "jawopen:0.2 mouthpucker:0.8 mouthfunnel:0.4"},
                /* ee */ {"fclmthe", "ve", "ee", "e", "mouthe", "mthe", "え", "jawopen:0.3 mouthstretchleft:0.7 mouthstretchright:0.7"},
                /* oh */ {"fclmtho", "voh", "oh", "o", "moutho", "mtho", "お", "jawopen:0.5 mouthfunnel:0.8"},
                /* blink */ {"fcleyeclose", "blink", "blinkboth", "blinks", "eyeblink", "eyesblink", "eyeclose", "eyesclose", "eyeclosed", "eyesclosed", "closeeyes",
                             "まばたき", "eyeblinkleft eyeblinkright"},
                /* blinkLeft */ {"fcleyeclosel", "blinkleft", "blinkl", "leftblink", "eyecloseleft", "eyeclosel", "winkleft", "winkl", "ウィンク", "eyeblinkleft"},
                /* blinkRight */ {"fcleyecloser", "blinkright", "blinkr", "rightblink", "eyecloseright", "eyecloser", "winkright", "winkr", "ウィンク右", "eyeblinkright"},
                /* lookUp */ {"lookup", "eyelookup", "eyeslookup", "eyelookupleft eyelookupright"},
                /* lookDown */ {"lookdown", "eyelookdown", "eyeslookdown", "eyelookdownleft eyelookdownright"},
                /* lookLeft */ {"lookleft", "eyelookleft", "eyeslookleft", "eyelookoutleft eyelookinright"},
                /* lookRight */ {"lookright", "eyelookright", "eyeslookright", "eyelookinleft eyelookoutright"},
                /* neutral */ {"fclallneutral", "neutral"},
            }};
            return G;
        }

        constexpr std::string_view GESTURE_NAMES[GESTURE_COUNT] = {"neutral", "fist", "open", "point", "victory", "rocknroll", "handgun", "thumbsup"};

        // --- clips

        // a channel's value at t: 3 floats, or a rotation's 4
        void sampleChannel(const SAnimChannel& c, float t, float out[4]) {
            const size_t comps = c.path == PATH_R ? 4 : 3;
            const size_t keys  = c.times.size();
            const bool   cubic = c.interp == INTERP_CUBIC;
            auto         at    = [&](size_t k) { return &c.values[(cubic ? k * 3 + 1 : k) * comps]; };
            if (t <= c.times.front() || keys == 1)
                std::copy_n(at(0), comps, out);
            else if (t >= c.times.back())
                std::copy_n(at(keys - 1), comps, out);
            else {
                const size_t k  = (size_t)(std::ranges::upper_bound(c.times, t) - c.times.begin()) - 1;
                const float  dt = c.times[k + 1] - c.times[k];
                const float  u  = dt > 0 ? (t - c.times[k]) / dt : 0.f;
                const float *a = at(k), *b = at(k + 1);
                if (c.interp == INTERP_STEP)
                    std::copy_n(a, comps, out);
                else if (cubic) {
                    const float* outTan = &c.values[(k * 3 + 2) * comps];
                    const float* inTan  = &c.values[((k + 1) * 3) * comps];
                    const float  u2 = u * u, u3 = u2 * u;
                    for (size_t i = 0; i < comps; ++i)
                        out[i] = (2 * u3 - 3 * u2 + 1) * a[i] + (u3 - 2 * u2 + u) * dt * outTan[i] + (-2 * u3 + 3 * u2) * b[i] + (u3 - u2) * dt * inTan[i];
                } else if (c.path == PATH_R) {
                    const Quat q = slerp({a[0], a[1], a[2], a[3]}, {b[0], b[1], b[2], b[3]}, u);
                    out[0] = q.x, out[1] = q.y, out[2] = q.z, out[3] = q.w;
                } else
                    for (size_t i = 0; i < comps; ++i)
                        out[i] = a[i] + (b[i] - a[i]) * u;
            }
        }

        // a clip at t over a pose: what it doesn't move keeps its own
        void sampleClip(const SAnimClip& clip, float t, std::vector<STRS>& pose) {
            for (const auto& c : clip.channels) {
                float out[4];
                sampleChannel(c, t, out);
                STRS& p = pose[c.node];
                switch (c.path) {
                    case PATH_T: p.t = {out[0], out[1], out[2]}; break;
                    case PATH_R: p.r = Quat{out[0], out[1], out[2], out[3]}.normalized(); break;
                    case PATH_S: p.s = {out[0], out[1], out[2]}; break;
                }
            }
        }

        // a clip turns the fingers from rest (5 degrees or more)
        bool movesFingers(const SAvatarModel& md, const SAnimClip& clip) {
            std::vector<bool> finger(md.nodes.size(), false);
            for (int b = HB_FINGERS; b < HB_COUNT; ++b)
                if (md.human[b] >= 0)
                    finger[md.human[b]] = true;
            for (const auto& ch : clip.channels) {
                if (ch.path != PATH_R || !finger[ch.node])
                    continue;
                const Quat   r   = md.nodes[ch.node].rest.r;
                const size_t per = ch.interp == INTERP_CUBIC ? 3 : 1, off = ch.interp == INTERP_CUBIC ? 1 : 0;
                for (size_t k = 0; k < ch.times.size(); ++k) {
                    const float* q = &ch.values[(k * per + off) * 4];
                    if (std::abs(q[0] * r.x + q[1] * r.y + q[2] * r.z + q[3] * r.w) < 0.999f)
                        return true;
                }
            }
            return false;
        }

        // a clip by its name, or loosely; -1 = none
        int clipNamed(const std::vector<SAnimClip>& clips, std::string_view name) {
            for (int loose = 0; loose < 2 && !name.empty(); ++loose) {
                const std::string want = loose ? normName(name) : lower(std::string(name));
                for (size_t i = 0; i < clips.size(); ++i)
                    if ((loose ? normName(clips[i].name) : lower(clips[i].name)) == want)
                        return (int)i;
            }
            return -1;
        }

        // which way a model faces at rest (model space, level): a humanoid's front from its legs, else its arms;
        // an animal's head is in front
        V3 facingOf(const SAvatarModel& model, const std::vector<M4>& restGlobal) {
            const auto& h   = model.human;
            auto        pos = [&](int b) { return origin(restGlobal[h[b]]); };
            V3          fwd{0, 0, 1}; // glTF's convention
            if (model.humanoid) {
                V3 across = pos(HB_L_UPPER_LEG) - pos(HB_R_UPPER_LEG);
                across.y  = 0;
                if (length(across) < 1e-5f) {
                    across   = pos(HB_L_UPPER_ARM) - pos(HB_R_UPPER_ARM);
                    across.y = 0;
                }
                if (length(across) > 1e-6f)
                    fwd = normalize(cross(across, UP));
            } else if (h[HB_HIPS] >= 0 && h[HB_HEAD] >= 0) {
                const V3 d = pos(HB_HEAD) - pos(HB_HIPS);
                if (const V3 flat{d.x, 0, d.z}; length(flat) > 0.5f * length(d))
                    fwd = normalize(flat);
            }
            return fwd;
        }

        // --- emotes: clips on no one in particular, made for the avatar
        //
        // A humanoid's bones turn as a world turn from a T pose, in a frame of its own: +X its left, +Y up, +Z
        // forward (VRM 1.0's). What one humanoid does, another does the same way: its T pose turned the same.

        constexpr float PI = TAU * 0.5f;

        // a humanoid at rest
        struct SRig {
            const SAvatarModel*        md = nullptr;
            std::vector<M4>            restGlobal;
            std::vector<Quat>          restRot; // per node
            Quat                       facing;  // its frame -> model space
            std::array<Quat, HB_COUNT> tpose{}; // per bone, in model space: rest -> a T pose
            V3                         hips;    // at rest, model space
            float                      height = 1; // of the hips over the ankles, model units
        };

        // the bone above one, as the limbs count (not the model's own)
        constexpr int humanParent(int b) {
            switch (b) {
                case HB_HIPS: return -1;
                case HB_SPINE: return HB_HIPS;
                case HB_CHEST: return HB_SPINE;
                case HB_UPPER_CHEST: return HB_CHEST;
                case HB_NECK: return HB_UPPER_CHEST;
                case HB_HEAD: return HB_NECK;
                case HB_L_UPPER_LEG:
                case HB_R_UPPER_LEG: return HB_HIPS;
                case HB_L_SHOULDER:
                case HB_R_SHOULDER: return HB_UPPER_CHEST;
                case HB_L_EYE:
                case HB_R_EYE:
                case HB_JAW: return HB_HEAD;
                case HB_L_TOES: return HB_L_FOOT;
                case HB_R_TOES: return HB_R_FOOT;
                default:
                    if (b >= HB_FINGERS)
                        return (b - HB_FINGERS) / (FINGER_COUNT * 3) == 0 ? HB_L_HAND : HB_R_HAND;
                    return b - 1; // down a limb
            }
        }

        SRig rigOf(const SAvatarModel& md) {
            SRig        r;
            const auto& h = md.human;
            r.md          = &md;
            r.restGlobal.resize(md.nodes.size());
            r.restRot.resize(md.nodes.size());
            for (size_t i = 0; i < md.nodes.size(); ++i) {
                const auto& n   = md.nodes[i];
                r.restGlobal[i] = n.parent >= 0 ? r.restGlobal[n.parent] * n.rest.matrix() : n.rest.matrix();
                r.restRot[i]    = rotationOf(r.restGlobal[i]);
            }
            if (!md.humanoid)
                return r;
            auto pos = [&](int b) { return origin(r.restGlobal[h[b]]); };
            r.facing = Quat::axisAngle(UP, std::atan2(md.forward.x, md.forward.z));
            r.hips   = pos(HB_HIPS);
            float feet = 1e30f, legs = 0;
            for (int s = 0; s < 2; ++s) {
                const int ul = s ? HB_R_UPPER_LEG : HB_L_UPPER_LEG;
                feet         = std::min(feet, pos(h[ul + 2] >= 0 ? ul + 2 : ul + 1).y);
                legs         = std::max(legs, length(pos(ul + 1) - pos(ul)) * 2.f);
            }
            r.height = r.hips.y - feet > 0.1f * legs ? r.hips.y - feet : std::max(legs, 1e-4f);

            // the limbs straight out: the arms to the sides, the legs down
            auto along = [&](int b, int to) -> std::optional<V3> {
                if (h[b] < 0 || h[to] < 0)
                    return std::nullopt;
                const V3 d = pos(to) - pos(b);
                return length(d) > 1e-6f ? std::optional(normalize(d)) : std::nullopt;
            };
            std::array<bool, HB_COUNT> own{};
            for (int s = 0; s < 2; ++s) {
                const float sx = s ? -1.f : 1.f;
                const int   ua = s ? HB_R_UPPER_ARM : HB_L_UPPER_ARM, ul = s ? HB_R_UPPER_LEG : HB_L_UPPER_LEG, hand = ua + 2;
                // the hand toward its middle finger, else another
                std::optional<V3> fingers;
                for (int f : {FINGER_MIDDLE, FINGER_INDEX, FINGER_RING, FINGER_LITTLE})
                    if (!fingers)
                        fingers = along(hand, fingerBone(s, f, 0));
                const std::pair<int, std::optional<V3>> limbs[] = {
                    {ua, along(ua, ua + 1)}, {ua + 1, along(ua + 1, hand)}, {hand, fingers}, {ul, along(ul, ul + 1)}, {ul + 1, along(ul + 1, ul + 2)}};
                for (const auto& [b, d] : limbs)
                    if (d) {
                        r.tpose[b] = arc(*d, r.facing.rotate(b >= HB_L_UPPER_ARM && b <= HB_R_HAND ? V3{sx, 0, 0} : V3{0, -1, 0}));
                        own[b]     = true;
                    }
            }
            // the rest as the bone above them (the torso's is none)
            for (int b = 0; b < HB_COUNT; ++b)
                if (!own[b])
                    r.tpose[b] = humanParent(b) >= 0 && b != HB_L_SHOULDER && b != HB_R_SHOULDER && humanParent(b) > HB_HEAD ? r.tpose[humanParent(b)] : Quat{};
            return r;
        }

        // a clip in the frame above: per frame, every bone's turn from the T pose and the hips' move from rest (in
        // hips heights); the faces by name (the value's x is the weight)
        struct SNormClip {
            std::string                                       name;
            float                                             duration = 0;
            std::vector<float>                                times;
            std::vector<std::array<Quat, HB_COUNT>>           turn;
            std::vector<V3>                                   move;
            std::array<bool, HB_COUNT>                        has{};
            bool                                              moves = false;
            std::vector<std::pair<std::string, SAnimChannel>> faces;
        };

        // the clip for a humanoid
        SAnimClip retarget(const SNormClip& nc, const SRig& rig) {
            const auto& md = *rig.md;
            const auto& h  = md.human;
            SAnimClip   clip;
            clip.name     = nc.name;
            clip.duration = nc.duration;
            std::vector<int> own(md.nodes.size(), -1), bones;
            for (int b = 0; b < HB_COUNT; ++b)
                if (h[b] >= 0 && nc.has[b]) {
                    own[h[b]] = b;
                    bones.push_back(b);
                }
            std::vector<SAnimChannel> rot(bones.size());
            for (size_t i = 0; i < bones.size(); ++i) {
                rot[i].node  = h[bones[i]];
                rot[i].path  = PATH_R;
                rot[i].times = nc.times;
                rot[i].values.reserve(nc.times.size() * 4);
            }
            SAnimChannel hips;
            hips.node  = h[HB_HIPS];
            hips.path  = PATH_T;
            hips.times = nc.times;
            const int  hp      = md.nodes[h[HB_HIPS]].parent;
            const M4   toLocal = hp >= 0 ? rig.restGlobal[hp].inverse() : M4::identity();
            const Quat R = rig.facing, Rc = rig.facing.conj();

            std::vector<Quat> E(md.nodes.size());
            for (size_t k = 0; k < nc.times.size(); ++k) {
                for (size_t n = 0; n < md.nodes.size(); ++n) {
                    const int p = md.nodes[n].parent;
                    E[n] = own[n] >= 0 ? (R * nc.turn[k][own[n]] * Rc * rig.tpose[own[n]]).normalized() : p >= 0 ? E[p] : Quat{};
                }
                for (size_t i = 0; i < bones.size(); ++i) {
                    const int  n  = h[bones[i]], p = md.nodes[n].parent;
                    const Quat gp = p >= 0 ? rig.restRot[p] : Quat{}, ep = p >= 0 ? E[p] : Quat{};
                    Quat       q  = (gp.conj() * ep.conj() * E[n] * gp * md.nodes[n].rest.r).normalized();
                    auto&      v  = rot[i].values;
                    if (k > 0 && q.x * v[v.size() - 4] + q.y * v[v.size() - 3] + q.z * v[v.size() - 2] + q.w * v[v.size() - 1] < 0)
                        q = {-q.x, -q.y, -q.z, -q.w};
                    v.insert(v.end(), {q.x, q.y, q.z, q.w});
                }
                if (nc.moves) {
                    const V3 t = toLocal.point(rig.hips + R.rotate(nc.move[k]) * rig.height);
                    hips.values.insert(hips.values.end(), {t.x, t.y, t.z});
                }
            }
            clip.channels = std::move(rot);
            if (nc.moves)
                clip.channels.push_back(std::move(hips));
            return clip;
        }

        // a humanoid's clip in the frame above
        SNormClip canonical(const SAnimClip& clip, const SRig& rig, const std::atomic<bool>& cancel) {
            const auto& md = *rig.md;
            const auto& h  = md.human;
            SNormClip   nc;
            nc.name     = clip.name;
            nc.duration = clip.duration;
            // at its keys if they're few and in straight lines between, else 60 a second
            std::vector<float> keys;
            bool               linear = true;
            for (const auto& c : clip.channels) {
                linear = linear && c.interp == INTERP_LINEAR;
                keys.insert(keys.end(), c.times.begin(), c.times.end());
            }
            std::ranges::sort(keys);
            keys.erase(std::unique(keys.begin(), keys.end(), [](float a, float b) { return b - a < 1e-4f; }), keys.end());
            if (linear && keys.size() <= 4096 && !keys.empty())
                nc.times = std::move(keys);
            else
                for (int i = 0, n = std::max(1, (int)std::ceil(clip.duration * 60.f)); i <= n; ++i)
                    nc.times.push_back(clip.duration * (float)i / (float)n);

            // (a VRM animation's bones without curves are left as they are, as VRM animation players have it: one made
            // for the upper body leaves the legs to the walking)
            std::vector<bool> turned(md.nodes.size(), md.humanFrom != "VRMA");
            for (const auto& c : clip.channels)
                if (c.path == PATH_R)
                    turned[c.node] = true;
            for (int b = 0; b < HB_COUNT; ++b)
                nc.has[b] = h[b] >= 0 && b != HB_L_EYE && b != HB_R_EYE && turned[h[b]];
            const Quat         Rc = rig.facing.conj();
            std::vector<STRS>  pose(md.nodes.size());
            std::vector<M4>    g(md.nodes.size());
            std::array<Quat, HB_COUNT> undo; // rest -> T pose, undone: rest's inverse and the T pose's
            for (int b = 0; b < HB_COUNT; ++b)
                if (nc.has[b])
                    undo[b] = rig.restRot[h[b]].conj() * rig.tpose[b].conj();
            nc.turn.resize(nc.times.size());
            nc.move.resize(nc.times.size());
            for (size_t k = 0; k < nc.times.size(); ++k) {
                if ((k & 63) == 0)
                    check(cancel);
                for (size_t n = 0; n < md.nodes.size(); ++n)
                    pose[n] = md.nodes[n].rest;
                sampleClip(clip, nc.times[k], pose);
                for (size_t n = 0; n < md.nodes.size(); ++n)
                    g[n] = md.nodes[n].parent >= 0 ? g[md.nodes[n].parent] * pose[n].matrix() : pose[n].matrix();
                for (int b = 0; b < HB_COUNT; ++b)
                    nc.turn[k][b] = nc.has[b] ? (Rc * rotationOf(g[h[b]]) * undo[b] * rig.facing).normalized() : Quat{};
                nc.move[k] = Rc.rotate(origin(g[h[HB_HIPS]]) - rig.hips) / rig.height;
                nc.moves   = nc.moves || length(nc.move[k]) > 1e-4f;
            }
            return nc;
        }

        // --- posing a humanoid, for the built in emotes

        // a humanoid's measure, in the frame above and hips heights: the hips at (0, 1, 0), the ankles at y = 0
        struct SBody {
            std::array<V3, HB_COUNT>   at{}; // at rest
            std::array<bool, HB_COUNT> has{};
            float                      upper[2]{}, fore[2]{}, thigh[2]{}, shin[2]{}; // per side, 0 the left
            float                      arm = 0.6f;                                   // shoulder to wrist
        };

        SBody bodyOf(const SRig& rig) {
            SBody       b;
            const auto& h = rig.md->human;
            for (int i = 0; i < HB_COUNT; ++i)
                if (h[i] >= 0) {
                    b.has[i] = true;
                    b.at[i]  = rig.facing.conj().rotate(origin(rig.restGlobal[h[i]]) - rig.hips) / rig.height + UP;
                }
            auto len = [&](int a, int c) { return b.has[a] && b.has[c] ? length(b.at[c] - b.at[a]) : 0.f; };
            for (int s = 0; s < 2; ++s) {
                const int ua = s ? HB_R_UPPER_ARM : HB_L_UPPER_ARM, ul = s ? HB_R_UPPER_LEG : HB_L_UPPER_LEG;
                b.upper[s]   = std::max(len(ua, ua + 1), 0.05f);
                b.fore[s]    = b.has[ua + 2] ? std::max(len(ua + 1, ua + 2), 0.05f) : b.upper[s];
                b.thigh[s]   = std::max(len(ul, ul + 1), 0.05f);
                b.shin[s]    = b.has[ul + 2] ? std::max(len(ul + 1, ul + 2), 0.05f) : b.thigh[s];
            }
            b.arm = (b.upper[0] + b.fore[0] + b.upper[1] + b.fore[1]) * 0.5f;
            return b;
        }

        // the turn taking t0 to t, and h0 (square to t0) as near to h as it goes
        Quat frameTo(const V3& t0, const V3& h0, const V3& t, const V3& h) {
            V3 hp = h - t * dot(h, t);
            if (length(hp) < 1e-5f)
                return arc(t0, t);
            hp = normalize(hp);
            return (Quat::fromBasis(t, hp, cross(t, hp)) * Quat::fromBasis(t0, h0, cross(t0, h0)).conj()).normalized();
        }

        // where a limb from t0 bends at u when turned there the shortest way: h0 turned along, square to u
        V3 hingeAt(const V3& t0, const V3& h0, const V3& u) {
            V3 a = arc(t0, u).rotate(h0);
            a    = a - u * dot(a, u);
            return length(a) > 1e-5f ? normalize(a) : perpendicular(u);
        }

        // a direction partway round from a to b
        V3 towards(const V3& a, const V3& b, float w) {
            return slerp(Quat{}, arc(normalize(a), normalize(b)), w).rotate(normalize(a));
        }

        float ramp(float t, float a, float b) {
            return smoothstep01((t - a) / (b - a));
        }

        // an arm: the upper arm along u, the elbow bent, turned about u from where it'd bend (twist); the hand
        // along `along` with the palm facing `palm`, or zero for straight on; in the frame of the chest
        struct SArm {
            V3    u{0, -1, 0};
            float bend = 0, twist = 0;
            V3    along, palm;
        };

        class CPoser {
          public:
            std::array<Quat, HB_COUNT> turn; // the pose: bones turned from the T pose, in the frame above
            std::array<bool, HB_COUNT> set;  // which it has
            V3                         move; // the hips, from rest

            explicit CPoser(const SBody& b) : m_b(b) {
                start();
            }

            void start() {
                turn.fill(Quat{});
                set.fill(false);
                m_local.fill(Quat{});
                move = {};
                torso();
            }

            // the whole body turned about the hips, and moved
            void body(const Quat& q, const V3& moved = {}) {
                m_local[HB_HIPS] = q;
                move             = moved;
                torso();
            }

            // a bone of the torso (spine to head) turned from the one below
            void bend(int bone, const Quat& q) {
                m_local[bone] = q;
                torso();
            }

            // the arm in the chest's frame from a direction and bend
            SArm armOf(int side, const V3& u, const V3& f, const V3& along = {}, const V3& palm = {}) const {
                const V3    t0{side ? -1.f : 1.f, 0, 0}, h0{0, side ? 1.f : -1.f, 0};
                const V3    un = normalize(u), fn = normalize(f);
                SArm        a{un, std::acos(std::clamp(dot(un, fn), -1.f, 1.f)), 0, along, palm};
                const V3    c  = cross(un, fn);
                if (length(c) > 1e-4f) {
                    const V3 fb = hingeAt(t0, h0, un), hinge = normalize(c);
                    a.twist     = std::atan2(dot(cross(fb, hinge), un), dot(fb, hinge));
                }
                return a;
            }

            // where a bone's joint is as posed so far (the frame above): from the hips, up through the bones above it
            V3 jointAt(int bone) const {
                int chain[8], n = 0;
                for (int b = bone; b >= 0 && n < 8; b = humanParent(b))
                    if (m_b.has[b] || b == HB_HIPS)
                        chain[n++] = b;
                V3 p = UP + move;
                for (int i = n - 1; i > 0; --i)
                    p += turnAt(chain[i]).rotate(m_b.at[chain[i - 1]] - m_b.at[chain[i]]);
                return p;
            }

            // a bone's turn as posed so far (one that isn't set turns with the one above)
            Quat turnAt(int b) const {
                while (!set[b] && humanParent(b) >= 0)
                    b = humanParent(b);
                return turn[b];
            }

            const SBody& measure() const {
                return m_b;
            }

            // the arm reaching for a point from between the shoulders (arm lengths, the chest's frame), the
            // elbow toward `elbow`
            SArm reach(int side, const V3& target, const V3& elbow, const V3& along = {}, const V3& palm = {}) const {
                const int ua     = side ? HB_R_UPPER_ARM : HB_L_UPPER_ARM;
                const V3  mid    = (m_b.at[HB_L_UPPER_ARM] + m_b.at[HB_R_UPPER_ARM]) * 0.5f;
                const auto [u, f] = twoBone(m_b.at[ua], mid + target * m_b.arm, m_b.upper[side], m_b.fore[side], elbow);
                return armOf(side, u, f, along, palm);
            }

            void arm(int side, const SArm& a) {
                const auto [ua, la] = armBones(side, a);
                apply(side, armFinish(side, ua, la, handOf(side, a, la)));
            }

            // partway from one arm to another: its direction, bend and twist (turning the bones the short way
            // round would swing the forearm through the body), the hand from the forearm
            void arm(int side, const SArm& a, const SArm& b, float w) {
                float twist = b.twist - a.twist;
                twist -= TAU * std::round(twist / TAU);
                const SArm m{towards(a.u, b.u, w), a.bend + (b.bend - a.bend) * w, a.twist + twist * w, {}, {}};
                const auto [ua, la] = armBones(side, m);
                const Quat ha = handOf(side, a, armBones(side, a).second), hb = handOf(side, b, armBones(side, b).second);
                apply(side, armFinish(side, ua, la, slerp(ha, hb, w)));
            }

            // a leg: the thigh along `thigh` in the hips' frame, the knee bent, the foot turned toes down (pitch)
            void leg(int side, const V3& thigh, float knee, float pitch = 0) {
                const V3   t0{0, -1, 0}, h0{1, 0, 0};
                const V3   u = normalize(thigh), a = hingeAt(t0, h0, u);
                const Quat hips = turn[HB_HIPS], ul = frameTo(t0, h0, u, a), ll = Quat::axisAngle(a, knee) * ul;
                legTurns(side, {hips * ul, hips * ll, hips * ll * Quat::axisAngle(h0, pitch)});
            }

            // a foot on a spot (the ankle, the frame above), the knee forward; flat unless pitched
            void step(int side, const V3& ankle, float pitch = 0, float out = 0.15f) {
                const int  ul  = side ? HB_R_UPPER_LEG : HB_L_UPPER_LEG;
                const V3   t0{0, -1, 0}, h0{1, 0, 0};
                const Quat hq  = turn[HB_HIPS];
                const V3   hip = UP + move + hq.rotate(m_b.at[ul] - m_b.at[HB_HIPS]);
                const auto [u, f] = twoBone(hip, ankle, m_b.thigh[side], m_b.shin[side], hq.rotate(V3{side ? -out : out, 0, 1}));
                const V3   c = cross(u, f);
                const V3   a = length(c) > 1e-4f ? normalize(c) : hingeAt(t0, h0, u);
                const Quat t = frameTo(t0, h0, u, a), l = Quat::axisAngle(a, std::acos(std::clamp(dot(u, f), -1.f, 1.f))) * t;
                legTurns(side, {t, l, Quat::axisAngle(h0, pitch)});
            }

            // a leg reaching for an ankle (the frame above), bending in the plane toward `knee`; the foot turned as
            // `foot` has it (in the frame above). Nearly straight, it straightens softly the last bit of the way (the
            // ankle 2 mm short where a standing leg reaches furthest): as the ankle nears full reach the knee would
            // otherwise straighten ever faster, and snap
            void legTo(int side, const V3& ankle, const Quat& foot, const V3& knee) {
                const int  ul  = side ? HB_R_UPPER_LEG : HB_L_UPPER_LEG;
                const V3   t0{0, -1, 0}, h0{1, 0, 0};
                const V3   hip = UP + move + turn[HB_HIPS].rotate(m_b.at[ul] - m_b.at[HB_HIPS]);
                V3         to  = ankle;
                {
                    constexpr float SOFT = 0.015f;
                    const float     full = m_b.thigh[side] + m_b.shin[side], soft = full * (1.f - SOFT), d = length(ankle - hip);
                    if (d > soft)
                        to = hip + (ankle - hip) * ((soft + SOFT * full * (1.f - std::exp(-(d - soft) / (SOFT * full)))) / d);
                }
                const auto [u, f] = twoBone(hip, to, m_b.thigh[side], m_b.shin[side], knee);
                // the knee's hinge: square to the plane the leg bends in (so it holds with the knee straight too)
                V3 d = ankle - hip;
                d    = length(d) > 1e-6f ? normalize(d) : V3{0, -1, 0};
                V3 n = knee - d * dot(knee, d);
                n    = length(n) > 1e-6f ? normalize(n) : perpendicular(d);
                const V3   a = normalize(cross(n, d));
                const Quat t = frameTo(t0, h0, u, a), l = Quat::axisAngle(a, std::acos(std::clamp(dot(u, f), -1.f, 1.f))) * t;
                legTurns(side, {t, l, foot});
            }

            // the toes turned as `toes` has it (the frame above)
            void toes(int side, const Quat& toes) {
                const int b = side ? HB_R_TOES : HB_L_TOES;
                turn[b]     = toes.normalized();
                set[b]      = true;
            }

            // where the ankle is at rest
            V3 ankle(int side) const {
                const int ul = side ? HB_R_UPPER_LEG : HB_L_UPPER_LEG;
                return m_b.has[ul + 2] ? V3{m_b.at[ul + 2].x, 0, m_b.at[ul + 2].z} : V3{m_b.at[ul].x, 0, m_b.at[ul].z};
            }

            // bones that aren't set turn with the one above
            void finish() {
                for (int b = 0; b < HB_COUNT; ++b)
                    if (!set[b] && humanParent(b) >= 0)
                        turn[b] = turn[humanParent(b)];
            }

          private:
            const SBody&               m_b;
            std::array<Quat, HB_COUNT> m_local{};

            void torso() {
                Quat q = m_local[HB_HIPS];
                for (int b = HB_HIPS; b <= HB_HEAD; ++b) {
                    if (b > HB_HIPS)
                        q = q * m_local[b];
                    turn[b] = q.normalized();
                    set[b]  = true;
                }
            }

            static std::pair<V3, V3> twoBone(const V3& from, const V3& to, float l1, float l2, const V3& hint) {
                const V3 d    = to - from;
                float    dist = length(d);
                const V3 dir  = dist > 1e-6f ? d / dist : V3{0, -1, 0};
                dist          = std::clamp(dist, std::abs(l1 - l2) + 1e-4f, (l1 + l2) * 0.999f);
                const float c = std::clamp((l1 * l1 + dist * dist - l2 * l2) / (2 * l1 * dist), -1.f, 1.f);
                V3          n = hint - dir * dot(hint, dir);
                n             = length(n) > 1e-6f ? normalize(n) : perpendicular(dir);
                const V3 mid  = from + (dir * c + n * std::sqrt(1 - c * c)) * l1;
                return {normalize(mid - from), normalize(from + dir * dist - mid)};
            }

            // in the chest's frame: the upper arm, and the forearm before the hand twists it
            std::pair<Quat, Quat> armBones(int side, const SArm& a) const {
                const V3   t0{side ? -1.f : 1.f, 0, 0}, h0{0, side ? 1.f : -1.f, 0};
                const V3   u     = normalize(a.u);
                const V3   hinge = Quat::axisAngle(u, a.twist).rotate(hingeAt(t0, h0, u));
                const Quat ua    = frameTo(t0, h0, u, hinge);
                return {ua, Quat::axisAngle(hinge, a.bend) * ua};
            }

            // the hand turned from the forearm (straight on from it unless the arm says)
            Quat handOf(int side, const SArm& a, const Quat& la) const {
                if (length(a.along) <= 1e-6f)
                    return {};
                const V3 t0{side ? -1.f : 1.f, 0, 0}, n0{0, -1, 0};
                const V3 along = normalize(a.along);
                const V3 palm  = length(a.palm - along * dot(a.palm, along)) > 1e-4f ? a.palm : perpendicular(along);
                Quat     rel   = la.conj() * frameTo(t0, n0, along, palm);
                if (rel.w < 0)
                    rel = {-rel.x, -rel.y, -rel.z, -rel.w};
                return rel.normalized();
            }

            // the upper arm, forearm and hand: the forearm takes half the hand's twist
            static std::array<Quat, 3> armFinish(int side, const Quat& ua, const Quat& la, const Quat& hand) {
                const V3    t0{side ? -1.f : 1.f, 0, 0};
                const float phi = 2.f * std::atan2(dot(V3{hand.x, hand.y, hand.z}, t0), hand.w);
                return {ua, la * Quat::axisAngle(t0, phi * 0.5f), la * hand};
            }

            void apply(int side, const std::array<Quat, 3>& q) {
                const int  ua  = side ? HB_R_UPPER_ARM : HB_L_UPPER_ARM;
                const Quat top = turn[HB_UPPER_CHEST];
                for (int i = 0; i < 3; ++i) {
                    turn[ua + i] = (top * q[i]).normalized();
                    set[ua + i]  = true;
                }
            }

            void legTurns(int side, const std::array<Quat, 3>& q) {
                const int ul = side ? HB_R_UPPER_LEG : HB_L_UPPER_LEG;
                for (int i = 0; i < 3; ++i) {
                    turn[ul + i] = q[i].normalized();
                    set[ul + i]  = true;
                }
            }
        };

        // --- the built in emotes: look alikes of VRChat's (theirs aren't free to copy)

        // a face while the emote plays: faded in from `from` and out by `to` (seconds; to < 0 = the end, and it
        // stays)
        struct SFaceKey {
            std::string_view name;
            float            weight = 1, from = 0, to = -1;
        };

        struct SBuiltin {
            std::string_view        name;
            float                   duration;
            bool                    loop, hold, grounded;
            std::array<int8_t, 2>   gesture; // left, right; -1 = the player's
            std::array<SFaceKey, 2> faces;   // those without a name aren't
            void (*pose)(CPoser&, float t);
        };

        // the arm hanging at its side
        SArm restArm(int side) {
            const float sx = side ? -1.f : 1.f;
            return {normalize(V3{sx * 0.12f, -1, 0.03f}), 0.25f, 0, {}, {}};
        }

        // an arm through poses, eased from one to the next
        struct SArmKey {
            float t;
            SArm  a;
        };

        void armKeys(CPoser& p, int side, std::initializer_list<SArmKey> keys, float t) {
            const SArmKey* prev = keys.begin();
            for (const SArmKey& k : keys) {
                if (t < k.t) {
                    p.arm(side, prev->a, k.a, &k == prev ? 1.f : ramp(t, prev->t, k.t));
                    return;
                }
                prev = &k;
            }
            p.arm(side, prev->a);
        }

        // the feet where they stand
        void stand(CPoser& p) {
            for (int s = 0; s < 2; ++s)
                p.step(s, p.ankle(s));
        }

        void poseWave(CPoser& p, float t) {
            stand(p);
            p.arm(0, restArm(0));
            const float up    = ramp(t, 0, 0.4f) * (1 - ramp(t, 2.0f, 2.5f));
            const float swing = 0.35f * std::sin(TAU * 2 * (t - 0.4f)) * ramp(t, 0.25f, 0.55f);
            const V3    f     = Quat::axisAngle({0, 0, 1}, swing).rotate(normalize(V3{-0.15f, 1, 0.1f}));
            p.arm(1, restArm(1), p.armOf(1, {-0.75f, 0.45f, 0.3f}, f, f, {0, 0, 1}), up);
            p.bend(HB_HEAD, Quat::axisAngle({0, 0, 1}, 0.08f * up));
        }

        void poseClap(CPoser& p, float t) {
            stand(p);
            const float up   = ramp(t, 0, 0.35f) * (1 - ramp(t, 2.6f, 3.0f));
            const float open = 0.5f + 0.5f * std::cos(TAU * 3 * (t - 0.35f)); // together three times a second
            const float half = 0.04f + 0.16f * open;
            for (int s = 0; s < 2; ++s) {
                const float sx = s ? -1.f : 1.f;
                p.arm(s, restArm(s), p.reach(s, {sx * half, -0.35f, 0.55f}, {sx, -1, -0.2f}, {0, 0.4f, 1}, {-sx, 0, 0}), up);
            }
            p.bend(HB_HEAD, Quat::axisAngle({1, 0, 0}, 0.1f * up));
        }

        void posePoint(CPoser& p, float t) {
            stand(p);
            p.arm(0, restArm(0));
            const float up = ramp(t, 0, 0.35f) * (1 - ramp(t, 2.0f, 2.5f));
            const V3    u  = normalize(V3{-0.15f, 0.1f, 1});
            const V3    f  = Quat::axisAngle(UP, 0.2f).rotate(u);
            p.arm(1, restArm(1), p.armOf(1, u, f, f, {0.3f, -1, 0}), up);
            p.bend(HB_HEAD, Quat::axisAngle(UP, -0.1f * up));
        }

        void poseCheer(CPoser& p, float t) {
            const float up     = ramp(t, 0, 0.3f) * (1 - ramp(t, 2.6f, 3.0f));
            const float bounce = 0.5f - 0.5f * std::cos(TAU * 2 * t);
            p.body({}, {0, -0.05f * bounce * up, 0});
            stand(p);
            for (int s = 0; s < 2; ++s) {
                const float sx = s ? -1.f : 1.f;
                p.arm(s, restArm(s), {normalize(V3{sx * 0.55f, 1, 0.15f}), 0.2f + 0.8f * bounce, -sx * PI * 0.5f, {}, {}}, up);
            }
            p.bend(HB_HEAD, Quat::axisAngle({1, 0, 0}, -0.15f * up));
        }

        void poseDance(CPoser& p, float t) {
            const float phi  = std::fmod(t / 2.f, 1.f);
            const float q    = phi * 4; // four beats
            const int   beat = std::min(3, (int)q);
            const float f    = smoothstep01(q - (float)beat);
            // step touch: the right steps out, the left follows, the left steps back, the right follows
            constexpr float S = 0.22f;
            float           off[2]{};
            const int       moving = beat == 0 || beat == 3 ? 1 : 0;
            switch (beat) {
                case 0: off[1] = -S * f; break;
                case 1: off[1] = -S, off[0] = -S * f; break;
                case 2: off[1] = -S, off[0] = -S * (1 - f); break;
                default: off[1] = -S * (1 - f); break;
            }
            // down on the beat, the hips over the foot that's down
            const float bob  = 0.5f + 0.5f * std::cos(TAU * q);
            const float snap = smoothstep01(std::min(1.f, (q - (float)beat) / 0.35f)); // the arm's moves are quick
            const bool  high = beat % 2 == 0;                                      // pointing up, else down across
            const float lean = high ? snap : 1 - snap;
            p.body(Quat::axisAngle(UP, -0.25f * lean + 0.1f) * Quat::axisAngle({0, 0, 1}, 0.06f * (lean - 0.5f)),
                   {(off[0] + off[1]) * 0.5f, -0.06f * bob, 0});
            for (int s = 0; s < 2; ++s)
                p.step(s, p.ankle(s) + V3{off[s], s == moving ? 0.06f * std::sin(PI * f) : 0.f, 0});
            // the right points up and out, then down across the body; the left hand on the hip
            const SArm up   = p.armOf(1, {-0.55f, 0.8f, 0.25f}, {-0.5f, 0.85f, 0.2f}, {-0.5f, 0.85f, 0.2f}, {0, 0, 1});
            const SArm down = p.armOf(1, {0.1f, -0.75f, 0.6f}, {0.35f, -0.6f, 0.7f}, {0.35f, -0.6f, 0.7f}, {0, 0, 1});
            p.arm(1, high ? down : up, high ? up : down, snap);
            p.arm(0, p.reach(0, {0.3f, -0.78f, -0.02f}, {1, 0.1f, -0.5f}, {-0.3f, -1, 0.3f}, {-1, 0, 0}));
            p.bend(HB_NECK, Quat::axisAngle({1, 0, 0}, 0.1f * bob));
            p.bend(HB_HEAD, Quat::axisAngle(UP, -0.2f * lean) * Quat::axisAngle({1, 0, 0}, -0.25f * lean + 0.1f));
        }

        void poseBackflip(CPoser& p, float t) {
            constexpr float UP_AT = 0.45f, DOWN_AT = 1.1f;
            const float     spin  = ramp(t, UP_AT, DOWN_AT);
            float           y;
            if (t < 0.3f)
                y = -0.25f * ramp(t, 0, 0.3f);
            else if (t < UP_AT)
                y = -0.25f + 0.35f * ramp(t, 0.3f, UP_AT);
            else if (t < DOWN_AT) {
                const float u = (t - (UP_AT + DOWN_AT) * 0.5f) / ((DOWN_AT - UP_AT) * 0.5f);
                y             = 0.1f + 0.6f * (1 - u * u);
            } else
                y = 0.1f - 0.35f * ramp(t, DOWN_AT, 1.25f) + 0.25f * ramp(t, 1.3f, 1.7f);
            const float lean = (0.3f * ramp(t, 0, 0.3f) - 0.5f * ramp(t, 0.3f, UP_AT)) * (1 - ramp(t, UP_AT, 0.7f)) +
                0.3f * ramp(t, DOWN_AT, 1.25f) * (1 - ramp(t, 1.3f, 1.7f));
            p.body(Quat::axisAngle({1, 0, 0}, lean - TAU * spin), {0, y, -0.1f * std::sin(PI * spin)});
            if (t <= UP_AT || t >= DOWN_AT)
                stand(p);
            else {
                const float tuck = ramp(t, UP_AT, 0.6f) * (1 - ramp(t, 0.95f, DOWN_AT));
                for (int s = 0; s < 2; ++s)
                    p.leg(s, towards({0, -1, 0}, {0, 0.35f, 1}, tuck), 2.2f * tuck, 0.3f * tuck);
            }
            for (int s = 0; s < 2; ++s) {
                const float sx = s ? -1.f : 1.f;
                armKeys(p, s,
                        {{0, restArm(s)},
                         {0.3f, {normalize(V3{sx * 0.2f, -0.8f, -0.5f}), 0.2f, 0, {}, {}}},
                         {UP_AT, {normalize(V3{sx * 0.3f, 1, 0.1f}), 0.2f, 0, {}, {}}},
                         {0.65f, {normalize(V3{sx * 0.3f, 0.2f, 1}), 0.8f, 0, {}, {}}},
                         {0.95f, {normalize(V3{sx * 0.3f, 0.2f, 1}), 0.8f, 0, {}, {}}},
                         {1.2f, {normalize(V3{sx * 0.3f, -0.2f, 1}), 0.4f, 0, {}, {}}},
                         {1.65f, restArm(s)}},
                        t);
            }
        }

        void poseSadKick(CPoser& p, float t) {
            const float down = ramp(t, 0, 0.4f) * (1 - ramp(t, 2.4f, 3.0f));
            p.body({}, {0.03f * down, -0.02f * down, 0});
            p.bend(HB_CHEST, Quat::axisAngle({1, 0, 0}, 0.15f * down));
            p.bend(HB_NECK, Quat::axisAngle({1, 0, 0}, 0.25f * down));
            p.bend(HB_HEAD, Quat::axisAngle({1, 0, 0}, 0.35f * down));
            p.step(0, p.ankle(0));
            // the right foot scuffs the ground: back, forward along it, home
            struct SFootKey {
                float t;
                V3    at; // off the ankle's rest
                float pitch;
            };
            static constexpr SFootKey KEYS[] = {{0.8f, {0, 0, 0}, 0},        {1.0f, {0, 0.06f, -0.12f}, 0.3f}, {1.3f, {0, 0.04f, 0.25f}, 0.5f},
                                                {1.45f, {0, 0.02f, 0.28f}, 0.2f}, {1.8f, {0, 0, 0}, 0}};
            V3    at    = KEYS[0].at;
            float pitch = 0;
            for (size_t k = 1; k < std::size(KEYS); ++k)
                if (t >= KEYS[k - 1].t && t < KEYS[k].t) {
                    const float w = ramp(t, KEYS[k - 1].t, KEYS[k].t);
                    at            = lerp(KEYS[k - 1].at, KEYS[k].at, w);
                    pitch         = lerpf(KEYS[k - 1].pitch, KEYS[k].pitch, w);
                }
            p.step(1, p.ankle(1) + at, pitch);
            for (int s = 0; s < 2; ++s) {
                SArm a = restArm(s);
                a.u    = normalize(V3{(s ? -0.06f : 0.06f), -1, 0.08f});
                a.bend = 0.1f;
                p.arm(s, restArm(s), a, down);
            }
        }

        void poseDie(CPoser& p, float t) {
            const float hit = ramp(t, 0, 0.1f) * (1 - ramp(t, 0.15f, 0.45f));
            const float s   = std::clamp((t - 0.15f) / 1.15f, 0.f, 1.f);
            const float th  = PI * 0.5f * s * s; // falling back
            const float sn = std::sin(th), cs = std::cos(th);
            p.body(Quat::axisAngle({1, 0, 0}, -th), {0, 0.12f + 0.88f * cs - 1, -0.03f * hit - 0.8f * sn});
            p.bend(HB_CHEST, Quat::axisAngle({1, 0, 0}, -0.2f * hit));
            p.bend(HB_HEAD, Quat::axisAngle({1, 0, 0}, -0.3f * hit) * Quat::axisAngle(UP, 0.6f * ramp(t, 0.9f, 1.4f)));
            if (t < 0.15f)
                stand(p);
            else {
                // the knees give; on the ground, the soles on it
                p.leg(0, {0, -std::cos(0.35f * sn), std::sin(0.35f * sn)}, 0.97f * sn);
                p.leg(1, {0, -std::cos(0.15f * sn), std::sin(0.15f * sn)}, 0.5f * sn);
            }
            for (int i = 0; i < 2; ++i) {
                const float sx = i ? -1.f : 1.f;
                p.arm(i, restArm(i), {normalize(V3{sx * 0.8f, 0.25f, -0.15f}), 0.3f, 0, {}, {}}, ramp(t, 0.15f, 0.9f));
            }
        }

        constexpr int8_t THEIRS = -1; // the player's gesture

        const SBuiltin BUILTINS[] = {
            {"Wave", 2.5f, false, false, true, {THEIRS, GESTURE_OPEN}, {{{"happy", 0.4f, 0.1f, 2.4f}}}, poseWave},
            {"Clap", 3.0f, false, false, true, {GESTURE_OPEN, GESTURE_OPEN}, {{{"happy", 0.7f, 0.2f, 2.8f}}}, poseClap},
            {"Point", 2.5f, false, false, true, {THEIRS, GESTURE_POINT}, {}, posePoint},
            {"Cheer", 3.0f, false, false, true, {GESTURE_FIST, GESTURE_FIST}, {{{"happy", 1, 0.1f, 2.8f}}}, poseCheer},
            {"Dance", 2.0f, true, false, true, {THEIRS, GESTURE_POINT}, {{{"happy", 0.6f}}}, poseDance},
            {"Backflip", 1.7f, false, false, false, {GESTURE_FIST, GESTURE_FIST}, {}, poseBackflip},
            {"Sad Kick", 3.0f, false, false, true, {THEIRS, THEIRS}, {{{"sad", 1, 0.2f, 2.8f}}}, poseSadKick},
            {"Die", 1.6f, false, true, false, {GESTURE_NEUTRAL, GESTURE_NEUTRAL}, {{{"blink", 1, 1.0f}}}, poseDie},
        };

        SNormClip bake(const SBuiltin& b, const SBody& body) {
            SNormClip nc;
            nc.name     = b.name;
            nc.duration = b.duration;
            CPoser    p(body);
            const int frames = std::max(1, (int)std::round(b.duration * 30));
            for (int i = 0; i <= frames; ++i) {
                const float t = b.duration * (float)i / (float)frames;
                p.start();
                b.pose(p, t);
                p.finish();
                nc.times.push_back(t);
                nc.turn.push_back(p.turn);
                nc.move.push_back(p.move);
                for (int k = 0; k < HB_COUNT; ++k)
                    nc.has[k] = nc.has[k] || p.set[k];
                nc.moves = nc.moves || length(p.move) > 1e-4f;
            }
            for (const auto& f : b.faces) {
                if (f.name.empty())
                    continue;
                SAnimChannel c;
                c.path  = PATH_T;
                auto key = [&](float at, float w) {
                    c.times.push_back(at);
                    c.values.insert(c.values.end(), {w, 0.f, 0.f});
                };
                const float to = f.to < 0 ? b.duration : f.to;
                if (b.loop)
                    key(0, f.weight);
                else {
                    const float in = std::min(f.from + 0.25f, (f.from + to) * 0.5f), out = std::max(to - 0.25f, in);
                    if (f.from > 0)
                        key(f.from, 0);
                    key(in, f.weight);
                    key(out, f.weight);
                    if (f.to >= 0)
                        key(to, 0);
                }
                nc.faces.emplace_back(std::string(f.name), std::move(c));
            }
            return nc;
        }

        // an emote of a clip made for the avatar: the faces it names that the avatar has
        std::shared_ptr<SAvatarEmote> emoteOf(const SAvatarModel& md, SAnimClip anim, const std::vector<std::pair<std::string, SAnimChannel>>& faces) {
            auto e  = std::make_shared<SAvatarEmote>();
            e->name = anim.name;
            e->anim = std::move(anim);
            for (const auto& [name, c] : faces)
                if (const int x = md.findExpression(name); x >= 0) {
                    e->faces.push_back(c);
                    e->faces.back().node = x;
                }
            return e;
        }

        std::vector<std::shared_ptr<SAvatarEmote>> builtinEmotes(const SAvatarModel& md, const std::atomic<bool>& cancel) {
            std::vector<std::shared_ptr<SAvatarEmote>> out;
            if (!md.humanoid)
                return out;
            const SRig  rig  = rigOf(md);
            const SBody body = bodyOf(rig);
            for (const auto& b : BUILTINS) {
                check(cancel);
                const SNormClip nc = bake(b, body);
                auto            e  = emoteOf(md, retarget(nc, rig), nc.faces);
                e->from            = "built in";
                e->loop            = b.loop;
                e->hold            = b.hold;
                e->grounded        = b.grounded;
                e->gesture         = b.gesture;
                out.push_back(std::move(e));
            }
            return out;
        }

        // a clip of a model that isn't a humanoid (or onto one that isn't), by the names of the nodes: turns as they
        // are, moves and sizes where the two are alike at rest
        SAnimClip clipByNames(const SAnimClip& clip, const SAvatarModel& from, const SAvatarModel& to) {
            SAnimClip out;
            out.name     = clip.name;
            out.duration = clip.duration;
            std::vector<int> map(from.nodes.size(), -2);
            auto             find = [&](int n) {
                if (map[n] != -2)
                    return map[n];
                map[n] = -1;
                for (int loose = 0; loose < 2 && map[n] < 0; ++loose) {
                    const std::string want = loose ? normName(from.nodes[n].name) : lower(from.nodes[n].name);
                    for (size_t i = 0; i < to.nodes.size() && map[n] < 0; ++i)
                        if ((loose ? normName(to.nodes[i].name) : lower(to.nodes[i].name)) == want)
                            map[n] = (int)i;
                }
                return map[n];
            };
            for (const auto& c : clip.channels) {
                const int n = find(c.node);
                if (n < 0)
                    continue;
                if (c.path != PATH_R) {
                    const V3 a = c.path == PATH_T ? from.nodes[c.node].rest.t : from.nodes[c.node].rest.s;
                    const V3 b = c.path == PATH_T ? to.nodes[n].rest.t : to.nodes[n].rest.s;
                    if (length(a - b) > 0.01f * std::max({length(a), length(b), 1e-3f}))
                        continue;
                }
                out.channels.push_back(c);
                out.channels.back().node = n;
            }
            return out;
        }

        // emotes from a file's clips (a VRM animation, or a glTF / VRM with clips), made for the avatar; `clip` picks
        // one by name
        std::vector<std::shared_ptr<SAvatarEmote>> emotesFromFile(const std::string& file, const SAvatarModel& target, const std::atomic<bool>& cancel,
                                                                  std::vector<std::string>& log, std::string& error, std::string_view clip = {});

        // an attack's swings, per arm, made for the avatar from a VRM animation in memory (see below)
        bool attackClips(const void* bytes, size_t size, const std::string& what, const SAvatarModel& target, const std::atomic<bool>& cancel,
                         std::array<SAvatarAttack, 2>& out, std::string& error);

        // the built in attacks, punches made in Blender (tools/blend2vrma.py): as third person sees it, and as first
        // person's eyes do
        constexpr unsigned char ATTACK_VRMA[] = {
#embed "../assets/attack.vrma"
        };
        constexpr unsigned char ATTACK_FIRST_VRMA[] = {
#embed "../assets/attack-first-person.vrma"
        };

        // a walk's or a run's body (see below)
        bool gaitClip(const void* bytes, size_t size, const std::string& what, const std::atomic<bool>& cancel, SGaitClip& out, std::string& error);

        // the built in walk and run, made in Blender (tools/blender/h3d_walk.py, tools/blend2vrma.py)
        constexpr unsigned char WALK_VRMA[] = {
#embed "../assets/walk.vrma"
        };
        constexpr unsigned char RUN_VRMA[] = {
#embed "../assets/run.vrma"
        };

        struct SBuild {
            const SAvatarRequest&     req;
            const std::atomic<bool>&  cancel;
            cgltf_data*               data = nullptr;
            SAvatarModel&             model;
            std::vector<std::string>& log;

            gltf::SMaterials          mats;
            std::vector<int>          nodeIndex; // glTF node -> ours
            std::vector<bool>         inScene;   // ours
            std::vector<int>          depth;
            std::vector<bool>         skinJoint; // ours: used by a skin
            std::vector<uint32_t>     skinBase;  // glTF skin -> first of its joints
            std::vector<M4>           restGlobal;
            std::map<std::tuple<int, int, int>, std::vector<uint32_t>> batches; // by material, part, variant map
            int                       part = 0;                                    // of the mesh node being read
            bool                      vrm = false;
            size_t                    skippedDraco = 0, skippedOther = 0;
            SJson                     vrmJson;                  // the VRM extension
            int                       vrmVersion = 0;           // 0 none, 1 VRM 0.x, 2 VRM 1.0
            std::vector<std::vector<SMorphDelta>> targets;      // per morph target of the mesh node being read
            SJson                     settings;                 // the settings file's, {} = none
            std::string               settingsName;             // its file name
            bool                      emoteSource = false;      // read for its clips, to make emotes of (see emotesFromFile)
            std::string               emotesMade;               // what emotes() made, for the log

            void nodes() {
                nodeIndex.assign(data->nodes_count, -1);
                auto add = [&](const cgltf_node* root, bool scene) {
                    std::vector<std::pair<const cgltf_node*, int>> stack{{root, -1}};
                    while (!stack.empty()) {
                        auto [nd, parent] = stack.back();
                        stack.pop_back();
                        const size_t gi = cgltf_node_index(data, nd);
                        if (nodeIndex[gi] >= 0)
                            continue;
                        const int me  = (int)model.nodes.size();
                        nodeIndex[gi] = me;
                        SAvatarNode n;
                        n.name   = nd->name ? nd->name : std::format("node {}", gi);
                        n.parent = parent;
                        if (nd->has_matrix) {
                            M4 m;
                            std::memcpy(m.m, nd->matrix, sizeof(m.m));
                            n.rest = decompose(m);
                        } else {
                            if (nd->has_translation)
                                n.rest.t = {nd->translation[0], nd->translation[1], nd->translation[2]};
                            if (nd->has_rotation)
                                n.rest.r = Quat{nd->rotation[0], nd->rotation[1], nd->rotation[2], nd->rotation[3]}.normalized();
                            if (nd->has_scale)
                                n.rest.s = {nd->scale[0], nd->scale[1], nd->scale[2]};
                        }
                        // (a broken file's: what isn't a number would make the whole skeleton's, and the walk's, none)
                        auto finite = [](const V3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
                        if (!finite(n.rest.t) || !finite(n.rest.s) || !std::isfinite(n.rest.r.x + n.rest.r.y + n.rest.r.z + n.rest.r.w)) {
                            log.push_back(std::format("{}'s transform isn't all numbers: left at its parent's", n.name));
                            n.rest = {};
                        }
                        model.nodes.push_back(std::move(n));
                        inScene.push_back(scene);
                        depth.push_back(parent >= 0 ? depth[parent] + 1 : 0);
                        // reversed, so children keep their order
                        for (size_t i = nd->children_count; i-- > 0;)
                            stack.push_back({nd->children[i], me});
                    }
                };
                const cgltf_scene* scene = data->scene ? data->scene : data->scenes_count ? &data->scenes[0] : nullptr;
                if (scene)
                    for (size_t i = 0; i < scene->nodes_count; ++i)
                        add(scene->nodes[i], true);
                for (size_t i = 0; i < data->nodes_count; ++i)
                    if (!data->nodes[i].parent)
                        add(&data->nodes[i], !scene);
                for (size_t i = 0; i < data->nodes_count; ++i) // only in a cycle
                    add(&data->nodes[i], false);

                restGlobal.resize(model.nodes.size());
                for (size_t i = 0; i < model.nodes.size(); ++i) {
                    const auto& n = model.nodes[i];
                    restGlobal[i] = n.parent >= 0 ? restGlobal[n.parent] * n.rest.matrix() : n.rest.matrix();
                }
                skinJoint.assign(model.nodes.size(), false);
            }

            void skins() {
                skinBase.resize(data->skins_count);
                for (size_t s = 0; s < data->skins_count; ++s) {
                    const cgltf_skin& sk = data->skins[s];
                    skinBase[s]          = (uint32_t)model.joints.size();
                    for (size_t j = 0; j < sk.joints_count; ++j) {
                        SAvatarJoint jt;
                        jt.node = nodeIndex[cgltf_node_index(data, sk.joints[j])];
                        if (sk.inverse_bind_matrices && j < sk.inverse_bind_matrices->count)
                            cgltf_accessor_read_float(sk.inverse_bind_matrices, j, jt.inverseBind.m, 16);
                        skinJoint[jt.node] = true;
                        model.joints.push_back(jt);
                    }
                }
            }

            void primitive(const cgltf_primitive& prim, int skin, const std::function<int()>& ownJoint) {
                if (prim.type != cgltf_primitive_type_triangles && prim.type != cgltf_primitive_type_triangle_strip && prim.type != cgltf_primitive_type_triangle_fan)
                    return;
                if (prim.has_draco_mesh_compression) {
                    ++skippedDraco;
                    return;
                }
                const cgltf_accessor *aPos = nullptr, *aNrm = nullptr, *aUV[2] = {nullptr, nullptr}, *aCol = nullptr, *aJoints = nullptr, *aWeights = nullptr;
                for (size_t i = 0; i < prim.attributes_count; ++i) {
                    const auto& at = prim.attributes[i];
                    switch (at.type) {
                        case cgltf_attribute_type_position: aPos = at.data; break;
                        case cgltf_attribute_type_normal: aNrm = at.data; break;
                        case cgltf_attribute_type_texcoord:
                            if (at.index < 2)
                                aUV[at.index] = at.data;
                            break;
                        case cgltf_attribute_type_color:
                            if (at.index == 0)
                                aCol = at.data;
                            break;
                        case cgltf_attribute_type_joints:
                            if (at.index == 0)
                                aJoints = at.data;
                            break;
                        case cgltf_attribute_type_weights:
                            if (at.index == 0)
                                aWeights = at.data;
                            break;
                        default: break;
                    }
                }
                if (!aPos || aPos->count == 0) {
                    ++skippedOther;
                    return;
                }
                const size_t n = aPos->count;
                // (a skin with no joints: as if it had none, it would name joints past the end)
                if (!aJoints || !aWeights || aJoints->count != n || aWeights->count != n || (skin >= 0 && data->skins[skin].joints_count == 0))
                    skin = -1;
                const int    mat       = prim.material ? (int)cgltf_material_index(data, prim.material) : mats.defaultMaterial;
                const size_t skinCount = skin >= 0 ? data->skins[skin].joints_count : 0;
                const int    own       = skin >= 0 ? -1 : ownJoint();

                std::vector<float> pos(n * 3), nrm, uv[2], col;
                cgltf_accessor_unpack_floats(aPos, pos.data(), n * 3);
                if (aNrm && aNrm->count == n) {
                    nrm.resize(n * 3);
                    cgltf_accessor_unpack_floats(aNrm, nrm.data(), n * 3);
                }
                for (int k = 0; k < 2; ++k) {
                    if (aUV[k] && aUV[k]->count == n) {
                        uv[k].resize(n * 2);
                        cgltf_accessor_unpack_floats(aUV[k], uv[k].data(), n * 2);
                    }
                }
                size_t colComps = 0;
                if (aCol && aCol->count == n) {
                    colComps = cgltf_num_components(aCol->type);
                    col.resize(n * colComps);
                    cgltf_accessor_unpack_floats(aCol, col.data(), n * colComps);
                }

                std::vector<uint32_t> idx;
                if (prim.indices) {
                    idx.resize(prim.indices->count);
                    for (size_t i = 0; i < idx.size(); ++i)
                        idx[i] = (uint32_t)cgltf_accessor_read_index(prim.indices, i);
                } else {
                    idx.resize(n);
                    std::iota(idx.begin(), idx.end(), 0u);
                }
                if (prim.type != cgltf_primitive_type_triangles) {
                    std::vector<uint32_t> list;
                    for (size_t i = 2; i < idx.size(); ++i) {
                        if (prim.type == cgltf_primitive_type_triangle_fan)
                            list.insert(list.end(), {idx[0], idx[i - 1], idx[i]});
                        else if (i % 2 == 0)
                            list.insert(list.end(), {idx[i - 2], idx[i - 1], idx[i]});
                        else
                            list.insert(list.end(), {idx[i - 1], idx[i - 2], idx[i]});
                    }
                    idx = std::move(list);
                }
                idx.resize(idx.size() / 3 * 3);

                if (nrm.empty()) {
                    nrm.assign(n * 3, 0.f);
                    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                        const uint32_t a = idx[t], b = idx[t + 1], c = idx[t + 2];
                        if (a >= n || b >= n || c >= n)
                            continue;
                        auto P = [&](uint32_t i) { return V3{pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]}; };
                        const V3 fn = cross(P(b) - P(a), P(c) - P(a));
                        for (uint32_t v : {a, b, c})
                            for (int k = 0; k < 3; ++k)
                                nrm[v * 3 + k] += (&fn.x)[k];
                    }
                    for (size_t i = 0; i < n; ++i) {
                        const V3 v = normalize({nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]});
                        nrm[i * 3] = v.x, nrm[i * 3 + 1] = v.y, nrm[i * 3 + 2] = v.z;
                    }
                }

                const uint32_t base = (uint32_t)model.vertices.size();
                const int      bu   = mats.baseUV[mat];
                for (size_t i = 0; i < n; ++i) {
                    SAvatarVertex v{};
                    std::copy_n(&pos[i * 3], 3, v.pos);
                    std::copy_n(&nrm[i * 3], 3, v.normal);
                    const auto &u0 = uv[bu], &u1 = uv[1 - bu];
                    if (!u0.empty())
                        std::copy_n(&u0[i * 2], 2, v.uv);
                    if (!u1.empty())
                        std::copy_n(&u1[i * 2], 2, v.uv1);
                    for (int c = 0; c < 4; ++c) {
                        const float f = c < (int)colComps ? col[i * colComps + c] : 1.f;
                        v.color[c]    = (uint8_t)std::lround(std::clamp(f, 0.f, 1.f) * 255.f);
                    }

                    if (skin < 0) {
                        v.joints[0]  = (uint16_t)own;
                        v.weights[0] = 255;
                    } else {
                        cgltf_uint j[4] = {0, 0, 0, 0};
                        float      w[4] = {0, 0, 0, 0};
                        cgltf_accessor_read_uint(aJoints, i, j, 4);
                        cgltf_accessor_read_float(aWeights, i, w, 4);
                        float sum = 0;
                        for (int k = 0; k < 4; ++k) {
                            if (j[k] >= skinCount || !(w[k] > 0.f))
                                w[k] = 0, j[k] = 0;
                            sum += w[k];
                        }
                        if (sum <= 1e-6f)
                            w[0] = sum = 1;
                        int q[4], total = 0, big = 0;
                        for (int k = 0; k < 4; ++k) {
                            q[k] = (int)std::lround(w[k] / sum * 255.f);
                            total += q[k];
                            if (q[k] > q[big])
                                big = k;
                        }
                        q[big] += 255 - total; // rounding: the weights must add up exactly
                        for (int k = 0; k < 4; ++k) {
                            v.joints[k]  = (uint16_t)(skinBase[skin] + j[k]);
                            v.weights[k] = (uint8_t)std::clamp(q[k], 0, 255);
                        }
                    }
                    model.vertices.push_back(v);
                }
                // the materials it takes in material variants (KHR_materials_variants), shared by the primitives alike
                std::vector<std::pair<int, int>> map;
                for (size_t k = 0; k < prim.mappings_count; ++k)
                    if (prim.mappings[k].material && prim.mappings[k].variant < data->variants_count)
                        map.push_back({(int)prim.mappings[k].variant, (int)cgltf_material_index(data, prim.mappings[k].material)});
                std::ranges::sort(map);
                int vm = 0;
                if (!map.empty()) {
                    const auto it = std::ranges::find(model.variantMaps, map);
                    vm            = (int)(it - model.variantMaps.begin());
                    if (it == model.variantMaps.end())
                        model.variantMaps.push_back(std::move(map));
                }
                auto& out = batches[{mat, part, vm}];
                for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                    if (idx[t] >= n || idx[t + 1] >= n || idx[t + 2] >= n)
                        continue;
                    out.insert(out.end(), {base + idx[t], base + idx[t + 1], base + idx[t + 2]});
                }

                // morph targets: only the vertices they move
                for (size_t t = 0; t < prim.targets_count && t < targets.size(); ++t) {
                    const cgltf_morph_target& mt = prim.targets[t];
                    const cgltf_accessor *    dp = nullptr, *dn = nullptr;
                    for (size_t a = 0; a < mt.attributes_count; ++a) {
                        const auto& at = mt.attributes[a];
                        if (!at.data || at.data->count != n || cgltf_num_components(at.data->type) != 3)
                            continue;
                        if (at.type == cgltf_attribute_type_position)
                            dp = at.data;
                        else if (at.type == cgltf_attribute_type_normal)
                            dn = at.data;
                    }
                    if (!dp && !dn)
                        continue;
                    std::vector<float> P(n * 3, 0.f), N(n * 3, 0.f);
                    if (dp)
                        cgltf_accessor_unpack_floats(dp, P.data(), n * 3);
                    if (dn)
                        cgltf_accessor_unpack_floats(dn, N.data(), n * 3);
                    auto& deltas = targets[t];
                    for (size_t i = 0; i < n; ++i) {
                        const float *p = &P[i * 3], *q = &N[i * 3];
                        if (std::abs(p[0]) + std::abs(p[1]) + std::abs(p[2]) < 1e-7f && std::abs(q[0]) + std::abs(q[1]) + std::abs(q[2]) < 1e-4f)
                            continue;
                        SMorphDelta d{(uint32_t)(base + i), {p[0], p[1], p[2]}, {q[0], q[1], q[2]}};
                        deltas.push_back(d);
                    }
                }
            }

            // names of a mesh's morph targets: mesh.extras.targetNames, or (older UniVRM) the primitives'
            std::vector<std::string> targetNames(const cgltf_mesh& mesh) const {
                std::vector<std::string> out;
                for (size_t i = 0; i < mesh.target_names_count; ++i)
                    out.push_back(mesh.target_names[i] ? mesh.target_names[i] : "");
                if (!out.empty())
                    return out;
                for (size_t p = 0; p < mesh.primitives_count; ++p) {
                    const char* ex = mesh.primitives[p].extras.data;
                    SJson       j;
                    if (!ex || !CJsonReader(ex).read(j))
                        continue;
                    if (const auto* names = jarr(j.get("targetNames"))) {
                        for (const auto& s : *names)
                            out.emplace_back(jstr(&s));
                        return out;
                    }
                }
                return out;
            }

            void addMorphs(size_t gi) {
                const cgltf_node&              nd    = data->nodes[gi];
                const cgltf_mesh&              mesh  = *nd.mesh;
                const std::vector<std::string> names = targetNames(mesh);
                for (size_t t = 0; t < targets.size(); ++t) {
                    SAvatarMorph mo;
                    mo.name     = t < names.size() && !names[t].empty() ? names[t] : std::format("{} {}", model.nodes[nodeIndex[gi]].name, t);
                    mo.gltfNode = (int)gi;
                    mo.gltfMesh = (int)cgltf_mesh_index(data, &mesh);
                    mo.target   = (int)t;
                    mo.rest     = t < nd.weights_count ? nd.weights[t] : t < mesh.weights_count ? mesh.weights[t] : 0.f;
                    mo.first    = (uint32_t)model.morphDeltas.size();
                    mo.count    = (uint32_t)targets[t].size();
                    model.morphDeltas.insert(model.morphDeltas.end(), targets[t].begin(), targets[t].end());
                    model.morphs.push_back(std::move(mo));
                }
                targets.clear();
            }

            // the part a primitive belongs to: its mesh node's, or one of its own that its extras name ("hypr3d_part": what
            // unity2hypr3d splits off a mesh for an MA Mesh Cutter that a toggle switches)
            int primitivePart(const cgltf_primitive& prim, int nodePart, int ni, size_t gi) {
                SJson j;
                if (!prim.extras.data || !CJsonReader(prim.extras.data).read(j))
                    return nodePart;
                const std::string name(jstr(j.get("hypr3d_part")));
                if (name.empty())
                    return nodePart;
                for (size_t p = nodePart + 1; p < model.parts.size(); ++p)
                    if (model.parts[p].name == name)
                        return (int)p;
                model.parts.push_back({name, ni, (int)gi});
                return (int)model.parts.size() - 1;
            }

            void meshes() {
                for (size_t gi = 0; gi < data->nodes_count; ++gi) {
                    const cgltf_node& nd = data->nodes[gi];
                    const int         ni = nodeIndex[gi];
                    if (!nd.mesh || ni < 0 || !inScene[ni])
                        continue;
                    int  own      = -1;
                    auto ownJoint = [&] {
                        if (own < 0) {
                            own = (int)model.joints.size();
                            model.joints.push_back({ni, M4::identity()});
                        }
                        return own;
                    };
                    const int skin = nd.skin ? (int)cgltf_skin_index(data, nd.skin) : -1;
                    size_t    morphTargets = 0;
                    for (size_t p = 0; p < nd.mesh->primitives_count; ++p)
                        morphTargets = std::max(morphTargets, nd.mesh->primitives[p].targets_count);
                    targets.assign(morphTargets, {});
                    const int nodePart = (int)model.parts.size();
                    model.parts.push_back({model.nodes[ni].name.empty() ? std::format("mesh {}", nodePart) : model.nodes[ni].name, ni, (int)gi});
                    for (size_t p = 0; p < nd.mesh->primitives_count; ++p) {
                        part = primitivePart(nd.mesh->primitives[p], nodePart, ni, gi);
                        primitive(nd.mesh->primitives[p], skin, ownJoint);
                    }
                    part = nodePart;
                    addMorphs(gi);
                    check(cancel);
                }
                if (!model.morphDeltas.empty()) {
                    model.morphFirst = UINT32_MAX;
                    for (const auto& d : model.morphDeltas) {
                        model.morphFirst = std::min(model.morphFirst, d.vertex);
                        model.morphEnd   = std::max(model.morphEnd, d.vertex + 1);
                    }
                }

                // opaque and alpha tested first, blended last
                for (size_t v = 0; v < data->variants_count; ++v)
                    model.variants.push_back(data->variants[v].name ? data->variants[v].name : std::format("variant {}", v));
                for (int pass = 0; pass < 2; ++pass) {
                    if (pass == 1)
                        model.blendFrom = model.batches.size();
                    for (auto& [key, idx] : batches) {
                        const auto [mat, pt, vm] = key;
                        if (idx.empty() || (model.materials[mat].alphaMode == ALPHA_BLEND) != (pass == 1))
                            continue;
                        model.batches.push_back({mat, pt, (uint32_t)model.indices.size(), (uint32_t)idx.size(), vm});
                        model.parts[pt].triangles += idx.size() / 3;
                        model.indices.insert(model.indices.end(), idx.begin(), idx.end());
                    }
                }
                model.triangles = model.indices.size() / 3;
            }

            // --- humanoid

            V3 restPos(int node) const {
                return origin(restGlobal[node]);
            }

            bool isAncestor(int a, int b) const {
                for (int p = model.nodes[b].parent; p >= 0; p = model.nodes[p].parent)
                    if (p == a)
                        return true;
                return false;
            }

            bool fromVRM() {
                for (size_t i = 0; i < data->data_extensions_count; ++i) {
                    const auto& ext = data->data_extensions[i];
                    if (!ext.name || !ext.data)
                        continue;
                    const std::string_view name = ext.name;
                    // a VRM animation says which bone is which as VRM 1.0 does
                    const bool anim = name == "VRMC_vrm_animation";
                    if (name != "VRM" && name != "VRMC_vrm" && !anim)
                        continue;
                    vrm = !anim;
                    if (!CJsonReader(ext.data).read(vrmJson)) {
                        log.push_back(std::format("couldn't read the {} extension", name));
                        vrmJson = {};
                        return false;
                    }
                    vrmVersion         = name == "VRM" ? 1 : 2;
                    const SJson& root  = vrmJson;
                    const SJson* bones = root.get("humanoid") ? root.get("humanoid")->get("humanBones") : nullptr;
                    if (!bones)
                        return false;
                    auto set = [&](std::string_view bone, const SJson* node) {
                        const int hb = humanBoneOf(bone, vrmVersion == 2);
                        if (hb < 0 || !node || node->type != SJson::J_NUM || !(node->num >= 0 && node->num < (double)data->nodes_count))
                            return;
                        model.human[hb] = nodeIndex[(size_t)node->num];
                    };
                    if (bones->type == SJson::J_ARR) // VRM 0.x: [{"bone": "hips", "node": 3}, ...]
                        for (const auto& b : bones->arr) {
                            if (const SJson* bn = b.get("bone"); bn && bn->type == SJson::J_STR)
                                set(bn->str, b.get("node"));
                        }
                    else if (bones->type == SJson::J_OBJ) // VRM 1.0: {"hips": {"node": 3}, ...}
                        for (const auto& [bone, v] : bones->obj)
                            set(bone, v.get("node"));
                    model.humanFrom = name == "VRM" ? "VRM" : anim ? "VRMA" : "VRM 1.0";
                    return true;
                }
                return false;
            }

            void fromNames() {
                std::vector<int> order;
                for (int i = 0; i < (int)model.nodes.size(); ++i)
                    if (skinJoint[i] || (emoteSource && model.joints.empty())) // clips can come without a mesh
                        order.push_back(i);
                std::ranges::stable_sort(order, [&](int a, int b) { return depth[a] < depth[b]; });
                int spine = 0;
                for (int i : order) {
                    const SBoneName bn = boneName(model.nodes[i].name);
                    const SKey      k  = keyOf(bn.key, bn.side);
                    int             hb = -1;
                    switch (k.kind) {
                        case K_CENTER: hb = k.bone; break;
                        case K_SPINE:
                            if (spine < 3)
                                hb = HB_SPINE + spine++;
                            break;
                        case K_LEG: hb = (bn.side == 1 ? HB_L_UPPER_LEG : HB_R_UPPER_LEG) + k.bone; break;
                        case K_ARM: hb = (bn.side == 1 ? HB_L_SHOULDER : HB_R_SHOULDER) + k.bone; break;
                        case K_EYE: hb = bn.side == 1 ? HB_L_EYE : HB_R_EYE; break;
                        case K_TOES: hb = bn.side == 1 ? HB_L_TOES : HB_R_TOES; break;
                        case K_NONE: break;
                    }
                    if (hb >= 0 && model.human[hb] < 0)
                        model.human[hb] = i;
                }
                model.humanFrom = "bone names";
            }

            // the settings file's "humanoid": {"leftUpperArm": "Arm_L", "Left Thumb Proximal": "Thumb1_L", ...}, as the
            // converter has it from the Unity avatar; over what the rig says
            void fromSettings() {
                const SJson* map = settings.get("humanoid");
                if (!map || map->type != SJson::J_OBJ)
                    return;
                const bool vrm1 = std::ranges::any_of(map->obj, [](const auto& kv) { return lower(kv.first).contains("thumbmetacarpal"); });
                for (const auto& [bone, node] : map->obj) {
                    const int hb = humanBoneOf(bone, vrm1), n = nodeNamed(jstr(&node));
                    if (hb < 0 || n < 0) {
                        missing.push_back(hb < 0 ? std::format("humanoid bone {}", bone) : std::string(jstr(&node)));
                        continue;
                    }
                    std::ranges::replace(model.human, n, -1);
                    model.human[hb] = n;
                }
                model.humanFrom = model.humanFrom.empty() ? settingsName : model.humanFrom + ", " + settingsName;
            }

            // fingers the rig doesn't say: by their names under each hand ("Index1_L", "LeftHandPinky2",
            // "f_ring.01.L", "Bip01 L Finger21"), else by where they are
            void fingersFromNames() {
                std::vector<std::vector<int>> kidsOf(model.nodes.size());
                for (size_t i = 0; i < model.nodes.size(); ++i)
                    if (model.nodes[i].parent >= 0)
                        kidsOf[model.nodes[i].parent].push_back((int)i);
                auto& human = model.human;
                // 3ds Max's "Finger0" is the thumb, "Finger12" the index's third
                auto numbered = [&](int n) {
                    const auto t = tokens(model.nodes[n].name);
                    for (size_t i = 0; i + 1 < t.size(); ++i)
                        if ((t[i] == "finger" || t[i] == "fingers" || t[i] == "digit") && std::isdigit((unsigned char)t[i + 1][0]))
                            return t[i + 1][0] - '0';
                    return -1;
                };
                for (int hand = 0; hand < 2; ++hand) {
                    const int h = human[hand == 0 ? HB_L_HAND : HB_R_HAND];
                    if (h < 0)
                        continue;
                    std::vector<int> under; // the hand's bones, parents first
                    for (size_t i = 0; i < under.size() + 1; ++i)
                        for (int c : kidsOf[i == 0 ? h : under[i - 1]])
                            under.push_back(c);
                    auto taken = [&](int n) { return std::ranges::find(human, n) != human.end() || (!model.joints.empty() && !skinJoint[n]); };
                    // down a finger from its first bone: to the child that's the same finger, else the only child
                    auto follow = [&](int n, int f, const std::function<int(int)>& which) {
                        for (int s = 0; s < 3 && n >= 0 && !taken(n); ++s) {
                            human[fingerBone(hand, f, s)] = n;
                            int next = -1;
                            for (int c : kidsOf[n])
                                if (which(c) == f)
                                    next = next < 0 ? c : next;
                            n = next >= 0 ? next : kidsOf[n].size() == 1 ? kidsOf[n][0] : -1;
                        }
                    };
                    auto mapped = [&] {
                        return std::ranges::any_of(std::views::iota(0, (int)FINGER_COUNT), [&](int f) { return human[fingerBone(hand, f, 0)] >= 0; });
                    };
                    const bool had = mapped();
                    for (int f = 0; f < FINGER_COUNT; ++f)
                        if (human[fingerBone(hand, f, 0)] < 0)
                            for (int n : under)
                                if (fingerOf(model.nodes[n].name) == f) {
                                    follow(n, f, [&](int c) { return fingerOf(model.nodes[c].name); });
                                    break;
                                }
                    if (had || mapped())
                        continue;
                    // counted from 0 if there's a Finger0
                    const int base = std::ranges::any_of(under, [&](int n) { return numbered(n) == 0; }) ? 0 : 1;
                    for (int f = 0; f < FINGER_COUNT; ++f)
                        for (int n : under)
                            if (numbered(n) - base == f) {
                                follow(n, f, [&](int c) { return numbered(c) - base; });
                                break;
                            }
                    if (mapped())
                        continue;

                    // by where they are: chains out of the hand; the thumb's starts nearest the wrist, then the one
                    // furthest forward is the index (a T or A pose has the palms down)
                    std::vector<int> chains;
                    for (int root = h, i = 0; i < 3 && root >= 0; ++i) { // through a "FingersBase" or two
                        chains.clear();
                        for (int c : kidsOf[root])
                            if (!kidsOf[c].empty() && (model.joints.empty() || skinJoint[c]))
                                chains.push_back(c);
                        root = chains.size() == 1 ? chains[0] : -1;
                    }
                    if (chains.size() < 4 || chains.size() > 5)
                        continue;
                    const V3 wrist = restPos(h);
                    V3       mid{};
                    for (int c : chains)
                        mid += restPos(c) * (1.f / chains.size());
                    const V3 along = normalize(mid - wrist);
                    auto     out   = [&](int c) { return dot(restPos(c) - wrist, along); };
                    std::ranges::sort(chains, [&](int a, int b) { return out(a) < out(b); });
                    if (chains.size() == 5 || out(chains[0]) < 0.6f * out(chains.back())) {
                        follow(chains[0], FINGER_THUMB, [](int) { return -1; });
                        chains.erase(chains.begin());
                    }
                    const V3 fwd = cross(restPos(human[HB_L_UPPER_LEG]) - restPos(human[HB_R_UPPER_LEG]), UP);
                    std::ranges::sort(chains, [&](int a, int b) { return dot(restPos(a), fwd) > dot(restPos(b), fwd); });
                    int f = FINGER_INDEX;
                    for (int c : chains)
                        follow(c, f++, [](int) { return -1; });
                }
            }

            // the fingers the rig doesn't say, then each only as far as it hangs together
            void fingers() {
                if (!model.joints.empty() || emoteSource)
                    fingersFromNames();
                auto& h = model.human;
                for (int hand = 0; hand < 2; ++hand)
                    for (int f = 0; f < FINGER_COUNT; ++f) {
                        int up = h[hand == 0 ? HB_L_HAND : HB_R_HAND], s = 0;
                        for (; s < 3; ++s) {
                            const int n = h[fingerBone(hand, f, s)];
                            if (n < 0 || up < 0 || !isAncestor(up, n))
                                break;
                            up = n;
                        }
                        for (int k = s; k < 3; ++k)
                            h[fingerBone(hand, f, k)] = -1;
                        model.fingers += s == 3;
                    }
            }

            void humanoid() {
                if (!fromVRM() && (!model.joints.empty() || emoteSource))
                    fromNames();
                fromSettings();
                const auto& h = model.human;
                for (int b : {HB_HIPS, HB_HEAD, HB_L_UPPER_LEG, HB_L_LOWER_LEG, HB_R_UPPER_LEG, HB_R_LOWER_LEG, HB_L_UPPER_ARM, HB_L_LOWER_ARM, HB_R_UPPER_ARM,
                              HB_R_LOWER_ARM})
                    if (h[b] < 0)
                        return;
                // the chain has to hang together
                for (auto [a, b] : {std::pair{HB_L_UPPER_LEG, HB_L_LOWER_LEG}, {HB_R_UPPER_LEG, HB_R_LOWER_LEG}, {HB_L_UPPER_ARM, HB_L_LOWER_ARM},
                                    {HB_R_UPPER_ARM, HB_R_LOWER_ARM}})
                    if (!isAncestor(h[a], h[b]))
                        return;
                for (auto [a, b] : {std::pair{HB_L_LOWER_LEG, HB_L_FOOT}, {HB_R_LOWER_LEG, HB_R_FOOT}, {HB_L_LOWER_ARM, HB_L_HAND}, {HB_R_LOWER_ARM, HB_R_HAND}})
                    if (h[b] >= 0 && !isAncestor(h[a], h[b]))
                        model.human[b] = -1;
                // the face hangs off the head, toes off the feet
                for (auto [a, b] : {std::pair{HB_HEAD, HB_L_EYE}, {HB_HEAD, HB_R_EYE}, {HB_HEAD, HB_JAW}, {HB_L_FOOT, HB_L_TOES}, {HB_R_FOOT, HB_R_TOES}})
                    if (h[b] >= 0 && (h[a] < 0 || !isAncestor(h[a], h[b])))
                        model.human[b] = -1;
                if (restPos(h[HB_HEAD]).y <= restPos(h[HB_HIPS]).y || restPos(h[HB_L_LOWER_LEG]).y >= restPos(h[HB_L_UPPER_LEG]).y ||
                    restPos(h[HB_R_LOWER_LEG]).y >= restPos(h[HB_R_UPPER_LEG]).y)
                    return;
                fingers();
                model.humanoid = true;
            }

            // --- size and facing

            void place() {
                const auto& h   = model.human;
                const V3    fwd = facingOf(model, restGlobal);
                model.forward   = fwd;

                // bounds of the skinned mesh at rest
                std::vector<M4> skin(model.joints.size());
                for (size_t j = 0; j < skin.size(); ++j)
                    skin[j] = restGlobal[model.joints[j].node] * model.joints[j].inverseBind;
                SAABB b = SAABB::empty();
                for (const auto& v : model.vertices) {
                    V3 p{};
                    for (int k = 0; k < 4; ++k)
                        if (v.weights[k])
                            p += skin[v.joints[k]].point({v.pos[0], v.pos[1], v.pos[2]}) * (v.weights[k] / 255.f);
                    b.grow(p);
                }
                const V3    size   = b.size();
                const float height = std::max(size.y, 1e-4f);
                const float extent = std::max({size.x, size.y, size.z, 1e-4f});
                float       s      = 1;
                if (req.height > 0)
                    s = req.height / height;
                else if (model.humanoid) {
                    if (height < 0.5f || height > 2.5f) {
                        s = 1.75f / height;
                        log.push_back(std::format("the avatar is {:.3g} units tall, scaling it to 1.75 m (set plugin:hypr3d:avatar_height to override)", height));
                    }
                } else if (extent < 0.2f || extent > 3.f) {
                    s = 1.2f / extent;
                    log.push_back(std::format("the avatar is {:.3g} units across, scaling it to 1.2 m (set plugin:hypr3d:avatar_height to override)", extent));
                }

                const V3   rt   = normalize(cross(fwd, UP));
                const Quat turn = Quat::fromBasis(rt, UP, -fwd).conj(); // fwd -> -Z
                V3         pivot = b.center();
                if (model.humanoid)
                    pivot = restPos(h[HB_HIPS]);
                const V3 p = turn.rotate(pivot) * s;
                // on its lowest point, or where the settings file's "floor" says the ground is (MA's Floor Adjuster)
                const float floor = (float)jnum(settings.get("floor"), b.min.y);
                model.fix    = M4::trs({-p.x, -floor * s, -p.z}, turn, {s, s, s});
                model.scale  = s;
                model.height = height * s;
            }

            // where each foot of a humanoid touches the ground at rest, from the skin that goes with the foot (a shoe,
            // the toes): the back of its sole is the heel, the front the tips of the toes, the ball of the foot most of
            // the way between (at the toes' bone, if it has one); a guess from the height when there's too little of it
            void footShapes() {
                const auto& h = model.human;
                if (!model.humanoid)
                    return;
                std::vector<int8_t> side(model.nodes.size(), -1); // the foot a node goes with
                for (int s = 0; s < 2; ++s)
                    if (const int f = h[s ? HB_R_FOOT : HB_L_FOOT]; f >= 0)
                        side[f] = (int8_t)s;
                for (size_t i = 0; i < model.nodes.size(); ++i) // (parents come first)
                    if (const int p = model.nodes[i].parent; side[i] < 0 && p >= 0)
                        side[i] = side[p];
                std::vector<M4> skin(model.joints.size());
                for (size_t j = 0; j < skin.size(); ++j)
                    skin[j] = restGlobal[model.joints[j].node] * model.joints[j].inverseBind;
                std::array<std::vector<V3>, 2> pts;
                for (const auto& v : model.vertices) {
                    int most = 0; // the joint it follows most
                    for (int k = 1; k < 4; ++k)
                        if (v.weights[k] > v.weights[most])
                            most = k;
                    const int s = v.weights[most] ? side[model.joints[v.joints[most]].node] : -1;
                    if (s < 0)
                        continue;
                    V3 p{};
                    for (int k = 0; k < 4; ++k)
                        if (v.weights[k])
                            p += skin[v.joints[k]].point({v.pos[0], v.pos[1], v.pos[2]}) * (v.weights[k] / 255.f);
                    pts[s].push_back(model.fix.point(p));
                }
                for (int s = 0; s < 2; ++s) {
                    const int   foot  = h[s ? HB_R_FOOT : HB_L_FOOT] >= 0 ? h[s ? HB_R_FOOT : HB_L_FOOT] : h[s ? HB_R_LOWER_LEG : HB_L_LOWER_LEG];
                    const V3    ankle = model.fix.point(origin(restGlobal[foot]));
                    SFootShape& fs    = model.feet[s];
                    // a foot as long as a sixth of the height, the ankle a quarter of the way from the heel
                    const float len = 0.15f * std::max(model.height, 0.3f), up = std::max(ankle.y, 0.02f);
                    fs.heel = {0, -up, 0.25f * len};
                    fs.ball = {0, -up, -0.5f * len};
                    fs.toe  = {0, -up, -0.75f * len};
                    if (pts[s].size() < 12)
                        continue;
                    float low = 1e30f;
                    for (const V3& p : pts[s])
                        low = std::min(low, p.y);
                    // the sole: its lowest centimetre or two, from back (+z) to front
                    const float band = std::max(0.015f, 0.01f * model.height);
                    float       back = -1e30f, front = 1e30f;
                    for (const V3& p : pts[s])
                        if (p.y < low + band) {
                            back  = std::max(back, p.z);
                            front = std::min(front, p.z);
                        }
                    if (back - front < 0.3f * len || back < ankle.z - 0.02f || front > ankle.z + 0.02f)
                        continue; // not a foot's sole
                    const int toes = h[s ? HB_R_TOES : HB_L_TOES];
                    float     ballZ = back + (front - back) * 0.72f;
                    if (toes >= 0)
                        if (const float z = model.fix.point(origin(restGlobal[toes])).z; z < back && z > front)
                            ballZ = z;
                    // how low the sole is at the heel and at the ball (a high heel stands on both)
                    auto lowNear = [&](float z, float reach) {
                        float y = 1e30f;
                        for (const V3& p : pts[s])
                            if (std::abs(p.z - z) < reach)
                                y = std::min(y, p.y);
                        return y < 1e29f ? y : low;
                    };
                    const float heelY = lowNear(back - 0.015f, 0.02f), ballY = lowNear(ballZ, 0.02f);
                    fs.heel     = {0, heelY - ankle.y, std::max(back - 0.01f, ankle.z + 0.01f) - ankle.z};
                    fs.ball     = {0, ballY - ankle.y, std::min(ballZ, ankle.z - 0.02f) - ankle.z};
                    fs.toe      = {0, low - ankle.y, std::min(front, ballZ - 0.01f) - ankle.z};
                    fs.measured = true;
                }
            }

            // what a humanoid's arms keep clear of (SBodyClearance), from the skin at rest that's shown: the body's, but
            // for the arms, the head (and its hair) and what constraints turn after something else (a backpack's arm that
            // follows the arm), up to the chest and no further out than a skirt goes (not what it carries: wings); and
            // each arm's own round its bones
            void bodyClearance() {
                const auto& h = model.human;
                if (!model.humanoid || h[HB_HIPS] < 0)
                    return;
                // per node: -1 the head's, 0 the body's, 1 + 3 * side + k an arm's (k: 0 upper arm, 1 forearm, 2 hand)
                std::vector<int8_t> part(model.nodes.size(), 0);
                const int           head = h[HB_NECK] >= 0 ? h[HB_NECK] : h[HB_HEAD];
                for (const auto& nc : model.constraints)
                    if (nc.type != SNodeConstraint::ROLL && nc.node >= 0)
                        part[nc.node] = -1;
                for (size_t i = 0; i < model.nodes.size(); ++i) { // (parents come first)
                    const int p = model.nodes[i].parent;
                    part[i]     = (int)i == head || part[i] < 0 ? -1 : p >= 0 ? part[p] : 0;
                    for (int s = 0; s < 2; ++s)
                        for (int k = 0; k < 3; ++k)
                            if ((int)i == h[(s ? HB_R_UPPER_ARM : HB_L_UPPER_ARM) + k])
                                part[i] = (int8_t)(1 + 3 * s + k);
                }
                std::vector<uint8_t> shown(model.vertices.size(), 0);
                for (const auto& bt : model.batches)
                    if (bt.part < 0 || bt.part >= (int)model.parts.size() || !model.parts[bt.part].hidden)
                        for (uint32_t k = bt.first; k < bt.first + bt.count && k < model.indices.size(); ++k)
                            shown[model.indices[k]] = 1;
                // a part whose skin all follows one hand alone (a microphone, a sword) is held, not the hand: it doesn't
                // count for how long and thick the hand is (an arm held out till a long one cleared a skirt would stick out)
                auto handOnly = [&](uint32_t v) {
                    const auto& vx = model.vertices[v];
                    for (int k = 0; k < 4; ++k)
                        if (vx.weights[k] >= 250) {
                            const int pt = part[model.joints[vx.joints[k]].node];
                            return pt == 3 || pt == 6; // (1 + 3 * side + 2)
                        }
                    return false;
                };
                std::vector<uint8_t> held(model.vertices.size(), 0);
                std::vector<int8_t>  heldPart(model.parts.size(), 1);
                for (const auto& bt : model.batches)
                    if (bt.part >= 0 && bt.part < (int)model.parts.size())
                        for (uint32_t k = bt.first; k < bt.first + bt.count && k < model.indices.size() && heldPart[bt.part]; ++k)
                            heldPart[bt.part] = handOnly(model.indices[k]);
                for (const auto& bt : model.batches)
                    if (bt.part >= 0 && bt.part < (int)model.parts.size() && heldPart[bt.part] && model.parts.size() > 1)
                        for (uint32_t k = bt.first; k < bt.first + bt.count && k < model.indices.size(); ++k)
                            held[model.indices[k]] = 1;
                std::vector<M4> skin(model.joints.size());
                for (size_t j = 0; j < skin.size(); ++j)
                    skin[j] = restGlobal[model.joints[j].node] * model.joints[j].inverseBind;
                // the arms' bones at rest, and the way the hand goes (toward its middle finger)
                std::array<std::array<V3, 3>, 2> bone{};
                std::array<V3, 2>                handDir{};
                std::array<bool, 2>              arm{};
                for (int s = 0; s < 2; ++s) {
                    const int ua = s ? HB_R_UPPER_ARM : HB_L_UPPER_ARM;
                    if (!(arm[s] = h[ua] >= 0 && h[ua + 1] >= 0 && h[ua + 2] >= 0))
                        continue;
                    for (int k = 0; k < 3; ++k)
                        bone[s][k] = model.fix.point(restPos(h[ua + k]));
                    handDir[s] = bone[s][2] - bone[s][1];
                    for (int f : {FINGER_MIDDLE, FINGER_INDEX, FINGER_RING, FINGER_LITTLE})
                        if (const int n = h[fingerBone(s, f, 0)]; n >= 0) {
                            handDir[s] = model.fix.point(restPos(n)) - bone[s][2];
                            break;
                        }
                    handDir[s] = normalize(handDir[s]);
                    auto finite = [](const V3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
                    arm[s]      = finite(bone[s][0]) && finite(bone[s][1]) && finite(bone[s][2]) && finite(handDir[s]); // (not a broken file's)
                }
                const V3 hips = model.fix.point(restPos(h[HB_HIPS]));
                if ((!arm[0] && !arm[1]) || !std::isfinite(hips.x) || !std::isfinite(hips.y) || !std::isfinite(hips.z))
                    return;
                const int   chest = h[HB_CHEST] >= 0 ? h[HB_CHEST] : h[HB_SPINE];
                const float top = chest >= 0 ? model.fix.point(restPos(chest)).y : hips.y + 0.15f * model.height, far = 0.25f * model.height;
                std::vector<V3>       body;
                std::array<std::vector<V3>, 6> limb; // (per arm and bone)
                for (size_t v = 0; v < model.vertices.size(); ++v) {
                    const auto& vx   = model.vertices[v];
                    int         most = 0; // the joint it follows most
                    for (int k = 1; k < 4; ++k)
                        if (vx.weights[k] > vx.weights[most])
                            most = k;
                    const int pt = vx.weights[most] ? part[model.joints[vx.joints[most]].node] : -1;
                    if (pt < 0 || !shown[v] || held[v])
                        continue;
                    V3 p{};
                    for (int k = 0; k < 4; ++k)
                        if (vx.weights[k])
                            p += skin[vx.joints[k]].point({vx.pos[0], vx.pos[1], vx.pos[2]}) * (vx.weights[k] / 255.f);
                    p = model.fix.point(p);
                    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                        continue; // (a broken file's skin)
                    if (pt == 0) {
                        if (p.y < top && std::hypot(p.x - hips.x, p.z - hips.z) < far)
                            body.push_back(p - hips);
                    } else if (arm[(pt - 1) / 3])
                        limb[pt - 1].push_back(p);
                }
                auto& c = model.clearance;
                if (body.empty())
                    return;
                float low = 0;
                for (const V3& p : body)
                    low = std::min(low, p.y);
                c.y0   = low;
                c.rows = std::max(1, (int)std::ceil((top - hips.y - low) / c.dy));
                c.out.assign((size_t)c.rows * c.bearings, 0.f);
                for (const V3& p : body) {
                    const int i = std::clamp((int)((p.y - c.y0) / c.dy), 0, c.rows - 1);
                    const int k = std::clamp((int)((std::atan2(p.z, p.x) + PI) / TAU * c.bearings), 0, c.bearings - 1);
                    c.out[(size_t)i * c.bearings + k] = std::max(c.out[(size_t)i * c.bearings + k], std::hypot(p.x, p.z));
                }
                for (int s = 0; s < 2; ++s) {
                    if (!arm[s])
                        continue;
                    for (const V3& p : limb[3 * s + 2])
                        c.hand[s] = std::max(c.hand[s], dot(p - bone[s][2], handDir[s]));
                    c.hand[s] = std::max(c.hand[s], 0.02f);
                    for (int k = 0; k < 3; ++k) {
                        const V3 a = bone[s][k], d = k < 2 ? bone[s][k + 1] - a : handDir[s] * c.hand[s];
                        const float l2 = std::max(dot(d, d), 1e-8f);
                        for (const V3& p : limb[3 * s + k]) {
                            const float t = dot(p - a, d) / l2;
                            auto&       r = c.arm[s][k][std::clamp((int)(t * 4.f), 0, 3)];
                            r             = std::max(r, length(p - a - d * t));
                        }
                    }
                }
                c.measured = true;
            }

            // First person, the camera in the eyes: each batch without the triangles that go with the head (it and what
            // hangs off it: the face, hair, a hat), a copy of the rest after all the batches' own (SAvatarBatch::fpFirst;
            // theirs drawn as before, in the same order: blended ones look the same): a triangle is the head's when a
            // corner of it mostly is. And where the eyes are, between them: the eye bones', else a guess from the head's
            // skin. A humanoid with a head and both arms (its hands are posed in view) only
            void firstPerson() {
                const auto& h = model.human;
                if (!model.humanoid || h[HB_HEAD] < 0)
                    return;
                for (int b : {HB_L_UPPER_ARM, HB_L_LOWER_ARM, HB_L_HAND, HB_R_UPPER_ARM, HB_R_LOWER_ARM, HB_R_HAND})
                    if (h[b] < 0)
                        return;
                const int            headN = h[HB_HEAD];
                std::vector<uint8_t> headNode(model.nodes.size(), 0);
                for (size_t i = 0; i < model.nodes.size(); ++i) // (parents come first)
                    headNode[i] = (int)i == headN || (model.nodes[i].parent >= 0 && headNode[model.nodes[i].parent]);
                std::vector<uint16_t> headW(model.vertices.size(), 0); // how much of it the head moves, of 255
                for (size_t v = 0; v < model.vertices.size(); ++v)
                    for (int k = 0; k < 4; ++k)
                        if (const auto& vx = model.vertices[v]; vx.weights[k] && vx.joints[k] < model.joints.size() && headNode[model.joints[vx.joints[k]].node])
                            headW[v] += vx.weights[k];
                auto                  isHead = [&](uint32_t i) { return i < headW.size() && headW[i] > 127; };
                const size_t          own    = model.indices.size();
                std::vector<uint32_t> body;
                size_t                hidden = 0;
                for (auto& bt : model.batches) {
                    if (bt.first + (size_t)bt.count > own || bt.count % 3)
                        continue; // (left drawn whole)
                    body.clear();
                    for (uint32_t t = 0; t < bt.count; t += 3) {
                        const uint32_t* tri = model.indices.data() + bt.first + t;
                        if (!isHead(tri[0]) && !isHead(tri[1]) && !isHead(tri[2]))
                            body.insert(body.end(), tri, tri + 3);
                    }
                    if (body.size() == bt.count)
                        continue;
                    hidden += (bt.count - body.size()) / 3;
                    bt.fpFirst = (uint32_t)model.indices.size(), bt.fpCount = (uint32_t)body.size();
                    model.indices.insert(model.indices.end(), body.begin(), body.end());
                }

                // the eyes: between the eye bones, where they're in the face (ahead of the head's joint, over it, under
                // its top)
                std::vector<M4> skin(model.joints.size());
                for (size_t j = 0; j < skin.size(); ++j)
                    skin[j] = restGlobal[model.joints[j].node] * model.joints[j].inverseBind;
                const V3 headAt = model.fix.point(restPos(headN));
                float    top    = headAt.y;
                std::vector<V3> face; // the head's skin near it, avatar space (not long hair)
                for (size_t v = 0; v < model.vertices.size(); ++v) {
                    if (headW[v] < 200)
                        continue;
                    const auto& vx = model.vertices[v];
                    V3          p{};
                    for (int k = 0; k < 4; ++k)
                        if (vx.weights[k])
                            p += skin[vx.joints[k]].point({vx.pos[0], vx.pos[1], vx.pos[2]}) * (vx.weights[k] / 255.f);
                    p = model.fix.point(p);
                    if (length(p - headAt) < 0.2f * model.height) {
                        face.push_back(p);
                        top = std::max(top, p.y);
                    }
                }
                V3          eyes;
                bool        found = false;
                std::string from  = "eye bones";
                if (h[HB_L_EYE] >= 0 && h[HB_R_EYE] >= 0) {
                    eyes  = model.fix.point((restPos(h[HB_L_EYE]) + restPos(h[HB_R_EYE])) * 0.5f);
                    found = eyes.y > headAt.y && eyes.y < std::max(top, headAt.y + 0.05f) && eyes.z < headAt.z - 0.01f;
                }
                if (!found) {
                    // about 45% of the way up from the head's joint to its top, as far ahead as the face goes there less
                    // a little (from the bridge of the nose back to the middle of the eyes)
                    from        = "a guess from the head";
                    const float y = headAt.y + 0.45f * std::max(top - headAt.y, 0.05f * model.height);
                    float       front = headAt.z - 0.05f * model.height;
                    for (const V3& p : face)
                        if (std::abs(p.y - y) < 0.015f * model.height && std::abs(p.x - headAt.x) < 0.025f * model.height)
                            front = std::min(front, p.z);
                    eyes = {headAt.x, y, front + 0.015f * model.height};
                }
                model.eyes      = restGlobal[headN].inverse().point(model.fix.inverse().point(eyes));
                model.eyeHeight = eyes.y;
                log.push_back(std::format("first person: the eyes {:.2f} m up, {:.1f} cm ahead of the head's joint ({}); {} of {} triangles the head's", eyes.y,
                                          (headAt.z - eyes.z) * 100.f, from, hidden, own / 3));
            }

            // --- clips

            static V3 keyT(const SAnimChannel& c, size_t k) {
                const size_t i = (c.interp == INTERP_CUBIC ? k * 3 + 1 : k) * 3;
                return {c.values[i], c.values[i + 1], c.values[i + 2]};
            }

            void clips() {
                for (size_t ai = 0; ai < data->animations_count; ++ai) {
                    const cgltf_animation& an = data->animations[ai];
                    SAnimClip              clip;
                    clip.name = an.name && *an.name ? an.name : std::format("clip {}", ai);
                    float t0 = 1e30f, t1 = -1e30f;
                    for (size_t ci = 0; ci < an.channels_count; ++ci) {
                        const auto& ch = an.channels[ci];
                        if (!ch.target_node || !ch.sampler || !ch.sampler->input || !ch.sampler->output)
                            continue;
                        SAnimChannel c;
                        switch (ch.target_path) {
                            case cgltf_animation_path_type_translation: c.path = PATH_T; break;
                            case cgltf_animation_path_type_rotation: c.path = PATH_R; break;
                            case cgltf_animation_path_type_scale: c.path = PATH_S; break;
                            default: continue; // morph weights
                        }
                        c.node = nodeIndex[cgltf_node_index(data, ch.target_node)];
                        c.interp = ch.sampler->interpolation == cgltf_interpolation_type_step ? INTERP_STEP :
                            ch.sampler->interpolation == cgltf_interpolation_type_cubic_spline ? INTERP_CUBIC :
                                                                                                    INTERP_LINEAR;
                        const size_t keys  = ch.sampler->input->count;
                        const size_t comps = c.path == PATH_R ? 4 : 3;
                        const size_t per   = c.interp == INTERP_CUBIC ? 3 : 1;
                        if (keys == 0 || ch.sampler->output->count < keys * per)
                            continue;
                        c.times.resize(keys);
                        cgltf_accessor_unpack_floats(ch.sampler->input, c.times.data(), keys);
                        // times in order, and within a quarter of an hour (a clip is baked at 60 frames a second)
                        if (!std::ranges::all_of(c.times, [](float t) { return t >= -MAX_CLIP && t <= MAX_CLIP; }) || !std::ranges::is_sorted(c.times) ||
                            c.times.back() - c.times.front() > MAX_CLIP)
                            continue;
                        c.values.resize(keys * per * comps);
                        for (size_t k = 0; k < keys * per; ++k)
                            cgltf_accessor_read_float(ch.sampler->output, k, &c.values[k * comps], comps);
                        t0 = std::min(t0, c.times.front());
                        t1 = std::max(t1, c.times.back());
                        clip.channels.push_back(std::move(c));
                    }
                    if (clip.channels.empty())
                        continue;
                    for (auto& c : clip.channels)
                        for (float& t : c.times)
                            t -= t0;
                    clip.duration = std::clamp(t1 - t0, 0.f, MAX_CLIP);
                    if (!emoteSource) // an emote goes where it goes (a jump, a fall)
                        rootMotion(clip);
                    model.clips.push_back(std::move(clip));
                }

                // pick one per kind, the plainest name wins ("Walk" over "Walk_Back")
                std::array<size_t, CLIP_COUNT> words;
                words.fill(SIZE_MAX);
                std::vector<int> unmatched;
                for (int i = 0; i < (int)model.clips.size(); ++i) {
                    const int kind = clipKind(model.clips[i].name);
                    if (kind < 0) {
                        unmatched.push_back(i);
                        continue;
                    }
                    const size_t w = tokens(model.clips[i].name).size();
                    if (w < words[kind]) {
                        words[kind]         = w;
                        model.clipFor[kind] = i;
                    }
                }
                if (model.clipFor[CLIP_WALK] < 0 && model.clipFor[CLIP_RUN] < 0 && unmatched.size() == 1)
                    model.clipFor[CLIP_WALK] = unmatched[0]; // one clip that says nothing: most likely a walk cycle
            }

            // How fast a clip moves forward, and taking that out: the player moves the avatar.
            void rootMotion(SAnimClip& clip) {
                if (clip.duration <= 0.05f)
                    return;
                const int     hips = model.human[HB_HIPS];
                SAnimChannel* root = nullptr;
                for (auto& c : clip.channels) {
                    if (c.path != PATH_T || c.times.size() < 2)
                        continue;
                    if (hips >= 0 ? c.node == hips : skinJoint[c.node] && (!root || depth[c.node] < depth[root->node]))
                        root = &c;
                }
                if (!root)
                    return;
                const int   parent = model.nodes[root->node].parent;
                const M4    pg     = parent >= 0 ? restGlobal[parent] : M4::identity();
                const V3    local  = keyT(*root, root->times.size() - 1) - keyT(*root, 0);
                V3          flat   = pg.dir(local);
                flat.y             = 0;
                const float dist   = length(flat) * model.scale;
                if (dist < 0.1f)
                    return;
                clip.naturalSpeed = dist / clip.duration;
                const V3 back     = pg.inverse().dir(flat); // the horizontal drift, in the channel's space
                const V3 slope    = back / clip.duration;
                const bool cubic  = root->interp == INTERP_CUBIC;
                for (size_t k = 0; k < root->times.size(); ++k) {
                    const V3 d = back * (root->times[k] / clip.duration);
                    float*   v = &root->values[(cubic ? k * 3 + 1 : k) * 3];
                    v[0] -= d.x, v[1] -= d.y, v[2] -= d.z;
                    if (cubic)
                        for (size_t tan : {k * 3, k * 3 + 2}) {
                            float* tv = &root->values[tan * 3];
                            tv[0] -= slope.x, tv[1] -= slope.y, tv[2] -= slope.z;
                        }
                }
            }

            // --- the face

            // every morph a VRM 0.x mesh or VRM 1.0 node index and target name
            void bindMorph(SExpression& e, bool byNode, int id, int target, float weight) const {
                for (size_t i = 0; i < model.morphs.size(); ++i) {
                    const auto& mo = model.morphs[i];
                    if (mo.target == target && (byNode ? mo.gltfNode : mo.gltfMesh) == id)
                        e.morphs.push_back({(int)i, weight});
                }
            }

            static eOverride overrideOf(std::string_view s) {
                return s == "block" ? OVERRIDE_BLOCK : s == "blend" ? OVERRIDE_BLEND : OVERRIDE_NONE;
            }

            static void setValue(SExpression::SMaterialBind& b, float x, float y, float z, float w) {
                b.value[0] = x, b.value[1] = y, b.value[2] = z, b.value[3] = w;
            }

            // blendShapeMaster: mesh binds weigh 0..100, material values are Unity's (colors in sRGB, V flipped)
            void vrm0Expressions() {
                const SJson* master = vrmJson.get("blendShapeMaster");
                const auto*  groups = jarr(master ? master->get("blendShapeGroups") : nullptr);
                if (!groups)
                    return;
                for (const auto& g : *groups) {
                    SExpression            e;
                    const std::string_view preset = jstr(g.get("presetName"));
                    e.name                        = jstr(g.get("name"));
                    e.preset                      = preset.empty() || preset == "unknown" ? -1 : presetOf(preset);
                    if (e.name.empty())
                        e.name = e.preset >= 0 ? std::string(PRESET_NAMES[e.preset]) : std::format("expression {}", model.expressions.size());
                    e.binary = jbool(g.get("isBinary"));
                    if (const auto* binds = jarr(g.get("binds")))
                        for (const auto& b : *binds)
                            bindMorph(e, false, gltf::fileInt(jnum(b.get("mesh"), -1), -1), gltf::fileInt(jnum(b.get("index"), -1), -1),
                                      (float)jnum(b.get("weight"), 100) / 100.f);
                    if (const auto* values = jarr(g.get("materialValues")))
                        for (const auto& v : *values) {
                            const std::string_view mat = jstr(v.get("materialName")), prop = jstr(v.get("propertyName"));
                            const auto*            tv  = jarr(v.get("targetValue"));
                            if (!tv || tv->size() < 4)
                                continue;
                            float t[4];
                            for (int k = 0; k < 4; ++k)
                                t[k] = (float)jnum(&(*tv)[k], 0);
                            for (size_t m = 0; m < data->materials_count; ++m) {
                                if (!data->materials[m].name || std::string_view(data->materials[m].name) != mat)
                                    continue;
                                SExpression::SMaterialBind mb;
                                mb.material = (int)m;
                                if (prop == "_MainTex_ST") {
                                    mb.prop = SExpression::MP_UV;
                                    setValue(mb, t[0], t[1], t[2], 1.f - t[3] - t[1]);
                                } else if (prop == "_Color") {
                                    mb.prop = SExpression::MP_COLOR;
                                    setValue(mb, srgbToLinear(t[0]), srgbToLinear(t[1]), srgbToLinear(t[2]), t[3]);
                                } else if (prop == "_EmissionColor") {
                                    mb.prop = SExpression::MP_EMISSIVE;
                                    setValue(mb, srgbToLinear(t[0]), srgbToLinear(t[1]), srgbToLinear(t[2]), 1);
                                } else
                                    continue; // MToon's own (shade, rim, outline)
                                e.materials.push_back(mb);
                            }
                        }
                    model.expressions.push_back(std::move(e));
                }
            }

            // expressions.preset and .custom: weights 0..1, colors linear, texture transforms in glTF's UV space
            void vrm1Expressions() {
                const SJson* ex = vrmJson.get("expressions");
                if (!ex)
                    return;
                auto material = [&](const SJson& b) {
                    const double m = jnum(b.get("material"), -1);
                    return m >= 0 && m < (double)data->materials_count ? (int)m : -1;
                };
                auto read = [&](const std::string& name, const SJson& j, int preset) {
                    SExpression e;
                    e.name           = name;
                    e.preset         = preset;
                    e.binary         = jbool(j.get("isBinary"));
                    e.overrideBlink  = overrideOf(jstr(j.get("overrideBlink")));
                    e.overrideLookAt = overrideOf(jstr(j.get("overrideLookAt")));
                    e.overrideMouth  = overrideOf(jstr(j.get("overrideMouth")));
                    if (const auto* binds = jarr(j.get("morphTargetBinds")))
                        for (const auto& b : *binds)
                            bindMorph(e, true, gltf::fileInt(jnum(b.get("node"), -1), -1), gltf::fileInt(jnum(b.get("index"), -1), -1), (float)jnum(b.get("weight"), 1));
                    if (const auto* binds = jarr(j.get("materialColorBinds")))
                        for (const auto& b : *binds) {
                            const int              m    = material(b);
                            const std::string_view type = jstr(b.get("type"));
                            const auto*            tv   = jarr(b.get("targetValue"));
                            if (m < 0 || !tv || tv->size() < 3 || (type != "color" && type != "emissionColor"))
                                continue; // MToon's own (shade, matcap, rim, outline)
                            SExpression::SMaterialBind mb;
                            mb.material = m;
                            mb.prop     = type == "color" ? SExpression::MP_COLOR : SExpression::MP_EMISSIVE;
                            setValue(mb, (float)jnum(&(*tv)[0], 1), (float)jnum(&(*tv)[1], 1), (float)jnum(&(*tv)[2], 1),
                                     tv->size() > 3 ? (float)jnum(&(*tv)[3], 1) : model.materials[m].baseColor[3]);
                            e.materials.push_back(mb);
                        }
                    if (const auto* binds = jarr(j.get("textureTransformBinds")))
                        for (const auto& b : *binds) {
                            const int m = material(b);
                            if (m < 0)
                                continue;
                            const auto *sc = jarr(b.get("scale")), *of = jarr(b.get("offset"));
                            auto        at = [](const std::vector<SJson>* a, size_t i, float def) { return a && a->size() > i ? (float)jnum(&(*a)[i], def) : def; };
                            SExpression::SMaterialBind mb;
                            mb.material = m;
                            mb.prop     = SExpression::MP_UV;
                            setValue(mb, at(sc, 0, 1), at(sc, 1, 1), at(of, 0, 0), at(of, 1, 0));
                            e.materials.push_back(mb);
                        }
                    model.expressions.push_back(std::move(e));
                };
                if (const SJson* p = ex->get("preset"); p && p->type == SJson::J_OBJ)
                    for (const auto& [name, j] : p->obj)
                        read(name, j, presetOf(name));
                if (const SJson* c = ex->get("custom"); c && c->type == SJson::J_OBJ)
                    for (const auto& [name, j] : c->obj)
                        read(name, j, -1);
            }

            // the shape keys a preset is likely made of, by their names
            // the name is `key` once words for parts of the face are taken off its front ("eye_smile", "lip_a")
            static bool bareIs(std::string_view name, std::string_view key) {
                static constexpr std::string_view PARTS[] = {"expression", "blendshape", "eyebrows", "eyebrow", "eyeblow", "mouth", "brows", "brow", "face",
                                                             "eyes",       "lips",       "fcl",      "all",     "mth",     "brw",   "eye",   "lip",  "bs"};
                for (const std::string_view part : PARTS)
                    if (name.size() > part.size() && name.starts_with(part) && (name.substr(part.size()) == key || bareIs(name.substr(part.size()), key)))
                        return true;
                return false;
            }

            // the first of the preset's alternatives with any shape key of its names; then again with the parts
            // of the face taken off the names
            bool guess(int preset, const std::vector<std::string>& names, std::vector<SExpression::SMorphBind>& out) const {
                for (int pass = 0; pass < 2; ++pass) {
                    for (const std::string_view alt : shapeKeyGuesses()[preset]) {
                        std::vector<SExpression::SMorphBind> binds;
                        for (size_t pos = 0; pos < alt.size();) {
                            size_t end = alt.find(' ', pos);
                            if (end == std::string_view::npos)
                                end = alt.size();
                            std::string_view key = alt.substr(pos, end - pos);
                            pos                  = end + 1;
                            float w              = 1;
                            if (const size_t c = key.find(':'); c != std::string_view::npos) {
                                w   = std::strtof(std::string(key.substr(c + 1)).c_str(), nullptr);
                                key = key.substr(0, c);
                            }
                            // VRoid's come with the mesh's prefix: "M_F00_000_00_Fcl_EYE_Close"
                            const bool suffix = key.starts_with("fcl");
                            for (size_t i = 0; i < names.size(); ++i)
                                if (pass == 0 ? (suffix ? names[i].ends_with(key) : names[i] == key) : !suffix && bareIs(names[i], key))
                                    binds.push_back({(int)i, w});
                        }
                        if (!binds.empty()) {
                            out = std::move(binds);
                            return true;
                        }
                    }
                }
                return false;
            }

            // "<avatar>.hypr3d.json" next to it: outfit toggles, and the faces VRChat's FX layer makes (the
            // converter writes it for VRChat avatars)
            void readSettings(const std::filesystem::path& file) {
                std::error_code ec;
                for (const auto& p : {std::filesystem::path(file).replace_extension(".hypr3d.json"), std::filesystem::path(file.string() + ".hypr3d.json")}) {
                    std::vector<uint8_t> text;
                    if (!std::filesystem::is_regular_file(p, ec) || !gltf::readFile(p.string(), text))
                        continue;
                    settingsName = p.filename().string();
                    CJsonReader r(std::string_view((const char*)text.data(), text.size()));
                    if (!r.read(settings) || settings.type != SJson::J_OBJ) {
                        log.push_back(std::format("{} isn't valid JSON (line {}), left out", settingsName, r.line()));
                        settings = {};
                        return;
                    }
                    model.settings = p.string();
                    return;
                }
            }

            // what the settings file names that the model doesn't have
            std::vector<std::string> missing;

            void morphsOf(const SJson* shapes, const std::function<void(int, float)>& add) {
                if (!shapes || shapes->type != SJson::J_OBJ)
                    return;
                for (const auto& [name, w] : shapes->obj) {
                    const auto found = model.findMorphs(name);
                    if (found.empty())
                        missing.push_back(name);
                    for (int m : found)
                        add(m, std::clamp((float)jnum(&w, 1), 0.f, 1.f));
                }
            }

            void partsOf(const SJson* names, std::vector<int>& out) {
                if (const auto* list = jarr(names))
                    for (const auto& n : *list) {
                        const auto found = model.findParts(jstr(&n));
                        if (found.empty())
                            missing.push_back(std::string(jstr(&n)));
                        out.insert(out.end(), found.begin(), found.end());
                    }
            }

            // the settings file's expressions, and those of them that say how blinking goes
            std::vector<int> mine;
            std::set<int>    ownBlink;

            // "expressions": [{"name", "preset", "shapes": {"shape key" or "mesh node/shape key": weight 0..1},
            // "binary", "blink"/"lookAt"/"mouth": "block", "blend" or "none"}]; for one the model has by that name,
            // what it leaves out stays as it was
            void settingsExpressions() {
                const auto* list = jarr(settings.get("expressions"));
                if (!list)
                    return;
                for (const auto& x : *list) {
                    const std::string name(jstr(x.get("name")));
                    if (name.empty())
                        continue;
                    int at = -1;
                    for (size_t i = 0; i < model.expressions.size() && at < 0; ++i)
                        if (lower(model.expressions[i].name) == lower(name))
                            at = (int)i;
                    if (at < 0) {
                        at = (int)model.expressions.size();
                        model.expressions.push_back({});
                        model.expressions[at].name = name;
                    }
                    SExpression& e = model.expressions[at];
                    if (const SJson* p = x.get("preset"))
                        if ((e.preset = presetOf(jstr(p))) < 0 && jstr(p) != "none")
                            missing.push_back(std::format("preset {}", jstr(p)));
                    if (const SJson* b = x.get("binary"))
                        e.binary = jbool(b);
                    if (const SJson* o = x.get("blink")) {
                        e.overrideBlink = overrideOf(jstr(o));
                        ownBlink.insert(at);
                    }
                    if (const SJson* o = x.get("lookAt"))
                        e.overrideLookAt = overrideOf(jstr(o));
                    if (const SJson* o = x.get("mouth"))
                        e.overrideMouth = overrideOf(jstr(o));
                    if (const SJson* shapes = x.get("shapes")) {
                        e.morphs.clear();
                        morphsOf(shapes, [&](int m, float w) { e.morphs.push_back({m, w}); });
                    }
                    if (std::ranges::find(mine, at) == mine.end())
                        mine.push_back(at);
                }
            }

            // material variants by name (the model's KHR_materials_variants)
            void variantsOf(const SJson* names, std::vector<int>& out) {
                if (const auto* list = jarr(names))
                    for (const auto& n : *list) {
                        const int v = model.findVariant(jstr(&n));
                        if (v < 0)
                            missing.push_back(std::format("material variant {}", jstr(&n)));
                        else if (std::ranges::find(out, v) == out.end())
                            out.push_back(v);
                    }
            }

            // "transforms": {"node": {"t": [x, y, z], "r": [x, y, z, w], "s": [x, y, z]}}: what of each node's own
            // translation, rotation and scale is set
            void posesOf(const SJson* j, std::vector<SNodePose>& out) {
                if (!j || j->type != SJson::J_OBJ)
                    return;
                for (const auto& [name, v] : j->obj) {
                    const int n = nodeNamed(name);
                    if (n < 0) {
                        missing.push_back(name);
                        continue;
                    }
                    SNodePose p;
                    p.node = n;
                    p.trs  = model.nodes[n].rest;
                    if (const auto* a = jarr(v.get("t")); a && a->size() == 3) {
                        p.set |= SNodePose::T;
                        p.trs.t = jvec(v.get("t"), p.trs.t);
                    }
                    if (const auto* a = jarr(v.get("r")); a && a->size() == 4) {
                        p.set |= SNodePose::R;
                        p.trs.r = Quat{(float)jnum(&(*a)[0], 0), (float)jnum(&(*a)[1], 0), (float)jnum(&(*a)[2], 0), (float)jnum(&(*a)[3], 1)}.normalized();
                    }
                    if (const auto* a = jarr(v.get("s")); a && a->size() == 3) {
                        p.set |= SNodePose::S;
                        p.trs.s = jvec(v.get("s"), p.trs.s);
                    }
                    if (p.set)
                        out.push_back(p);
                }
            }

            // "hidden": [parts]; "toggles": [{"name", "group" or "groups": [...], "on", "show": [parts], "hide": [parts],
            // "shapes": {...}, "variants": [material variants], "transforms": {...}, "loop": {"seconds", "a": {"shapes",
            // "transforms"}, "b": {...}}, "drop": [nodes]}]; "sliders": [{"name", "value", "keys": [{"at", "shapes",
            // "show", "hide", "variants", "transforms"}]}]; "fixed": [nodes]
            void outfit() {
                std::vector<int> hidden;
                partsOf(settings.get("hidden"), hidden);
                for (int p : hidden)
                    model.parts[p].hidden = true;
                if (const auto* f = jarr(settings.get("fixed")))
                    for (const auto& x : *f) {
                        if (const int n = nodeNamed(jstr(&x)); n >= 0)
                            model.fixed.push_back(n);
                        else
                            missing.push_back(std::string(jstr(&x)));
                    }
                if (const auto* list = jarr(settings.get("toggles")))
                    for (const auto& t : *list) {
                        SAvatarToggle tg;
                        tg.name = jstr(t.get("name"));
                        if (tg.name.empty() || model.findToggle(tg.name) >= 0)
                            continue;
                        if (const auto* gs = jarr(t.get("groups")))
                            for (const auto& g : *gs)
                                if (!jstr(&g).empty())
                                    tg.groups.emplace_back(jstr(&g));
                        if (tg.groups.empty() && !jstr(t.get("group")).empty())
                            tg.groups.emplace_back(jstr(t.get("group")));
                        tg.group = tg.groups.empty() ? std::string() : tg.groups[0];
                        tg.on    = jbool(t.get("on"));
                        partsOf(t.get("show"), tg.show);
                        partsOf(t.get("hide"), tg.hide);
                        morphsOf(t.get("shapes"), [&](int m, float w) { tg.shapes.push_back({m, w}); });
                        variantsOf(t.get("variants"), tg.variants);
                        posesOf(t.get("transforms"), tg.poses);
                        if (const SJson* l = t.get("loop"); l && l->type == SJson::J_OBJ && jnum(l->get("seconds"), 0) > 0.01) {
                            tg.loop.seconds = (float)jnum(l->get("seconds"), 0);
                            if (const SJson* a = l->get("a")) {
                                morphsOf(a->get("shapes"), [&](int m, float w) { tg.loop.shapesA.push_back({m, w}); });
                                posesOf(a->get("transforms"), tg.loop.posesA);
                            }
                            if (const SJson* b = l->get("b")) {
                                morphsOf(b->get("shapes"), [&](int m, float w) { tg.loop.shapesB.push_back({m, w}); });
                                posesOf(b->get("transforms"), tg.loop.posesB);
                            }
                        }
                        if (const auto* d = jarr(t.get("drop")))
                            for (const auto& x : *d) {
                                if (const int n = nodeNamed(jstr(&x)); n >= 0)
                                    tg.drop.push_back(n);
                                else
                                    missing.push_back(std::string(jstr(&x)));
                            }
                        // one of a group on at first, at most
                        if (tg.on)
                            for (const auto& o : model.toggles)
                                if (o.on && std::ranges::any_of(o.groups, [&](const std::string& g) { return std::ranges::find(tg.groups, g) != tg.groups.end(); }))
                                    tg.on = false;
                        model.toggles.push_back(std::move(tg));
                    }
                if (const auto* list = jarr(settings.get("sliders")))
                    for (const auto& s : *list) {
                        SAvatarSlider sl;
                        sl.name = jstr(s.get("name"));
                        if (sl.name.empty() || model.findSlider(sl.name) >= 0)
                            continue;
                        // "axes": 2: a 2D one, its "value" and keys' "at" [x, y], its keys a "grid" of n x n
                        const bool two = jnum(s.get("axes"), 1) == 2;
                        auto       xy  = [&](const SJson* j, float& x, float& y) {
                            const auto* a = jarr(j);
                            x = a && a->size() == 2 ? std::clamp((float)jnum(&(*a)[0], 0), -1.f, 1.f) : 0.f;
                            y = a && a->size() == 2 ? std::clamp((float)jnum(&(*a)[1], 0), -1.f, 1.f) : 0.f;
                        };
                        if (two) {
                            sl.grid = gltf::fileInt(jnum(s.get("grid"), 0), 2, 2, 1024);
                            xy(s.get("value"), sl.value, sl.valueY);
                        } else
                            sl.value = std::clamp((float)jnum(s.get("value"), 0), 0.f, 1.f);
                        if (const auto* keys = jarr(s.get("keys")))
                            for (const auto& k : *keys) {
                                SAvatarSlider::SKey key;
                                if (two)
                                    xy(k.get("at"), key.at, key.atY);
                                else
                                    key.at = std::clamp((float)jnum(k.get("at"), 0), 0.f, 1.f);
                                morphsOf(k.get("shapes"), [&](int m, float w) { key.shapes.push_back({m, w}); });
                                partsOf(k.get("show"), key.show);
                                partsOf(k.get("hide"), key.hide);
                                variantsOf(k.get("variants"), key.variants);
                                posesOf(k.get("transforms"), key.poses);
                                sl.keys.push_back(std::move(key));
                            }
                        if (two) {
                            std::ranges::stable_sort(sl.keys, [](const auto& a, const auto& b) { return a.atY != b.atY ? a.atY < b.atY : a.at < b.at; });
                            if (sl.keys.size() != (size_t)(sl.grid * sl.grid)) {
                                log.push_back(std::format("{}: the slider {} has {} keys, not a grid of {} x {}; left out", settingsName, sl.name, sl.keys.size(), sl.grid, sl.grid));
                                continue;
                            }
                        } else
                            std::ranges::stable_sort(sl.keys, {}, &SAvatarSlider::SKey::at);
                        if (!sl.keys.empty())
                            model.sliders.push_back(std::move(sl));
                    }
            }

            void expressions() {
                if (vrmVersion == 1)
                    vrm0Expressions();
                else if (vrmVersion == 2)
                    vrm1Expressions();
                const size_t fromVRM = model.expressions.size();
                settingsExpressions();
                const size_t defined = model.expressions.size();
                auto&        P       = model.preset;
                for (int i = 0; i < (int)defined; ++i)
                    if (const int p = model.expressions[i].preset; p >= 0 && P[p] < 0)
                        P[p] = i;
                // the settings file's take over their presets
                for (int i : mine)
                    if (const int p = model.expressions[i].preset; p >= 0 && P[p] != i) {
                        if (P[p] >= 0)
                            model.expressions[P[p]].preset = -1;
                        P[p] = i;
                    }
                // custom ones by a preset's name ("Surprised": VRM 0.x has no such preset)
                for (int i = 0; i < (int)defined; ++i) {
                    auto& e = model.expressions[i];
                    if (e.preset >= 0)
                        continue;
                    if (const int p = presetOf(e.name); p >= 0 && P[p] < 0) {
                        P[p]     = i;
                        e.preset = p;
                    }
                }

                // the rest from the shape keys' names (VRChat, VRoid, MMD, ARKit), empty ones too
                std::vector<std::string> names;
                for (const auto& mo : model.morphs)
                    names.push_back(normName(mo.name));
                bool guessed = false;
                for (int p = 0; p < EX_COUNT; ++p) {
                    if (P[p] >= 0 && (!model.expressions[P[p]].morphs.empty() || !model.expressions[P[p]].materials.empty()))
                        continue;
                    std::vector<SExpression::SMorphBind> binds;
                    if (!guess(p, names, binds))
                        continue;
                    guessed = true;
                    if (P[p] < 0) {
                        SExpression e;
                        e.name   = PRESET_NAMES[p];
                        e.preset = p;
                        P[p]     = (int)model.expressions.size();
                        model.expressions.push_back(std::move(e));
                    }
                    model.expressions[P[p]].morphs = std::move(binds);
                }
                // VRM 1.0 says what an expression blocks, and so can the settings file; for the others: no
                // blinking through a smile
                for (int p : {EX_HAPPY, EX_ANGRY, EX_SAD, EX_RELAXED, EX_SURPRISED})
                    if (const int e = P[p]; e >= 0 && !ownBlink.contains(e) && (e >= (int)fromVRM || vrmVersion != 2))
                        model.expressions[e].overrideBlink = OVERRIDE_BLEND;

                // and every shape key by its own name
                std::set<std::string>         taken;
                std::map<std::string, size_t> byName;
                for (const auto& e : model.expressions)
                    taken.insert(lower(e.name));
                for (size_t i = 0; i < model.morphs.size(); ++i) {
                    const std::string key = lower(model.morphs[i].name);
                    if (taken.contains(key))
                        continue;
                    const auto [it, fresh] = byName.try_emplace(key, model.expressions.size());
                    if (fresh) {
                        SExpression e;
                        e.name     = model.morphs[i].name;
                        e.shapeKey = true;
                        model.expressions.push_back(std::move(e));
                    }
                    model.expressions[it->second].morphs.push_back({(int)i, 1.f});
                }

                // the consonants lip sync shows (VRChat's visemes PP, FF, SS and CH): the settings file's "visemes"
                // ({"pp": {"shape key": weight, ...}, ...}, as the converter writes a VRChat avatar's), else shape keys by
                // VRChat's names ("vrc.v_pp"), which are expressions of their own by now
                static constexpr std::string_view CONSONANT[VISEME_COUNT - VOWEL_COUNT] = {"pp", "ff", "ss", "ch"};
                const SJson*                      vis = settings.get("visemes");
                for (int k = 0; k < VISEME_COUNT - VOWEL_COUNT; ++k) {
                    if (const SJson* shapes = vis && vis->type == SJson::J_OBJ ? vis->get(CONSONANT[k]) : nullptr) {
                        SExpression e;
                        e.name   = std::format("viseme {}", CONSONANT[k]);
                        e.viseme = true;
                        morphsOf(shapes, [&](int m, float w) { e.morphs.push_back({m, w}); });
                        if (!e.morphs.empty()) {
                            model.consonant[k] = (int)model.expressions.size();
                            model.expressions.push_back(std::move(e));
                            continue;
                        }
                    }
                    for (size_t e = 0; e < model.expressions.size(); ++e)
                        if (model.expressions[e].shapeKey && normName(model.expressions[e].name) == std::format("v{}", CONSONANT[k])) {
                            model.consonant[k]            = (int)e;
                            model.expressions[e].viseme = true;
                            break;
                        }
                }

                for (const auto& [yes, from] : {std::pair{fromVRM > 0, vrmVersion == 2 ? "VRM 1.0" : "VRM"}, std::pair{!mine.empty(), settingsName.c_str()},
                                                std::pair{guessed, "shape key names"}})
                    if (yes)
                        model.expressionsFrom += std::format("{}{}", model.expressionsFrom.empty() ? "" : " + ", from);

                lookAt();

                // the face each hand gesture makes, like most VRChat avatars have it
                for (int hand = 0; hand < 2; ++hand) {
                    auto&     g    = model.gestureFace[hand];
                    const int wink = P[hand == 0 ? EX_BLINK_L : EX_BLINK_R];
                    g[GESTURE_FIST]      = P[EX_ANGRY];
                    g[GESTURE_OPEN]      = P[EX_SURPRISED];
                    g[GESTURE_POINT]     = P[EX_SAD];
                    g[GESTURE_VICTORY]   = P[EX_HAPPY];
                    g[GESTURE_ROCK]      = P[EX_RELAXED];
                    g[GESTURE_GUN]       = wink >= 0 ? wink : P[EX_HAPPY];
                    g[GESTURE_THUMBS_UP] = P[EX_HAPPY];
                }
                // or as the settings file says: "gestures": {"left"/"right"/"both": {"fist": "expression" or "none"},
                // "combos": {"fist+open": "expression" or "none"}: the face while the left hand makes one sign and the
                // right the other}
                if (const SJson* gs = settings.get("gestures"); gs && gs->type == SJson::J_OBJ)
                    if (const SJson* combos = gs->get("combos"); combos && combos->type == SJson::J_OBJ)
                        for (const auto& [pair, face] : combos->obj) {
                            const size_t plus = pair.find('+');
                            const int    l = plus == std::string::npos ? -1 : gestureFromName(std::string_view(pair).substr(0, plus));
                            const int    r = plus == std::string::npos ? -1 : gestureFromName(std::string_view(pair).substr(plus + 1));
                            const int    e = jstr(&face) == "none" ? -1 : model.findExpression(jstr(&face));
                            if (l < 0 || r < 0 || (e < 0 && jstr(&face) != "none")) {
                                missing.push_back(l < 0 || r < 0 ? std::format("gestures {}", pair) : std::string(jstr(&face)));
                                continue;
                            }
                            model.gestureCombo[l][r] = e;
                        }
                if (const SJson* gs = settings.get("gestures"))
                    for (const std::string_view side : {"both", "left", "right"})
                        if (const SJson* h = gs->get(side); h && h->type == SJson::J_OBJ)
                            for (const auto& [gesture, face] : h->obj) {
                                const int g = gestureFromName(gesture);
                                const int e = jstr(&face) == "none" ? -1 : model.findExpression(jstr(&face));
                                if (g < 0 || (e < 0 && jstr(&face) != "none")) {
                                    missing.push_back(g < 0 ? std::format("gesture {}", gesture) : std::string(jstr(&face)));
                                    continue;
                                }
                                for (int hand = 0; hand < 2; ++hand)
                                    if (side == "both" || side == (hand == 0 ? "left" : "right"))
                                        model.gestureFace[hand][g] = e;
                            }
            }

            void lookAt() {
                SLookAt&    la   = model.lookAt;
                const auto& h    = model.human;
                const bool  eyes = model.humanoid && (h[HB_L_EYE] >= 0 || h[HB_R_EYE] >= 0);
                bool        looks = false;
                for (int p : {EX_LOOK_UP, EX_LOOK_DOWN, EX_LOOK_LEFT, EX_LOOK_RIGHT})
                    if (const int e = model.preset[p]; e >= 0 && (!model.expressions[e].morphs.empty() || !model.expressions[e].materials.empty()))
                        looks = true;
                if (vrmVersion) {
                    // VRM 0.x: firstPerson.lookAtHorizontalInner {xRange, yRange}...; VRM 1.0: lookAt.rangeMapHorizontalInner {inputMaxValue, outputScale}...
                    static constexpr std::string_view ROWS0[] = {"lookAtHorizontalInner", "lookAtHorizontalOuter", "lookAtVerticalDown", "lookAtVerticalUp"};
                    static constexpr std::string_view ROWS1[] = {"rangeMapHorizontalInner", "rangeMapHorizontalOuter", "rangeMapVerticalDown", "rangeMapVerticalUp"};
                    const bool   v1   = vrmVersion == 2;
                    const SJson* j    = vrmJson.get(v1 ? "lookAt" : "firstPerson");
                    const auto   type = jstr(j ? j->get(v1 ? "type" : "lookAtTypeName") : nullptr);
                    const bool   bone = type != (v1 ? "expression" : "BlendShape");
                    for (int r = 0; r < 4; ++r) {
                        const SJson* m = j ? j->get(v1 ? ROWS1[r] : ROWS0[r]) : nullptr;
                        la.range[r][0] = (float)jnum(m ? m->get(v1 ? "inputMaxValue" : "xRange") : nullptr, 90);
                        la.range[r][1] = (float)jnum(m ? m->get(v1 ? "outputScale" : "yRange") : nullptr, bone ? 10 : 1);
                    }
                    la.type = bone ? (eyes ? SLookAt::BONES : SLookAt::NONE) : looks ? SLookAt::EXPRESSIONS : SLookAt::NONE;
                } else if (eyes) {
                    la.type = SLookAt::BONES;
                    for (int r = 0; r < 4; ++r)
                        la.range[r][0] = 30, la.range[r][1] = r < 2 ? 12 : 8;
                } else if (looks) {
                    la.type = SLookAt::EXPRESSIONS;
                    for (auto& r : la.range)
                        r[0] = 30, r[1] = 1;
                }
                for (auto& r : la.range)
                    r[0] = std::max(r[0], 1.f);
            }

            // --- springs

            // a glTF node index from the JSON, as ours; -1 = none
            int gltfNode(const SJson* j) const {
                if (!j || j->type != SJson::J_NUM || !(j->num >= 0 && j->num < (double)data->nodes_count))
                    return -1;
                return nodeIndex[(size_t)j->num];
            }

            // [x, y, z], or VRM 0.x's {"x", "y", "z"}
            static V3 jvec(const SJson* j, const V3& def) {
                if (const auto* a = jarr(j); a && a->size() == 3)
                    return {(float)jnum(&(*a)[0], def.x), (float)jnum(&(*a)[1], def.y), (float)jnum(&(*a)[2], def.z)};
                if (j && j->type == SJson::J_OBJ)
                    return {(float)jnum(j->get("x"), def.x), (float)jnum(j->get("y"), def.y), (float)jnum(j->get("z"), def.z)};
                return def;
            }

            // meters per unit of the node's space, at rest
            float metersAt(int node) const {
                const float* m = restGlobal[node].m;
                return (length(V3{m[0], m[1], m[2]}) + length(V3{m[4], m[5], m[6]}) + length(V3{m[8], m[9], m[10]})) / 3.f * model.scale;
            }

            // model space -> the node's
            V3 inNode(int node, const V3& p) const {
                return restGlobal[node].inverse().point(p);
            }

            // a bone as the settings file names it
            int nodeNamed(std::string_view name) const {
                for (int loose = 0; loose < 2 && !name.empty(); ++loose) {
                    const std::string want = loose ? normName(name) : lower(std::string(name));
                    for (size_t i = 0; i < model.nodes.size(); ++i)
                        if ((loose ? normName(model.nodes[i].name) : lower(model.nodes[i].name)) == want)
                            return (int)i;
                }
                return -1;
            }

            std::vector<std::vector<int>> kids;    // per node
            std::vector<bool>             claimed; // moved by a spring
            std::vector<bool>             human;   // a humanoid bone, or has one under it: never swings

            int addCollider(int node, const V3& offset, const V3& tail, float meters, eColliderKind kind = COLLIDER_OUTSIDE) {
                model.springColliders.push_back({node, offset, tail, std::max(meters, 0.f), kind});
                return (int)model.springColliders.size() - 1;
            }

            // (j's limitFrame is the limit's own turn, from a frame whose y is along the bone)
            void addJoint(int node, int spring, SSpringJoint j, const V3& tail, float meters) {
                if (length(tail) * metersAt(node) < 1e-4f)
                    return; // no length to point with: it stays as the animation has it
                j.node   = node;
                j.spring = spring;
                j.tail   = tail;
                j.length = length(tail) * metersAt(node);
                j.radius = std::max(meters, 0.f);
                if (j.limit != LIMIT_NONE) {
                    // the shortest turn from y to the bone; straight down, half a turn round x
                    const V3 d   = normalize(tail);
                    const Quat y = 1.f + d.y < 1e-6f ? Quat{1, 0, 0, 0} : Quat{d.z, 0, -d.x, 1.f + d.y}.normalized();
                    j.limitFrame = (y * j.limitFrame).normalized();
                }
                model.springJoints.push_back(j);
                claimed[node] = true;
            }

            // VRMC_springBone_limit's limit: {"cone": {"angle", "rotation": [x, y, z, w]}}, {"hinge": {"angle", "rotation"}}
            // or {"spherical": {"pitch", "yaw", "rotation"}}, radians; the rotation turns it from a frame whose y is along
            // the bone. VRChat's PhysBones' Angle, Hinge and Polar limits come as these
            void readLimit(const SJson* limit, SSpringJoint& p) {
                const SJson* s = nullptr;
                auto         angle = [&](const char* k, float most) {
                    const double v = jnum(s->get(k), 0);
                    return std::isfinite(v) ? std::clamp((float)v, 0.f, most) : 0.f;
                };
                constexpr float HALF = TAU * 0.5f;
                if (!limit || limit->type != SJson::J_OBJ)
                    return;
                if ((s = limit->get("cone")) && s->type == SJson::J_OBJ)
                    p.limit = LIMIT_CONE, p.limitA = angle("angle", HALF);
                else if ((s = limit->get("hinge")) && s->type == SJson::J_OBJ)
                    p.limit = LIMIT_HINGE, p.limitA = angle("angle", HALF);
                else if ((s = limit->get("spherical")) && s->type == SJson::J_OBJ)
                    p.limit = LIMIT_SPHERICAL, p.limitA = angle("pitch", HALF), p.limitB = angle("yaw", HALF * 0.5f);
                else
                    return;
                p.limitFrame = {};
                if (const auto* r = jarr(s->get("rotation")); r && r->size() == 4) {
                    const Quat q{(float)jnum(&(*r)[0], 0), (float)jnum(&(*r)[1], 0), (float)jnum(&(*r)[2], 0), (float)jnum(&(*r)[3], 1)};
                    if (std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w))
                        p.limitFrame = q.normalized();
                }
            }

            // a bone and all under it swing, as VRM 0.x's bone groups have it: each toward its first child, the
            // last ones toward a made-up end `leaf` model units on (0: as long as the bone before). `byDepth`: each bone's
            // radius in model units by how far down from the root it is instead (the last for the rest)
            void addTree(int root, int spring, const SSpringJoint& params, float radius, const std::set<int>& ignore, float leaf, bool skinOnly,
                         const std::vector<float>& byDepth = {}) {
                auto skip = [&](int n) { return claimed[n] || human[n] || ignore.contains(n) || (skinOnly && !skinJoint[n]); };
                std::vector<std::pair<int, size_t>> stack{{root, 0}};
                while (!stack.empty()) {
                    const auto [n, depth] = stack.back();
                    stack.pop_back();
                    if (skip(n))
                        continue;
                    const auto it   = std::ranges::find_if(kids[n], [&](int c) { return !skip(c); });
                    V3         tail = it != kids[n].end() ? model.nodes[*it].rest.t : V3{};
                    if (it == kids[n].end()) {
                        const int p = model.nodes[n].parent;
                        V3        d = p >= 0 ? restPos(n) - restPos(p) : V3{};
                        const float len = leaf > 0 ? leaf : std::clamp(length(d), 0.02f / model.scale, 0.15f / model.scale);
                        if (length(d) < 1e-6f)
                            d = {0, -1, 0};
                        tail = inNode(n, restPos(n) + normalize(d) * len);
                    }
                    addJoint(n, spring, params, tail, byDepth.empty() ? radius : byDepth[std::min(depth, byDepth.size() - 1)] * metersAt(n));
                    for (auto c = kids[n].rbegin(); c != kids[n].rend(); ++c)
                        stack.push_back({*c, depth + 1});
                }
            }

            // VRM 0.x: secondaryAnimation {boneGroups: [{comment, stiffiness, gravityPower, gravityDir {x, y, z}, dragForce,
            // center, hitRadius, bones: [roots], colliderGroups: [i]}], colliderGroups: [{node, colliders: [{offset, radius}]}]},
            // in Unity's axes: z the other way
            void vrm0Springs() {
                const SJson*                  sa = vrmJson.get("secondaryAnimation");
                std::vector<std::vector<int>> groups;
                if (const auto* list = jarr(sa ? sa->get("colliderGroups") : nullptr))
                    for (const auto& g : *list) {
                        auto&     out  = groups.emplace_back();
                        const int node = gltfNode(g.get("node"));
                        if (const auto* cs = jarr(g.get("colliders")); cs && node >= 0)
                            for (const auto& c : *cs) {
                                V3 o = jvec(c.get("offset"), {});
                                o.z  = -o.z;
                                out.push_back(addCollider(node, o, o, (float)jnum(c.get("radius"), 0) * metersAt(node)));
                            }
                    }
                const auto* list = jarr(sa ? sa->get("boneGroups") : nullptr);
                if (!list)
                    return;
                for (const auto& g : *list) {
                    SSpring s;
                    s.name   = jstr(g.get("comment"));
                    s.center = gltfNode(g.get("center"));
                    if (const auto* cg = jarr(g.get("colliderGroups")))
                        for (const auto& i : *cg)
                            if (const double v = jnum(&i, -1); v >= 0 && v < (double)groups.size())
                                s.colliders.insert(s.colliders.end(), groups[(size_t)v].begin(), groups[(size_t)v].end());
                    SSpringJoint p;
                    p.stiffness  = (float)jnum(g.get("stiffiness"), jnum(g.get("stiffness"), 1));
                    p.gravity    = (float)jnum(g.get("gravityPower"), 0);
                    V3 dir       = jvec(g.get("gravityDir"), {0, -1, 0});
                    dir.z        = -dir.z;
                    p.gravityDir = normalize(dir);
                    p.drag       = std::clamp((float)jnum(g.get("dragForce"), 0.4), 0.f, 1.f);
                    p.scale      = model.scale;
                    const float radius = (float)jnum(g.get("hitRadius"), 0.02);
                    const int   index  = (int)model.springs.size();
                    model.springs.push_back(std::move(s));
                    if (const auto* bones = jarr(g.get("bones")))
                        for (const auto& b : *bones)
                            if (const int n = gltfNode(&b); n >= 0)
                                addTree(n, index, p, radius * metersAt(n), {}, 0.07f, false);
                }
                model.springsFrom = "VRM";
            }

            // VRMC_springBone (VRM 1.0): colliders [{node, shape: {sphere: {offset, radius}} or {capsule: {offset, radius,
            // tail}}}], colliderGroups [{colliders: [i]}], springs [{name, joints: [{node, stiffness, gravityPower, gravityDir,
            // dragForce, hitRadius}], colliderGroups: [i], center}]: each joint points at the next, the last is only its end.
            // A collider's VRMC_springBone_extended_collider says what it is instead: {shape: {sphere or capsule: {...,
            // inside}, plane: {offset, normal}}}
            void vrm1Springs() {
                const cgltf_extension* ext = nullptr;
                for (size_t i = 0; i < data->data_extensions_count && !ext; ++i)
                    if (const auto& e = data->data_extensions[i]; e.name && e.data && std::string_view(e.name) == "VRMC_springBone")
                        ext = &e;
                SJson j;
                if (!ext)
                    return;
                if (!CJsonReader(ext->data).read(j)) {
                    log.push_back("couldn't read the VRMC_springBone extension");
                    return;
                }
                std::vector<int> colliders; // ours per theirs, -1 = none
                if (const auto* list = jarr(j.get("colliders")))
                    for (const auto& c : *list) {
                        const int    node     = gltfNode(c.get("node"));
                        const SJson* extended = c.get("extensions") ? c.get("extensions")->get("VRMC_springBone_extended_collider") : nullptr;
                        const SJson* shape    = extended && extended->get("shape") ? extended->get("shape") : c.get("shape");
                        if (const SJson* plane = shape ? shape->get("plane") : nullptr; node >= 0 && plane) {
                            const V3 o = jvec(plane->get("offset"), {});
                            colliders.push_back(addCollider(node, o, o + normalize(jvec(plane->get("normal"), {0, 0, 1})), 0, COLLIDER_PLANE));
                            continue;
                        }
                        const SJson* capsule = shape ? shape->get("capsule") : nullptr;
                        const SJson* s       = capsule ? capsule : shape ? shape->get("sphere") : nullptr;
                        if (node < 0 || !s) {
                            colliders.push_back(-1);
                            continue;
                        }
                        const V3           o      = jvec(s->get("offset"), {});
                        const SJson*       inside = s->get("inside");
                        const eColliderKind kind  = inside && inside->type == SJson::J_BOOL && inside->num != 0 ? COLLIDER_INSIDE : COLLIDER_OUTSIDE;
                        colliders.push_back(addCollider(node, o, capsule ? jvec(s->get("tail"), o) : o, (float)jnum(s->get("radius"), 0) * metersAt(node), kind));
                    }
                std::vector<std::vector<int>> groups;
                if (const auto* list = jarr(j.get("colliderGroups")))
                    for (const auto& g : *list) {
                        auto& out = groups.emplace_back();
                        if (const auto* cs = jarr(g.get("colliders")))
                            for (const auto& i : *cs)
                                if (const double v = jnum(&i, -1); v >= 0 && v < (double)colliders.size() && colliders[(size_t)v] >= 0)
                                    out.push_back(colliders[(size_t)v]);
                    }
                if (const auto* list = jarr(j.get("springs")))
                    for (const auto& s : *list) {
                        SSpring sp;
                        sp.name   = jstr(s.get("name"));
                        sp.center = gltfNode(s.get("center"));
                        if (const auto* cg = jarr(s.get("colliderGroups")))
                            for (const auto& i : *cg)
                                if (const double v = jnum(&i, -1); v >= 0 && v < (double)groups.size())
                                    sp.colliders.insert(sp.colliders.end(), groups[(size_t)v].begin(), groups[(size_t)v].end());
                        const int index = (int)model.springs.size();
                        model.springs.push_back(std::move(sp));
                        const auto* joints = jarr(s.get("joints"));
                        for (size_t i = 0; joints && i + 1 < joints->size(); ++i) {
                            const SJson& a = (*joints)[i];
                            const int    n = gltfNode(a.get("node")), next = gltfNode((*joints)[i + 1].get("node"));
                            if (n < 0 || next < 0 || n == next || claimed[n] || human[n])
                                continue;
                            SSpringJoint p;
                            p.stiffness  = (float)jnum(a.get("stiffness"), 1);
                            p.gravity    = (float)jnum(a.get("gravityPower"), 0);
                            p.gravityDir = normalize(jvec(a.get("gravityDir"), {0, -1, 0}));
                            p.drag       = std::clamp((float)jnum(a.get("dragForce"), 0.5), 0.f, 1.f);
                            p.scale      = model.scale;
                            if (const SJson* e = a.get("extensions"); e && e->get("VRMC_springBone_limit"))
                                readLimit(e->get("VRMC_springBone_limit")->get("limit"), p);
                            addJoint(n, index, p, inNode(n, restPos(next)), (float)jnum(a.get("hitRadius"), 0) * metersAt(n));
                        }
                    }
                model.springsFrom = "VRM 1.0";
            }

            // colliders on the humanoid's body, for springs that don't come with any
            enum eBody : uint8_t {
                BODY_HEAD  = 1,
                BODY_CHEST = 2,
                BODY_HIPS  = 4,
                BODY_LEGS  = 8,
                BODY_ARMS  = 16,
                BODY_ALL   = 31,
            };
            std::array<std::vector<int>, 5> bodyParts;
            bool                            bodyMade = false;

            std::vector<int> body(int which) {
                const auto& h = model.human;
                if (!bodyMade && model.humanoid) {
                    // sizes for a 1.6 m avatar
                    const float m = model.height / 1.6f, u = m / model.scale; // meters, model units
                    const V3    F = model.forward, RT = normalize(cross(F, UP));
                    auto sphere = [&](int part, int bone, const V3& off, float r) {
                        if (const int n = h[bone]; n >= 0)
                            bodyParts[part].push_back(addCollider(n, inNode(n, restPos(n) + off * u), inNode(n, restPos(n) + off * u), r * m));
                    };
                    // across the body
                    auto across = [&](int part, int bone, const V3& off, float half, float r) {
                        if (const int n = h[bone]; n >= 0)
                            bodyParts[part].push_back(addCollider(n, inNode(n, restPos(n) + (off - RT * half) * u), inNode(n, restPos(n) + (off + RT * half) * u), r * m));
                    };
                    // along a limb
                    auto limb = [&](int part, int bone, int to, float r) {
                        if (const int n = h[bone], e = h[to]; n >= 0 && e >= 0)
                            bodyParts[part].push_back(addCollider(n, V3{}, inNode(n, restPos(e)), r * m));
                    };
                    sphere(0, HB_HEAD, UP * 0.07f, 0.085f);
                    across(1, h[HB_UPPER_CHEST] >= 0 ? HB_UPPER_CHEST : h[HB_CHEST] >= 0 ? HB_CHEST : HB_SPINE, F * 0.01f, 0.05f, 0.09f);
                    across(2, HB_HIPS, UP * -0.02f, 0.06f, 0.085f);
                    for (int s = 0; s < 2; ++s) {
                        limb(3, s ? HB_R_UPPER_LEG : HB_L_UPPER_LEG, s ? HB_R_LOWER_LEG : HB_L_LOWER_LEG, 0.065f);
                        limb(3, s ? HB_R_LOWER_LEG : HB_L_LOWER_LEG, s ? HB_R_FOOT : HB_L_FOOT, 0.05f);
                        limb(4, s ? HB_R_UPPER_ARM : HB_L_UPPER_ARM, s ? HB_R_LOWER_ARM : HB_L_LOWER_ARM, 0.04f);
                        limb(4, s ? HB_R_LOWER_ARM : HB_L_LOWER_ARM, s ? HB_R_HAND : HB_L_HAND, 0.035f);
                    }
                    int bit = 0;
                    for (const auto& part : bodyParts)
                        for (int k : part)
                            model.springColliders[k].body = (uint16_t)(1u << bit++);
                }
                bodyMade = true;
                std::vector<int> out;
                for (int p = 0; p < 5; ++p)
                    if (which & 1 << p)
                        out.insert(out.end(), bodyParts[p].begin(), bodyParts[p].end());
                return out;
            }

            // the settings file's, over the model's own:
            // "colliders": [{"name", "node", "offset": [x, y, z], "tail": [x, y, z] (a capsule), "radius", "inside": true (it keeps the
            // bones in)}, or a plane {"name", "node", "offset", "normal": [x, y, z]} (they keep to where it points), or a disc
            // {"name", "node", "offset", "disc": {"normal": [x, y, z], "radius"}, "radius"} (flat and round round offset, its
            // radius out to its edge, the collider's own radius round that)], in the node's units;
            // "springs": [{"name", "bones": [roots], "ignore": [bones], "stiffness", "drag", "gravity", "gravityDir": [x, y, z],
            // "radius" (or [one for each bone down from a root, the last for the rest]), "center": bone, "immobile", "colliders":
            // [names; "body" for the ones made for the body], "limit": a VRMC_springBone_limit limit (see readLimit) for each
            // bone}]: each root and all under it swing, as VRM 0.x has it. A spring that doesn't name colliders keeps out of
            // all of the file's, or the body's.
            void settingsSprings(const std::vector<SJson>& list) {
                std::map<std::string, std::vector<int>> named;
                std::vector<int>                        all;
                if (const auto* cs = jarr(settings.get("colliders")))
                    for (const auto& c : *cs) {
                        const int node = nodeNamed(jstr(c.get("node")));
                        if (node < 0) {
                            missing.push_back(std::string(jstr(c.get("node"))));
                            continue;
                        }
                        const V3     o      = jvec(c.get("offset"), {});
                        const SJson* inside = c.get("inside");
                        const SJson* disc   = c.get("disc");
                        int          k      = -1;
                        if (disc && disc->type == SJson::J_OBJ) {
                            // (an older hypr3d, that has no discs, keeps the bones out of a sphere that big round its middle)
                            k = addCollider(node, o, o + normalize(jvec(disc->get("normal"), {0, 1, 0})), (float)jnum(c.get("radius"), 0.05) * metersAt(node), COLLIDER_DISC);
                            model.springColliders[k].disc = std::max((float)jnum(disc->get("radius"), 0), 0.f) * metersAt(node);
                        } else
                            k = c.get("normal") ? addCollider(node, o, o + normalize(jvec(c.get("normal"), {0, 1, 0})), 0, COLLIDER_PLANE) :
                                                  addCollider(node, o, jvec(c.get("tail"), o), (float)jnum(c.get("radius"), 0.05) * metersAt(node),
                                                              inside && inside->type == SJson::J_BOOL && inside->num != 0 ? COLLIDER_INSIDE : COLLIDER_OUTSIDE);
                        named[lower(std::string(jstr(c.get("name"))))].push_back(k);
                        all.push_back(k);
                    }
                for (const auto& s : list) {
                    SSpring sp;
                    sp.name           = jstr(s.get("name"));
                    sp.immobile       = std::clamp((float)jnum(s.get("immobile"), immobile()), 0.f, 1.f);
                    sp.parentImmobile = std::clamp((float)jnum(s.get("parentImmobile"), 0), 0.f, 1.f);
                    if (const SJson* c = s.get("center"); c && (sp.center = nodeNamed(jstr(c))) < 0)
                        missing.push_back(std::string(jstr(c)));
                    if (const auto* cs = jarr(s.get("colliders"))) {
                        for (const auto& c : *cs) {
                            const std::string name = lower(std::string(jstr(&c)));
                            const auto        it   = named.find(name);
                            const auto        add  = name == "body" ? body(BODY_ALL) : it != named.end() ? it->second : std::vector<int>{};
                            if (add.empty() && name != "body")
                                missing.push_back(std::format("collider {}", jstr(&c)));
                            sp.colliders.insert(sp.colliders.end(), add.begin(), add.end());
                        }
                    } else
                        sp.colliders = all.empty() ? body(BODY_ALL) : all;
                    std::set<int> ignore;
                    if (const auto* is = jarr(s.get("ignore")))
                        for (const auto& i : *is) {
                            if (const int n = nodeNamed(jstr(&i)); n >= 0)
                                ignore.insert(n);
                            else
                                missing.push_back(std::string(jstr(&i)));
                        }
                    SSpringJoint p;
                    p.stiffness  = (float)jnum(s.get("stiffness"), 1);
                    p.drag       = std::clamp((float)jnum(s.get("drag"), 0.4), 0.f, 1.f);
                    p.gravity    = (float)jnum(s.get("gravity"), 0);
                    p.gravityDir = normalize(jvec(s.get("gravityDir"), {0, -1, 0}));
                    p.scale      = model.scale;
                    readLimit(s.get("limit"), p);
                    const float        radius = (float)jnum(s.get("radius"), 0.02);
                    std::vector<float> radii; // (a radius for each bone down from a root: a PhysBone's radius curve)
                    if (const auto* rs = jarr(s.get("radius")))
                        for (const auto& r : *rs)
                            radii.push_back(std::max((float)jnum(&r, 0.02), 0.f));
                    const int index = (int)model.springs.size();
                    model.springs.push_back(std::move(sp));
                    if (const auto* bones = jarr(s.get("bones")))
                        for (const auto& b : *bones) {
                            if (const int n = nodeNamed(jstr(&b)); n >= 0)
                                addTree(n, index, p, radius * metersAt(n), ignore, 0.07f, false, radii);
                            else
                                missing.push_back(std::string(jstr(&b)));
                        }
                }
            }

            // none given: hair, skirts, tails and the like, by their bones' names
            void heuristicSprings() {
                struct SKind {
                    const char*                   name;
                    std::vector<std::string_view> words; // what a word of the bone's name starts with, or (not ASCII) what it has in it
                    float                         stiffness, drag, gravity, radius; // for a 1.6 m avatar
                    int                           body;
                };
                static const SKind KINDS[] = {
                    // long chains bend at every bone: gravity keeps a long ponytail from streaming out level at a run
                    {"hair", {"hair", "bang", "ponytail", "twintail", "kami", "髪"}, 2.f, 0.5f, 0.1f, 0.02f, BODY_HEAD | BODY_CHEST},
                    {"skirt", {"skirt", "スカート"}, 1.2f, 0.6f, 0.05f, 0.03f, BODY_HIPS | BODY_LEGS},
                    {"cape", {"cape", "cloak", "mantle", "マント"}, 1.f, 0.5f, 0.1f, 0.03f, BODY_CHEST | BODY_HIPS | BODY_LEGS},
                    {"sleeves", {"sleeve", "袖"}, 2.f, 0.5f, 0.05f, 0.f, 0},
                    {"ribbons", {"ribbon", "リボン"}, 1.f, 0.5f, 0.05f, 0.02f, BODY_HEAD | BODY_CHEST},
                    {"tail", {"tail", "尻尾", "しっぽ"}, 2.5f, 0.5f, 0.05f, 0.03f, BODY_HIPS | BODY_LEGS},
                    {"ears", {"ear", "mimi", "耳"}, 4.f, 0.5f, 0.f, 0.f, 0},
                    {"breasts", {"breast", "bust", "boob", "oppai", "胸"}, 6.f, 0.8f, 0.f, 0.f, 0},
                };
                auto kindOf = [&](const std::string& name) {
                    const auto words = tokens(name);
                    for (int k = 0; k < (int)std::size(KINDS); ++k)
                        for (auto w : KINDS[k].words)
                            if ((unsigned char)w[0] >= 0x80 ? name.find(w) != std::string::npos : std::ranges::any_of(words, [&](const std::string& t) { return t.starts_with(w); }))
                                return k;
                    return -1;
                };
                std::vector<bool> weighted(model.nodes.size(), false);
                for (const auto& v : model.vertices)
                    for (int k = 0; k < 4; ++k)
                        if (v.weights[k] && v.joints[k] < model.joints.size())
                            weighted[model.joints[v.joints[k]].node] = true;
                const float ref = model.height / 1.6f;
                std::array<int, std::size(KINDS)> spring;
                spring.fill(-1);
                // bones nothing's weighted to (a group's root) stay put, as do those that branch into several chains:
                // the chains under them swing
                std::function<void(int, int)> chain = [&](int n, int k) {
                    const size_t chains = std::ranges::count_if(kids[n], [&](int c) { return skinJoint[c] && !human[c]; });
                    if (skinJoint[n] && weighted[n] && chains <= 1) {
                        if (spring[k] < 0) {
                            spring[k] = (int)model.springs.size();
                            model.springs.push_back({KINDS[k].name, -1, body(KINDS[k].body)});
                        }
                        SSpringJoint p;
                        p.stiffness = KINDS[k].stiffness;
                        p.drag      = KINDS[k].drag;
                        p.gravity   = KINDS[k].gravity;
                        p.scale     = ref;
                        addTree(n, spring[k], p, KINDS[k].radius * ref, {}, 0, true);
                        return;
                    }
                    for (int c : kids[n])
                        if (!human[c])
                            chain(c, k);
                };
                std::vector<bool> seen(model.nodes.size(), false); // under a bone of some kind
                for (size_t i = 0; i < model.nodes.size(); ++i) {
                    if (const int p = model.nodes[i].parent; p >= 0 && seen[p]) {
                        seen[i] = true;
                        continue;
                    }
                    if (!skinJoint[i] || human[i])
                        continue;
                    if (const int k = kindOf(model.nodes[i].name); k >= 0) {
                        seen[i] = true;
                        chain((int)i, k);
                    }
                }
                model.springsFrom = "bone names";
            }

            // how little the avatar moving through the world swings springs with no center: the settings file's "immobile".
            // VRM's springs were made for sitting in front of a camera; at a run (4.5 m/s) with all the air still, long
            // hair streams out level behind
            float immobile() const {
                return std::clamp((float)jnum(settings.get("immobile"), 0.9), 0.f, 1.f);
            }

            void springs() {
                const size_t count = model.nodes.size();
                kids.assign(count, {});
                claimed.assign(count, false);
                human.assign(count, false);
                for (size_t i = 0; i < count; ++i)
                    if (model.nodes[i].parent >= 0)
                        kids[model.nodes[i].parent].push_back((int)i);
                for (int b : model.human)
                    if (b >= 0)
                        human[b] = true;
                for (size_t i = count; i-- > 0;)
                    if (human[i] && model.nodes[i].parent >= 0)
                        human[model.nodes[i].parent] = true;

                if (const auto* list = jarr(settings.get("springs"))) {
                    settingsSprings(*list);
                    model.springsFrom = settingsName;
                } else {
                    if (vrmVersion == 1)
                        vrm0Springs();
                    vrm1Springs();
                    if (model.springJoints.empty()) {
                        model.springs.clear();
                        model.springColliders.clear();
                        bodyParts = {};
                        bodyMade  = false;
                        heuristicSprings();
                    }
                }
                if (!jarr(settings.get("springs")))
                    for (auto& s : model.springs)
                        s.immobile = immobile();
                // the springs that got no bones go
                std::vector<int> remap(model.springs.size(), -1);
                for (const auto& j : model.springJoints)
                    remap[j.spring] = 0;
                std::vector<SSpring> kept;
                for (size_t s = 0; s < model.springs.size(); ++s)
                    if (remap[s] == 0) {
                        remap[s] = (int)kept.size();
                        kept.push_back(std::move(model.springs[s]));
                        auto& c = kept.back().colliders;
                        std::ranges::sort(c);
                        c.erase(std::unique(c.begin(), c.end()), c.end());
                    }
                model.springs = std::move(kept);
                for (auto& j : model.springJoints)
                    j.spring = remap[j.spring];
                // a bone with a limit leaves out the body's colliders its tail starts inside of (deeper than START_IN): its
                // limit keeps it out of the body, as a PhysBone's does, and the body made up here isn't the avatar's where it
                // is (a necktie on a chest: pushed off it, it stood out, and swung round through it)
                constexpr float START_IN = 0.005f;
                for (auto& j : model.springJoints) {
                    if (j.limit == LIMIT_NONE)
                        continue;
                    const V3 tail = restGlobal[j.node].point(j.tail);
                    for (int k : model.springs[j.spring].colliders) {
                        const auto& c = model.springColliders[k];
                        if (!c.body)
                            continue;
                        const V3    a = restGlobal[c.node].point(c.offset), ab = restGlobal[c.node].point(c.tail) - a;
                        const float ll = dot(ab, ab);
                        const V3    q  = ll > 1e-12f ? a + ab * std::clamp(dot(tail - a, ab) / ll, 0.f, 1.f) : a;
                        if (length(tail - q) * model.scale < c.radius + j.radius - START_IN)
                            j.startsIn |= c.body;
                    }
                }
                // what each spring hangs from: its first root's parent, unless a spring moves that
                std::vector<int> jointOf(model.nodes.size(), -1);
                for (const auto& j : model.springJoints)
                    jointOf[j.node] = j.spring;
                for (const auto& j : model.springJoints) {
                    auto& sp = model.springs[j.spring];
                    if (sp.carrier >= 0 || sp.parentImmobile <= 0)
                        continue;
                    if (const int p = model.nodes[j.node].parent; p >= 0 && jointOf[p] != j.spring) {
                        sp.carrier = p;
                        for (int a = p; a >= 0 && sp.carrier >= 0; a = model.nodes[a].parent)
                            if (jointOf[a] >= 0)
                                sp.carrier = -1, sp.parentImmobile = 0;
                    }
                }
                if (model.springJoints.empty()) {
                    model.springColliders.clear();
                    model.springsFrom.clear();
                }
                // parents first, as the nodes are
                std::ranges::stable_sort(model.springJoints, {}, &SSpringJoint::node);
            }

            // VRMC_node_constraint on the nodes, in an order where what a constraint looks at is done before it
            void constraints() {
                std::vector<SNodeConstraint> found;
                std::vector<int>             of(model.nodes.size(), -1); // node -> its constraint
                for (size_t gi = 0; gi < data->nodes_count; ++gi) {
                    const cgltf_node& nd = data->nodes[gi];
                    for (size_t e = 0; e < nd.extensions_count; ++e) {
                        const auto& ext = nd.extensions[e];
                        if (!ext.name || !ext.data || std::string_view(ext.name) != "VRMC_node_constraint")
                            continue;
                        SJson j;
                        if (!CJsonReader(ext.data).read(j)) {
                            log.push_back(std::format("couldn't read the node constraint of {}", nd.name ? nd.name : "a node"));
                            continue;
                        }
                        const SJson*    c = j.get("constraint");
                        const SJson*    spec;
                        SNodeConstraint k;
                        if ((spec = c ? c->get("roll") : nullptr)) {
                            k.type       = SNodeConstraint::ROLL;
                            const auto a = jstr(spec->get("rollAxis"));
                            k.axis       = a == "Y" ? V3{0, 1, 0} : a == "Z" ? V3{0, 0, 1} : V3{1, 0, 0};
                        } else if ((spec = c ? c->get("aim") : nullptr)) {
                            k.type                                     = SNodeConstraint::AIM;
                            static constexpr std::string_view AXES[] = {"PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ"};
                            const auto                        a      = std::ranges::find(AXES, jstr(spec->get("aimAxis"))) - std::begin(AXES);
                            V3                                axis{};
                            (&axis.x)[a < 6 ? a / 2 : 0] = a < 6 && a % 2 ? -1.f : 1.f;
                            k.axis                       = axis;
                        } else if ((spec = c ? c->get("rotation") : nullptr))
                            k.type = SNodeConstraint::ROTATION;
                        else
                            continue;
                        k.node   = nodeIndex[gi];
                        k.source = gltfNode(spec->get("source"));
                        k.weight = std::clamp((float)jnum(spec->get("weight"), 1), 0.f, 1.f);
                        if (k.node < 0 || k.source < 0 || k.source == k.node || of[k.node] >= 0)
                            continue;
                        of[k.node] = (int)found.size();
                        found.push_back(k);
                    }
                }
                // what a constraint reads: its source and what that hangs off, and what its node hangs off
                std::vector<uint8_t>     state(found.size(), 0); // 1 being ordered, 2 done
                size_t                   loops = 0;
                std::function<bool(int)> visit = [&](int c) {
                    if (state[c])
                        return state[c] == 2;
                    state[c] = 1;
                    bool ok  = true;
                    for (int from : {found[c].source, model.nodes[found[c].node].parent})
                        for (int n = from; n >= 0 && ok; n = model.nodes[n].parent)
                            if (of[n] >= 0 && of[n] != c)
                                ok = visit(of[n]);
                    state[c] = 2;
                    if (ok)
                        model.constraints.push_back(found[c]);
                    else
                        ++loops;
                    return ok;
                };
                for (size_t c = 0; c < found.size(); ++c)
                    visit((int)c);
                if (loops)
                    log.push_back(std::format("left out {} node constraints that go round in a loop", loops));
            }

            // --- emotes

            std::shared_ptr<SAvatarEmote> ownEmote(const SAnimClip& clip) const {
                auto              e = std::make_shared<SAvatarEmote>();
                const std::string low = lower(clip.name);
                e->name               = clip.name;
                e->from               = "own clip";
                e->anim               = clip;
                e->loop               = low.contains("loop") || low.contains("dance");
                e->fingers            = movesFingers(model, clip);
                return e;
            }

            // the built in ones (humanoids), the model's clips that aren't for walking about, the settings file's
            // ("emotes": [{"name", "file", "clip", "loop", "hold", "grounded", "speed", "sound"}]: a file by where the settings
            // file is, else a clip of the model's; "sound" an Ogg Vorbis file it plays, by where the settings file is too)
            // and the request's files; a later one of a name replaces an earlier one
            void emotes() {
                std::vector<std::shared_ptr<SAvatarEmote>> all;
                std::vector<std::string>                   own, files;
                size_t                                     builtIn = 0;
                auto                                       add     = [&](std::shared_ptr<SAvatarEmote> e) {
                    for (auto& o : all)
                        if (lower(o->name) == lower(e->name)) {
                            o = std::move(e);
                            return;
                        }
                    all.push_back(std::move(e));
                };
                for (auto& e : builtinEmotes(model, cancel)) {
                    add(std::move(e));
                    ++builtIn;
                }
                for (size_t i = 0; i < model.clips.size(); ++i)
                    if (std::ranges::find(model.clipFor, (int)i) == model.clipFor.end()) {
                        add(ownEmote(model.clips[i]));
                        own.push_back(model.clips[i].name);
                    }
                auto fromFile = [&](const std::string& file, std::string_view clip, const std::string& who) {
                    std::string error;
                    auto        made = emotesFromFile(file, model, cancel, log, error, clip);
                    if (!error.empty())
                        log.push_back(who.empty() ? error : std::format("{}: {}", who, error));
                    return made;
                };
                if (const auto* list = jarr(settings.get("emotes")))
                    for (const auto& x : *list) {
                        const std::string_view                     file = jstr(x.get("file")), clip = jstr(x.get("clip"));
                        std::vector<std::shared_ptr<SAvatarEmote>> made;
                        if (!file.empty()) {
                            std::filesystem::path p(file);
                            if (p.is_relative())
                                p = std::filesystem::path(model.settings).parent_path() / p;
                            made = fromFile(p.string(), clip, settingsName);
                        } else if (const int c = clipNamed(model.clips, clip); c >= 0)
                            made.push_back(ownEmote(model.clips[c]));
                        else
                            missing.push_back(clip.empty() ? std::string("an emote without a file or clip") : std::format("clip {}", clip));
                        const std::string             name(jstr(x.get("name")));
                        std::shared_ptr<const SSound> sound;
                        if (const std::string_view s = jstr(x.get("sound")); !s.empty() && !made.empty()) {
                            std::filesystem::path p(s);
                            if (p.is_relative())
                                p = std::filesystem::path(model.settings).parent_path() / p;
                            std::string error;
                            check(cancel);
                            if (sound = loadSound(p.string(), error); !sound)
                                log.push_back(std::format("{}: {}", settingsName, error));
                        }
                        for (auto& e : made) {
                            if (!name.empty())
                                e->name = made.size() == 1 ? name : std::format("{} {}", name, e->name);
                            if (const SJson* v = x.get("loop"))
                                e->loop = jbool(v);
                            if (const SJson* v = x.get("hold"))
                                e->hold = jbool(v);
                            if (const SJson* v = x.get("grounded"))
                                e->grounded = jbool(v);
                            if (const SJson* v = x.get("speed"))
                                e->speed = std::clamp((float)jnum(v, 1), 0.05f, 20.f);
                            e->sound = sound;
                            if (!file.empty())
                                files.push_back(std::format("{} ({}{})", e->name, e->from,
                                                            sound ? std::format(", sound {}, {:.1f} s", std::filesystem::path(sound->file).filename().string(), sound->duration()) : ""));
                            add(std::move(e));
                        }
                    }
                for (const auto& f : req.emotes)
                    for (auto& e : fromFile(f, {}, "")) {
                        files.push_back(std::format("{} ({})", e->name, e->from));
                        add(std::move(e));
                    }
                model.emotes.assign(all.begin(), all.end());

                std::vector<std::string> said;
                if (builtIn)
                    said.push_back(std::format("{} built in", builtIn));
                if (!own.empty())
                    said.push_back("its own " + join(own));
                if (!files.empty())
                    said.push_back("from files " + join(files));
                emotesMade = join(said, "; ");
            }

            // the attacks (a humanoid's): the request's, else the settings file's "attack" ("FILE", or {"file": FILE,
            // "firstPerson": FILE}: VRM animations of the right arm's punch, by where the settings file is; first person's
            // the built in one unless it says), else the built in ones; made for this avatar
            void attacks() {
                if (!model.humanoid)
                    return;
                auto make = [&](const std::string& file, const unsigned char* builtIn, size_t size, std::array<SAvatarAttack, 2>& out) -> std::string {
                    std::string error;
                    if (!file.empty()) {
                        std::vector<uint8_t> bytes;
                        if (!gltf::readFile(file, bytes))
                            error = std::format("attack {} can't be read", file);
                        else if (attackClips(bytes.data(), bytes.size(), file, model, cancel, out, error))
                            return std::filesystem::path(file).filename().string();
                        log.push_back(error + ": the built in one instead");
                        error.clear();
                    }
                    if (attackClips(builtIn, size, "built in attack", model, cancel, out, error))
                        return "built in";
                    log.push_back(error);
                    out = {};
                    return "none";
                };
                auto fromSettings = [&](const SJson* v) {
                    std::filesystem::path f(jstr(v));
                    return f.empty() || f.is_absolute() ? f.string() : (std::filesystem::path(model.settings).parent_path() / f).string();
                };
                std::string file = req.attack, first = req.attackFirst;
                if (const SJson* a = settings.get("attack"); a && file.empty()) {
                    file = fromSettings(a->type == SJson::J_OBJ ? a->get("file") : a);
                    if (first.empty() && a->type == SJson::J_OBJ)
                        first = fromSettings(a->get("firstPerson"));
                }
                const std::string third = make(file, ATTACK_VRMA, sizeof ATTACK_VRMA, model.attacks);
                const std::string fp    = make(first, ATTACK_FIRST_VRMA, sizeof ATTACK_FIRST_VRMA, model.attacksFirst);
                model.attacksFrom       = third == fp ? third : std::format("{}, first person {}", third, fp);
            }

            // the walk and the run (a humanoid's that walks procedurally: how its body goes over the steps): the settings
            // file's "walk" ({"walk": FILE, "run": FILE}: VRM animations of one stride, by where the settings file is; the
            // built in one for what it leaves out; "none": the walking's own body), else the built in ones
            void gaitClips() {
                if (!model.humanoid)
                    return;
                const SJson* w = settings.get("walk");
                if (w && w->type == SJson::J_STR && jstr(w) == "none") {
                    model.gaitFrom = "none";
                    return;
                }
                std::array<std::string, 2> said;
                for (int k = 0; k < 2; ++k) {
                    const char*           key  = k ? "run" : "walk";
                    const unsigned char*  in   = k ? RUN_VRMA : WALK_VRMA;
                    const size_t          size = k ? sizeof RUN_VRMA : sizeof WALK_VRMA;
                    auto                  clip = std::make_shared<SGaitClip>();
                    std::string           error;
                    std::filesystem::path f(w && w->type == SJson::J_OBJ ? std::string(jstr(w->get(key))) : std::string());
                    if (!f.empty()) {
                        if (f.is_relative())
                            f = std::filesystem::path(model.settings).parent_path() / f;
                        std::vector<uint8_t> bytes;
                        if (!gltf::readFile(f.string(), bytes))
                            error = std::format("{} {} can't be read", key, f.string());
                        else if (gaitClip(bytes.data(), bytes.size(), f.string(), cancel, *clip, error)) {
                            model.gaitClips[k] = clip;
                            said[k]            = f.filename().string();
                            continue;
                        }
                        log.push_back(error + ": the built in one instead");
                        error.clear();
                    }
                    if (gaitClip(in, size, std::format("built in {}", key), cancel, *clip, error))
                        model.gaitClips[k] = clip, said[k] = "built in";
                    else
                        log.push_back(error);
                }
                if (!model.gaitClips[0] || !model.gaitClips[1])
                    model.gaitClips = {}, said = {"none", "none"};
                model.gaitFrom = said[0] == said[1] ? said[0] : std::format("walk {}, run {}", said[0], said[1]);
            }

            // "hands": {"file": a VRM animation, "left"/"right": {"fist": time, ...}}: a gesture's finger pose, the animation's
            // at that time (unity2hypr3d writes the Gesture layer's hand poses so)
            void hands() {
                const SJson* h = settings.get("hands");
                if (!h || h->type != SJson::J_OBJ || !model.humanoid)
                    return;
                std::filesystem::path p{std::string(jstr(h->get("file")))};
                if (p.empty())
                    return;
                if (p.is_relative())
                    p = std::filesystem::path(model.settings).parent_path() / p;
                std::string error;
                const auto  made = emotesFromFile(p.string(), model, cancel, log, error, {});
                if (made.empty()) {
                    log.push_back(std::format("{}: the hand poses: {}", settingsName, error));
                    return;
                }
                const SAnimClip& anim = made[0]->anim;
                int              n    = 0;
                for (int hand = 0; hand < 2; ++hand) {
                    const SJson* side = h->get(hand ? "right" : "left");
                    if (!side || side->type != SJson::J_OBJ)
                        continue;
                    for (const auto& [name, t] : side->obj) {
                        const int g = gestureFromName(name);
                        if (g < 0) {
                            missing.push_back(std::format("gesture {}", name));
                            continue;
                        }
                        std::vector<STRS> pose;
                        for (const auto& nd : model.nodes)
                            pose.push_back(nd.rest);
                        sampleClip(anim, (float)jnum(&t, 0), pose);
                        for (int f = 0; f < FINGER_COUNT; ++f)
                            for (int s = 0; s < 3; ++s) {
                                const int b                         = model.human[fingerBone(hand, f, s)];
                                model.handPose[hand][g][f * 3 + s] = b >= 0 ? pose[b].r : Quat{};
                            }
                        model.handPoseSet[hand][g] = true;
                        ++n;
                    }
                }
                if (n)
                    log.push_back(std::format("hand poses: {} from {}", n, p.filename().string()));
            }

            static std::string join(const std::vector<std::string>& list, std::string_view by = ", ") {
                std::string s;
                for (const auto& x : list)
                    s += (s.empty() ? "" : std::string(by)) + x;
                return s;
            }

            void reportMissing() {
                if (missing.empty())
                    return;
                std::string list;
                for (const auto& m : missing)
                    list += std::format("{}\"{}\"", list.empty() ? "" : ", ", m);
                log.push_back(std::format("{}: not in the model: {}", settingsName, list));
            }
        };

        std::vector<std::shared_ptr<SAvatarEmote>> emotesFromFile(const std::string& file, const SAvatarModel& target, const std::atomic<bool>& cancel,
                                                                  std::vector<std::string>& log, std::string& error, std::string_view clip) {
            std::vector<std::shared_ptr<SAvatarEmote>> out;
            std::error_code                            ec;
            const auto                                 abs = std::filesystem::absolute(file, ec);
            // a folder: the animations in it
            if (std::filesystem::is_directory(abs, ec)) {
                std::vector<std::filesystem::path> in;
                for (const auto& d : std::filesystem::directory_iterator(abs, ec))
                    if (d.is_regular_file(ec) && isEmoteFile(d.path().string()))
                        in.push_back(d.path());
                std::ranges::sort(in);
                for (const auto& f : in) {
                    std::string why;
                    for (auto& e : emotesFromFile(f.string(), target, cancel, log, why, {}))
                        out.push_back(std::move(e));
                    if (!why.empty())
                        log.push_back(why);
                }
                if (out.empty())
                    error = in.empty() ? std::format("{} has no .vrma, .glb, .gltf or .vrm files", file) : std::format("nothing in {} moves this avatar", file);
                return out;
            }
            if (!std::filesystem::is_regular_file(abs, ec)) {
                error = std::format("emote {} doesn't exist", file);
                return out;
            }
            auto guard = gltf::open(abs.string(), "emote", error);
            if (!guard)
                return out;
            const std::string from = abs.filename().string();

            SAvatarModel             src;
            const SAvatarRequest     req{abs.string()};
            std::vector<std::string> quiet; // what it says of the model it's read as
            SBuild                   sb{req, cancel, guard.get(), src, quiet};
            sb.emoteSource = true;
            sb.nodes();
            sb.skins();
            sb.humanoid();
            sb.clips();
            if (src.clips.empty()) {
                error = std::format("{} has no animations", from);
                return out;
            }
            src.forward = facingOf(src, sb.restGlobal);

            // a VRM animation's faces: nodes the expressions ride on, their x the weight
            std::vector<std::pair<int, std::string>> faceNodes;
            if (const SJson* ex = sb.vrmJson.get("expressions"))
                for (const char* kind : {"preset", "custom"})
                    if (const SJson* group = ex->get(kind); group && group->type == SJson::J_OBJ)
                        for (const auto& [name, v] : group->obj)
                            if (const double n = jnum(v.get("node"), -1); n >= 0 && n < (double)sb.nodeIndex.size() && sb.nodeIndex[(size_t)n] >= 0)
                                faceNodes.push_back({sb.nodeIndex[(size_t)n], name});

            // and where its eyes look
            int eyeNode = -1;
            if (const SJson* la = sb.vrmJson.get("lookAt"))
                if (const double n = jnum(la->get("node"), -1); n >= 0 && n < (double)sb.nodeIndex.size())
                    eyeNode = sb.nodeIndex[(size_t)n];

            const int want = clip.empty() ? -1 : clipNamed(src.clips, clip);
            if (!clip.empty() && want < 0) {
                error = std::format("{} has no clip {}", from, clip);
                return out;
            }
            const bool human = src.humanoid && target.humanoid;
            SRig       srcRig, dstRig;
            if (human) {
                srcRig = rigOf(src);
                dstRig = rigOf(target);
            }
            std::vector<std::string> empty;
            for (int i = 0; i < (int)src.clips.size(); ++i) {
                if (want >= 0 && i != want)
                    continue;
                check(cancel);
                const SAnimClip&                                  c = src.clips[i];
                std::vector<std::pair<std::string, SAnimChannel>> faces;
                for (const auto& [node, name] : faceNodes)
                    for (const auto& ch : c.channels)
                        if (ch.node == node && ch.path == PATH_T)
                            faces.push_back({name, ch});
                SAnimClip anim = human ? retarget(canonical(c, srcRig, cancel), dstRig) : clipByNames(c, src, target);
                if (anim.channels.empty() && faces.empty()) {
                    empty.push_back(c.name);
                    continue;
                }
                const bool fingers = human ? movesFingers(src, c) : movesFingers(target, anim);
                auto       e       = emoteOf(target, std::move(anim), faces);
                for (const auto& ch : c.channels)
                    if (eyeNode >= 0 && ch.node == eyeNode && ch.path == PATH_R && target.lookAt.type != SLookAt::NONE)
                        e->eyes = {ch};
                const auto low     = lower(c.name);
                e->name            = src.clips.size() == 1 || want >= 0 ? abs.stem().string() : c.name;
                e->from            = from;
                e->fingers         = fingers;
                e->loop            = low.contains("loop") || low.contains("dance");
                out.push_back(std::move(e));
            }
            if (!empty.empty())
                log.push_back(std::format("{}: left out {} (they move nothing the avatar has)", from, SBuild::join(empty)));
            if (!human && !out.empty())
                log.push_back(std::format("{}: made by node names, as {} a humanoid", from, src.humanoid ? "the avatar isn't" : "it isn't"));
            if (out.empty() && error.empty())
                error = std::format("{}: nothing in it moves this avatar{}", from, human ? "" : " (the two aren't both humanoids, and their node names don't match)");
            return out;
        }

        // --- attacks: a punch made for any humanoid (the right arm's), made for the avatar, and its mirror image for the
        // left arm

        // what of the body an attack has: the trunk (turned on as it turns it from rest) and the arms (as it has them);
        // per node (CAvatarAnimator::m_attackPart)
        enum eAttackPart : uint8_t {
            ATTACK_NONE,
            ATTACK_TRUNK,
            ATTACK_ARM,
        };
        constexpr bool attackTrunkBone(int b) {
            return b == HB_SPINE || b == HB_CHEST || b == HB_UPPER_CHEST || b == HB_NECK || b == HB_HEAD;
        }
        constexpr bool attackArmBone(int b) {
            return b >= HB_L_SHOULDER && b <= HB_R_HAND;
        }

        // the other side's bone
        constexpr int mirrorBone(int b) {
            if (b >= HB_FINGERS)
                return b < HB_FINGERS + FINGER_COUNT * 3 ? b + FINGER_COUNT * 3 : b - FINGER_COUNT * 3;
            if (b >= HB_L_UPPER_LEG && b <= HB_L_FOOT)
                return b + 3;
            if (b >= HB_R_UPPER_LEG && b <= HB_R_FOOT)
                return b - 3;
            if (b >= HB_L_SHOULDER && b <= HB_L_HAND)
                return b + 4;
            if (b >= HB_R_SHOULDER && b <= HB_R_HAND)
                return b - 4;
            switch (b) {
                case HB_L_EYE: return HB_R_EYE;
                case HB_R_EYE: return HB_L_EYE;
                case HB_L_TOES: return HB_R_TOES;
                case HB_R_TOES: return HB_L_TOES;
                default: return b;
            }
        }

        // a clip's mirror image: what one side's bones do, the other's, turned the other way round across the middle (the
        // frame's +X is the left)
        SNormClip mirrored(const SNormClip& nc) {
            SNormClip m = nc;
            for (int b = 0; b < HB_COUNT; ++b)
                m.has[mirrorBone(b)] = nc.has[b];
            for (size_t k = 0; k < nc.times.size(); ++k) {
                for (int b = 0; b < HB_COUNT; ++b) {
                    const Quat& q            = nc.turn[k][b];
                    m.turn[k][mirrorBone(b)] = {q.x, -q.y, -q.z, q.w};
                }
                m.move[k].x = -nc.move[k].x;
            }
            return m;
        }

        // an attack's swings, per arm, made for the avatar from a VRM animation of a humanoid (a GLB in memory: its first
        // clip, the right arm's punch) of the trunk and arms: when the fists are up, it strikes, the other's may start and
        // it lets go, from its markers (the animation's extras: {"markers": {"ready": seconds, "hit", "next", "out"}})
        bool attackClips(const void* bytes, size_t size, const std::string& what, const SAvatarModel& target, const std::atomic<bool>& cancel,
                         std::array<SAvatarAttack, 2>& out, std::string& error) {
            auto guard = gltf::openMemory(bytes, size, what, error);
            if (!guard)
                return false;
            SAvatarModel             src;
            const SAvatarRequest     req{what};
            std::vector<std::string> quiet;
            SBuild                   sb{req, cancel, guard.get(), src, quiet};
            sb.emoteSource = true;
            sb.nodes();
            sb.skins();
            sb.humanoid();
            sb.clips();
            if (!src.humanoid || src.clips.empty()) {
                error = std::format("{} isn't a humanoid's animation", what);
                return false;
            }
            src.forward             = facingOf(src, sb.restGlobal);
            const SAnimClip& c      = src.clips[0];
            const SRig       srcRig = rigOf(src);
            SNormClip        nc     = canonical(c, srcRig, cancel);
            bool             any    = false;
            for (int b = 0; b < HB_COUNT; ++b) {
                nc.has[b] = nc.has[b] && (attackTrunkBone(b) || attackArmBone(b));
                any       = any || (nc.has[b] && attackArmBone(b));
            }
            if (!any) {
                error = std::format("{} doesn't move the arms", what);
                return false;
            }
            nc.moves         = false;
            const SRig rig   = rigOf(target);
            out[1]           = {};
            out[0]           = {};
            out[1].anim      = retarget(nc, rig);
            out[0].anim      = retarget(mirrored(nc), rig);
            out[1].anim.name = c.name;
            out[0].anim.name = c.name + " (mirrored)";
            // (where its wrists are from its eyes, in arm lengths, the way it faces: at its times, as it was made; the eyes'
            // middle, else a guess over the head's joint; the mirror image for the left arm's)
            const auto& sh = src.human;
            if (sh[HB_HEAD] >= 0 && sh[HB_L_UPPER_ARM] >= 0 && sh[HB_L_LOWER_ARM] >= 0 && sh[HB_L_HAND] >= 0 && sh[HB_R_UPPER_ARM] >= 0 &&
                sh[HB_R_LOWER_ARM] >= 0 && sh[HB_R_HAND] >= 0) {
                const SRig& from = srcRig;
                auto        len  = [&](int a, int b) { return length(origin(from.restGlobal[sh[b]]) - origin(from.restGlobal[sh[a]])); };
                const float arm  = 0.5f * (len(HB_L_UPPER_ARM, HB_L_LOWER_ARM) + len(HB_L_LOWER_ARM, HB_L_HAND) + len(HB_R_UPPER_ARM, HB_R_LOWER_ARM) +
                                         len(HB_R_LOWER_ARM, HB_R_HAND));
                std::vector<STRS> pose(src.nodes.size());
                std::vector<M4>   g(src.nodes.size());
                for (float t : nc.times) {
                    if (arm < 1e-6f)
                        break;
                    for (size_t n = 0; n < src.nodes.size(); ++n)
                        pose[n] = src.nodes[n].rest;
                    sampleClip(c, t, pose);
                    for (size_t n = 0; n < src.nodes.size(); ++n)
                        g[n] = src.nodes[n].parent >= 0 ? g[src.nodes[n].parent] * pose[n].matrix() : pose[n].matrix();
                    const V3 eyes = sh[HB_L_EYE] >= 0 && sh[HB_R_EYE] >= 0 ? (origin(g[sh[HB_L_EYE]]) + origin(g[sh[HB_R_EYE]])) * 0.5f :
                                                                             origin(g[sh[HB_HEAD]]) + from.facing.rotate(V3{0, 0.15f, 0.09f}) * arm;
                    std::array<V3, 2> at;
                    for (int s = 0; s < 2; ++s)
                        at[s] = from.facing.conj().rotate(origin(g[sh[s ? HB_R_HAND : HB_L_HAND]]) - eyes) * (1.f / arm);
                    out[1].reachT.push_back(t);
                    out[1].reach.push_back(at);
                    out[0].reachT.push_back(t);
                    out[0].reach.push_back({V3{-at[1].x, at[1].y, at[1].z}, V3{-at[0].x, at[0].y, at[0].z}});
                }
            }
            // (the markers, by the clip's name: an animation with no channels isn't a clip)
            const float dur = c.duration;
            SJson       marks;
            for (size_t i = 0; i < guard->animations_count; ++i)
                if (const auto& an = guard->animations[i]; (an.name ? std::string(an.name) : std::format("clip {}", i)) == c.name && an.extras.data) {
                    SJson x;
                    if (CJsonReader(an.extras.data).read(x) && x.get("markers"))
                        marks = *x.get("markers");
                    break;
                }
            auto at = [&](const char* name, float def) { return std::clamp((float)jnum(marks.get(name), def), 0.f, dur); };
            for (auto& a : out) {
                a.ready = at("ready", 0.1f);
                a.hit   = std::max(a.ready, at("hit", 0.2f));
                a.next  = std::max(a.hit, at("next", a.hit + 1.f / 60));
                a.out   = std::max(a.next, at("out", dur - 0.25f));
            }
            return true;
        }

        // A walk's or a run's body from a VRM animation of a humanoid (a GLB in memory: its first clip, one stride, the left
        // heel landing at its start and its end): in the frame above at GAIT_STEPS even steps of the stride, and the
        // means (the turns' sign-aligned sum, normalized: their spread is small)
        constexpr int GAIT_STEPS = 64;
        bool gaitClip(const void* bytes, size_t size, const std::string& what, const std::atomic<bool>& cancel, SGaitClip& out, std::string& error) {
            auto guard = gltf::openMemory(bytes, size, what, error);
            if (!guard)
                return false;
            SAvatarModel             src;
            const SAvatarRequest     req{what};
            std::vector<std::string> quiet;
            SBuild                   sb{req, cancel, guard.get(), src, quiet};
            sb.emoteSource = true;
            sb.nodes();
            sb.skins();
            sb.humanoid();
            sb.clips();
            if (!src.humanoid || src.clips.empty() || src.clips[0].duration <= 0) {
                error = std::format("{} isn't a humanoid's animation", what);
                return false;
            }
            src.forward            = facingOf(src, sb.restGlobal);
            const SAnimClip& c     = src.clips[0];
            const SNormClip  nc    = canonical(c, rigOf(src), cancel);
            if (nc.times.size() < 2 || !nc.has[HB_HIPS] || !nc.has[HB_L_UPPER_ARM] || !nc.has[HB_R_UPPER_ARM]) {
                error = std::format("{} doesn't move the hips and the arms", what);
                return false;
            }
            out      = {};
            out.name = c.name;
            out.has  = nc.has;
            out.turn.resize(GAIT_STEPS);
            out.move.resize(GAIT_STEPS);
            std::array<Quat, HB_COUNT> sum{};
            for (auto& q : sum)
                q = {0, 0, 0, 0};
            for (int i = 0; i < GAIT_STEPS; ++i) {
                const float  t = nc.duration * (float)i / GAIT_STEPS;
                const size_t k = std::min((size_t)(std::ranges::upper_bound(nc.times, t) - nc.times.begin()), nc.times.size() - 1);
                const size_t j = k > 0 ? k - 1 : 0;
                const float  dt = nc.times[k] - nc.times[j], u = dt > 1e-6f ? std::clamp((t - nc.times[j]) / dt, 0.f, 1.f) : 0.f;
                for (int b = 0; b < HB_COUNT; ++b) {
                    Quat q = slerp(nc.turn[j][b], nc.turn[k][b], u).normalized();
                    if (sum[b].x * q.x + sum[b].y * q.y + sum[b].z * q.z + sum[b].w * q.w < 0)
                        q = {-q.x, -q.y, -q.z, -q.w};
                    out.turn[i][b] = q;
                    sum[b]         = {sum[b].x + q.x, sum[b].y + q.y, sum[b].z + q.z, sum[b].w + q.w};
                }
                out.move[i] = lerp(nc.move[j], nc.move[k], u);
                out.meanMove += out.move[i] * (1.f / GAIT_STEPS);
            }
            for (int b = 0; b < HB_COUNT; ++b)
                out.mean[b] = sum[b].normalized();
            return true;
        }

        // a gait clip at a part of its stride (0..1): every bone's turn, and the hips' move
        void sampleGait(const SGaitClip& c, float phase, std::array<Quat, HB_COUNT>& turn, V3& move) {
            const float  x = (phase - std::floor(phase)) * (float)c.turn.size();
            const size_t i = std::min((size_t)x, c.turn.size() - 1), j = (i + 1) % c.turn.size();
            const float  u = x - (float)i;
            for (int b = 0; b < HB_COUNT; ++b)
                turn[b] = slerp(c.turn[i][b], c.turn[j][b], u);
            move = lerp(c.move[i], c.move[j], u);
        }

    }

    // --- loading

    SAvatarResult loadAvatar(const SAvatarRequest& req, const std::atomic<bool>& cancel) {
        SAvatarResult res;
        res.req   = req;
        auto& log = res.log;

        std::error_code ec;
        const auto      abs = std::filesystem::absolute(req.path, ec);
        if (!std::filesystem::is_regular_file(abs, ec)) {
            res.error = std::format("avatar {} doesn't exist", req.path);
            return res;
        }
        const std::string path = abs.string();

        auto guard = gltf::open(path, "avatar", res.error);
        if (!guard)
            return res;
        cgltf_data* data = guard.get();

        auto model  = std::make_shared<SAvatarModel>();
        model->path = path;
        model->name = abs.stem().string();

        SBuild b{req, cancel, data, *model, log};
        b.mats           = gltf::readMaterials(data, false); // its vertices have no tangents
        b.nodes();
        b.skins();
        b.readSettings(abs);
        b.humanoid(); // before the materials: tells whether it's a VRM
        if (b.vrm)
            for (auto& m : b.mats.materials)
                m.unlit = false; // MToon's fallback is unlit, but lit fits the world better
        model->images    = std::move(b.mats.images);
        model->materials = b.mats.materials;
        b.meshes();
        if (b.skippedDraco)
            log.push_back(std::format("skipped {} draco compressed meshes (not supported)", b.skippedDraco));
        if (model->indices.empty()) {
            res.error = std::format("{} has no triangles", req.path);
            return res;
        }
        if (model->joints.size() > 65535) {
            res.error = std::format("{} has {} joints, too many", req.path, model->joints.size());
            return res;
        }
        check(cancel);
        b.place();
        b.footShapes();
        b.clips();
        b.expressions();
        b.outfit();
        b.springs();
        b.constraints();
        b.bodyClearance(); // (after the outfit and the constraints: what starts hidden, or follows an arm, doesn't count)
        b.firstPerson();
        b.emotes();
        b.attacks();
        b.gaitClips();
        b.hands();
        b.reportMissing();
        check(cancel);
        gltf::decodeImages(data, abs.parent_path().string(), model->images, b.mats.imageSlot, cancel, log);

        std::string clips;
        static constexpr const char* KIND[] = {"idle", "walk", "run", "jump", "fall"};
        for (int k = 0; k < CLIP_COUNT; ++k)
            if (model->clipFor[k] >= 0)
                clips += std::format("{}{}={}", clips.empty() ? "" : ", ", KIND[k], model->clips[model->clipFor[k]].name);
        log.push_back(std::format("avatar {}: {} triangles, {} joints, {:.2f} m tall, {}, clips: {}", model->name, model->triangles, model->joints.size(),
                                  model->height, model->humanoid ? std::format("humanoid (from {}{})", model->humanFrom, model->fingers ? std::format(", {} fingers", model->fingers) : "") : "not humanoid",
                                  clips.empty() ? "none" : clips));
        if (!model->expressions.empty() || model->lookAt.type != SLookAt::NONE) {
            const size_t shapeKeys = std::ranges::count_if(model->expressions, [](const SExpression& e) { return e.shapeKey; });
            std::string  presets;
            for (int p = 0; p < EX_COUNT; ++p)
                if (model->preset[p] >= 0)
                    presets += std::format("{}{}", presets.empty() ? "" : " ", PRESET_NAMES[p]);
            std::string consonants;
            for (int k = 0; k < VISEME_COUNT - VOWEL_COUNT; ++k)
                if (model->consonant[k] >= 0)
                    consonants += std::format(" {}", VISEME_NAMES[VOWEL_COUNT + k]);
            static constexpr const char* EYES[] = {"don't move", "turn (bones)", "turn (expressions)"};
            log.push_back(std::format("face: {} expressions{}, {} shape keys, {} morphs; presets: {}; eyes {}{}", model->expressions.size() - shapeKeys,
                                      model->expressionsFrom.empty() ? "" : " from " + model->expressionsFrom, shapeKeys, model->morphs.size(),
                                      presets.empty() ? "none" : presets, EYES[model->lookAt.type],
                                      consonants.empty() ? "" : "; lip sync's consonants:" + consonants));
        }
        if (model->parts.size() > 1 || !model->settings.empty()) {
            const size_t hidden = std::ranges::count_if(model->parts, [](const SAvatarPart& p) { return p.hidden; });
            log.push_back(std::format("outfit: {} parts{}, {} toggles{}{}{}{}", model->parts.size(), hidden ? std::format(" ({} hidden)", hidden) : "",
                                      model->toggles.size(), model->sliders.empty() ? "" : std::format(", {} sliders", model->sliders.size()),
                                      model->variants.empty() ? "" : std::format(", {} material variants", model->variants.size()),
                                      model->fixed.empty() ? "" : std::format(", {} held in the world", model->fixed.size()),
                                      model->settings.empty() ? "" : " from " + b.settingsName));
        }
        if (!model->springJoints.empty() || !model->constraints.empty()) {
            std::string physics;
            if (!model->springJoints.empty())
                physics = std::format("{} springs, {} bones, {} colliders from {}", model->springs.size(), model->springJoints.size(), model->springColliders.size(),
                                      model->springsFrom);
            if (!model->constraints.empty())
                physics += std::format("{}{} node constraints", physics.empty() ? "" : "; ", model->constraints.size());
            log.push_back("physics: " + physics);
        }
        if (!model->emotes.empty())
            log.push_back(std::format("emotes: {}", b.emotesMade));
        if (!model->attacksFrom.empty())
            log.push_back(std::format("attacks: {}", model->attacksFrom));
        if (!model->gaitFrom.empty())
            log.push_back(std::format("walk and run: {}", model->gaitFrom));
        if (model->humanoid) {
            const auto& f = model->feet;
            log.push_back(std::format("feet{}: the ankles {:.0f} and {:.0f} cm up, the heels {:.0f} cm behind them, the balls {:.0f} cm ahead",
                                      f[0].measured && f[1].measured ? "" : f[0].measured || f[1].measured ? " (one guessed)" : " (guessed)", -f[0].heel.y * 100,
                                      -f[1].heel.y * 100, (f[0].heel.z + f[1].heel.z) * 50, -(f[0].ball.z + f[1].ball.z) * 50));
            if (const auto& c = model->clearance; c.measured) {
                // (how far out the body goes, at its widest down to the thighs: a skirt's hem, else the hips)
                float widest = 0, at = 0;
                for (int i = 0; i < c.rows && c.y0 + c.dy * i < 0; ++i) {
                    if (c.y0 + c.dy * i < -0.2f * model->height)
                        continue;
                    float sum = 0;
                    for (int k = 0; k < c.bearings; ++k)
                        sum += c.out[(size_t)i * c.bearings + k];
                    if (sum / c.bearings > widest)
                        widest = sum / c.bearings, at = c.y0 + c.dy * (i + 0.5f);
                }
                float sleeve = 0;
                for (int s = 0; s < 2; ++s)
                    for (float r : c.arm[s][1])
                        sleeve = std::max(sleeve, r);
                log.push_back(std::format("arms keep clear of the body: {:.0f} cm out round the hips at its widest ({:.0f} cm below them), the forearms {:.0f} cm "
                                          "thick at most, the hands {:.0f} cm long",
                                          widest * 100, -at * 100, sleeve * 100, (c.hand[0] + c.hand[1]) * 50));
            }
        }
        res.model = std::move(model);
        return res;
    }

    SEmoteResult loadEmotes(const SEmoteRequest& req, const std::atomic<bool>& cancel) {
        SEmoteResult res;
        res.req = req;
        if (!req.model) {
            res.error = "no avatar to make emotes for";
            return res;
        }
        std::vector<std::string> errors;
        for (const auto& file : req.files) {
            std::string error;
            for (auto& e : emotesFromFile(file, *req.model, cancel, res.log, error))
                res.emotes.push_back(std::move(e));
            if (!error.empty())
                errors.push_back(std::move(error));
        }
        // some did: what didn't is only said
        if (res.emotes.empty())
            res.error = errors.empty() ? std::format("{}: no emotes in it", req.path) : SBuild::join(errors, "; ");
        else
            res.log.insert(res.log.end(), errors.begin(), errors.end());
        return res;
    }

    bool isEmoteFile(std::string_view path) {
        const size_t dot = path.rfind('.');
        if (dot == std::string_view::npos || path.find('/', dot) != std::string_view::npos)
            return false;
        const std::string ext = lower(std::string(path.substr(dot)));
        return ext == ".vrma" || ext == ".glb" || ext == ".gltf" || ext == ".vrm";
    }

    // --- looking things up by name

    int SAvatarModel::findExpression(std::string_view name) const {
        if (name.empty())
            return -1;
        const std::string low = lower(std::string(name));
        for (size_t i = 0; i < expressions.size(); ++i)
            if (lower(expressions[i].name) == low)
                return (int)i;
        if (const int p = presetOf(name); p >= 0 && preset[p] >= 0)
            return preset[p];
        const std::string loose = normName(name);
        for (size_t i = 0; i < expressions.size(); ++i)
            if (normName(expressions[i].name) == loose)
                return (int)i;
        return -1;
    }

    int SAvatarModel::findToggle(std::string_view name) const {
        if (name.empty())
            return -1;
        const std::string low = lower(std::string(name)), loose = normName(name);
        for (size_t i = 0; i < toggles.size(); ++i)
            if (lower(toggles[i].name) == low)
                return (int)i;
        for (size_t i = 0; i < toggles.size(); ++i)
            if (normName(toggles[i].name) == loose)
                return (int)i;
        return -1;
    }

    int SAvatarModel::findSlider(std::string_view name) const {
        if (name.empty())
            return -1;
        const std::string low = lower(std::string(name)), loose = normName(name);
        for (size_t i = 0; i < sliders.size(); ++i)
            if (lower(sliders[i].name) == low)
                return (int)i;
        for (size_t i = 0; i < sliders.size(); ++i)
            if (normName(sliders[i].name) == loose)
                return (int)i;
        return -1;
    }

    int SAvatarModel::findVariant(std::string_view name) const {
        for (size_t i = 0; i < variants.size(); ++i)
            if (variants[i] == name)
                return (int)i;
        const std::string low = lower(std::string(name));
        for (size_t i = 0; i < variants.size(); ++i)
            if (lower(variants[i]) == low)
                return (int)i;
        return -1;
    }

    // by its own name or a parent node's: "Jacket" is every mesh under the jacket's node too, as a GameObject
    // turned off in Unity hides its children
    std::vector<int> SAvatarModel::findParts(std::string_view name) const {
        std::vector<int> out;
        for (int loose = 0; loose < 2 && out.empty() && !name.empty(); ++loose) {
            const std::string want = loose ? normName(name) : lower(std::string(name));
            auto              is   = [&](const std::string& n) { return (loose ? normName(n) : lower(n)) == want; };
            for (size_t i = 0; i < parts.size(); ++i) {
                bool yes = is(parts[i].name);
                for (int n = parts[i].node >= 0 ? nodes[parts[i].node].parent : -1; n >= 0 && !yes; n = nodes[n].parent)
                    yes = is(nodes[n].name);
                if (yes)
                    out.push_back((int)i);
            }
        }
        return out;
    }

    // "name" on any mesh, or "mesh node/name" (when no shape key is called that)
    std::vector<int> SAvatarModel::findMorphs(std::string_view name) const {
        std::vector<int> out;
        auto             find = [&](std::string_view key, const std::vector<int>* in) {
            for (int loose = 0; loose < 2 && out.empty() && !key.empty(); ++loose) {
                const std::string want = loose ? normName(key) : lower(std::string(key));
                for (size_t i = 0; i < morphs.size(); ++i)
                    if ((loose ? normName(morphs[i].name) : lower(morphs[i].name)) == want &&
                        (!in || std::ranges::any_of(*in, [&](int p) { return parts[p].gltfNode == morphs[i].gltfNode; })))
                        out.push_back((int)i);
            }
        };
        find(name, nullptr);
        if (const size_t slash = name.rfind('/'); out.empty() && slash != std::string_view::npos) {
            const std::vector<int> in = findParts(name.substr(0, slash));
            find(name.substr(slash + 1), &in);
        }
        return out;
    }

    // --- walking
    //
    // A humanoid without clips of its own walks procedurally. Each foot is planted where it lands and stays there, in
    // the world, till it lifts; the body is carried over the feet. How long a stride is and how often it comes follow
    // the speed and the legs' length as they do in people (dynamic similarity: stride = 2.5 legs x Froude^0.3, after
    // Alexander 1976), and so does how long each foot is down (the duty factor: about 60% walking, 30-40% running).
    // Between steps it goes as gait studies measure it: the foot lands on its heel and pushes off over its ball, the
    // pelvis rises over the standing leg walking (and sinks onto it running), sways over it, drops on the other side
    // and turns with the stride, the trunk turns against the pelvis and the arms swing against the legs, the head
    // stays level. Standing, it steps when the body moves off its feet or turns away from them

    namespace {
        constexpr float GRAVITY = 9.81f;

        float wrapPi(float a) {
            a = std::fmod(a + PI, TAU);
            if (a < 0)
                a += TAU;
            return a - PI;
        }

        float frac(float x) {
            return x - std::floor(x);
        }

        // a difference of phases (of a cycle) the short way round, -0.5..0.5
        float phaseDiff(float d) {
            return d - std::round(d);
        }

        // toward `to`, at most `step`
        float approach(float from, float to, float step) {
            return from < to ? std::min(from + step, to) : std::max(from - step, to);
        }

        // 0 to 1 with no speed or acceleration at either end: how a reach, or a foot through the air, goes
        float minJerk(float s) {
            s = std::clamp(s, 0.f, 1.f);
            return s * s * s * (10.f + s * (-15.f + 6.f * s));
        }

        // a curve through keys (t rising from 0 to 1), smoothly between them (Catmull-Rom), level at the ends
        struct SCurveKey {
            float t, v;
        };
        template <size_t N>
        float through(const SCurveKey (&k)[N], float s) {
            if (s <= k[0].t)
                return k[0].v;
            if (s >= k[N - 1].t)
                return k[N - 1].v;
            size_t i = 0;
            while (i + 2 < N && s > k[i + 1].t)
                ++i;
            const float dt = k[i + 1].t - k[i].t, u = (s - k[i].t) / dt, u2 = u * u, u3 = u2 * u;
            auto        slope = [&](size_t j) { return j == 0 || j + 1 >= N ? 0.f : (k[j + 1].v - k[j - 1].v) / (k[j + 1].t - k[j - 1].t); };
            return (2 * u3 - 3 * u2 + 1) * k[i].v + (u3 - 2 * u2 + u) * slope(i) * dt + (-2 * u3 + 3 * u2) * k[i + 1].v + (u3 - u2) * slope(i + 1) * dt;
        }

        // The foot's pitch (radians, toes down > 0) through its time on the ground, from landing to lifting: on the heel,
        // toes up; flat; the heel rising as it pushes off over its ball. Walking lands further back on the heel and rolls
        // longer; running lands flatter and pushes off harder
        constexpr SCurveKey WALK_STANCE_PITCH[] = {{0, -0.26f}, {0.12f, -0.03f}, {0.2f, 0}, {0.5f, 0}, {0.75f, 0.24f}, {1, 0.9f}};
        constexpr SCurveKey RUN_STANCE_PITCH[]  = {{0, -0.13f}, {0.1f, 0}, {0.4f, 0}, {0.7f, 0.32f}, {1, 0.95f}};
        // and through the air: toes down as it lifts, turned up to clear the ground, the heel first to land
        constexpr SCurveKey WALK_SWING_PITCH[] = {{0, 0.9f}, {0.2f, 0.4f}, {0.42f, 0.03f}, {0.62f, -0.1f}, {0.85f, -0.16f}, {1, -0.26f}};
        constexpr SCurveKey RUN_SWING_PITCH[]  = {{0, 0.95f}, {0.25f, 0.75f}, {0.5f, 0.38f}, {0.75f, 0.02f}, {0.9f, -0.08f}, {1, -0.13f}};
        // how high the foot goes through the air (legs): walking, up at once as the knee bends and then low over the
        // ground; running, the heel kicked up behind
        constexpr SCurveKey WALK_LIFT[]  = {{0, 0}, {0.15f, 0.07f}, {0.3f, 0.065f}, {0.5f, 0.035f}, {0.75f, 0.025f}, {0.9f, 0.015f}, {1, 0}};
        constexpr SCurveKey RUN_LIFT[]   = {{0, 0}, {0.2f, 0.2f}, {0.38f, 0.27f}, {0.55f, 0.22f}, {0.75f, 0.11f}, {0.9f, 0.04f}, {1, 0}};
        constexpr SCurveKey STAND_LIFT[] = {{0, 0}, {0.4f, 0.05f}, {0.75f, 0.035f}, {1, 0}};
        // How far the knee bends walking (radians), as gait studies measure it (Perry and Burnfield): late in its time
        // on the ground, nearly straight as the heel rises, bending as the foot pushes off (38° as it leaves the
        // ground); through the air, folding up to 60° early (the foot clears the ground by it) and straight again
        // to land. The heel rises as far as keeps the knee to that, and the foot goes up as high as it does
        constexpr SCurveKey WALK_STANCE_KNEE[] = {{0.4f, 0.1f}, {0.65f, 0.14f}, {0.8f, 0.24f}, {0.9f, 0.42f}, {1, 0.66f}};
        constexpr SCurveKey WALK_SWING_KNEE[]  = {{0, 0.66f}, {0.12f, 0.87f}, {0.25f, 1.f}, {0.33f, 1.05f}, {0.45f, 0.93f}, {0.6f, 0.68f}, {0.75f, 0.35f}, {0.88f, 0.14f}, {1, 0.07f}};
        // the least the heel rises, late on the ground, whatever the knee
        constexpr SCurveKey WALK_HEEL_LEAST[] = {{0.5f, 0}, {0.8f, 0.1f}, {1, 0.5f}};

        // how far the pelvis turns from a planted foot, radians (a hip turns about 40° each way, with the knee's give),
        // and how fast a planted foot pivots on its ball when the body turns further, radians a second (people turning
        // round pivot 30-60° on the foot they stand on, then put the next one down turned: the turn goes over the steps,
        // not all on one foot)
        constexpr float HIP_TURN   = 0.5f;
        constexpr float PIVOT_RATE = 5.f;
        // a planted foot doesn't pivot where its heel would come over ground higher than it stands by more than this
        // (meters, a slope's rise aside): on stairs that's into the step behind it, and people step round there instead
        constexpr float PIVOT_CLEAR = 0.03f;
        // how fast the pelvis turns at most (radians a second: a quick turn round, as people's goes, a few hundred degrees
        // a second), and how quickly that changes (radians a second a second)
        constexpr float HIP_RATE = 6.f, HIP_ACCEL = 50.f;
        // Turning round, people slow down, turn, then go: the legs don't set off the new way till the pelvis faces it
        // near enough. What the legs carry goes toward a way further than GO_ON (radians) off where the pelvis faces less,
        // GO_LEAST of it past GO_OFF, while the body is still turning to it; less so the further it has fallen behind
        // (fully past GO_GIVE of the way from BODY_LAG_SOFT to BODY_LAG), and running (RUN_GO of the way to not at all:
        // a run turns round in a curve; held back less, a running zigzag goes on across the pelvis, the legs splayed, and
        // more, it falls behind till it's dragged along)
        constexpr float GO_ON = 0.35f, GO_OFF = 1.8f, GO_LEAST = 0.25f, GO_GIVE = 0.5f, RUN_GO = 0.4f;
        // turning far, the steps go at least TURN_CADENCE strides a second (turning on the spot), and don't slow down
        // while the pelvis turns faster than CADENCE_TURN (radians a second: round a tight curve)
        constexpr float TURN_CADENCE = 1.1f, CADENCE_TURN = 2.f;
        // a planted foot further out to its side of its hip than OUT_EASY (meters) hurries the steps: OUT_HURRY more for each
        // meter past it, up to OUT_MOST (the steps then 1.7 times as quick: hurried more, a step lasts a tenth of a second,
        // the knee folding and the foot flicking up in a few frames)
        constexpr float OUT_EASY = 0.06f, OUT_HURRY = 6.f, OUT_MOST = 1.3f;
        // a foot in the air no further out to its side of its hip, or in under the body, than this (meters): it hangs
        // from the hip, whichever way the pelvis turns meanwhile
        constexpr float SWING_OUT = 0.05f, SWING_IN = 0.03f;
        // how quickly the body's turning speeds up or slows down, radians a second a second (a quick turn: 7 radians a
        // second in about a ninth of a second)
        constexpr float TURN_ACCEL = 60.f;
        // how quickly where the feet go follows a new way to turn, radians a second, and how fast where a foot in the air
        // lands moves at most, meters a second over the body's speed (else it would snap there)
        constexpr float TURN_PLAN = 10.f, RETARGET = 2.5f;
        // What the legs carry follows the player's body: toward its velocity at BODY_K of the difference a second (the
        // push building up over a few hundredths of a second), speeding up, slowing down and turning back no harder than
        // legs push (m/s², walking and running: a body over a foot planted half a leg ahead slows at about g/2), and a
        // little quicker than it to close a gap (BODY_CATCH_UP of the gap a second, at most 3 cm/s and BODY_CATCH_UP_MOST
        // of its speed over it: setting off it trails a little, rather than hurrying to catch up); harder past BODY_LAG_SOFT behind (6 cm more for each m/s it goes), never more than
        // BODY_LAG - BODY_LAG_SOFT further; onto it no quicker than stops in the gap left when a wall stopped it (up to
        // BODY_WALL m/s²); the keys let go, going on past it no further than BODY_REST (and a tenth of a second at
        // its speed), and stopped within twice that, left there
        constexpr float BODY_K = 8.f, BODY_WALK_ACCEL = 7.f, BODY_RUN_ACCEL = 10.f, BODY_CATCH_UP = 1.f, BODY_CATCH_UP_MOST = 0.03f, BODY_LAG_SOFT = 0.3f,
                        BODY_LAG = 0.5f, BODY_WALL = 40.f, BODY_REST = 0.2f;
        // going back and forth (wayToFace holding its facing), it faces the way it has been going when that's faster
        // than WAY_GOING (m/s): tapping the keys back the other way only slows it down
        constexpr float WAY_GOING = 0.3f;
        // turning, the head looks ahead into the turn: HEAD_LEAD of what's left of it, at most HEAD_LEAD_MOST (radians)
        constexpr float HEAD_LEAD = 0.6f, HEAD_LEAD_MOST = 0.7f;
        // how far the trunk turns from the pelvis at most (radians), and how quickly it turns there (a second)
        constexpr float TRUNK_TWIST = 0.6f, TRUNK_W = 14.f;
        // how quickly an arm goes out to keep clear of the body as it's swung now (a second, critically damped)
        constexpr float ARM_PUSH_W = 40.f;
        // the walk's and the run's clips (SGaitClip) are as they are at these speeds (m/s, for legs of GAIT_LEG meters: a
        // longer leg's at the speed that's as quick for it, as dynamic similarity has it); slower or quicker their swing
        // is that much smaller or bigger (down to GAIT_LEAST, up to GAIT_MOST of it)
        constexpr float GAIT_WALK = 1.5f, GAIT_RUN = 4.5f, GAIT_LEG = 0.8f, GAIT_LEAST = 0.35f, GAIT_MOST = 1.2f;
        // how far out the clips' arms need to be held to keep clear of the body is known at this many steps of the
        // stride, every GAIT_NEED_EVERY-th of them looked at again each frame
        constexpr int GAIT_NEED_STEPS = 24, GAIT_NEED_EVERY = 8;
        // how fast a foot's pitch changes at most, radians a second (walking, it pushes off at about 8), and how
        // quickly that speed changes (radians a second a second): a foot that starts or stops rolling at once flicks
        constexpr float FOOT_ROLL = 16.f, FOOT_ROLL_ACCEL = 400.f;
        // how quickly a foot in the air goes up or down faster or slower for the knee's fold, meters a second a second
        // (walking steadily it needs 90 at most)
        constexpr float RAISE_ACCEL = 120.f;
        // how quickly the steps slow down (strides a second, a second) as the body slows turning back or far: the feet
        // keep stepping
        constexpr float CADENCE_DROP = 0.4f;
        // how far a foot put down across a stair's edge moves along itself (ahead or back) to be on one tread, meters
        constexpr float STAIR_SHIFT = 0.16f, STAIR_ROOM = 0.03f; // (and how much room it leaves round itself, meters)
        // how hard the pelvis speeds up going down for the legs to reach (m/s²), and how fast it goes (m/s)
        constexpr float LOWER_ACCEL = 60.f, LOWER_SPEED = 3.f;
        // the steepest slope a foot lies along (radians), and how far the ground under it may be from a line and still
        // count as one (meters: more is a stair's edge, or rubble)
        constexpr float SLOPE_MOST = 0.45f, SLOPE_EVEN = 0.012f;

        // Off the ground the legs, the arms and the body go on from how they were, toward the pose for how it goes, as
        // springs (a limb with weight: no snapping to it). A jump (or a fall), by how far through it is (`ju`: 0 leaving
        // the ground at JUMP_UP m/s, 1/2 at the top, 1 coming down as fast): from standing both legs tuck and reach down
        // to land, the arms swinging up then out; from a run the leg that was stepping goes on ahead and the other trails
        // (a stride in the air), the arms against them, the leading leg reaching to land first. Angles in radians: the
        // hip's ahead (thigh from straight down), the knee's bend, the toes down, an arm swung ahead
        constexpr float JUMP_UP = 6.3f;
        constexpr SCurveKey JUMP_HIP[]        = {{0, 0.55f}, {0.5f, 0.35f}, {1, 0.15f}};
        constexpr SCurveKey JUMP_KNEE[]       = {{0, 1.1f}, {0.5f, 0.7f}, {1, 0.28f}};
        constexpr SCurveKey JUMP_TOES[]       = {{0, 0.6f}, {0.5f, 0.45f}, {1, 0.15f}};
        constexpr SCurveKey JUMP_LEAD_HIP[]   = {{0, 0.75f}, {0.5f, 0.6f}, {1, 0.42f}};
        constexpr SCurveKey JUMP_LEAD_KNEE[]  = {{0, 1.2f}, {0.45f, 0.65f}, {1, 0.22f}};
        constexpr SCurveKey JUMP_LEAD_TOES[]  = {{0, 0.35f}, {1, 0.1f}};
        constexpr SCurveKey JUMP_TRAIL_HIP[]  = {{0, -0.35f}, {0.5f, -0.15f}, {1, 0.f}};
        constexpr SCurveKey JUMP_TRAIL_KNEE[] = {{0, 1.f}, {0.5f, 1.15f}, {1, 0.8f}};
        constexpr SCurveKey JUMP_TRAIL_TOES[] = {{0, 0.9f}, {1, 0.55f}};
        constexpr SCurveKey JUMP_ARM[]        = {{0, 0.8f}, {0.5f, 0.4f}, {1, 0.3f}};   // (both, from standing)
        constexpr SCurveKey JUMP_ARM_OUT[]    = {{0, 0.3f}, {0.5f, 0.55f}, {1, 0.6f}};
        constexpr SCurveKey JUMP_ARM_AHEAD[]  = {{0, 0.6f}, {1, 0.35f}};                 // (running: against the leading leg)
        constexpr SCurveKey JUMP_ARM_BACK[]   = {{0, -0.55f}, {1, -0.2f}};              // (on its side)
        constexpr SCurveKey JUMP_ARM_RUN_OUT[]  = {{0, 0.2f}, {1, 0.45f}};
        constexpr SCurveKey JUMP_ARM_RUN_BEND[] = {{0, 1.25f}, {1, 0.7f}};
        constexpr SCurveKey JUMP_LEAN[]       = {{0, 0.05f}, {0.5f, -0.02f}, {1, 0.08f}}; // (the trunk ahead of the pelvis)
        constexpr SCurveKey JUMP_RUN_LEAN[]   = {{0, 0.18f}, {0.5f, 0.08f}, {1, 0.12f}};
        // Flying: the body lies along the way it goes, the faster the flatter: FLY_LIE at most, FLY_LIE_SPEED m/s taking
        // it most of the way there (the plugin's 8 m/s about 55°, flat out 70°), less climbing, as it goes straight up
        // upright; leaning into speeding up (FLY_PUSH radians an m/s², up to FLY_PUSH_MOST) and back as it slows down (up
        // to FLY_BRAKE_MOST: a flare); banked into turns as a flier is (the pull round it against gravity) up to
        // FLY_BANK; its turns no faster than FLY_TURN (radians a second, speeding up at FLY_TURN_ACCEL)
        constexpr float FLY_LIE = 1.3f, FLY_LIE_SPEED = 5.5f, FLY_PUSH = 0.03f, FLY_PUSH_MOST = 0.35f, FLY_BRAKE_MOST = 0.5f, FLY_BANK = 0.7f;
        constexpr float FLY_TURN = 4.f, FLY_TURN_ACCEL = 25.f;
        // hovering it bobs this far (meters) and sways; flying its legs flutter a little
        constexpr float FLY_BOB = 0.022f;
        // how quickly a jump's pose and a flier's follow what they're after (radians a second), and how springy (1: not at all)
        constexpr float AIR_LEG_W = 11.f, AIR_LEG_Z = 0.7f, FLY_LEG_W = 8.f, FLY_LEG_Z = 0.55f, AIR_ARM_W = 9.f, AIR_ARM_Z = 0.7f;
        constexpr float FLY_BODY_W = 6.5f, FLY_BODY_Z = 0.7f;
        // Where the pose changes all at once (leaving the ground, landing, taking to the air), what was shown goes on as it
        // went and settles into the new pose critically damped at this rate (a second): not snapping to it
        constexpr float SETTLE_AIR = 18.f, SETTLE_LAND = 20.f, SETTLE_FLY = 10.f;
        // landing, the hips go on down at this much of the speed the body stopped at, and come back up as the pose settles
        // (the rest of the knees' give is the walking's own dip)
        constexpr float LAND_GIVE = 0.5f;

        // the player's velocity (now cv) t seconds on, going to what the keys ask for as the plugin moves it
        V3 keysAhead(const V3& cv, const V3& wish, const SAvatarMotion& m, float t) {
            const V3    c{cv.x, 0, cv.z}, d = V3{wish.x, 0, wish.z} - c;
            const float most = (dot(wish, c) < 0 ? m.turnBack : length(wish) < length(c) ? m.decel : m.accel) * t, dl = length(d);
            return c + (dl > most ? d * (most / dl) : d);
        }

        // one step (dt) of what the legs carry (at p, going v, speeding up a) after the player's body (at cp going cv, the
        // keys asking for `wish`: going `lead` a moment on), speeding up no harder than `most`: see BODY_K. `go` of the
        // velocity it's after (turning round: see GO_ON)
        void follow(V3& p, V3& v, V3& a, const V3& cp, const V3& cv, const V3& lead, const V3& wish, bool wall, float most, float dt, float go = 1.f) {
            const V3 off{cp.x - p.x, 0, cp.z - p.z};
            // (further behind than BODY_LAG_SOFT (and a little more, the faster), harder and quicker, up to twice as hard
            // at BODY_LAG)
            const float soft = BODY_LAG_SOFT + 0.06f * length(V3{cv.x, 0, cv.z}), hard = soft + BODY_LAG - BODY_LAG_SOFT;
            const float far  = smoothstep01((length(off) - soft) / (hard - soft));
            most *= 1.f + far;
            // (stopped a little way past it, the keys let go: left there, not shuffled back)
            const bool still = length(wish) < 0.05f && length(V3{cv.x, 0, cv.z}) < 0.05f && length(off) < 2.f * BODY_REST;
            V3         back  = still ? V3{} : off * BODY_CATCH_UP;
            if (const float bl = length(back), cap = 0.03f + (BODY_CATCH_UP_MOST + 0.15f * far) * length(cv); bl > cap)
                back = back * (cap / bl);
            // (toward the velocity wanted at BODY_K of the difference a second, no harder than `most`; the push itself
            // following that, critically damped; the player's a moment on, as the time that takes, so as not to fall
            // behind: turning back, the body starts slowing as the key goes down)
            // (turning round, held back by `go`: less so the further behind, going anyway past BODY_LAG_SOFT and a bit,
            // not dragged along all at once at BODY_LAG)
            V3 want = (V3{lead.x, 0, lead.z} + back) * lerpf(go, 1.f, smoothstep01(far / GO_GIVE));
            V3 push = (want - v) * BODY_K;
            push.y  = 0;
            if (const float pl = length(push); pl > most)
                push = push * (most / pl);
            // (a wall stopped the player's body, or the keys push on into it: slowing onto it in the gap left, as hard
            // as that takes; the keys let go, as legs stop, going on past where the player's stopped but no further
            // than BODY_REST and a tenth of a second at its speed; not turning back, when the body goes on over the
            // foot it stops on)
            const bool pushing = wall || (length(wish) > 0.05f && dot(wish, v) > 0), letGo = length(wish) < 0.05f;
            if (const float sp = length(v); sp > 1e-3f && (pushing || letGo) && dot(cv, v) >= 0) {
                const V3    n    = v / sp;
                const float gain = dot(v - cv, n), gap = std::max(0.f, dot(off, n) + (pushing ? 0.f : BODY_REST + 0.1f * sp));
                if (const float need = gain > 0 ? gain * gain / (2.f * std::max(gap, 0.01f)) : 0.f; need > 0.5f * most)
                    push -= n * (dot(push, n) + std::min(need, BODY_WALL));
            }
            a += (push - a) * (1.f - std::exp(-dt * 4.f * BODY_K));
            v += a * dt;
            v.y = 0;
            p += v * dt;
            // (dragged along at the most behind it; past it, as far as a stop can take it)
            if (const V3 o{cp.x - p.x, 0, cp.z - p.z}; length(o) > (dot(o, v) >= 0 ? hard : hard + 2.f * BODY_REST)) {
                const V3 u = o / length(o);
                p += o * (1.f - hard / length(o));
                if (const float lag = dot(cv - v, u); lag > 0)
                    v += u * lag;
            }
        }

        // toward `to` from x (moving at v, both updated), critically damped at w (radians a second), over dt
        void springTo(float& x, float& v, float to, float w, float dt) {
            const float d = x - to, e = std::exp(-w * dt), k = v + w * d;
            x = to + (d + k * dt) * e;
            v = (v - w * k * dt) * e;
        }

        // a value and how fast it changes, going somewhere as a spring does
        struct SSpring {
            float x = 0, v = 0;
            // toward `to` at w (radians a second), damped z (1 critically, less springy), over dt: in steps short enough
            // to hold at any frame rate
            void to(float to, float w, float z, float dt) {
                const int   n = std::max(1, (int)std::ceil(dt * 240.f));
                const float h = dt / n;
                for (int i = 0; i < n; ++i) {
                    v += (-(x - to) * w * w - 2.f * z * w * v) * h;
                    x += v * h;
                }
            }
        };

        // a turn as a rotation vector (along its axis, as long as its angle in radians, the short way round), and back
        V3 rotationVector(Quat q) {
            if (q.w < 0)
                q = {-q.x, -q.y, -q.z, -q.w};
            const V3    v{q.x, q.y, q.z};
            const float s = length(v);
            return s < 1e-7f ? v * 2.f : v * (2.f * std::atan2(s, q.w) / s);
        }
        Quat fromRotationVector(const V3& r) {
            const float a = length(r);
            return a < 1e-7f ? Quat{r.x * 0.5f, r.y * 0.5f, r.z * 0.5f, 1.f}.normalized() : Quat::axisAngle(r / a, a);
        }

        // the plugin's yaw: 0 faces -Z, turning right is positive; an avatar at that yaw is turned by yawTurn
        Quat yawTurn(float yaw) {
            return Quat::axisAngle(UP, -yaw);
        }
        V3 forwardOf(float yaw) {
            return {std::sin(yaw), 0, -std::cos(yaw)};
        }
        V3 rightOf(float yaw) {
            return {std::cos(yaw), 0, std::sin(yaw)};
        }

        // a foot's pitch (toes down > 0) in avatar space, about its left
        Quat footPitch(float pitch) {
            return Quat::axisAngle({1, 0, 0}, -pitch);
        }

        // the ankle over a foot's spot on the ground (under the ankle while the foot is flat), the foot pitched about
        // its heel (pitch < 0) or its front (> 0), which stay where they are: the ball, with toes of its own that bend,
        // else the tips of the toes (a shoe rolls over its front edge)
        V3 ankleOver(const V3& at, float yaw, float pitch, const SFootShape& fs, float ankleH, bool toes) {
            V3 pivot = pitch < 0 ? fs.heel : toes ? fs.ball : fs.toe;
            pivot.x  = 0;
            return at + yawTurn(yaw).rotate(V3{0, ankleH, 0} + pivot - footPitch(pitch).rotate(pivot));
        }

        // how long each foot is on the ground, of a stride: walking about 60%, less the faster, running 30-40%
        float dutyFactor(float v, float run) {
            return lerpf(std::clamp(0.64f - 0.03f * v, 0.56f, 0.66f), std::clamp(0.5f - 0.035f * v, 0.27f, 0.42f), run);
        }

        struct SGaitFoot {
            bool  swing = false; // in the air
            bool  timed = false; // a step of its own (standing still), not the gait's
            V3    at;            // on the ground under the ankle (were the foot flat): planted there, or where it's got to
            float yaw   = 0;     // which way it points (the plugin's yaw)
            V3    from, to;      // a step's ends, on the ground
            float fromYaw = 0, toYaw = 0;
            float fromOut = 0, toOut = 0; // how far out to its side of the body they are (of the pelvis then), meters
            float fromPitch = 0; // how it was pitched as it lifted
            float s         = 0; // how far through the step, 0..1
            float start     = 0; // the gait's: its phase as it lifted
            float rate      = 0; // a step of its own: s a second
            float phase     = 0; // the gait's: its phase last frame
            float pitch     = 0; // as it's drawn
            bool  locked    = false; // near the end of a step: where it lands stays
            float spin      = 0;     // planted, pivoting on its ball: 1 as fast as it goes
            float strain    = 0;     // planted: how far the ankle is from the hip, of as far as the leg reaches
            float heel      = 0;     // its pitch last frame, as drawn
            float roll = 0, rollFrom = 0; // how fast its pitch changes, and did last frame
            float over      = -1;    // planted: how far past the leg's reach the body has gone from it, meters (< 0 short of it)
            float tight     = 0;     // planted: how far down the pelvis has to come for the leg to reach, of as far as it goes
            float liftKnee  = 0;     // `knee` as it lifted
            float landed    = -1;    // when it came down (the gait's time)
            float fold = 0, foldV = 0; // in the air: how much its knee folds as people's (0 to 1)
            float raise = 0, raiseV = 0, raiseTo = -1; // in the air: how far up that takes the foot (meters), how fast, and
                                                       // where it was headed last frame (-1: it's just lifted)
            float gaitW     = 1;     // in the air: how much it goes as the gait's step (1) or a step of its own (0), eased
                                     // from one to the other when the walking stops or starts again mid-step
            bool  wait      = false; // down from a step of its own while walking: it lifts once its time on the ground comes round
            float toY = 0, toYV = 0; // in the air: the height it goes to, eased (where it lands moving onto the next stair)
            bool  toYSet = false;
            // how it lies along the ground (toes down > 0): a slope's pitch under it (level on a stair's tread); in the air,
            // from where it lifted to where it lands
            float gp = 0, fromGp = 0, toGp = 0;
            float knee      = 0;     // the hip to the ankle, meters, as drawn
            V3    ankle;             // world, as drawn
        };

        // the walking of one model
        struct SGait {
            SRig              rig;
            SBody             body;
            std::vector<int>  own; // per node: its humanoid bone, -1 = none
            std::vector<Quat> scratch;
            M4                hipsToLocal = M4::identity(); // model space -> the hips' parent's
            M4                fixInv      = M4::identity(); // avatar space -> model space
            Quat              toFrame;                      // turns in avatar space -> the frame above (CPoser's)
            std::array<Quat, 2> footUntilt;                 // undoes what straightening the legs does to the feet
            float             unit = 1;                     // meters per unit of the frame above
            float             leg  = 0.8f;                  // the hip joint's height over the soles, meters
            float             hipH = 0.9f;                  // the hips' (pelvis bone's), meters
            std::array<float, 2> thigh{0.4f, 0.4f}, shin{0.4f, 0.4f}, ankleH{0.07f, 0.07f}; // meters
            std::array<float, 2> ahead{};                   // how far forward each ankle stands at rest, meters
            std::array<V3, 2>    hipAt;                     // the hip joints from the pelvis, avatar space
            float             stance = 0.08f;               // half the width between the ankles at rest, meters
            float             armOut = 0.12f;               // the arms out from the body, radians, to clear the hips
            std::array<SFootShape, 2> foot;
            std::array<bool, 2>       toes{};       // the foot has toes of its own
            std::array<float, 2>      toesUp{1, 1}; // how far its toes come up as it lands, of a bare foot's (high heels: less)
            // how far the ankle goes ahead, and up, as the foot pushes off (walking, running), meters
            std::array<float, 2> pushAhead{}, pushUp{};

            bool      live = false, moving = false, wasAir = false;
            float     phase = 0; // of a stride, 0..1: the left foot lands at 0, the right at 0.5
            // where the body's swing (the pelvis, the trunk, the arms) is from the phase: when the phase jumps (starting
            // again, a foot lifting early) the body goes on from where it was and catches up, critically damped
            float     phaseOff = 0, phaseOffV = 0;
            float     run = 0, crouch = 0, moveW = 0, moveWV = 0;
            float     runArms = 0, runArmsV = 0; // `run` for the arms and the trunk: eased in and out (see below)
            float     stride = 0, cadence = 0, duty = 0.6f, swingTime = 0.4f;
            SGaitFoot feet[2];
            float     groundY = 0, groundV = 0; // under the feet (world), springy
            float     groundRate = 0, lastSupport = NAN; // how fast what's under the feet goes up (the last 1/6 s), and it last frame
            float     slope = 0; // how steeply the ground goes up the way it goes (stairs, a slope), from the body's climb
            float     dip = 0, dipV = 0;        // the knees giving as it lands
            float     lowered = 0, loweredV = 0; // how far the pelvis came down for the legs to reach, last frame, and how fast
            float     hurry   = 1;              // how much quicker than the stride's the steps go (a sharp turn)
            float     hurrySmooth = 1;          // (eased)
            float     rollW = 0;                // standing: the pelvis onto a leg (+ right), eased
            float     airVy = 0;                // the fastest it fell, this time in the air
            float     pause = 0;                // standing: till the next step may start
            float     time  = 0;
            V3        lastP, lastVel, accel; // (lastP the player's body)
            V3        bodyP, bodyV, bodyA;   // what the legs carry (horizontal), following the player's body
            V3        lastVelC;              // the player's body's velocity last frame
            float     wall = 0;              // how long ago something stopped the player's body, counting down
            float     lastYaw = 0, yawRate = 0;
            float     velTurn = 0; // how fast the way it goes turns, radians a second
            float     wishTurn = 0; // how fast the way the keys ask for turns (the camera turning), radians a second
            V3        lastWish;
            float     turnLeft = 0; // how far the body has to turn yet (turnBody's), eased
            float     forward = 1; // how much of where it goes is ahead of it, -1 backward
            float     hipLag  = 0, hipLagV = 0; // the pelvis's yaw from the body's (the plugin's yaw): behind it, turning;
                                                // and how fast the pelvis turns (in the world)
            float     twist = 0, twistV = 0;   // the trunk turned from the pelvis back toward the body's yaw, eased
            std::array<float, 2> armClear{}; // how much further out each arm is held to keep clear of a skirt, radians
            std::array<float, 2> armPush{}, armPushV{}; // (as it's swung now, eased)
            std::array<float, 2> clipClear{}, clipClearV{}, clipPush{}, clipPushV{}; // (the same for the walk's and run's clips' arms)
            std::array<std::array<float, GAIT_NEED_STEPS>, 2> clipNeed{}; // (how far out each arm needs, at steps of the stride)
            unsigned             clipTick = 0;
            V3        pelvis; // world, as drawn
            // off the ground (see JUMP_*, FLY_*): where the pose has got to, going toward what it's after as springs
            struct SAirLeg {
                SSpring hip, knee, out, toes; // the thigh ahead of straight down, the knee's bend, the leg out to its side, the toes down
            };
            struct SAirArm {
                SSpring ahead, out, bend; // as the walking's (armAt): swung ahead, out from the body, the elbow's bend
            };
            std::array<SAirLeg, 2> airLeg{};
            std::array<SAirArm, 2> airArm{};
            SSpring   airPitch, airRoll, airYaw, airLean; // the pelvis pitched ahead, rolled (right > 0), turned; the trunk ahead of it
            SSpring   flyE;                   // flying, eased (0..1)
            int       airLead = -1;           // the leg that went on ahead as it left the ground, -1 neither (from standing)
            float     airRun  = 0;            // how fast it went as it left the ground: 0 standing, 1 running
            bool      grounded = true;        // last frame
            std::array<V3, 2> armWas{};       // the walking's arms last frame (ahead, out, bend): the air's go on from them
            float     leanWas = 0;            // the walking's trunk lean last frame
        };

        SGait& gaitOf(const std::shared_ptr<void>& p) {
            return *static_cast<SGait*>(p.get());
        }

        // what a model's walking needs to know about it, measured at rest
        std::shared_ptr<void> makeGait(const SAvatarModel& md) {
            auto  gp = std::make_shared<SGait>();
            auto& g  = *gp;
            const auto& h = md.human;
            g.rig  = rigOf(md);
            g.body = bodyOf(g.rig);
            g.own.assign(md.nodes.size(), -1);
            for (int b = 0; b < HB_COUNT; ++b)
                if (h[b] >= 0)
                    g.own[h[b]] = b;
            if (const int hp = md.nodes[h[HB_HIPS]].parent; hp >= 0)
                g.hipsToLocal = g.rig.restGlobal[hp].inverse();
            g.fixInv  = md.fix.inverse();
            g.toFrame = (g.rig.facing.conj() * rotationOf(md.fix).conj()).normalized();
            g.unit    = g.rig.height * md.scale;
            auto at   = [&](int b) { return md.fix.point(origin(g.rig.restGlobal[h[b]])); };
            const V3 hips = at(HB_HIPS);
            float    sole = 0;
            g.stance      = 0;
            for (int s = 0; s < 2; ++s) {
                const int ul  = s ? HB_R_UPPER_LEG : HB_L_UPPER_LEG;
                const V3  ank = at(h[ul + 2] >= 0 ? ul + 2 : ul + 1);
                g.foot[s]     = md.feet[s];
                g.toes[s]     = h[s ? HB_R_TOES : HB_L_TOES] >= 0;
                g.ankleH[s]   = std::max(0.01f, -(g.foot[s].heel.y + g.foot[s].ball.y) * 0.5f);
                sole += (ank.y - g.ankleH[s]) * 0.5f;
                g.thigh[s] = g.body.thigh[s] * g.unit;
                g.shin[s]  = g.body.shin[s] * g.unit;
                g.hipAt[s] = at(ul) - hips;
                g.ahead[s] = -ank.z;
                g.stance += std::abs(ank.x) * 0.5f;
                // the feet as the legs, straightened, have them: undone (they stay as they stand)
                g.footUntilt[s] = (g.rig.facing.conj() * g.rig.tpose[ul + 2].conj() * g.rig.facing).normalized();
                // Landing on the heel, toes up, turns the ankle back about the heel, the more the higher the ankle is over
                // it: over a high heel, as far again as over a bare foot's, and the leg reaching that much further for
                // it takes the pelvis down. People in high heels land flatter: here the ankle goes back no further than
                // over a bare foot's heel (7 cm under it and 6 behind, 1.6 cm)
                const float land = -through(WALK_STANCE_PITCH, 0);
                const float back = g.ankleH[s] * std::sin(land) - std::max(0.f, g.foot[s].heel.z) * (1.f - std::cos(land));
                g.toesUp[s]      = std::clamp(0.016f / std::max(back, 1e-3f), 0.25f, 1.f);
            }
            g.stance = std::max(g.stance, 0.02f);
            g.hipH   = hips.y - sole;
            g.leg    = std::max(0.2f, 0.5f * (at(HB_L_UPPER_LEG).y + at(HB_R_UPPER_LEG).y) - sole);
            // the hands hang clear of the hips: out as far as the hip joints are, and a bit (a skirt, a coat)
            const float shoulders = 0.5f * std::abs(at(HB_L_UPPER_ARM).x - at(HB_R_UPPER_ARM).x);
            const float hipsHalf  = 0.5f * std::abs(g.hipAt[0].x - g.hipAt[1].x);
            const float arm       = 0.5f * (g.body.upper[0] + g.body.fore[0] + g.body.upper[1] + g.body.fore[1]) * g.unit;
            g.armOut = std::clamp(std::asin(std::clamp((hipsHalf + 0.09f * g.leg - shoulders) / std::max(arm, 0.1f), 0.f, 0.6f)), 0.1f, 0.45f);
            return gp;
        }

        // a pose in the frame above (CPoser's) onto the model's nodes: the bones it sets turned, the hips moved; the
        // others keep their own turn, and go along with the bone above
        void applyFrame(SGait& g, const std::array<Quat, HB_COUNT>& turn, const std::array<bool, HB_COUNT>& set, const V3& move, std::vector<STRS>& pose) {
            const auto& md = *g.rig.md;
            const Quat  R = g.rig.facing, Rc = R.conj();
            auto&       E = g.scratch;
            E.resize(md.nodes.size());
            for (size_t n = 0; n < md.nodes.size(); ++n) {
                const int p = md.nodes[n].parent, b = g.own[n];
                E[n]        = b >= 0 && set[b] ? (R * turn[b] * Rc * g.rig.tpose[b]).normalized() : p >= 0 ? E[p] : Quat{};
            }
            for (int b = 0; b < HB_COUNT; ++b) {
                const int n = md.human[b];
                if (n < 0 || !set[b])
                    continue;
                const int  p  = md.nodes[n].parent;
                const Quat gp = p >= 0 ? g.rig.restRot[p] : Quat{}, ep = p >= 0 ? E[p] : Quat{};
                pose[n].r     = (gp.conj() * ep.conj() * E[n] * gp * md.nodes[n].rest.r).normalized();
            }
            pose[md.human[HB_HIPS]].t = g.hipsToLocal.point(g.rig.hips + R.rotate(move) * g.rig.height);
        }
    }

    float CAvatarAnimator::turnBody(float yaw, float want, float dt, float speed) {
        float err = wrapPi(want - yaw);
        // right round (going back the way it came): toward the side of the foot behind, which swings round while the
        // one ahead pivots (either way round, the other foot would have to step across it)
        if (std::abs(err) < 2.3f)
            m_turnSide = 0;
        else {
            if (!m_turnSide && m_gait && m_gaitUsed)
                if (const SGait& g = gaitOf(m_gait); g.moving && !g.wasAir) {
                    const V3    F = forwardOf(yaw);
                    auto        along = [&](const SGaitFoot& ft) { return dot(ft.swing ? ft.to : ft.at, F); };
                    const float d     = along(g.feet[0]) - along(g.feet[1]); // (the left ahead of the right)
                    if (std::abs(d) > 0.05f)
                        m_turnSide = d > 0 ? 1 : -1;
                }
            // (from standing, or with the feet level: the way it's turning already, or the short way; kept till it's round)
            if (!m_turnSide)
                m_turnSide = std::abs(m_turnRate) > 0.5f ? (m_turnRate > 0 ? 1 : -1) : err > 0 ? 1 : -1;
            if (m_turnSide && (err > 0) != (m_turnSide > 0))
                err += m_turnSide > 0 ? TAU : -TAU;
        }
        // as fast as closes the gap in about a ninth of a second, no faster than about half a turn in two steps (radians a
        // second), nor than it can still slow down in; getting to that speed and back no quicker than TURN_ACCEL (at once,
        // the whole body would jerk round as each key goes down)
        if (std::abs(wrapPi(yaw - m_turnYaw)) > 1e-3f) // (the yaw was set: from still)
            m_turnRate = 0;
        // (flying, slower: FLY_TURN, as a flier banks round)
        const float accel = m_flying ? FLY_TURN_ACCEL : TURN_ACCEL, most = m_flying ? FLY_TURN : speed > 3.f ? 5.f : 7.f, need = std::abs(err);
        m_turnRate = approach(m_turnRate, std::copysign(std::min({need * (m_flying ? 4.f : 9.f), most, std::sqrt(2.f * accel * need)}), err), accel * dt);
        m_turnYaw        = wrapPi(yaw + m_turnRate * dt);
        m_turnLeft       = err - m_turnRate * dt;
        m_turnKnown      = true;
        return m_turnYaw;
    }

    std::optional<float> CAvatarAnimator::wayToFace(const SAvatarMotion& m, bool thirdPerson, float bodyYaw, float keysYaw) {
        // (going back and forth: another turn back within ALT_BACK of one; held one way ALT_HOLD, it turns to it;
        // running, it doesn't hold (it would run sideways); where it will be going WAY_AHEAD seconds on)
        // (holding, it faces the way it has been going the last WAY_MEAN seconds, when it goes somewhere as it taps: the
        // keys back the other way only slow it down)
        constexpr float ALT_BACK = 0.5f, ALT_HOLD = 0.4f, WAY_AHEAD = 0.25f, WAY_MEAN = 0.4f;
        m_wayClock += m.dt;
        m_wayMean += (V3{m.vel.x, 0, m.vel.z} - m_wayMean) * (1.f - std::exp(-m.dt / WAY_MEAN));
        const V3 w{m.wish.x, 0, m.wish.z};
        if (!thirdPerson || length(w) < 0.05f)
            return std::nullopt;
        const V3 dir = normalize(w);
        if (length(m_wayLast) > 0.5f) {
            if (dot(dir, m_wayLast) < -0.5f) {
                if (m_wayClock - m_wayBack < ALT_BACK && !m_wayHold && !m.run)
                    m_wayHold = true, m_wayYaw = std::abs(wrapPi(bodyYaw - keysYaw)) > 2.1f ? wrapPi(keysYaw + PI) : keysYaw;
                m_wayBack = m_wayClock;
            }
            if (dot(dir, m_wayLast) < 0.99f)
                m_waySince = m_wayClock;
        } else
            m_waySince = m_wayClock;
        m_wayLast = dir;
        if (m_wayHold && m_wayClock - m_waySince > ALT_HOLD)
            m_wayHold = false;
        if (m_wayHold && length(m_wayMean) > WAY_GOING)
            m_wayYaw = std::atan2(m_wayMean.x, -m_wayMean.z);
        if (m_wayHold && !m.run)
            return m_wayYaw;
        m_wayHold = false;
        // (running, hardly ahead: the body turns as the way it goes does)
        const V3 to = keysAhead(m.vel, w, m, WAY_AHEAD * std::clamp((3.f - length(V3{m.vel.x, 0, m.vel.z})) / 1.5f, 0.2f, 1.f));
        return length(to) > 0.05f ? std::atan2(to.x, -to.z) : std::atan2(dir.x, -dir.z);
    }

    void CAvatarAnimator::gait(const SAvatarMotion& m, bool air, std::vector<STRS>& pose) {
        const auto& md = *m_model;
        if (!m_gait)
            m_gait = makeGait(md);
        SGait&      g   = gaitOf(m_gait);
        const float dt  = std::clamp(m.dt, 0.f, 0.05f);
        const V3    Pc  = origin(m.world); // (the player's body, as it moves)
        const V3    Fw  = m.world.dir({0, 0, -1});
        const float yaw = std::atan2(Fw.x, -Fw.z);
        const V3    F = forwardOf(yaw), Rt = rightOf(yaw);
        V3          velC{m.vel.x, 0, m.vel.z};
        if (length(velC) < 1e-4f && m.speed > 1e-3f)
            velC = F * m.speed;
        const bool grounded = !air && !m.flying;
        g.time += dt;
        // where the body will face t seconds on: turning on as turnBody turns it (at most 7 radians a second, speeding up
        // at TURN_ACCEL, no further than it has to; what's left of the turn eased, as when a key goes down it jumps, and
        // where a foot lands would with it); else as fast as it turns now, a moment on
        const bool turnKnown = m_turnKnown;
        m_turnKnown          = false;
        g.turnLeft           = turnKnown ? approach(g.turnLeft, m_turnLeft, TURN_PLAN * dt) : 0.f;
        auto       yawIn     = [&](float t) {
            if (!turnKnown)
                return yaw + std::clamp(g.yawRate * std::min(t, 0.15f), -0.7f, 0.7f);
            const float sgn = g.turnLeft >= 0 ? 1.f : -1.f, r = std::max(0.f, m_turnRate * sgn);
            const float t1 = std::clamp((7.f - r) / TURN_ACCEL, 0.f, t);
            return yaw + sgn * std::min(std::abs(g.turnLeft), r * t1 + 0.5f * TURN_ACCEL * t1 * t1 + 7.f * (t - t1));
        };

        // the first time, back from a clip, or the body jumped (a respawn, a teleport: a move its velocity doesn't
        // account for; a stair's step up is half a metre at most): standing where it is
        const V3   moved = Pc - g.lastP, unexplained = moved - V3{velC.x, m.vy, velC.z} * dt;
        const bool fresh = !g.live || !m_gaitUsed || length(V3{unexplained.x, 0, unexplained.z}) > 0.5f || std::abs(unexplained.y) > 1.f;
        // (leaving the ground, landing, taking to the air from it: the pose changes all at once there, and what was shown
        // settles into the new one; see SETTLE_AIR)
        if (!fresh && grounded != g.grounded)
            m_settleAsk = std::max(m_settleAsk, grounded ? SETTLE_LAND : m.flying ? SETTLE_FLY : SETTLE_AIR);
        const bool leaving = !grounded && (g.grounded || fresh); // (the air's pose starts from how it was)
        g.grounded         = grounded;
        // What the legs carry: the body going as a person's does, no quicker to start, stop or go back the other way
        // than legs push it (the player's body starts at 10 m/s² and stops at 14: a person's feet couldn't keep under
        // that). It follows the player's (see BODY_K): at a steady speed with it, behind it as it speeds up, slows down or
        // turns back, never past it into a wall; with it in the air
        const float bodyMost = lerpf(BODY_WALK_ACCEL, BODY_RUN_ACCEL, m.run ? 1.f : g.run); // (Shift down: as it sets off)
        // (the player's body stopped quicker than the plugin slows it: it hit something)
        g.wall     = length(velC - g.lastVelC) > 1.5f * std::max({m.accel, m.decel, m.turnBack}) * dt + 0.05f ? 0.25f : std::max(0.f, g.wall - dt);
        g.lastVelC = velC;
        // The pelvis's yaw (last frame's), and where it will face t seconds on, standing on foot `stand`: toward where
        // the body will (yawIn), no further than the hip turns from that foot, which pivots on its ball toward it
        // meanwhile (once it's a little off, no faster than PIVOT_RATE). Where the legs go is worked out from the pelvis:
        // the body (and the head and the trunk after it) turns first, the legs step round after it
        // (from the body's yaw now, as it was: the body has turned since; not wrapped, turning right round it may lag
        // it by near half a turn)
        const float lagNow = fresh ? 0.f : g.hipLag - wrapPi(yaw - g.lastYaw), hy = yaw + lagNow;
        // (and as far as it can turn by then: from as fast as it turns now, speeding up at HIP_ACCEL to HIP_RATE)
        auto        hipIn  = [&](int stand, float t) {
            const float want = yawIn(t) - hy; // (from the pelvis now)
            const auto& o    = g.feet[stand];
            float       f    = wrapPi((o.swing ? o.toYaw : o.yaw) - hy);
            if (const float err = want - f; !o.swing && std::abs(err) > 0.3f)
                f += std::copysign(std::min(std::abs(err) - 0.3f, PIVOT_RATE * t), err);
            const float to = f + std::clamp(want - f, -HIP_TURN, HIP_TURN), r = std::clamp(g.hipLagV * (to >= 0 ? 1.f : -1.f), 0.f, HIP_RATE);
            const float t1 = std::min((HIP_RATE - r) / HIP_ACCEL, t);
            return hy + std::copysign(std::min(std::abs(to), r * t1 + 0.5f * HIP_ACCEL * t1 * t1 + HIP_RATE * (t - t1)), to);
        };
        // (how far out to foot s's side of the pelvis p is, meters)
        auto outOf = [&](int s, const V3& p) { return dot(V3{p.x - g.pelvis.x, 0, p.z - g.pelvis.z}, rightOf(hy)) * (s ? 1.f : -1.f); };
        // (how much of the velocity it's after the legs go for, the pelvis facing hipYaw while the body turns to bodyYaw:
        // see GO_ON)
        auto goFor = [&](const V3& to, float hipYaw, float bodyYaw) {
            if (length(V3{to.x, 0, to.z}) < 0.05f)
                return 1.f;
            const float wy = std::atan2(to.x, -to.z), off = std::abs(wrapPi(wy - hipYaw));
            const float turning = smoothstep01((off - std::abs(wrapPi(wy - bodyYaw)) - 0.3f) / 0.6f);
            const float along   = smoothstep01((std::cos(off) - std::cos(GO_OFF)) / (std::cos(GO_ON) - std::cos(GO_OFF)));
            // (running, a turn goes round in a curve at speed: held back less, RUN_GO)
            return lerpf(1.f, lerpf(lerpf(GO_LEAST, 1.f, RUN_GO * (m.run ? 1.f : g.run)), 1.f, along), turning);
        };
        if (fresh || !grounded)
            g.bodyP = Pc, g.bodyV = velC, g.bodyA = {};
        else {
            const V3 lead = keysAhead(velC, m.wish, m, 1.f / BODY_K);
            follow(g.bodyP, g.bodyV, g.bodyA, Pc, velC, lead, m.wish, g.wall > 0, bodyMost, dt, goFor(lead, hy, yawIn(0.15f)));
        }
        const V3    P{g.bodyP.x, Pc.y, g.bodyP.z};
        const V3    vel = g.bodyV;
        const float v = length(vel), L = g.leg;
        // how steeply the ground goes up the way it goes, from how fast the body climbs (up or down stairs, a slope)
        if (fresh || !grounded)
            g.slope = 0;
        else if (v > 0.3f && dt > 1e-4f)
            g.slope += (std::clamp((Pc.y - g.lastP.y) / dt / v, -1.f, 1.f) - g.slope) * (1.f - std::exp(-dt * 8.f));
        else
            g.slope *= std::exp(-dt * 2.f);
        const V3 way = v > 0.3f ? vel / v : F;
        // where a foot can stand: the ground there, looked for near the body's feet (as far up or down as that slope
        // goes: running down stairs a foot lands most of a metre below)
        auto groundAt = [&](V3 p) {
            const float near = P.y + g.slope * dot(V3{p.x - P.x, 0, p.z - P.z}, way);
            p.y              = near;
            if (m.ground)
                if (const auto y = m.ground(p.x, p.z, near); y && std::isfinite(*y) && std::abs(*y - near) < 0.8f)
                    p.y = *y;
            return p;
        };
        // the ground under a foot pointing that way there (heel, ankle, middle, toes): on one line (a slope no steeper
        // than SLOPE_MOST, or level), its pitch (toes down > 0), and the height it stands at: a slope's under the ankle,
        // lying along it; else the highest (across a stair's edge, on the edge, not its toes inside the next step up)
        struct SUnder {
            float y = 0, lo = 0, hi = 0, pitch = 0;
            bool  even = true; // (with room round it)
            int   fit  = 3;    // how well it stands there: 3 even with room round it, 2 even, 1 all but the heel on one
                               // tread (hanging over its edge, a long foot up a stair), 0 across an edge
        };
        auto under = [&](V3 p, float fyaw, int s) {
            // (one line a little past its heel and toes too, STAIR_ROOM: a foot coming down heel first onto a stair
            // clears the edge behind it)
            const V3    f    = forwardOf(fyaw);
            const float back = std::max(g.foot[s].heel.z, 0.f), toe = std::max(-g.foot[s].toe.z, 0.01f);
            const float yb = groundAt(p - f * (back + STAIR_ROOM)).y, yh = groundAt(p - f * back).y, ya = groundAt(p).y;
            const float ym = groundAt(p + f * (0.5f * toe)).y, yt = groundAt(p + f * toe).y, yf = groundAt(p + f * (toe + 0.5f * STAIR_ROOM)).y;
            const float k = (yt - yh) / (back + toe); // (up along it, a meter)
            auto        on = [&](float y, float d) { return std::abs(y - (ya + k * d)) < SLOPE_EVEN; };
            SUnder      u;
            const bool  line = on(yh, -back) && on(ym, 0.5f * toe) && on(yt, toe) && std::abs(k) < std::tan(SLOPE_MOST);
            u.lo    = std::min({yh, ya, ym, yt}), u.hi = std::max({yh, ya, ym, yt});
            u.even  = line && on(yb, -back - STAIR_ROOM) && on(yf, toe + 0.5f * STAIR_ROOM);
            u.fit   = u.even ? 3 : line ? 2 : std::max({ya, ym, yt}) - std::min({ya, ym, yt}) < SLOPE_EVEN && yh < ya ? 1 : 0;
            u.pitch = line ? -std::atan(k) : 0.f;
            u.y     = line ? ya : u.hi;
            return u;
        };
        auto footY = [&](V3 p, float fyaw, int s) { return under(p, fyaw, s).y; };
        // where a foot put down there stands (stairs, a kerb): the whole of it on one tread (or one slope), moved along
        // itself as little as that takes (up to STAIR_SHIFT), else on the edge; on rough ground (bumps, no step) where it is
        auto footing = [&](V3 p, float fyaw, int s) {
            const V3 f = forwardOf(fyaw);
            const auto u0 = under(p, fyaw, s);
            if (u0.even || u0.hi - u0.lo < 0.06f)
                return V3{p.x, u0.y, p.z};
            // (the nearest that fits best: with room round it, else even, else a long foot's heel over an edge)
            V3  best{p.x, u0.y, p.z};
            int fit = u0.fit;
            for (float d = 0.02f; d <= STAIR_SHIFT + 1e-4f && fit < 3; d += 0.02f)
                for (const float sg : {1.f, -1.f})
                    if (const V3 q = p + f * (sg * d); fit < 3)
                        if (const auto u = under(q, fyaw, s); u.fit > fit)
                            best = {q.x, u.y, q.z}, fit = u.fit;
            return best;
        };
        // the ankle over a foot's spot as ankleOver puts it on level ground, the whole foot turned to lie along the
        // ground there (gp)
        auto ankleAt = [&](const V3& at, float fyaw, float pitch, float gp, int s) {
            return at + yawTurn(fyaw).rotate(footPitch(gp).rotate(ankleOver({}, 0, pitch, g.foot[s], g.ankleH[s], g.toes[s])));
        };
        // where each foot stands at rest, the body as it is now
        const float wide = g.stance * (1.f + 0.3f * g.crouch);
        auto        spot = [&](int s) { return footing(P + Rt * (s ? wide : -wide) + F * g.ahead[s], yaw, s); };

        if (fresh) {
            for (int s = 0; s < 2; ++s) {
                auto& ft = g.feet[s];
                ft       = {};
                ft.at    = spot(s);
                ft.yaw   = yaw;
                ft.ankle = ankleOver(ft.at, yaw, 0, g.foot[s], g.ankleH[s], g.toes[s]);
            }
            g.live = true;
            g.moving = g.wasAir = false;
            g.phase = g.phaseOff = g.phaseOffV = g.dip = g.dipV = g.groundV = g.yawRate = g.hipLag = g.hipLagV = g.lowered = g.loweredV = g.rollW = 0;
            g.groundY = P.y, g.groundRate = 0, g.lastSupport = NAN;
            g.lastVel = vel, g.accel = {}, g.lastYaw = yaw;
        }
        g.yawRate += (wrapPi(yaw - g.lastYaw) / std::max(dt, 1e-4f) - g.yawRate) * (1.f - std::exp(-dt * 12.f));
        if (v > 0.3f && length(g.lastVel) > 0.3f && dt > 1e-4f) {
            const float turned = wrapPi(std::atan2(vel.x, -vel.z) - std::atan2(g.lastVel.x, -g.lastVel.z));
            g.velTurn += (std::clamp(turned / dt, -6.f, 6.f) - g.velTurn) * (1.f - std::exp(-dt * 12.f));
        } else
            g.velTurn = 0;
        // (a key going down or up turns it at once: that's no curve)
        if (length(m.wish) > 0.3f && length(g.lastWish) > 0.3f && dt > 1e-4f) {
            const float turned = wrapPi(std::atan2(m.wish.x, -m.wish.z) - std::atan2(g.lastWish.x, -g.lastWish.z));
            g.wishTurn += ((std::abs(turned) < 0.3f ? std::clamp(turned / dt, -6.f, 6.f) : 0.f) - g.wishTurn) * (1.f - std::exp(-dt * 12.f));
        } else
            g.wishTurn = 0;
        g.lastWish = m.wish;
        if (dt > 1e-4f)
            g.accel += ((vel - g.lastVel) / dt - g.accel) * (1.f - std::exp(-dt * 6.f));
        g.lastP = Pc, g.lastVel = vel, g.lastYaw = yaw;

        // walk or run: as the player asks, or when a walk can't go that fast (Froude number over 0.8)
        const float froude = v * v / (GRAVITY * L);
        g.run    = approach(g.run, (m.run && v > 1.f) || froude > 0.8f ? 1.f : 0.f, dt * 4.f);
        g.crouch = approach(g.crouch, m.crouched && !m.flying ? 1.f : 0.f, dt * 5.f);

        // how high the pelvis (the hips' bone) goes, over the ground: walking over the standing leg it rises by aWalk
        // over its mean and sinks between steps (the legs' reach can take it lower there, below; the whole of it
        // 4-5 cm at a brisk walk, as people's: over the standing leg the knee stays a little bent); running it sinks
        // by aRun as the leg takes the weight and rises as much in the air, its mean as the foot lands and leaves
        const float bob = L * (0.01f + 0.008f * std::min(v, 2.5f)); // (as far as the legs are made to reach, below)
        const float hWalk = g.hipH - L * 0.0275f, aWalk = 0.7f * bob;
        const float hRun = g.hipH - L * 0.065f, aRun = L * (0.03f + 0.005f * std::min(v, 7.f));
        const float hCrouch = 0.36f * g.hipH * g.crouch;

        // stride, cadence and duty factor for the speed. The stance (the ground the body covers over a foot) no
        // longer than the legs span: from as far ahead as a leg reaches landing on its heel to as far behind as it
        // reaches pushing off, and what rolling the foot carries the ankle along. Walking, a longer one takes shorter
        // quicker steps (short legs and high heels do); running, the foot's time on the ground gets shorter, as it
        // does going faster. The foot lands where that leaves as much of each reach to spare. (The legs reach from a
        // pelvis no more than 3% of the leg below where the stride takes it between steps: further, and each step
        // would drop the body twice as far as people's does)
        // (going sideways or backwards, as in first person, the steps are shorter, the feet rolled less) (up or down
        // stairs or a slope, the reaches are along the ground: up, further ahead than behind, down the other way round;
        // down a flight a foot lands nearly under the body, as people's does, else the legs can't reach it)
        const float ahead_ = v > 0.05f ? dot(vel / v, forwardOf(hy)) : 1.f; // how much of it is forward (of the pelvis), -1 backward
        g.forward          = approach(g.forward, ahead_, dt * 4.f);
        const float frontal = std::max(0.f, g.forward);
        float beta   = dutyFactor(v, g.run);
        float stride = 2.5f * L * std::pow(std::max(froude, 1e-6f), 0.3f) * (1.f - 0.2f * g.crouch) * (0.55f + 0.45f * frontal);
        float ahead = 0;
        {
            const float pelvis = lerpf(g.hipH - L * 0.05f - bob, hRun, g.run) - hCrouch;
            const float landP  = lerpf(through(WALK_STANCE_PITCH, 0), through(RUN_STANCE_PITCH, 0), g.run);
            const float pushP  = lerpf(through(WALK_STANCE_PITCH, 1), through(RUN_STANCE_PITCH, 1), g.run);
            float       fwd = 0, back = 0, roll = 0;
            const float sl = std::clamp(g.slope, -1.f, 1.f);
            // (how far along the ground a leg reaches from a hip h over it: x² + (h - sl·x)² = reach²)
            auto along = [&](float h, float reach, float sl) {
                const float a = 1.f + sl * sl, b = sl * h, c = h * h - reach * reach;
                return std::max(0.f, (b + std::sqrt(std::max(0.f, b * b - a * c))) / a);
            };
            for (int s = 0; s < 2; ++s) {
                const float hip   = pelvis + g.hipAt[s].y;
                const float reach = 0.995f * (g.thigh[s] + g.shin[s]);
                const V3    land  = ankleOver({}, 0, landP * g.toesUp[s], g.foot[s], g.ankleH[s], g.toes[s]);
                const V3    push  = ankleOver({}, 0, pushP, g.foot[s], g.ankleH[s], g.toes[s]);
                fwd += 0.5f * (along(hip - land.y, reach, sl) + land.z);
                back += 0.5f * along(hip - push.y, reach, -sl);
                roll += 0.5f * -push.z;
            }
            const float span = 0.95f * (fwd + back + roll);
            stride           = lerpf(std::min(stride, span / beta), stride, g.run);
            beta             = lerpf(beta, std::clamp(span / std::max(stride, 1e-3f), 0.2f, beta), g.run);
            ahead            = fwd - (fwd + back + roll - beta * stride) * fwd / std::max(fwd + back, 1e-3f);
        }
        // (how far the pelvis has yet to turn to where the body goes, the keys down: 0 to 1 turning far)
        const float turnFar = length(m.wish) > 0.1f ? smoothstep01((std::abs(wrapPi(yawIn(0.15f) - hy)) - 0.6f) / 0.8f) : 0.f;
        // (strides a second; slowing through a turn back the other way, or turning far, the steps don't slow with it:
        // only as the walking winds down; else they ease down to the pace, not at once; turning on the spot, as a quick
        // step's)
        float cadence = std::max(v / std::max(stride, 1e-3f), lerpf(lerpf(0.8f, 1.3f, g.run), TURN_CADENCE, turnFar) * std::sqrt(0.9f / L));
        if (g.moving)
            cadence = std::max(cadence, g.cadence - dt * (dot(m.wish, vel) < 0 || std::abs(g.turnLeft) > 1.f || std::abs(g.hipLagV) > CADENCE_TURN ? CADENCE_DROP : 3.f));
        stride              = v / cadence;
        ahead               = lerpf(0.5f * beta * stride, std::clamp(ahead, 0.f, 0.5f * beta * stride), frontal);
        g.stride = stride, g.cadence = cadence, g.duty = beta, g.swingTime = (1.f - beta) / cadence;

        // (stepping sideways the legs don't cross: a foot landing `left` seconds on lands on its own side of the other
        // one, across the way the pelvis will face as it stands on it (turning right round, the feet turn after it: across
        // their own way they would cross); nor further apart across than a step to the side goes: turning sharply, the
        // way it goes swings round faster than the body does, and a foot put down ahead on that way would land far out
        // to its side)
        auto        uncross = [&](int s, V3 to, float left) {
            const auto& other  = g.feet[1 - s];
            const V3    beside = other.swing ? other.to : other.at;
            const V3    rt     = rightOf(hipIn(1 - s, left + 0.1f));
            const float sd     = s ? -1.f : 1.f, side = lerpf(wide * 0.9f, wide * 0.35f, g.run);
            const float apart  = dot(beside - to, rt) * sd, least = std::max(0.3f * side, 0.06f * L), most = 2.f * wide + 0.25f * L;
            if (apart < least)
                to -= rt * (sd * (least - apart));
            else if (apart > most)
                to += rt * (sd * (apart - most));
            return to;
        };
        // where a foot lifting now (or on its way) lands: `ahead` of where the body will be then, on its own side of
        // the way. `left` = what's left of the swing, in seconds. Also how far it lands out to its side of the body (of
        // the pelvis as it will face), meters
        auto        landing = [&](int s, float left) {
            // (where the body will be, and how it goes then: following the player's, going on toward what the keys ask
            // for as the plugin moves it, turning on as they do (the camera turning); the step as long as for the speed
            // then, where it's slowing down or turning back, or waiting for the pelvis to turn)
            V3          at = P, bv = vel, ba = g.bodyA, cp = Pc, cv = velC;
            const float w  = std::abs(g.wishTurn) > 0.05f ? g.wishTurn : 0.f;
            for (int i = 0; i < 8; ++i) {
                const float h = left / 8.f, turn = std::clamp(w * h * (i + 0.5f), -1.2f, 1.2f);
                const V3    to{m.wish.x * std::cos(turn) - m.wish.z * std::sin(turn), 0, m.wish.x * std::sin(turn) + m.wish.z * std::cos(turn)};
                cv = keysAhead(cv, to, m, h);
                cp += cv * h;
                const V3 lead = keysAhead(cv, to, m, 1.f / BODY_K);
                follow(at, bv, ba, cp, cv, lead, to, g.wall > 0, bodyMost, h, goFor(lead, hipIn(1 - s, h * (i + 1)), yawIn(h * (i + 1) + 0.15f)));
            }
            at.y = P.y;
            const float bs = length(bv), stepAhead = ahead * std::clamp(bs / std::max(v, 0.3f), 0.f, 1.3f);
            const V3    fwd = bs > 0.05f ? bv / bs : v > 0.05f ? vel / v : forwardOf(yaw);
            // (turned toward where the body will face no further than the hip turns from the pelvis as it lands, which
            // is no further from the other foot: a sharp turn takes steps; and on its own side of the pelvis then: under
            // its hip)
            const float ply  = hipIn(1 - s, left);
            const float ly   = ply + std::clamp(yawIn(left) - ply, -HIP_TURN, HIP_TURN);
            const V3    rt   = rightOf(ply + (ly - ply) * (1.f - smoothstep01(std::abs(g.slope) / 0.2f)));
            const float side = lerpf(wide * 0.9f, wide * 0.35f, g.run) * (s ? 1.f : -1.f);
            // (a foot that will pivot on its ball as it stands, the body turning on past it, put down as far the other
            // way as its heel swings: the ankle ends under the hip, not in under the body; on even ground, where it
            // stands turned that far too: on a stair its heel would swing into the next step)
            const float rem = yawIn(left + g.duty / std::max(cadence, 0.1f)) - ly;
            const float piv = std::abs(rem) > 0.3f ? std::copysign(std::min(std::abs(rem) - 0.3f, PIVOT_RATE * g.duty / std::max(cadence, 0.1f)), rem) : 0.f;
            const V3    ball{0, 0, g.foot[s].ball.z}, place = uncross(s, at + fwd * stepAhead + rt * side, left);
            const V3    swung = place + yawTurn(ly + piv).rotate(ball) - yawTurn(ly).rotate(ball);
            const V3    to    = footing(std::abs(piv) > 0.05f && under(swung, ly, s).even && under(swung, ly + piv, s).even ? swung : place, ly, s);
            return std::tuple{to, ly, dot(V3{to.x - at.x, 0, to.z - at.z}, rt) * (s ? 1.f : -1.f)};
        };

        if (!grounded) {
            // off the ground: nothing planted, the fastest fall kept for the landing. Leaving it: the leg that goes on
            // ahead (the one stepping, the furthest through its step; standing, or walking with both down, neither), and
            // how fast it went
            if (leaving) {
                g.airLead  = -1;
                float most = -1;
                if (g.moving && !fresh)
                    for (int s = 0; s < 2; ++s)
                        if (const auto& ft = g.feet[s]; ft.swing && !ft.timed && ft.s > most)
                            most = ft.s, g.airLead = s;
                g.airRun = std::clamp((v - 0.8f) / 2.7f, 0.f, 1.f);
            }
            g.moving = false;
            for (auto& ft : g.feet)
                ft.swing = ft.timed = false;
            g.airVy  = g.wasAir ? std::min(g.airVy, m.vy) : m.vy;
            g.wasAir = true;
        } else if (g.wasAir) {
            // landed: the feet where they came down, the knees giving as it takes the fall. Going on (a key down, on its
            // way), the stride goes on from the foot that comes down first (the lower, else the one ahead), the other on
            // through the air if it's running (walking, it's down too): not stopping dead to start again
            g.wasAir = false;
            for (int s = 0; s < 2; ++s) {
                auto& ft = g.feet[s];
                ft       = {.at = footing(ft.ankle, yaw, s), .yaw = yaw, .pitch = ft.pitch, .heel = ft.pitch, .knee = ft.knee, .ankle = ft.ankle};
            }
            g.groundY = P.y, g.groundV = g.groundRate = 0, g.lastSupport = NAN;
            const bool going = v > 0.8f && length(m.wish) > 0.1f;
            if (going && beta < 0.5f) { // (walking, both come down: it steps on from standing)
                // (as far into its time on the ground as where it came down says: the body is over it as much as the
                // stride would have it by then, landing `ahead` of it)
                const V3    d     = g.feet[0].ankle - g.feet[1].ankle;
                const int   first = std::abs(d.y) > 0.03f ? (d.y < 0 ? 0 : 1) : dot(d, way) > 0 ? 0 : 1, other = 1 - first;
                const float over  = ahead - dot(g.feet[first].at - P, way);
                const float ph0   = beta * std::clamp(over / std::max(beta * stride, 0.05f), 0.f, 0.6f) + 1e-3f;
                g.moving          = true;
                g.phase           = frac(ph0 - 0.5f * first);
                g.phaseOff = g.phaseOffV = 0;
                g.feet[first].phase      = ph0;
                g.feet[first].landed     = g.time;
                auto&       ft           = g.feet[other];
                const float ph           = frac(g.phase + 0.5f * other);
                ft.phase                 = ph;
                if (ph >= beta) {
                    ft.swing = true, ft.gaitW = 1;
                    ft.from = ft.at, ft.fromYaw = yaw, ft.fromPitch = ft.pitch, ft.liftKnee = ft.knee, ft.fromOut = outOf(other, ft.at);
                    ft.start = beta, ft.s = (ph - beta) / std::max(1.f - beta, 1e-3f);
                    std::tie(ft.to, ft.toYaw, ft.toOut) = landing(other, (1.f - ph) / cadence);
                }
            }
            // (the hips going on down some as the body stops: see SETTLE_LAND)
            m_settleLift = LAND_GIVE;
            if (!m.flying)
                g.dipV += std::clamp(-g.airVy - 1.5f, 0.f, 10.f) * (going ? 0.1f : 0.16f);
        }

        if (grounded) {
            // starting: the foot furthest behind the way it goes lifts first (or one stepping already goes on); to go
            // somewhere behind it, stepping round at once (the legs go that way once the pelvis has turned to it)
            if (!g.moving && (v > 0.25f || turnFar > 0.5f)) {
                g.moving = true;
                // (turning far as it starts, the foot on the side it turns to: it opens the turn, as people step, where
                // the other would step across it)
                int lead = g.feet[0].swing ? 0 : g.feet[1].swing ? 1 : -1;
                if (lead < 0)
                    lead = turnKnown && std::abs(m_turnLeft) > 1.f ? (m_turnLeft > 0 ? 1 : 0) : dot(g.feet[0].at - g.feet[1].at, vel) < 0 ? 0 : 1;
                auto& ft = g.feet[lead];
                const float ph = ft.swing ? beta + ft.s * (1.f - beta) : beta, was = g.phase;
                g.phase        = frac(ph - 0.5f * lead);
                g.phaseOff     = phaseDiff(g.phaseOff + was - g.phase);
                ft.phase       = ft.swing ? ph : beta - 1e-4f;
                ft.start       = beta;
                ft.timed = ft.wait = false; // (a step of its own goes on as the gait's, eased into it)
                g.feet[1 - lead].phase = frac(ph + 0.5f);
            } else if (g.moving && v < 0.12f && length(velC) < 0.3f && length(m.wish) < 0.1f) {
                // stopping: a foot in the air comes down beside the other in what's left of its step (not going back
                // the other way, what the legs carry slowing through a stand as the player's already goes)
                g.moving = false;
                for (auto& ft : g.feet)
                    if (ft.swing) {
                        ft.timed = true;
                        ft.rate  = 1.f / std::max(0.14f, (1.f - ft.s) * g.swingTime) * (1.f - ft.s);
                    }
            }

            if (g.moving) {
                // (a planted foot the body leaves to its side, turning sharply, not ahead of it as it pushes off anyway;
                // or nearly out of the leg's reach: the pelvis down further than walking takes it, 0 to 1 and more as
                // it's dragged)
                auto strained = [&](int s) { return std::max(0.f, (g.feet[s].tight - 0.55f) / 0.25f) + std::max(0.f, g.feet[s].over / 0.015f); };
                auto leaving = [&](int s) {
                    const auto& ft = g.feet[s];
                    const V3    d{P.x - ft.at.x, 0, P.z - ft.at.z};
                    return !ft.swing && dot(d, vel) > 0 && (std::abs(dot(d, rightOf(ft.yaw))) > 0.35f * length(d) || strained(s) > 0);
                };
                // quicker steps while the standing foot is left behind (turning sharply, stopping short, going back)
                // (or nearly out of the leg's reach as the body leaves it)
                // (or left out to its side of the hip: walking round a tight curve, quicker shorter steps)
                float behind = 1.f;
                for (int s = 0; s < 2; ++s)
                    if (const auto& ft = g.feet[s]; !ft.swing) {
                        const V3    hip = P + yawTurn(hy).rotate(g.hipAt[s]);
                        const float out = dot(ft.ankle - hip, rightOf(hy)) * (s ? 1.f : -1.f);
                        behind          = std::max({behind, length(V3{hip.x - ft.ankle.x, 0, hip.z - ft.ankle.z}) / (0.7f * (g.thigh[s] + g.shin[s])),
                                                    std::abs(wrapPi(hy - ft.yaw)) / 0.7f, std::min(OUT_MOST, 1.f + std::max(0.f, out - OUT_EASY) * OUT_HURRY)});
                        if (leaving(s))
                            behind = std::max(behind, 1.f + 0.6f * std::min(strained(s), 1.f));
                    }
                g.hurry = std::min(behind * behind, 2.5f);
                g.phase = frac(g.phase + cadence * dt * g.hurry);
                g.hurrySmooth += (g.hurry - g.hurrySmooth) * (1.f - std::exp(-dt * 10.f));
                // (the body's swing going on at the eased pace: hurrying all at once, the arms and the pelvis would jerk)
                g.phaseOff = phaseDiff(g.phaseOff + cadence * dt * (g.hurrySmooth - g.hurry));
                // a foot the body has turned or moved away from, walking, lifts early (as people step out of a turn):
                // the stride goes on from its lifting, the other foot being down (the one that needs it more, and not
                // one just down)
                int   lift = -1;
                float need = 0;
                // (how long a foot in the air at phase ph has yet to go: turning, or speeding up or slowing down, the steps
                // as hurried as now; walking steadily as the stride goes, though the steps hurry to keep up)
                const float changing  = std::max({turnFar, smoothstep01(std::abs(g.turnLeft) / 0.8f), smoothstep01((length(g.accel) - 1.f) / 2.f)});
                auto        swingLeft = [&](float ph) { return (1.f - ph) / (cadence * lerpf(1.f, g.hurry, changing)); };
                for (int s = 0; s < 2; ++s) {
                    const auto& ft = g.feet[s];
                    const float ph = frac(g.phase + 0.5f * s);
                    if (ft.swing || g.feet[1 - s].swing || ph >= beta || ph < beta - 0.5f || g.time - ft.landed < 0.12f)
                        continue;
                    const V3    hip = P + yawTurn(hy).rotate(g.hipAt[s]);
                    const float dx  = length(V3{hip.x - ft.ankle.x, 0, hip.z - ft.ankle.z});
                    const float n   = std::max({dx / (0.8f * (g.thigh[s] + g.shin[s])), std::abs(wrapPi(yaw - ft.yaw)) / 0.9f, leaving(s) ? strained(s) : 0.f});
                    if (n > std::max(need, 1.f))
                        lift = s, need = n;
                }
                if (lift >= 0) {
                    const float was        = g.phase;
                    g.phase                = frac(beta - 0.5f * lift);
                    g.phaseOff             = phaseDiff(g.phaseOff + was - g.phase);
                    g.feet[lift].wait      = false;
                    g.feet[1 - lift].phase = frac(beta + 0.5f);
                    g.feet[lift].phase     = beta - 1e-4f;
                }
                for (int s = 0; s < 2; ++s) {
                    auto&       ft = g.feet[s];
                    const float ph = frac(g.phase + 0.5f * s);
                    if (ft.swing && ft.timed) {
                        // a step of its own from standing still going on (the other foot took the lead starting again):
                        // down where the gait puts it, then on the ground till its time there comes round
                        ft.s = std::min(1.f, ft.s + dt * ft.rate);
                        if (ft.s < 0.7f) {
                            const auto [to, ly, lo] = landing(s, (1.f - ft.s) / std::max(ft.rate, 1e-3f));
                            const float k           = 1.f - std::exp(-dt * 12.f);
                            ft.to += (to - ft.to) * k;
                            ft.toYaw += wrapPi(ly - ft.toYaw) * k;
                            ft.toOut += (lo - ft.toOut) * k;
                            ft.to.y = footY(ft.to, ft.toYaw, s);
                        }
                        if (ft.s >= 1.f) {
                            ft.swing = ft.timed = false, ft.wait = ph >= beta;
                            ft.at = ft.to, ft.yaw = ft.toYaw, ft.landed = g.time;
                        }
                    } else if (!ft.swing) {
                        if (ft.wait && ph < beta)
                            ft.wait = false;
                        // lifts as its time on the ground is over (not just before landing again: it waits a stride)
                        if (!ft.wait && ph >= beta && ph < 0.92f && ft.phase <= ph) {
                            ft.swing = true, ft.timed = false, ft.locked = false, ft.gaitW = 1;
                            ft.from = ft.at, ft.fromYaw = ft.yaw, ft.fromPitch = ft.pitch, ft.liftKnee = ft.knee, ft.fromOut = outOf(s, ft.at);
                            ft.start = ph, ft.s = 0;
                            std::tie(ft.to, ft.toYaw, ft.toOut) = landing(s, swingLeft(ph));
                        }
                    } else if (ph < ft.phase - 0.5f) {
                        // it came round: down
                        ft.swing = false;
                        ft.at = ft.to, ft.yaw = ft.toYaw, ft.landed = g.time;
                    } else {
                        ft.s = std::clamp((ph - ft.start) / std::max(1.f - ft.start, 1e-3f), 0.f, 1.f);
                        const V3 was = ft.to;
                        if (!ft.locked) {
                            const auto [to, ly, lo] = landing(s, swingLeft(ph));
                            const float k           = 1.f - std::exp(-dt * 15.f);
                            ft.to += (to - ft.to) * k;
                            ft.toYaw += wrapPi(ly - ft.toYaw) * k;
                            ft.toOut += (lo - ft.toOut) * k;
                            // (uncrossed as it goes, else it jumps as it locks; eased, as where the body will face jumps
                            // as a key goes down)
                            ft.to += (uncross(s, ft.to, swingLeft(ph)) - ft.to) * (1.f - std::exp(-dt * 25.f));
                            ft.locked = ft.s > 0.75f;
                        } else { // (where it lands stays, but for crossing the other leg as the body turns on, or the body
                                 // stopping short or turning back)
                            // (going there once it's 10 cm off, eased in from 6; and kept uncrossed eased, as at once the
                            // foot would jump as what it stands beside goes from where the other foot is to where it lands)
                            if (const auto [to, ly, lo] = landing(s, swingLeft(ph)); length(to - ft.to) > 0.06f)
                                ft.to += (to - ft.to) * (smoothstep01((length(to - ft.to) - 0.06f) / 0.04f) * (1.f - std::exp(-dt * 8.f)));
                            ft.to += (uncross(s, ft.to, swingLeft(ph)) - ft.to) * (1.f - std::exp(-dt * 25.f));
                        }
                        // (as it comes down, onto one tread where it's got to, not across the edge: heel or toes up in the air;
                        // going there no faster than RETARGET, below)
                        if (ft.locked) {
                            const V3 fl = footing(ft.to, ft.toYaw, s);
                            ft.to.x = fl.x, ft.to.z = fl.z;
                        }
                        if (V3 d{ft.to.x - was.x, 0, ft.to.z - was.z}; length(d) > (RETARGET + v) * dt)
                            ft.to = was + d * ((RETARGET + v) * dt / length(d));
                        // (on whatever's under it there: going onto the next stair, the higher)
                        ft.to.y = footY(ft.to, ft.toYaw, s);
                    }
                    ft.phase = ph;
                }
            } else {
                // standing: a foot that's off its spot (the body moved, or turned away from it) steps back under the body,
                // the one furthest off first
                g.hurry = 1;
                g.hurrySmooth += (1.f - g.hurrySmooth) * (1.f - std::exp(-dt * 10.f));
                g.pause -= dt;
                const bool stepping = g.feet[0].swing || g.feet[1].swing;
                if (!stepping && g.pause <= 0) {
                    int   pick  = -1;
                    float worst = 1.f;
                    for (int s = 0; s < 2; ++s) {
                        const auto& ft   = g.feet[s];
                        V3          d    = spot(s) - ft.at;
                        d.y              = 0;
                        const float need = std::max(length(d) / (0.11f * L), std::abs(wrapPi(yaw - ft.yaw)) / (std::abs(g.yawRate) > 1.f ? 0.3f : 0.45f));
                        if (need > worst) {
                            worst = need;
                            pick  = s;
                        }
                    }
                    if (pick >= 0) {
                        auto&       ft   = g.feet[pick];
                        const float dist = length(V3{ft.at.x - spot(pick).x, 0, ft.at.z - spot(pick).z});
                        ft.swing = ft.timed = true, ft.locked = ft.wait = false, ft.gaitW = 0;
                        ft.from = ft.at, ft.fromYaw = ft.yaw, ft.fromPitch = ft.pitch, ft.liftKnee = ft.knee, ft.fromOut = outOf(pick, ft.at);
                        ft.s    = 0;
                        ft.rate = 1.f / std::clamp(0.3f + 0.25f * dist / L, 0.32f, 0.55f);
                        ft.to   = spot(pick);
                        ft.toYaw = yaw + std::clamp(g.yawRate * 0.3f, -0.5f, 0.5f);
                        ft.toOut = wide;
                    }
                }
                for (int s = 0; s < 2; ++s) {
                    auto& ft = g.feet[s];
                    if (!ft.swing)
                        continue;
                    ft.s = std::min(1.f, ft.s + dt * ft.rate);
                    if (ft.s < 0.7f) { // (it follows the body; then it's coming down, on what's under it there)
                        const float k = 1.f - std::exp(-dt * 12.f);
                        ft.to += (spot(s) - ft.to) * k;
                        ft.toYaw += wrapPi(yaw + std::clamp(g.yawRate * 0.3f, -0.5f, 0.5f) - ft.toYaw) * k;
                        ft.to.y = footY(ft.to, ft.toYaw, s);
                    }
                    if (ft.s >= 1.f) {
                        ft.swing = ft.timed = false;
                        ft.at = ft.to, ft.yaw = ft.toYaw, ft.landed = g.time;
                        g.pause = 0.06f;
                    }
                }
            }
        }
        // (critically damped, as are the pelvis's turn and the knee's fold: eased in and out, not starting or stopping at
        // once as each key goes down or up)
        springTo(g.moveW, g.moveWV, g.moving ? std::clamp(v / 0.7f, 0.f, 1.f) : 0.f, 14.f, dt);
        g.moveW        = std::clamp(g.moveW, 0.f, 1.f);
        const float mw = g.moveW;

        // a planted foot the body turns away from pivots on its ball, as people turn on the standing foot (walking,
        // once it's a little off; standing, as far as the hip turns: past that it steps). The pelvis turns toward the
        // body's yaw as far as the planted legs let it, the trunk and the head turning on ahead of it
        if (grounded) {
            const float from = g.moving ? 0.3f : HIP_TURN;
            for (int s = 0; s < 2; ++s) {
                auto& ft   = g.feet[s];
                float rate = 0;
                // (how far the body is turned from it: by way of the pelvis, which turns after the body the way it turns)
                if (const float need = -lagNow - wrapPi(ft.yaw - hy), over = std::abs(need) - (ft.spin > 0.05f ? 0.03f : from); !ft.swing && over > 0) {
                    const float turn = std::copysign(std::min(over, PIVOT_RATE * dt), need);
                    const V3    ball{0, 0, g.foot[s].ball.z}, heel{0, 0, std::max(g.foot[s].heel.z, 0.f)};
                    const V3    at = ft.at + yawTurn(ft.yaw).rotate(ball) - yawTurn(ft.yaw + turn).rotate(ball);
                    // (not swinging its heel into a step: see PIVOT_CLEAR; it stays as it is, and it steps round instead)
                    if (groundAt(at + yawTurn(ft.yaw + turn).rotate(heel)).y < ft.at.y + std::tan(ft.gp) * heel.z + PIVOT_CLEAR) {
                        ft.at  = at;
                        ft.yaw = wrapPi(ft.yaw + turn);
                        rate   = std::abs(turn) / std::max(dt, 1e-4f) / PIVOT_RATE;
                    }
                }
                ft.spin += (rate - ft.spin) * (1.f - std::exp(-dt * 15.f));
            }
            // (the pelvis's yaw from the body's: it turns in the world, not along with the body)
            float lo = -1e9f, hi = 1e9f;
            for (const auto& ft : g.feet)
                if (!ft.swing) {
                    const float c = lagNow + wrapPi(ft.yaw - hy);
                    lo            = std::max(lo, c - HIP_TURN);
                    hi            = std::min(hi, c + HIP_TURN);
                }
            // (as fast as closes the gap in about a 25th of a second, no faster than HIP_RATE, speeding up and slowing
            // down no quicker than HIP_ACCEL: a foot lifting that held it back, it turns on after the other, not round
            // at once)
            const float need = (lo <= hi ? std::clamp(0.f, lo, hi) : 0.5f * (lo + hi)) - lagNow;
            g.hipLagV        = approach(g.hipLagV, std::copysign(std::min({std::abs(need) * 25.f, HIP_RATE, std::sqrt(2.f * HIP_ACCEL * std::abs(need))}), need), HIP_ACCEL * dt);
            g.hipLag         = lagNow + (std::abs(g.hipLagV * dt) > std::abs(need) && g.hipLagV * need > 0 ? need : g.hipLagV * dt);
        } else {
            float turn = 0;
            springTo(turn, g.hipLagV, -lagNow, 10.f, dt);
            g.hipLag = lagNow + turn;
            for (auto& ft : g.feet)
                ft.spin = 0;
        }

        const V3 Rp = rightOf(yaw + g.hipLag); // (the pelvis's right: what it sways and leans across)

        // the feet: planted ones rolled over the heel and the ball as the stride goes, the others on their way (backwards
        // the other way round: onto the toes first, off the heel; sideways flat)
        const float roll = g.forward >= 0 ? g.forward : 0.4f * g.forward;
        std::array<V3, 2>   ankleW;
        std::array<Quat, 2> footW;
        float               support = 0; // the ground under them
        auto toesUp = [&](int s, float pitch) { return pitch < 0 ? pitch * g.toesUp[s] : pitch; };
        // a foot's pitch this frame toward `want` from last frame's: no quicker than FOOT_ROLL, and speeding up and
        // slowing down to it no quicker than FOOT_ROLL_ACCEL (a second call in a frame starts over)
        auto rollTo = [&](SGaitFoot& ft, float want) {
            const float err = want - ft.heel, most = FOOT_ROLL_ACCEL * dt;
            const float stop = std::copysign(std::min(std::abs(err) / std::max(dt, 1e-4f), std::sqrt(2.f * FOOT_ROLL_ACCEL * std::abs(err))), err);
            ft.roll = std::clamp(std::clamp(stop, ft.rollFrom - most, ft.rollFrom + most), -FOOT_ROLL, FOOT_ROLL);
            return ft.heel + ft.roll * dt;
        };
        for (auto& ft : g.feet) {
            ft.rollFrom = ft.roll;
            ft.gaitW    = approach(ft.gaitW, ft.timed ? 0.f : 1.f, dt / 0.15f);
        }
        for (int s = 0; s < 2; ++s) {
            auto&       ft = g.feet[s];
            const auto& fs = g.foot[s];
            V3          at = ft.at;
            float       fy = ft.yaw, pitch = 0;
            if (!grounded)
                continue; // (the air's pose, below)
            if (!ft.swing) {
                const float u = std::clamp(frac(g.phase + 0.5f * s) / beta, 0.f, 1.f);
                pitch         = g.moving ? toesUp(s, lerpf(through(WALK_STANCE_PITCH, u), through(RUN_STANCE_PITCH, u), g.run) * mw * roll) : 0.f;
                if (!g.moving) // (flat again, gently)
                    pitch = ft.pitch * std::exp(-dt * 14.f);
                if (ft.spin > 1e-3f) // (pivoting: on its ball)
                    pitch = std::max(pitch, 0.15f * ft.spin);
                support += at.y * 0.5f;
                ft.gp += (under(at, fy, s).pitch - ft.gp) * (1.f - std::exp(-dt * 20.f));
            } else {
                // (running, the foot still comes down going ahead some: it doesn't reach out past where it lands and
                // come back as far)
                // (as the gait's step or one of its own, or on its way from one to the other)
                const float gw = smoothstep01(ft.gaitW);
                const float u = ft.s, e = lerpf(minJerk(u), u * u * (3.f - 2.f * u) + 0.6f * u * u * (u - 1.f), g.run * gw);
                // (the height it goes to eased as where it lands moves onto another stair, exactly that as it lands)
                if (!ft.toYSet)
                    ft.toY = ft.to.y, ft.toYV = 0, ft.toYSet = true, ft.fromGp = ft.gp;
                else
                    springTo(ft.toY, ft.toYV, ft.to.y, 30.f, dt);
                ft.toGp = under(ft.to, ft.toYaw, s).pitch;
                ft.gp   = lerpf(ft.fromGp, ft.toGp, e);
                V3 to = ft.to;
                to.y  = lerpf(ft.toY, ft.to.y, smoothstep01((ft.s - 0.8f) / 0.2f));
                at    = lerp(ft.from, to, e);
                // (under its hip, whichever way the pelvis turns meanwhile: out to its side, or in under the body, no
                // further than where it lifted and where it lands are, and SWING_OUT or SWING_IN, from the pelvis as it
                // was last frame; not swung out round the body as the pelvis turns over the other foot. Not onto
                // higher ground: turning round on a stair, across the pelvis is up or down the steps)
                {
                    const float sd = s ? 1.f : -1.f, ends = lerpf(ft.fromOut, ft.toOut, ft.s), hipOut = std::abs(g.hipAt[s].x);
                    const float was = dot(V3{at.x - g.pelvis.x, 0, at.z - g.pelvis.z}, Rp) * sd;
                    const float lim = std::clamp(was, std::min(ends, hipOut) - SWING_IN, std::max(ends, hipOut) + SWING_OUT);
                    if (const V3 in = at + Rp * (sd * (lim - was) * smoothstep01(ft.s / 0.25f) * smoothstep01((1.f - ft.s) / 0.25f));
                        std::abs(lim - was) > 1e-4f && under(in, ft.fromYaw + wrapPi(ft.toYaw - ft.fromYaw) * e, s).hi < std::max(ft.from.y, to.y) + 0.02f)
                        at.x = in.x, at.z = in.z;
                }
                // up a step: up first, then over; down one: over its edge, then down; else over as it goes; and over
                // whatever's under it on the way (a stair's edge), but where it lifts and lands
                const float rise = to.y - ft.from.y;
                at.y = ft.from.y + rise * (rise > 0.03f ? minJerk(std::min(1.f, ft.s * 1.7f)) : rise < -0.03f ? minJerk(std::clamp((e - 0.25f) / 0.75f, 0.f, 1.f)) : e);
                if (std::abs(rise) > 0.03f || std::abs(footY(ft.from, ft.fromYaw, s) - ft.from.y) > 0.03f)
                    at.y = std::max(at.y, lerpf(at.y, footY(at, ft.fromYaw + wrapPi(ft.toYaw - ft.fromYaw) * e, s) + 0.02f,
                                                smoothstep01(ft.s / 0.15f) * smoothstep01((1.f - ft.s) / 0.1f)));
                const float high = lerpf(through(STAND_LIFT, ft.s) * std::min(1.f, 0.4f + 5.f * length(ft.to - ft.from) / L),
                                         lerpf(through(WALK_LIFT, ft.s) * std::clamp(0.5f + 0.35f * v, 0.6f, 1.2f),
                                               through(RUN_LIFT, ft.s) * std::clamp(0.35f + 0.15f * v, 0.6f, 1.25f), g.run),
                                         gw);
                at.y += high * L * (1.f - 0.4f * g.crouch);
                fy = ft.fromYaw + wrapPi(ft.toYaw - ft.fromYaw) * e;
                pitch = lerpf(lerpf(ft.fromPitch, 0.f, smoothstep01(ft.s / 0.5f)) - 0.12f * std::sin(PI * ft.s),
                              lerpf(ft.fromPitch, toesUp(s, lerpf(through(WALK_SWING_PITCH, ft.s), through(RUN_SWING_PITCH, ft.s), g.run) * roll), smoothstep01(ft.s / 0.15f)), gw);
                support += (ft.from.y + (to.y - ft.from.y) * e) * 0.5f;
            }
            if (!ft.swing)
                ft.toYSet = false;
            pitch     = rollTo(ft, pitch); // (as a foot rolls)
            ft.pitch  = pitch;
            ankleW[s] = ankleAt(at, fy, pitch, ft.gp, s);
            footW[s]  = yawTurn(fy) * footPitch(pitch + ft.gp);
        }

        // the ground under the feet (up a stair as a foot gets there), springy; going somewhere, what the body goes over:
        // the ground's mean from a little behind it to a little ahead the way it goes (the line of a flight of stairs,
        // not a step at a time; running down one the feet are left behind on the steps above). Critically damped toward
        // it as it goes on up or down, else a pelvis following it up a flight lags behind, the knees bent
        if (grounded) {
            if (mw > 0.01f) {
                float line = 0;
                for (int i = -2; i <= 2; ++i)
                    line += groundAt(P + way * (0.17f * L * i)).y * 0.2f;
                support = lerpf(support, line, mw);
            }
            if (std::isfinite(g.lastSupport) && dt > 1e-4f)
                g.groundRate += ((support - g.lastSupport) / dt - g.groundRate) * (1.f - std::exp(-dt * 6.f));
            g.lastSupport = support;
            float x = g.groundY - (support - g.groundRate * dt), xv = g.groundV - g.groundRate;
            springTo(x, xv, 0.f, 14.f, dt);
            g.groundY = support + x, g.groundV = g.groundRate + xv;
            if (std::abs(g.groundY - support) > 0.6f)
                g.groundY = support, g.groundV = g.groundRate;
        } else
            g.groundY = P.y, g.groundV = g.groundRate = 0, g.lastSupport = NAN;
        // the knees giving as it lands, and back
        {
            constexpr float W = 11.f;
            g.dipV += (-g.dip * W * W - 2.f * 0.75f * W * g.dipV) * dt;
            g.dip = std::clamp(g.dip + g.dipV * dt, -0.03f * L, 0.3f * L);
        }

        // the pelvis: over the body, down a little as the knees bend (more running), rising and sinking with each
        // step (walking highest over the standing leg, running lowest), swaying over the standing foot
        springTo(g.phaseOff, g.phaseOffV, 0.f, 15.f, dt);
        const float phL = g.phase + g.phaseOff, mid = 4.f * PI * (phL - beta * 0.5f);
        const float gaitY = lerpf(hWalk + aWalk * std::cos(mid), hRun - aRun * std::cos(mid), g.run);
        float       rollW = 0; // standing: onto one leg
        if (grounded && !g.moving) {
            // standing: the weight onto the foot that stays while the other steps, else swaying slowly between them
            for (int s = 0; s < 2; ++s)
                if (g.feet[s].swing)
                    rollW += dot(g.feet[1 - s].at - P, Rp) * 0.55f * std::sin(PI * std::min(1.f, g.feet[s].s * 1.2f));
            rollW += L * 0.012f * std::sin(TAU * g.time / 6.5f) * (1.f - mw);
        }
        // (eased: stopping with a foot well out to the side, as from a sidestep, it doesn't jump onto it)
        g.rollW += (rollW - g.rollW) * (1.f - std::exp(-dt * 12.f));
        rollW = g.rollW;
        // (walking into running and back, the arms and the trunk ease from one to the other: at a steady rate, as the
        // legs' stride goes, the elbows would bend and the swing grow all at once as Shift goes down or up)
        springTo(g.runArms, g.runArmsV, g.run, 12.f, dt);
        const float ra = std::clamp(g.runArms, 0.f, 1.f);
        // The body over the steps as the avatar's walk and run have it (SGaitClip: made in Blender), at the body's phase:
        // walking to running as the arms ease, swung about its mean as far as the speed takes it (GAIT_WALK), as much of it
        // as the body walks (sw: standing, the walking's own). Its hips, trunk, head and arms in place of the walking's
        // own swing; what that adds for turning, leaning, crouching and keeping the arms clear of the body stays
        const bool                 styled = m_gaitStyle && grounded && md.gaitClips[0] && md.gaitClips[1];
        const float                amp    = std::clamp(v / (lerpf(GAIT_WALK, GAIT_RUN, ra) * std::sqrt(L / GAIT_LEG)), GAIT_LEAST, GAIT_MOST);
        std::array<Quat, HB_COUNT> sMean;
        V3                         sMeanMove;
        if (styled) {
            for (int b = 0; b < HB_COUNT; ++b)
                sMean[b] = slerp(md.gaitClips[0]->mean[b], md.gaitClips[1]->mean[b], ra);
            sMeanMove = lerp(md.gaitClips[0]->meanMove, md.gaitClips[1]->meanMove, ra);
        }
        auto styleAt = [&](float phase, std::array<Quat, HB_COUNT>& T, V3& move) {
            std::array<Quat, HB_COUNT> run;
            V3                         runMove;
            sampleGait(*md.gaitClips[0], phase, T, move);
            sampleGait(*md.gaitClips[1], phase, run, runMove);
            for (int b = 0; b < HB_COUNT; ++b)
                T[b] = slerp(sMean[b], slerp(T[b], run[b], ra), amp);
            move = lerp(sMeanMove, lerp(move, runMove, ra), amp);
        };
        std::array<Quat, HB_COUNT> sT;
        V3                         sMove;
        if (styled)
            styleAt(phL, sT, sMove);
        const float sw = styled ? smoothstep01(mw) : 0.f;
        const auto& sHas = styled ? md.gaitClips[0]->has : std::array<bool, HB_COUNT>{};
        // (the clip's bones as this avatar has them: a bone it doesn't have, as the one below it; its top (the arms'
        // frame) its upper chest, else chest)
        auto sAt = [&](const std::array<Quat, HB_COUNT>& T, int b) {
            for (; b > HB_HIPS && !sHas[b]; --b)
                ;
            return T[b];
        };
        // (the clip's sway: its hips to their left, in hips heights; here + = right, meters)
        const float lat = lerpf(-mw * L * 0.028f * (1.f - 0.65f * g.run) * std::cos(2.f * PI * (phL - beta * 0.5f)), -mw * (sMove.x - sMeanMove.x) * g.unit, sw) +
            rollW; // (+ = right)
        V3          pelvis = P + Rp * lat;
        pelvis.y  = (grounded ? g.groundY : P.y) + lerpf(g.hipH - 0.006f * L, gaitY, mw) - hCrouch - g.dip;

        // Off the ground (JUMP_*, FLY_*): the pose it goes to, and the springs going there; leaving the ground they start
        // from how the legs, the arms and the trunk were
        g.flyE.to(m.flying ? 1.f : 0.f, 7.f, 1.f, dt);
        const float fe = smoothstep01(std::clamp(g.flyE.x, 0.f, 1.f)); // (flying, eased)
        float       ju = 0.5f, hover = 0;                              // (how far through a jump; hovering, 0..1)
        if (!grounded) {
            if (leaving) {
                const Quat hq = yawTurn(yaw);
                const V3   was = fresh ? P + UP * g.hipH : g.pelvis;
                for (int s = 0; s < 2; ++s) {
                    auto&       al = g.airLeg[s];
                    const auto& ft = g.feet[s];
                    const V3    d  = hq.conj().rotate(ft.ankle - was - hq.rotate(g.hipAt[s])); // (avatar space: ahead -z, right +x)
                    const float t = g.thigh[s], sh = g.shin[s], dist = std::clamp(length(d), std::abs(t - sh) + 1e-3f, t + sh);
                    const float knee = std::acos(std::clamp((dist * dist - t * t - sh * sh) / (2.f * t * sh), -1.f, 1.f));
                    const float hip  = std::atan2(-d.z, -d.y) + std::asin(std::clamp(sh * std::sin(knee) / dist, -1.f, 1.f));
                    al.hip = {hip, 0}, al.knee = {knee, 0}, al.out = {std::asin(std::clamp(d.x * (s ? 1.f : -1.f) / dist, -1.f, 1.f)), 0};
                    al.toes     = {ft.pitch + hip - knee, 0};
                    g.airArm[s] = {{g.armWas[s].x, 0}, {g.armWas[s].y, 0}, {g.armWas[s].z, 0}};
                }
                g.airLean  = {g.leanWas, 0};
                g.airPitch = g.airRoll = g.airYaw = {};
                // (a jump: the body pushed up over a moment, not off like a shot)
                m_settleLift = m.vy > 1.f ? 1.f : 0.f;
            }
            // a jump or a fall
            ju            = std::clamp(0.5f - m.vy / (2.f * JUMP_UP), 0.f, 1.f);
            const float r = g.airLead >= 0 ? g.airRun : 0.f;
            // flying, by how it goes: ahead (cruise, and flat out), hovering, climbing or sinking with little way on
            const float u = dot(vel, F), side = dot(vel, Rt), vy = m.vy, sp = std::sqrt(u * u + side * side + vy * vy);
            const float cruise = 1.f - std::exp(-std::max(u, 0.f) / FLY_LIE_SPEED), fast = std::clamp((u - 9.f) / 6.f, 0.f, 1.f);
            const float climb = std::clamp(vy / 6.f, 0.f, 1.f) * (1.f - cruise), sink = std::clamp(-vy / 6.f, 0.f, 1.f) * (1.f - cruise);
            const float aF = dot(g.accel, F), aR = dot(g.accel, Rt);
            hover          = std::exp(-sp / 2.f);
            float pitchTo  = u >= 0 ? FLY_LIE * cruise * u / std::sqrt(u * u + std::max(vy, 0.f) * std::max(vy, 0.f) + 1e-4f) : -0.35f * (1.f - std::exp(u / 3.f));
            pitchTo += std::clamp(FLY_PUSH * aF, -FLY_BRAKE_MOST, FLY_PUSH_MOST) + 0.2f * std::clamp(-vy / 8.f, 0.f, 1.f) * cruise;
            float rollTo = std::clamp(std::atan(length(vel) * g.velTurn / GRAVITY) * 0.9f + 0.06f * side + 0.02f * aR, -FLY_BANK, FLY_BANK) +
                0.035f * std::sin(TAU * 0.21f * g.time) * hover;
            // (first person, from its eyes: lying along the flight would take the camera down and ahead; hardly, and
            // banking less)
            if (m.fp.on)
                pitchTo *= 0.25f, rollTo *= 0.5f;
            const float lw = lerpf(AIR_LEG_W, FLY_LEG_W, fe), lz = lerpf(AIR_LEG_Z, FLY_LEG_Z, fe);
            for (int s = 0; s < 2; ++s) {
                // jumping: tucked, reaching down to land; from a run the leg stepping goes on ahead, the other trailing
                const bool lead = s == g.airLead;
                float hipJ  = lerpf(through(JUMP_HIP, ju) + (s ? 0.04f : -0.02f), through(lead ? JUMP_LEAD_HIP : JUMP_TRAIL_HIP, ju), r);
                float kneeJ = lerpf(through(JUMP_KNEE, ju) + (s ? 0.08f : 0.f), through(lead ? JUMP_LEAD_KNEE : JUMP_TRAIL_KNEE, ju), r);
                float toesJ = lerpf(through(JUMP_TOES, ju), through(lead ? JUMP_LEAD_TOES : JUMP_TRAIL_TOES, ju), r);
                // flying: hovering the right leg bent and the left nearly straight, treading slowly; flying ahead trailing
                // along the body (the right knee bent, less flat out), fluttering a little; climbing they hang straight,
                // sinking they come up under it; swinging back as it speeds up and ahead as it slows
                const bool  bent = s == 1;
                const float sd   = s ? PI : 0.f;
                float hipF  = lerpf(bent ? 0.32f : 0.12f, bent ? 0.05f : -0.08f, cruise);
                float kneeF = lerpf(bent ? 0.95f : 0.3f, lerpf(bent ? 0.75f : 0.12f, bent ? 0.45f : 0.08f, fast), cruise);
                float toesF = lerpf(0.6f, 0.95f, cruise);
                hipF        = lerpf(lerpf(hipF, 0.02f, climb), bent ? 0.55f : 0.35f, sink);
                kneeF       = lerpf(lerpf(kneeF, bent ? 0.35f : 0.08f, climb), bent ? 1.f : 0.6f, sink);
                toesF       = lerpf(lerpf(toesF, 0.8f, climb), 0.35f, sink);
                kneeF += 0.1f * std::sin(TAU * 0.33f * g.time + sd) * hover + 0.07f * std::sin(TAU * 1.2f * g.time + sd) * cruise * (1.f - fast);
                hipF += 0.05f * std::sin(TAU * 0.33f * g.time + sd + 0.8f) * hover + std::clamp(-0.015f * aF, -0.3f, 0.3f);
                auto& al = g.airLeg[s];
                al.hip.to(lerpf(hipJ, hipF, fe), lw, lz, dt);
                al.knee.to(lerpf(kneeJ, kneeF, fe), lw, lz, dt);
                al.out.to(lerpf(0.06f, lerpf(0.05f, 0.02f, cruise), fe), lw, lz, dt);
                al.toes.to(lerpf(toesJ, toesF, fe), lw, lz, dt);
                al.knee.x = std::clamp(al.knee.x, 0.f, 2.4f);
                // the arms: from standing swinging up then out; from a run against the legs, bent as running; flying
                // hovering held out a little, drifting; ahead swept back along the body (closer flat out); climbing down
                // by it, sinking out; forward and out slowing down; out on the high side of a bank
                const float aheadJ = lerpf(through(JUMP_ARM, ju), through(lead ? JUMP_ARM_BACK : JUMP_ARM_AHEAD, ju), r);
                const float outJ   = lerpf(through(JUMP_ARM_OUT, ju), through(JUMP_ARM_RUN_OUT, ju), r);
                const float bendJ  = lerpf(0.4f, through(JUMP_ARM_RUN_BEND, ju), r);
                float aheadF = lerpf(lerpf(lerpf(0.12f, lerpf(-0.35f, -0.55f, fast), cruise), -0.15f, climb), 0.25f, sink);
                float outF   = lerpf(lerpf(lerpf(0.5f, lerpf(0.32f, 0.16f, fast), cruise), 0.28f, climb), 0.6f, sink);
                float bendF  = lerpf(lerpf(lerpf(0.5f, lerpf(0.25f, 0.12f, fast), cruise), 0.3f, climb), 0.45f, sink);
                aheadF += 0.06f * std::sin(TAU * 0.27f * g.time + s * 2.1f) * hover + std::clamp(-0.012f * aF, 0.f, 0.45f);
                outF += 0.05f * std::sin(TAU * 0.19f * g.time + s) * hover + std::clamp(-0.006f * aF, 0.f, 0.25f);
                const float up = g.airRoll.x * (s ? -1.f : 1.f); // (this arm's side up, rolled)
                outF += up > 0 ? 0.3f * up : 0.12f * up;
                auto& aa = g.airArm[s];
                aa.ahead.to(lerpf(aheadJ, aheadF, fe), AIR_ARM_W, AIR_ARM_Z, dt);
                aa.out.to(lerpf(outJ, outF, fe), AIR_ARM_W, AIR_ARM_Z, dt);
                aa.bend.to(lerpf(bendJ, bendF, fe), AIR_ARM_W, AIR_ARM_Z, dt);
                aa.out.x  = std::clamp(aa.out.x, 0.f, 1.4f);
                aa.bend.x = std::clamp(aa.bend.x, 0.f, 2.4f);
            }
            // the trunk: jumping ahead a little (more from a run), upright at the top; flying curled a little hovering,
            // arched flying ahead
            const float leanJ = lerpf(through(JUMP_LEAN, ju), through(JUMP_RUN_LEAN, ju), r);
            const float leanF = lerpf(lerpf(lerpf(0.06f, lerpf(-0.15f, -0.22f, fast), cruise), -0.05f, climb), 0.1f, sink);
            g.airLean.to(lerpf(leanJ, leanF, fe), AIR_ARM_W, AIR_ARM_Z, dt);
            g.airPitch.to(pitchTo * fe, FLY_BODY_W, FLY_BODY_Z, dt);
            g.airRoll.to(rollTo * fe, FLY_BODY_W * 0.8f, FLY_BODY_Z, dt);
            g.airYaw.to(0.05f * std::sin(TAU * 0.13f * g.time + 1.f) * hover * fe, 3.f, 1.f, dt);
        } else
            g.airPitch = g.airRoll = g.airYaw = {};

        // its turns (the frame above: +x the avatar's left, +y up, +z ahead): with the stride (the hip of the leg going
        // ahead goes ahead too), dropping on the side of the swinging leg, tilted ahead a little; leaning into turns
        const float fwdA  = dot(g.accel, F);
        // (as far as it's pushed sideways, round a curve or setting off to its side: not as the body turns round on the
        // spot; walking, a little)
        const float leanMost = lerpf(0.1f, 0.2f, g.run);
        const float lean     = mw * std::clamp(std::atan(dot(g.accel, Rp) / GRAVITY) * 0.6f, -leanMost, leanMost);
        float yawP = -mw * lerpf(0.06f + 0.02f * std::min(v, 2.f), 0.1f, g.run) * std::cos(TAU * phL);
        // (standing, the hip over the leg taking the weight is the higher one)
        float rollP = mw * 0.07f * std::sin(TAU * (phL + 0.05f)) + lean - (1.f - mw) * 0.6f * rollW / L;
        float tiltP = mw * lerpf(0.03f, 0.09f, g.run) + 0.25f * g.crouch;
        if (sw > 0) { // (the clip's: about its mean, which tilts ahead)
            const V3 d = rotationVector(sMean[HB_HIPS].conj() * sT[HB_HIPS]), m0 = rotationVector(sMean[HB_HIPS]);
            yawP       = lerpf(yawP, mw * d.y, sw);
            rollP      = lerpf(rollP, mw * d.z + lean - (1.f - mw) * 0.6f * rollW / L, sw);
            tiltP      = lerpf(tiltP, mw * (m0.x + d.x) + 0.25f * g.crouch, sw);
        }
        const float lagP  = -g.hipLag; // (the frame above turns left > 0)
        const Quat  hipsQ = Quat::axisAngle(UP, yawP + lagP + g.airYaw.x) * Quat::axisAngle({0, 0, 1}, rollP + g.airRoll.x) *
            Quat::axisAngle({1, 0, 0}, tiltP + g.airPitch.x);
        pelvis += Rp * (std::sin(lean) * g.hipH * 0.5f);

        // the pelvis no higher than the planted legs reach
        const M4   toAvatar = m.world.inverse();
        const Quat bodyQ    = yawTurn(yaw);
        const Quat hipsW    = bodyQ * (g.toFrame.conj() * hipsQ * g.toFrame); // (frame above -> avatar space -> world)
        // Walking ahead, the knees bend as people's do (WALK_STANCE_KNEE, WALK_SWING_KNEE): late on the ground the heel
        // rises as far as keeps the knee to that while the pelvis comes down to the foot landing ahead (so that foot
        // holds the pelvis down only if even its heel right up doesn't reach), and a foot in the air goes up as far
        // as the knee folds
        const float kneeW    = (1.f - g.run) * mw * std::clamp(2.f * g.forward, 0.f, 1.f); // (easing out as it stops)
        auto        stanceU  = [&](int s) { return std::clamp(frac(g.phase + 0.5f * s) / beta, 0.f, 1.f); };
        auto        heelUp   = [&](int s) { return kneeW > 0.01f && !g.feet[s].swing && stanceU(s) >= 0.4f; };
        auto        heelMost = [&](int s) { // (as far as it rises by this frame: no quicker than a foot rolls)
            return approach(g.feet[s].heel, 1.15f, FOOT_ROLL * dt);
        };
        auto        kneeAt   = [&](int s, float bend) { // (the hip to the ankle with the knee bent that far)
            const float a = g.thigh[s], b = g.shin[s];
            return std::min(std::sqrt(a * a + b * b + 2.f * a * b * std::cos(bend)), 0.995f * (a + b));
        };
        if (!grounded)
            g.lowered = g.loweredV = 0;
        else {
            // (a foot coming down counts too, the more the nearer it is to landing, so the pelvis is down as it lands)
            float lower = 0;
            for (int s = 0; s < 2; ++s) {
                const auto& ft = g.feet[s];
                const float w  = ft.swing ? smoothstep01(ft.gaitW) * smoothstep01((ft.s - 0.5f) / 0.5f) : 1.f;
                if (w <= 0)
                    continue;
                const V3 ankle = ft.swing ? ankleAt(ft.to, ft.toYaw, toesUp(s, lerpf(through(WALK_SWING_PITCH, 1), through(RUN_SWING_PITCH, 1), g.run)), ft.toGp, s)
                    : heelUp(s)           ? ankleAt(ft.at, ft.yaw, lerpf(ft.pitch, std::max(ft.pitch, heelMost(s)), kneeW), ft.gp, s)
                                          : ankleW[s];
                // (from where the hip will be as it lands)
                const V3    hip   = pelvis + hipsW.rotate(g.hipAt[s]) + (ft.swing ? vel * ((1.f - ft.s) * g.swingTime) : V3{});
                const float reach = 0.995f * (g.thigh[s] + g.shin[s]);
                const float dx    = length(V3{hip.x - ankle.x, 0, hip.z - ankle.z});
                const float up    = std::sqrt(std::max(0.f, reach * reach - dx * dx));
                lower             = std::max(lower, w * (hip.y - ankle.y - up));
                if (!ft.swing)
                    g.feet[s].tight = (hip.y - ankle.y - up) / (0.12f * L);
            }
            // (no further than the knees bend well; up or down stairs or a slope further, and as far as a foot stands
            // or comes down below what the pelvis goes over, as people's do going down (the leg behind bent, the one
            // ahead reaching down to the next step): a foot still out of reach is dragged after the body)
            float below = 0;
            for (const auto& ft : g.feet) { // (a foot ahead, or under it: one behind lifts, as people's does going up)
                const V3    p = ft.swing ? ft.to : ft.at;
                const float w = ft.swing ? smoothstep01(ft.gaitW) * smoothstep01((ft.s - 0.5f) / 0.5f) : 1.f;
                if (dot(V3{p.x - P.x, 0, p.z - P.z}, way) > -0.1f * L)
                    below = std::max(below, w * (g.groundY - p.y));
            }
            const float lowest = std::min(L * (0.12f + 0.2f * std::min(std::abs(g.slope), 1.f)) + below, 0.45f * L);
            lower              = std::min(lower, lowest);
            pelvis.y -= lower;
            for (int s = 0; s < 2; ++s) {
                auto& ft = g.feet[s];
                if (!heelUp(s))
                    continue;
                // the heel up: the least from its curve's floor that brings the ankle near enough the hip, else as
                // near as it goes this frame
                const V3    hip   = pelvis + hipsW.rotate(g.hipAt[s]);
                const float u     = stanceU(s), want = kneeAt(s, through(WALK_STANCE_KNEE, u));
                auto        dist  = [&](float p) { return length(ankleAt(ft.at, ft.yaw, p, ft.gp, s) - hip); };
                const float most  = heelMost(s);
                const float least = std::min({through(WALK_HEEL_LEAST, u), ft.pitch, most});
                float       best  = least, near = dist(least);
                for (int i = 1; i <= 24 && near > want; ++i) {
                    const float p = least + (most - least) * i / 24.f, d = dist(p);
                    if (d <= want) {
                        float lo = least + (most - least) * (i - 1) / 24.f, hi = p;
                        for (int k = 0; k < 8; ++k)
                            (dist(0.5f * (lo + hi)) > want ? lo : hi) = 0.5f * (lo + hi);
                        best = hi, near = want;
                    } else if (d < near)
                        best = p, near = d;
                }
                ft.pitch  = rollTo(ft, lerpf(ft.pitch, best, kneeW));
                ankleW[s] = ankleAt(ft.at, ft.yaw, ft.pitch, ft.gp, s);
                footW[s]  = yawTurn(ft.yaw) * footPitch(ft.pitch + ft.gp);
                // (and the pelvis down to it, if that doesn't reach)
                const float reach = 0.995f * (g.thigh[s] + g.shin[s]), dx = length(V3{hip.x - ankleW[s].x, 0, hip.z - ankleW[s].z});
                const float over = hip.y - ankleW[s].y - std::sqrt(std::max(0.f, reach * reach - dx * dx)), room = lowest - lower;
                ft.tight         = (lower + over) / (0.12f * L);
                if (over > 0 && room > 0) {
                    pelvis.y -= std::min(over, room);
                    lower += std::min(over, room);
                }
            }
            // (down as far as the legs need, as a body goes down: speeding up no harder than LOWER_ACCEL, to at most
            // LOWER_SPEED (a foot landing on a stair well below needs a lot more of a sudden; walking, the least bit
            // more a frame, followed all but exactly); and back up smoothly as a foot that held it down lifts, as
            // people's goes: critically damped, not at a speed that starts and stops at once, a hop at every step)
            if (lower > g.lowered) {
                const float want = std::min(std::sqrt(2.f * LOWER_ACCEL * (lower - g.lowered)), LOWER_SPEED);
                g.loweredV       = std::min(want, std::max(g.loweredV, 0.f) + LOWER_ACCEL * dt);
                g.lowered        = std::min(lower, g.lowered + g.loweredV * dt);
            } else
                springTo(g.lowered, g.loweredV, lower, 22.f, dt);
            pelvis.y -= g.lowered - lower;
            lower = g.lowered;
            for (int s = 0; s < 2; ++s) {
                auto& ft = g.feet[s];
                if (ft.swing)
                    continue;
                const V3    hip   = pelvis + hipsW.rotate(g.hipAt[s]);
                const float reach = 0.995f * (g.thigh[s] + g.shin[s]);
                const float up    = hip.y - ankleW[s].y;
                V3          d{ankleW[s].x - hip.x, 0, ankleW[s].z - hip.z};
                const float most = std::sqrt(std::max(0.f, reach * reach - up * up)), dx = length(d);
                ft.strain        = dx / std::max(most, 1e-3f);
                ft.over          = dx - most;
                // (not into a stair's riser: where the ground is higher it stays, the leg short of it; up or down a
                // slope along it)
                if (const V3 pull = d * ((most - dx) / std::max(dx, 1e-4f)); dx > most + 1e-4f) {
                    const auto u = under(ft.at + pull, ft.yaw, s);
                    if (u.y < ft.at.y + 0.02f || (u.even && u.y < ft.at.y + 0.1f)) {
                        const V3 by = pull + V3{0, u.even && std::abs(u.y - ft.at.y) < 0.1f ? u.y - ft.at.y : 0.f, 0};
                        ft.at += by;
                        ankleW[s] += by;
                    }
                }
            }
            // a foot in the air up as far as the knee folds, landing where it goes (easing in and out as the stride
            // starts and stops)
            for (int s = 0; s < 2; ++s) {
                auto& ft = g.feet[s];
                springTo(ft.fold, ft.foldV, ft.timed ? 0.f : kneeW, 20.f, dt);
                if (!ft.swing) {
                    ft.raise = ft.raiseV = 0, ft.raiseTo = -1;
                    continue;
                }
                const V3    hip    = pelvis + hipsW.rotate(g.hipAt[s]);
                V3&         at     = ankleW[s];
                // (from as it lifted; less in a quick step)
                const float want   = lerpf(ft.liftKnee, kneeAt(s, through(WALK_SWING_KNEE, ft.s) / g.hurrySmooth), smoothstep01(ft.s / 0.2f));
                const float across = length(V3{at.x - hip.x, 0, at.z - hip.z});
                // (no higher than a step's: with the foot far behind, as going back, it doesn't kick up to the hip)
                const float y = want > across ? hip.y - std::sqrt(want * want - across * across) : hip.y, most = 0.12f * L;
                // Followed as it goes, but speeding up and slowing down no quicker than RAISE_ACCEL, and slowing down in
                // time for where it stops (on the swing's own height, or at `most`): stopping a hurried or cut-short
                // step, what the knee needs comes and goes faster than a foot moves, and the foot would stop dead
                // (onto it, as fast as it goes plus what closes the gap slowing down to it; not past it)
                const float to = std::clamp(y - at.y, 0.f, most), err = to - ft.raise, rdt = std::max(dt, 1e-4f);
                float       rv = (ft.raiseTo < 0 ? 0.f : (to - ft.raiseTo) / rdt) + std::copysign(std::sqrt(2.f * RAISE_ACCEL * std::abs(err)), err);
                rv             = err >= 0 ? std::min(rv, err / rdt) : std::max(rv, err / rdt);
                rv             = std::clamp(rv, -std::sqrt(2.f * RAISE_ACCEL * ft.raise), std::sqrt(2.f * RAISE_ACCEL * (most - ft.raise)));
                ft.raiseV      = std::clamp(rv, ft.raiseV - RAISE_ACCEL * dt, ft.raiseV + RAISE_ACCEL * dt);
                ft.raise       = std::clamp(ft.raise + ft.raiseV * dt, 0.f, most);
                ft.raiseTo     = to;
                at.y += ft.raise * ft.fold * (1.f - smoothstep01((ft.s - 0.75f) / 0.25f));
            }
        }

        // off the ground: the legs as the air's springs have them, from the hips as the pelvis lies, the feet along the
        // shins; hovering the body bobs, flying it rises a little as it lies down (its middle where a standing body's is);
        // coming down from a jump the pelvis goes as far down or up as puts the lower foot on the ground as the body lands
        if (!grounded) {
            const float bobF = (FLY_BOB * std::sin(TAU * 0.4f * g.time) + 0.006f * std::sin(TAU * 0.93f * g.time + 0.7f)) * hover;
            pelvis = P + UP * (g.hipH * lerpf(0.97f, 0.95f, fe) + fe * (bobF + 0.22f * g.hipH * std::sin(std::max(0.f, g.airPitch.x))));
            float lowest = 1e30f;
            for (int s = 0; s < 2; ++s) {
                const auto& al = g.airLeg[s];
                const float sx = s ? 1.f : -1.f, shinA = al.hip.x - al.knee.x, co = std::cos(al.out.x), so = std::sin(al.out.x);
                const V3    thighD{sx * so, -std::cos(al.hip.x) * co, -std::sin(al.hip.x) * co}, shinD{sx * so, -std::cos(shinA) * co, -std::sin(shinA) * co};
                ankleW[s] = pelvis + hipsW.rotate(g.hipAt[s] + thighD * g.thigh[s] + shinD * g.shin[s]);
                footW[s]  = hipsW * Quat::axisAngle({1, 0, 0}, shinA - al.toes.x);
                lowest    = std::min(lowest, ankleW[s].y - g.ankleH[s]);
            }
            if (const float over = (lowest - P.y) * (1.f - fe) * smoothstep01((ju - 0.55f) / 0.35f); std::abs(over) > 0) {
                pelvis.y -= over;
                for (auto& a : ankleW)
                    a.y -= over;
            }
            for (int s = 0; s < 2; ++s) {
                auto& ft = g.feet[s];
                ft.ankle = ankleW[s];
                ft.pitch = std::asin(std::clamp(-footW[s].rotate({0, 0, -1}).y, -1.f, 1.f)); // (toes down, as the landing takes it)
                ft.roll  = 0;
                ft.yaw   = yaw;
            }
        } else
            for (int s = 0; s < 2; ++s)
                g.feet[s].ankle = ankleW[s];
        g.pelvis = pelvis;
        for (int s = 0; s < 2; ++s) { // (and for the status: how far the knee bends)
            g.feet[s].knee = length(ankleW[s] - pelvis - hipsW.rotate(g.hipAt[s]));
            g.feet[s].heel = g.feet[s].pitch;
        }

        // into the frame above
        auto frameOf = [&](const V3& world) { return g.rig.facing.conj().rotate(g.fixInv.point(toAvatar.point(world)) - g.rig.hips) / g.rig.height + UP; };
        auto turnOf  = [&](const Quat& world) { return (g.toFrame * bodyQ.conj() * world * g.toFrame.conj()).normalized(); };

        CPoser p(g.body);
        p.body(hipsQ, frameOf(pelvis) - UP);
        // the trunk turns against the pelvis (the shoulders stay square) and leans ahead, more running, starting and
        // crouching; breathing
        // (off the ground, as the air's spring has it)
        const float accelLean = std::clamp(std::atan(fwdA / GRAVITY) * 0.6f, -0.12f, 0.3f) * std::min(1.f, mw + 0.5f);
        const float trunkLean = (grounded ? mw * lerpf(0.055f, 0.1f, ra) + accelLean + 0.3f * g.crouch - 0.6f * tiltP : g.airLean.x) +
            0.012f * std::sin(TAU * g.time / 4.f);
        if (grounded)
            g.leanWas = trunkLean - 0.012f * std::sin(TAU * g.time / 4.f);
        // (and back round from where the pelvis lags behind a turn, as far as the spine twists (TRUNK_TWIST, easing
        // into it), turning there critically damped: after the head, not whipping round with the body)
        {
            const float twist = std::clamp(lagP, -TRUNK_TWIST, TRUNK_TWIST), knee = 0.6f * TRUNK_TWIST;
            const float soft  = std::abs(twist) <= knee ? twist : std::copysign(knee + (TRUNK_TWIST - knee) * std::tanh((std::abs(lagP) - knee) / (TRUNK_TWIST - knee)), lagP);
            if (fresh)
                g.twist = soft, g.twistV = 0;
            springTo(g.twist, g.twistV, soft, TRUNK_W, dt);
        }
        const Quat  trunk     = Quat::axisAngle(UP, -yawP * lerpf(1.6f, 1.9f, ra) - g.twist) * Quat::axisAngle({0, 0, 1}, -0.8f * (rollP - lean)) *
                           Quat::axisAngle({1, 0, 0}, trunkLean);
        const auto& h         = md.human;
        const int   torso     = (h[HB_SPINE] >= 0) + (h[HB_CHEST] >= 0) + (h[HB_UPPER_CHEST] >= 0);
        Quat        trunkDone = hipsQ;
        // (the clip's: its trunk over its hips bone by bone, as far as this avatar has them, the top one up to the clip's
        // top; on it what the walking adds turning (the twist back), speeding up and crouching, and breathing)
        const int  top   = h[HB_UPPER_CHEST] >= 0 ? HB_UPPER_CHEST : h[HB_CHEST] >= 0 ? HB_CHEST : HB_SPINE;
        const Quat added = Quat::axisAngle(UP, -g.twist) * Quat::axisAngle({1, 0, 0}, accelLean + 0.15f * g.crouch + 0.012f * std::sin(TAU * g.time / 4.f));
        Quat       below = sw > 0 ? sT[HB_HIPS] : Quat{};
        for (int b : {HB_SPINE, HB_CHEST, HB_UPPER_CHEST})
            if (h[b] >= 0) {
                Quat part = slerp(Quat{}, trunk, 1.f / std::max(torso, 1));
                if (sw > 0) {
                    const Quat at = sAt(sT, b == top ? HB_UPPER_CHEST : b);
                    part          = slerp(part, below.conj() * at * slerp(Quat{}, added, 1.f / std::max(torso, 1)), sw);
                    below         = at;
                }
                p.bend(b, part);
                trunkDone = trunkDone * part;
            }
        // the head level, looking where it goes (the camera's look turns it after); flying lying down, as far as a neck
        // bends back (HEAD_MOST), looking a little down ahead past that
        // Turning, it looks ahead into the turn, where the body will face (HEAD_LEAD of what's left of the turn, up to
        // HEAD_LEAD_MOST radians): as people's head turns first, then the trunk, then the pelvis and the feet
        constexpr float HEAD_MOST = 0.9f;
        const float     headLead = grounded ? std::clamp(g.turnLeft * HEAD_LEAD, -HEAD_LEAD_MOST, HEAD_LEAD_MOST) : 0.f;
        Quat            level    = slerp(Quat{}, trunkDone.conj() * Quat::axisAngle(UP, -headLead), 0.85f);
        if (const float turned = 2.f * std::acos(std::clamp(std::abs(level.w), 0.f, 1.f)); turned > HEAD_MOST)
            level = slerp(Quat{}, level, HEAD_MOST / turned);
        Quat neck = slerp(Quat{}, level, 0.45f), head = slerp(Quat{}, level, 0.55f);
        if (sw > 0) {
            // (the clip's head (level, nodding and tilting with the steps) looking into the turn the same way; its neck
            // half way, else between its top and the head; no further round from the trunk than a neck turns)
            const Quat hc = Quat::axisAngle(UP, -headLead) * sT[HB_HEAD];
            const Quat nc = Quat::axisAngle(UP, -0.45f * headLead) * (sHas[HB_NECK] ? sT[HB_NECK] : slerp(sAt(sT, HB_UPPER_CHEST), sT[HB_HEAD], 0.5f));
            Quat       nl = trunkDone.conj() * nc, hl = nc.conj() * hc;
            if (const Quat all = nl * hl; 2.f * std::acos(std::clamp(std::abs(all.w), 0.f, 1.f)) > HEAD_MOST) {
                const float k = HEAD_MOST / (2.f * std::acos(std::clamp(std::abs(all.w), 0.f, 1.f)));
                nl = slerp(Quat{}, nl, k), hl = slerp(Quat{}, hl, k);
            }
            neck = slerp(neck, nl, sw), head = slerp(head, hl, sw);
        }
        p.bend(HB_NECK, neck);
        p.bend(HB_HEAD, head);

        // the arms swing against the legs (the left ahead as the right foot lands), a little after them; walking
        // hanging, the elbows bent more as they come ahead; running bent near square, pumping
        const float armAmp = mw * lerpf(0.16f + 0.1f * std::min(v, 2.f), 0.55f, ra);
        // an arm swung that far (radians ahead), out from the body that far: the upper arm and the forearm (the chest's
        // frame). The forearm bent ahead from the upper arm, in the plane the arm swings in; running the hands come in
        // toward the middle as they come ahead
        // (off the ground, as the air's springs have it: swung, the elbow's bend)
        auto aheadOf = [&](int s, float swing) { return grounded ? swing + lerpf(0.04f, -0.08f, ra) * mw + 0.25f * g.crouch : swing; };
        auto bendOf  = [&](int s, float swing) {
            const float fore = armAmp > 1e-3f ? swing / armAmp : 0.f;
            return grounded ? lerpf(0.24f + (0.1f + 0.18f * (1.f + fore)) * mw, 1.45f + 0.28f * fore * mw, ra) + 0.25f * g.crouch : g.airArm[s].bend.x;
        };
        auto armAt = [&](int s, float swing, float out) {
            const float sx    = s ? -1.f : 1.f;
            const float ahead = aheadOf(s, swing), bend = bendOf(s, swing);
            const float fore  = armAmp > 1e-3f ? swing / armAmp : 0.f;
            const V3    u     = Quat::axisAngle({1, 0, 0}, -ahead).rotate({sx * std::sin(out), -std::cos(out), 0});
            V3          f     = Quat::axisAngle({1, 0, 0}, -bend).rotate(u);
            if (grounded)
                f.x -= sx * 0.35f * 1.3f * smoothstep01((fore + 0.3f) / 1.3f) * ra * mw;
            return std::pair{u, normalize(f)};
        };
        // How far an arm so posed goes into the body (a skirt, a coat), meters, < 0 clear of it: its elbow, forearm and
        // hand (their skin, a sleeve half counted: it gives) against how far out the body goes at rest round the hips
        const SBodyClearance& cl = md.clearance;
        auto into = [&](int s, const std::pair<V3, V3>& arm) {
            const SBody& b   = p.measure();
            const Quat   top = p.turnAt(HB_UPPER_CHEST), hq = p.turnAt(HB_HIPS);
            const V3     hip = UP + p.move, elbow = p.jointAt(s ? HB_R_UPPER_ARM : HB_L_UPPER_ARM) + top.rotate(arm.first) * b.upper[s];
            const V3     fore = top.rotate(arm.second);
            float        most = -1e30f;
            auto         at   = [&](const V3& q, float r) { // (a point of the arm, the frame above, its skin r meters round it)
                const V3    a   = g.toFrame.conj().rotate(hq.conj().rotate(q - hip)) * g.unit;
                const float rho = std::hypot(a.x, a.z);
                const int   i0 = (int)std::floor((a.y - r - cl.y0) / cl.dy), i1 = (int)std::floor((a.y + r - cl.y0) / cl.dy);
                const int   k   = (int)std::floor((std::atan2(a.z, a.x) + PI) / TAU * cl.bearings);
                const int   dk  = 1 + (int)(std::asin(std::clamp(r / std::max(rho, 1e-3f), 0.f, 1.f)) / TAU * cl.bearings);
                float       R   = 0;
                for (int i = std::max(i0, 0); i <= std::min(i1, cl.rows - 1); ++i)
                    for (int d = -dk; d <= dk; ++d)
                        R = std::max(R, cl.out[(size_t)i * cl.bearings + ((k + d) % cl.bearings + cl.bearings) % cl.bearings]);
                if (R > 0)
                    most = std::max(most, R + 0.01f + r - rho);
            };
            const auto& ar = cl.arm[s];
            at(elbow, ar[0][3]);
            for (int q = 0; q < 4; ++q)
                at(elbow + fore * (b.fore[s] * (q + 0.5f) / 4.f), 0.5f * ar[1][q]);
            const V3 wrist = elbow + fore * b.fore[s];
            for (int q = 0; q < 4; ++q)
                at(wrist + fore * (cl.hand[s] / g.unit * (q + 1) / 4.f), ar[2][q]);
            return most;
        };
        // the least an arm swung that far goes out to be clear of it
        auto clearOut = [&](int s, float swing, float out) {
            if (into(s, armAt(s, swing, out)) <= 0)
                return out;
            float lo = out, hi = out + 0.6f;
            if (into(s, armAt(s, swing, hi)) > 0)
                return hi;
            for (int i = 0; i < 10; ++i)
                (into(s, armAt(s, swing, 0.5f * (lo + hi))) > 0 ? lo : hi) = 0.5f * (lo + hi);
            return hi;
        };
        // the clip's arm (the walk's and the run's: see above) in the chest's frame, as its own chest had it: the upper
        // arm, the forearm, the hand along and its palm; turned out from the body as far again (about the chest's ahead)
        auto clipArm = [&](int s, const std::array<Quat, HB_COUNT>& T) {
            const Quat c  = sAt(T, HB_UPPER_CHEST).conj();
            const int  ua = s ? HB_R_UPPER_ARM : HB_L_UPPER_ARM;
            const V3   t0{s ? -1.f : 1.f, 0, 0};
            const Quat hq = c * T[ua + 2];
            return std::array<V3, 4>{(c * T[ua]).rotate(t0), (c * T[ua + 1]).rotate(t0), hq.rotate(t0), hq.rotate({0, -1, 0})};
        };
        auto turnedOut = [](int s, float a, std::array<V3, 4> arm) {
            const Quat q = Quat::axisAngle({0, 0, 1}, s ? -a : a);
            for (auto& d : arm)
                d = q.rotate(d);
            return arm;
        };
        // (how far out it has to be turned to be clear of the body, as clearOut has it of the walking's own)
        // (up to 1.2 radians: an arm brought in to this avatar's own hang from a clip made on one in a wide skirt may need
        // most of that back)
        auto clipOut = [&](int s, const std::array<V3, 4>& arm) {
            if (!cl.measured || into(s, {arm[0], arm[1]}) <= 0)
                return 0.f;
            float lo = 0, hi = 1.2f;
            if (const auto o = turnedOut(s, hi, arm); into(s, {o[0], o[1]}) > 0)
                return hi;
            for (int i = 0; i < 10; ++i) {
                const float mid = 0.5f * (lo + hi);
                const auto  o   = turnedOut(s, mid, arm);
                (into(s, {o[0], o[1]}) > 0 ? lo : hi) = mid;
            }
            return hi;
        };
        for (int s = 0; s < 2; ++s) {
            const float sx    = s ? -1.f : 1.f;
            const float swing = grounded ? -armAmp * std::cos(TAU * (phL + 0.5f * s - lerpf(0.06f, 0.03f, ra))) : g.airArm[s].ahead.x;
            float       out   = grounded ? g.armOut + 0.03f * std::sin(TAU * g.time / 5.f + s) * (1.f - mw) : g.airArm[s].out.x;
            SArm        clip{};
            V3          clipWas;
            if (sw > 0) {
                // The clip's arm: hanging as far out from the body as the walking's own does (for this avatar's hips: the
                // clip's are as far out as the avatar it was made on needed, in or out from that), then as far out again as
                // the whole stride needs to keep it clear of the body (a skirt; eased in slower than out); as it's swung
                // now only past that (critically damped): the body is known by 5° round it, and an arm going in and out
                // with each swing at once jitters
                const V3    um   = clipArm(s, sMean)[0];
                const float base = g.armOut - std::asin(std::clamp(sx * um.x, -1.f, 1.f));
                // (the stride at the same steps of it: walking steadily, the same; a third of them looked at again a frame,
                // the time that takes; followed critically damped, quicker out than in)
                float cycle = 0;
                if (cl.measured) {
                    auto& need = g.clipNeed[s];
                    for (int k = 0; k < GAIT_NEED_STEPS; ++k)
                        if (fresh || k % GAIT_NEED_EVERY == g.clipTick % GAIT_NEED_EVERY) {
                            std::array<Quat, HB_COUNT> T;
                            V3                         mv;
                            styleAt((float)k / GAIT_NEED_STEPS, T, mv);
                            need[k] = clipOut(s, turnedOut(s, base, clipArm(s, T)));
                        }
                    cycle = *std::ranges::max_element(need);
                }
                if (fresh)
                    g.clipClear[s] = cycle, g.clipClearV[s] = 0;
                else
                    springTo(g.clipClear[s], g.clipClearV[s], cycle, cycle > g.clipClear[s] ? 10.f : 4.f, dt);
                const auto arm = turnedOut(s, base, clipArm(s, sT));
                if (const float now = std::max(0.f, clipOut(s, arm) - g.clipClear[s]); fresh)
                    g.clipPush[s] = now, g.clipPushV[s] = 0;
                else
                    springTo(g.clipPush[s], g.clipPushV[s], now, ARM_PUSH_W, dt);
                const auto o = turnedOut(s, g.clipClear[s] + std::max(0.f, g.clipPush[s]), arm);
                clip         = p.armOf(s, o[0], o[1], o[2], o[3]);
                clipWas      = {std::atan2(o[0].z, -o[0].y), std::asin(std::clamp(sx * o[0].x, -1.f, 1.f)), std::acos(std::clamp(dot(o[0], o[1]), -1.f, 1.f))};
                if (sw >= 0.999f) {
                    g.armWas[s] = clipWas;
                    p.arm(s, clip);
                    continue;
                }
            }
            if (grounded) // (the air's arms go on from here; kept clear of a skirt as these are)
                g.armWas[s] = lerp(V3{aheadOf(s, swing), out, bendOf(s, swing)}, clipWas, sw);
            if (cl.measured) {
                // out as far as it keeps clear of a skirt: as it's swung now, and held out as far as the whole swing
                // needs (not in and out with each swing), easing in slower than out
                float cycle = 0;
                for (float w : {-1.f, -0.75f, -0.5f, -0.25f, 0.f, 0.25f, 0.5f, 0.75f, 1.f})
                    if (armAmp > 0.01f || w == 0.f)
                        cycle = std::max(cycle, clearOut(s, w * armAmp, out) - out);
                g.armClear[s] = approach(g.armClear[s], cycle, dt * (cycle > g.armClear[s] ? 2.f : 0.6f));
                // (and as it's swung now, critically damped at ARM_PUSH_W: how far out the body goes is known by 5° round
                // it, and at once the arm would jump out a step's worth as it passes from one to the next)
                if (const float now = clearOut(s, swing, out) - out; fresh)
                    g.armPush[s] = now, g.armPushV[s] = 0;
                else
                    springTo(g.armPush[s], g.armPushV[s], now, ARM_PUSH_W, dt);
                out += std::max(g.armClear[s], g.armPush[s]);
            }
            const auto [u, f] = armAt(s, swing, out);
            if (sw > 0)
                p.arm(s, p.armOf(s, u, f, f, {-sx, -0.35f, 0.1f}), clip, sw);
            else
                p.arm(s, p.armOf(s, u, f, f, {-sx, -0.35f, 0.1f}));
        }
        ++g.clipTick;
        // the collarbones as the clip has them, on the chest
        if (sw > 0)
            for (int sh : {HB_L_SHOULDER, HB_R_SHOULDER})
                if (h[sh] >= 0 && sHas[sh]) {
                    const Quat on = p.turnAt(HB_UPPER_CHEST);
                    p.turn[sh]    = slerp(on, on * (sAt(sT, HB_UPPER_CHEST).conj() * sT[sh]), sw).normalized();
                    p.set[sh]     = true;
                }

        // the legs to the feet, the knees over the toes (off the ground, ahead of the pelvis as it lies; the toes along
        // the foot)
        for (int s = 0; s < 2; ++s) {
            const Quat foot  = turnOf(footW[s]);
            const Quat level = turnOf(yawTurn(std::atan2(footW[s].rotate({0, 0, -1}).x, -footW[s].rotate({0, 0, -1}).z)));
            const V3   knee  = (grounded ? level : hipsQ).rotate({0, 0, 1}) + (grounded ? Quat{} : hipsQ).rotate(V3{s ? -0.12f : 0.12f, 0, 0});
            p.legTo(s, frameOf(ankleW[s]), foot * g.footUntilt[s], knee);
            if (h[s ? HB_R_TOES : HB_L_TOES] >= 0) // (on the ground as the heel rises)
                p.toes(s, grounded ? level * Quat::axisAngle({1, 0, 0}, std::min(g.feet[s].pitch, std::max(0.f, g.feet[s].pitch - 0.9f))) * g.footUntilt[s]
                                   : foot * g.footUntilt[s]);
        }
        p.finish();
        std::array<bool, HB_COUNT> set{};
        for (int b = 0; b < HB_FINGERS; ++b)
            set[b] = b != HB_L_EYE && b != HB_R_EYE && b != HB_JAW && (b != HB_L_TOES || p.set[b]) && (b != HB_R_TOES || p.set[b]);
        applyFrame(g, p.turn, set, p.move, pose);
    }

    std::string CAvatarAnimator::gaitStatus() const {
        if (!m_gait || !m_gaitUsed)
            return "null";
        const SGait& g = gaitOf(m_gait);
        std::string  feet;
        // how far the knee bends, degrees, from how far the ankle is from the hip
        auto kneeBend = [&](int s) {
            const float a = g.thigh[s], b = g.shin[s], d = std::clamp(g.feet[s].knee, std::abs(a - b), a + b);
            return 180.f - std::acos(std::clamp((a * a + b * b - d * d) / (2 * a * b), -1.f, 1.f)) * 180.f / PI;
        };
        for (int s = 0; s < 2; ++s) {
            const auto& ft = g.feet[s];
            feet += std::format(R"({}{{"planted": {}, "step": {:.3f}, "pitch": {:.3f}, "yaw": {:.1f}, "strain": {:.2f}, "tight": {:.2f}, "knee": {:.0f}, "at": [{:.3f}, {:.3f}, {:.3f}], "ankle": [{:.3f}, {:.3f}, {:.3f}]}})",
                                s ? ", " : "", !ft.swing, ft.swing ? ft.s : 0.f, ft.pitch, ft.yaw * 180.f / PI, ft.swing ? 0.f : ft.strain, ft.swing ? 0.f : ft.tight, kneeBend(s), ft.at.x, ft.at.y, ft.at.z, ft.ankle.x, ft.ankle.y, ft.ankle.z);
        }
        // off the ground: the leg that went ahead, how fast it went, flying (eased), how the body lies (degrees: pitched
        // ahead, rolled right), each leg's hip and knee and each arm's swing and spread (degrees)
        std::string air = "null";
        if (!g.grounded) {
            constexpr float D = 180.f / PI;
            air = std::format(R"({{"lead": {}, "run": {:.2f}, "fly": {:.2f}, "pitch": {:.1f}, "roll": {:.1f}, "legs": [[{:.0f}, {:.0f}], [{:.0f}, {:.0f}]], "arms": [[{:.0f}, {:.0f}], [{:.0f}, {:.0f}]]}})",
                              g.airLead, g.airRun, g.flyE.x, g.airPitch.x * D, g.airRoll.x * D, g.airLeg[0].hip.x * D, g.airLeg[0].knee.x * D, g.airLeg[1].hip.x * D,
                              g.airLeg[1].knee.x * D, g.airArm[0].ahead.x * D, g.airArm[0].out.x * D, g.airArm[1].ahead.x * D, g.airArm[1].out.x * D);
        }
        // (what the legs carry: where it is and how fast it goes, x z)
        return std::format(R"({{"moving": {}, "speed": {:.3f}, "run": {:.2f}, "phase": {:.3f}, "stride": {:.3f}, "cadence": {:.3f}, "duty": {:.3f}, "leg": {:.3f}, "pelvis": [{:.3f}, {:.3f}, {:.3f}], "ground": {:.3f}, "slope": {:.2f}, "lowered": {:.3f}, )"
                           R"("hipYaw": {:.1f}, "carry": [{:.3f}, {:.3f}, {:.3f}, {:.3f}], "feet": [{}], "air": {}, "body": {{"hips": {:.3f}, "thigh": {:.3f}, "shin": {:.3f}, "ankle": {:.3f}, "hipJoint": [{:.3f}, {:.3f}, {:.3f}], "stance": {:.3f}, )"
                           R"("armOut": {:.3f}}}}})",
                           g.moving, length(g.lastVel), g.run, g.phase, g.stride, g.cadence, g.duty, g.leg, g.pelvis.x, g.pelvis.y, g.pelvis.z, g.groundY, g.slope, g.lowered, wrapPi(g.lastYaw + g.hipLag) * 180.f / PI,
                           g.bodyP.x, g.bodyP.z, g.bodyV.x, g.bodyV.z, feet, air, g.hipH, g.thigh[0], g.shin[0], g.ankleH[0], g.hipAt[0].x, g.hipAt[0].y, g.hipAt[0].z, g.stance, g.armOut);
    }

    // --- animation

    void CAvatarAnimator::reset(std::shared_ptr<const SAvatarModel> model) {
        m_model = std::move(model);
        m_pose.clear();
        m_joints.clear();
        m_source   = {};
        m_state    = ST_IDLE;
        m_fade     = 1;
        m_clipTime = m_time = m_airTime = m_lift = 0;
        m_first    = true;
        m_gait.reset();
        m_gaitUsed = false;
        m_flying = m_flewOff = false;
        m_settleR.clear(), m_settleRV.clear(), m_settleT.clear(), m_settleTV.clear();
        m_settleNow.clear(), m_settleWas.clear();
        m_settleRate = m_settleAsk = m_settleDt = 0;
        m_settleLift = m_settleLiftWas = 0;
        m_settleRootSet = m_settleJust = m_settleFresh = false;
        m_settleNew.clear();
        m_exprW.clear();
        m_exprOut.clear();
        m_matOut.clear();
        m_morphW.clear();
        m_materials.clear();
        m_held       = -1;
        m_heldWeight = 1;
        m_blinkIn    = 2;
        m_blinkT     = -1;
        m_blinkAgain = false;
        m_saccadeIn  = 1;
        m_saccadeYaw = m_saccadePitch = 0;
        m_toggles.clear();
        m_sliders.clear();
        m_slidersY.clear();
        m_partSet.clear();
        m_shapeSet.clear();
        m_shown.clear();
        m_shapeBase.clear();
        m_variantOn.clear();
        m_batchMat.clear();
        m_nodePose.clear();
        m_loops.clear();
        m_dropBy.clear();
        m_dropAt.clear();
        m_dropTake.clear();
        m_springOf.clear();
        m_tail.clear();
        m_tailPrev.clear();
        m_carried.clear();
        m_again.clear();
        m_carrierAt.clear();
        m_carrierMove.clear();
        m_springGlobal.clear();
        m_centerAt.clear();
        m_centerInv.clear();
        m_colliderAt.clear();
        m_springUp.clear();
        m_springWas.clear();
        m_springPose.clear();
        m_springAcc  = 0;
        m_springLive = false;
        m_emotes.clear();
        m_emote     = -1;
        m_emoteTime = m_emoteW = 0;
        m_emoteLoop = m_emoteOut = false;
        m_emoteClock = -1;
        m_emoteSwap = 1;
        m_emoteNow.clear();
        m_emoteFrom.clear();
        m_emoteFace.clear();
        m_emoteEyes = 0;
        m_fpW = m_fpArmsW = m_fpTrunkW = 0;
        m_fpLive    = false;
        m_fpPoke    = 1;
        m_fpGesture = {-1, -1};
        m_swing = m_swingWas = {};
        m_swingCross = 1;
        m_attackW    = 0;
        m_swingNext  = -1;
        m_swingLast  = 0;
        m_swingAgo   = 1e9f;
        m_swings     = 0;
        m_swingFist  = {};
        m_attackPart.clear();
        m_attackPose.clear();
        m_attackWas.clear();
        if (!m_model)
            return;
        const auto& md = *m_model;
        m_emotes       = md.emotes;
        m_emoteFace.assign(md.expressions.size(), 0.f);
        m_exprW.assign(md.expressions.size(), 0.f);
        m_exprOut.assign(md.expressions.size(), 0.f);
        m_matOut.assign(md.expressions.size(), -1.f); // makes them the first time
        resetOutfit();
        m_morphW = m_shapeBase;
        if (std::ranges::any_of(md.expressions, [](const SExpression& e) { return !e.materials.empty(); }))
            m_materials = md.materials;
        for (const auto& n : md.nodes)
            m_pose.push_back(n.rest);
        m_from = m_target = m_pose;
        globals(m_pose, m_global);
        m_restGlobal = m_global;
        m_restGlobalRot.resize(md.nodes.size());
        for (size_t i = 0; i < md.nodes.size(); ++i)
            m_restGlobalRot[i] = rotationOf(m_global[i]);
        m_restFootY = lowestFoot(m_global);
        m_joints.assign(md.joints.size() * 12, 0.f);
        fingerAxes();
        // what the attacks' clips have of the body
        m_attackPart.assign(md.nodes.size(), ATTACK_NONE);
        for (int b = 0; b < HB_COUNT; ++b)
            if (const int n = md.human[b]; n >= 0)
                for (const auto* set : {&md.attacks, &md.attacksFirst})
                    for (const auto& at : *set)
                        for (const auto& c : at.anim.channels)
                            if (c.node == n && c.path == PATH_R && (attackArmBone(b) || attackTrunkBone(b)))
                                m_attackPart[n] = attackArmBone(b) ? ATTACK_ARM : ATTACK_TRUNK;
        m_attackPose = m_attackWas = m_pose;

        // what the springs move: their bones and all under them
        m_springOf.assign(md.nodes.size(), -1);
        for (size_t j = 0; j < md.springJoints.size(); ++j)
            m_springOf[md.springJoints[j].node] = (int)j;
        m_springFrom = (int)md.nodes.size();
        for (size_t i = 0; i < md.nodes.size(); ++i) {
            if (const int p = md.nodes[i].parent; m_springOf[i] == -1 && p >= 0 && m_springOf[p] != -1)
                m_springOf[i] = -2;
            if (m_springOf[i] != -1)
                m_springFrom = std::min(m_springFrom, (int)i);
        }
        m_tail.assign(md.springJoints.size(), V3{});
        m_tailPrev = m_tail;
        m_carried  = m_tail;
        m_again    = m_tail;
        m_carrierAt.assign(md.springs.size(), M4::identity());
        m_carrierMove = m_carrierAt;
        m_springGlobal.assign(md.nodes.size(), M4::identity());
        m_centerAt.assign(md.springs.size(), M4::identity());
        m_centerInv = m_centerAt;
        m_colliderAt.assign(md.springColliders.size(), {});
        // what the springs go by of the pose: what their roots hang from, their centers, carriers and colliders, and all
        // above those (to have them between two frames, springPoseAt())
        std::vector<char> up(md.nodes.size(), 0);
        auto              mark = [&](int n) {
            for (; n >= 0 && !up[n]; n = md.nodes[n].parent)
                up[n] = 1;
        };
        for (size_t i = 0; i < md.nodes.size(); ++i)
            if (const int p = md.nodes[i].parent; m_springOf[i] != -1 && p >= 0 && m_springOf[p] == -1)
                mark(p);
        for (const auto& sp : md.springs) {
            mark(sp.center);
            mark(sp.carrier);
        }
        for (const auto& k : md.springColliders)
            mark(k.node);
        for (size_t i = 0; i < md.nodes.size(); ++i)
            if (up[i])
                m_springUp.push_back((int)i);
        m_springPose.assign(md.nodes.size(), M4::identity());
    }

    CAvatarAnimator::SSource CAvatarAnimator::choose(eState st, const SAvatarMotion& m) const {
        const auto& md   = *m_model;
        const auto& clip = md.clipFor;
        switch (st) {
            case ST_AIR: {
                const int c = m.vy > 0.5f && clip[CLIP_JUMP] >= 0 ? clip[CLIP_JUMP] : clip[CLIP_FALL] >= 0 ? clip[CLIP_FALL] : clip[CLIP_JUMP];
                if (c >= 0)
                    return {SRC_CLIP, c, false, c == clip[CLIP_FALL]};
                if (md.humanoid)
                    return {SRC_PROC};
                return m_source; // nothing better: keep going
            }
            case ST_IDLE:
                if (clip[CLIP_IDLE] >= 0)
                    return {SRC_CLIP, clip[CLIP_IDLE]};
                if (md.humanoid)
                    return {SRC_PROC};
                if (clip[CLIP_WALK] >= 0 || clip[CLIP_RUN] >= 0)
                    return {SRC_CLIP, clip[CLIP_WALK] >= 0 ? clip[CLIP_WALK] : clip[CLIP_RUN], true};
                return {SRC_REST};
            case ST_WALK:
            case ST_RUN: {
                const int want = st == ST_WALK ? clip[CLIP_WALK] : clip[CLIP_RUN], other = st == ST_WALK ? clip[CLIP_RUN] : clip[CLIP_WALK];
                if (want >= 0 || other >= 0)
                    return {SRC_CLIP, want >= 0 ? want : other};
                if (md.humanoid)
                    return {SRC_PROC};
                return {SRC_REST};
            }
        }
        return {};
    }

    // `d` is a rotation in model space, applied to the bone relative to its parent's rest orientation
    void CAvatarAnimator::rotateBone(std::vector<STRS>& pose, int bone, const Quat& d) const {
        const int n = m_model->human[bone];
        if (n < 0)
            return;
        const int  p  = m_model->nodes[n].parent;
        const Quat gp = p >= 0 ? m_restGlobalRot[p] : Quat{};
        pose[n].r     = (gp.conj() * d * gp * pose[n].r).normalized();
    }

    namespace {
        // the gestures' hands, as VRChat's
        struct SHandPreset {
            std::array<float, FINGER_COUNT>     curl;   // thumb..little: 1 = a fist
            std::array<float, FINGER_COUNT - 1> spread; // index..little, toward the thumb
            float                               fold;   // the thumb's first bone, across the palm
            std::array<float, 3>                aim;    // where the thumb's other two point (along, across, out of the palm), or 0 to curl
            int                                 hold;   // the finger the thumb lies over, its tip over the next; else -1
        };
        constexpr SHandPreset HAND_POSES[GESTURE_COUNT] = {
            {{0.25f, 0.3f, 0.36f, 0.42f, 0.48f}, {0.06f, 0, -0.06f, -0.12f}, 0.2f, {}, -1}, // neutral: relaxed, curled more toward the little finger
            {{0, 1, 1, 1, 1}, {0, 0, 0, 0}, 0, {}, FINGER_INDEX},                           // fist
            {{0, 0, 0, 0, 0}, {0.5f, 0.1f, -0.3f, -0.6f}, -0.2f, {0.65f, 0.75f, -0.05f}, -1}, // open
            {{0, 0, 1, 1, 1}, {0, 0, 0, 0}, 0, {}, FINGER_MIDDLE},                          // point
            {{0, 0, 0, 1, 1}, {0.6f, -0.3f, 0, 0}, 0, {}, FINGER_RING},                     // victory
            {{0, 0, 1, 1, 0}, {0.2f, 0, 0, -0.3f}, 0, {}, FINGER_MIDDLE},                   // rock'n'roll
            {{0, 0, 1, 1, 1}, {0, 0, 0, 0}, -0.2f, {0.4f, 1, -0.1f}, -1},                   // handgun
            {{0, 1, 1, 1, 1}, {0, 0, 0, 0}, -0.2f, {0.3f, 1, -0.15f}, -1},                  // thumbs up
        };
        constexpr float DEG = 0.0174532925f;
        // how far each bone turns at 1: a finger's three; the thumb's first by fold, the other two by curl
        constexpr float SEG[3] = {80 * DEG, 95 * DEG, 60 * DEG}, THUMB[3] = {45 * DEG, 35 * DEG, 60 * DEG}, SPREAD = 12 * DEG;
    }

    // the ways each finger bends, from the hand at rest: along it out to the fingers, across it from the
    // little finger to the index, and out of the palm; then the thumb for each gesture
    void CAvatarAnimator::fingerAxes() {
        const auto& md = *m_model;
        m_handRig      = {};
        m_clipFingers.assign(md.clips.size(), 0);
        if (!md.fingers)
            return;
        std::vector<int> child(md.nodes.size(), -1);
        for (int i = (int)md.nodes.size(); i-- > 0;)
            if (md.nodes[i].parent >= 0)
                child[md.nodes[i].parent] = i;
        auto at = [&](int n) { return origin(m_restGlobal[n]); };
        for (int hand = 0; hand < 2; ++hand) {
            const int h = md.human[hand == 0 ? HB_L_HAND : HB_R_HAND];
            if (h < 0)
                continue;
            auto bone = [&](int f, int s) { return md.human[fingerBone(hand, f, s)]; };
            V3   mid{};
            int  count = 0, first = -1, last = -1;
            for (int f = FINGER_INDEX; f < FINGER_COUNT; ++f)
                if (bone(f, 0) >= 0) {
                    mid += at(bone(f, 0));
                    ++count;
                    first = first < 0 ? f : first;
                    last  = f;
                }
            if (!count)
                continue;
            mid            = mid * (1.f / count);
            const V3 along = normalize(mid - at(h));
            V3       across{};
            for (int s = 0; s < 3 && first != last && length(across) == 0; ++s) // the knuckles, else further out
                if (bone(first, s) >= 0 && bone(last, s) >= 0) {
                    const V3 v = at(bone(first, s)) - at(bone(last, s));
                    if (const V3 o = v - along * dot(v, along); length(o) > 0.15f * length(mid - at(h)))
                        across = normalize(o);
                }
            if (length(across) == 0) // palms down, the index in front
                across = normalize(md.forward - along * dot(md.forward, along));
            if (length(across) == 0 || length(along) == 0)
                continue;
            const V3 palm    = cross(along, across) * (hand == 0 ? 1.f : -1.f);
            const V3 thumbTo = normalize(palm * 0.5f - across * 0.85f);
            for (int f = 0; f < FINGER_COUNT; ++f) {
                V3 d = f == FINGER_THUMB ? normalize(along + across) : along;
                for (int s = 0; s < 3; ++s) {
                    const int b = bone(f, s);
                    if (b < 0)
                        break;
                    const int next = s < 2 && bone(f, s + 1) >= 0 ? bone(f, s + 1) : child[b];
                    if (next >= 0 && length(at(next) - at(b)) > 1e-6f)
                        d = normalize(at(next) - at(b));
                    auto& ax  = m_fingerAxes[hand][f * 3 + s];
                    ax.curl   = normalize(cross(d, f == FINGER_THUMB ? thumbTo : palm));
                    ax.spread = normalize(cross(d, across));
                }
            }
            m_handRig[hand] = true;

            // the thumb: turned as the preset says, or laid over the fingers it holds down
            auto whole = [&](int f) { return bone(f, 0) >= 0 && bone(f, 1) >= 0 && bone(f, 2) >= 0; };
            auto joint = [&](int f, int s) { // at rest: the finger's root, its next two joints, its tip
                if (s < 3)
                    return at(bone(f, s));
                const int b = bone(f, 2);
                return child[b] >= 0 ? at(child[b]) : at(b) + (at(b) - at(bone(f, 1))) * 0.8f;
            };
            auto restDir = [&](int s) { return normalize(joint(FINGER_THUMB, s + 1) - joint(FINGER_THUMB, s)); };
            for (int g = 0; g < GESTURE_COUNT; ++g) {
                const SHandPreset& p   = HAND_POSES[g];
                auto&              out = m_thumbPose[hand][g];
                for (int s = 0; s < 3; ++s)
                    out[s] = Quat::axisAngle(m_fingerAxes[hand][s].curl, (s == 0 ? p.fold : p.curl[FINGER_THUMB]) * THUMB[s]);
                if (!whole(FINGER_THUMB))
                    continue;
                V3 to[3] = {};
                if (p.hold >= 0 && whole(p.hold) && whole(p.hold + 1)) {
                    // over the middle of a curled finger's middle bone, on its back
                    auto over = [&](int f) {
                        Quat w{};
                        V3   j[3] = {joint(f, 0)};
                        for (int s = 0; s < 2; ++s) {
                            const SFingerAxes& ax = m_fingerAxes[hand][f * 3 + s];
                            w = w * Quat::axisAngle(ax.curl, p.curl[f] * SEG[s]) * (s == 0 ? Quat::axisAngle(ax.spread, p.spread[f - 1] * SPREAD) : Quat{});
                            j[s + 1] = j[s] + w.rotate(joint(f, s + 1) - joint(f, s));
                        }
                        return (j[1] + j[2]) * 0.5f - w.rotate(palm) * (0.45f * length(joint(f, 1) - joint(f, 0)));
                    };
                    const V3    ip = over(p.hold), tip = over(p.hold + 1), p0 = joint(FINGER_THUMB, 0);
                    const V3    rest1 = joint(FINGER_THUMB, 1) - p0, c = ip - p0;
                    const float L0 = length(rest1), L1 = length(joint(FINGER_THUMB, 2) - joint(FINGER_THUMB, 1)), dist = length(c);
                    // its first joint L0 from its root and L1 from where the next goes, toward the palm: opposed
                    V3 p1 = p0 + rest1;
                    if (dist >= L0 + L1)
                        p1 = p0 + c * (L0 / dist);
                    else if (dist > std::abs(L0 - L1)) {
                        const V3    u = c * (1.f / dist), pref = palm * 0.5f + across, side = pref - u * dot(pref, u);
                        const float a = (L0 * L0 - L1 * L1 + dist * dist) / (2 * dist);
                        p1 = p0 + u * a + (length(side) > 1e-6f ? normalize(side) : perpendicular(u)) * std::sqrt(std::max(0.f, L0 * L0 - a * a));
                    }
                    const V3 p2 = p1 + normalize(ip - p1) * L1;
                    to[0] = p1 - p0;
                    to[1] = p2 - p1;
                    to[2] = tip - p2;
                } else if (p.aim[0] != 0 || p.aim[1] != 0 || p.aim[2] != 0) { // its first bone folded, the rest pointing
                    to[0] = out[0].rotate(restDir(0));
                    to[1] = to[2] = along * p.aim[0] + across * p.aim[1] + palm * p.aim[2];
                } else
                    continue;
                Quat w{};
                for (int s = 0; s < 3; ++s) { // each bone turned from where its parent carries it
                    const Quat ws = length(to[s]) > 1e-6f ? arc(w.rotate(restDir(s)), normalize(to[s])) * w : w;
                    out[s]        = (w.conj() * ws).normalized();
                    w             = ws;
                }
            }
        }

        for (size_t c = 0; c < md.clips.size(); ++c)
            m_clipFingers[c] = movesFingers(md, md.clips[c]);
        for (int hand = 0; hand < 2; ++hand) {
            const SHandPreset& p = HAND_POSES[GESTURE_NEUTRAL];
            m_hand[hand]         = {};
            for (int f = 1; f < FINGER_COUNT; ++f) {
                m_hand[hand].curl[f]   = p.curl[f];
                m_hand[hand].spread[f] = p.spread[f - 1];
            }
            m_hand[hand].thumb = m_thumbPose[hand][GESTURE_NEUTRAL];
            m_handW[hand]      = 0;
        }
    }

    // the fingers as the hand's gesture (or the emote's) has them, eased in; over the animation's unless the hand
    // is neutral
    void CAvatarAnimator::hands(float dt, std::vector<STRS>& pose) {
        const auto&         md   = *m_model;
        const float         k    = 1.f - std::exp(-dt * 25.f);
        const SAvatarEmote* em   = m_emote >= 0 && !m_emoteOut ? m_emotes[m_emote].get() : nullptr;
        const bool          clip = em ? em->fingers : m_source.kind == SRC_CLIP && m_clipFingers[m_source.clip];
        for (int hand = 0; hand < 2; ++hand) {
            if (!m_handRig[hand])
                continue;
            const int          g    = m_swingFist[hand]                ? (int)GESTURE_FIST
                               : em && em->gesture[hand] >= 0 ? em->gesture[hand]
                               : m_fpGesture[hand] >= 0       ? m_fpGesture[hand]
                                                              : m_gesture[hand];
            const SHandPreset& want = HAND_POSES[g];
            SHandPose&         cur  = m_hand[hand];
            for (int f = 1; f < FINGER_COUNT; ++f) {
                cur.curl[f] += (want.curl[f] - cur.curl[f]) * k;
                cur.spread[f] += (want.spread[f - 1] - cur.spread[f]) * k;
            }
            for (int s = 0; s < 3; ++s)
                cur.thumb[s] = slerp(cur.thumb[s], m_thumbPose[hand][g][s], k);
            m_handW[hand] += ((g == GESTURE_NEUTRAL && clip ? 0.f : 1.f) - m_handW[hand]) * k;
            // a pose of the avatar's own for this gesture, eased in over the preset's
            const bool own = md.handPoseSet[hand][g];
            if (own) {
                if (m_handCustom[hand] < 1e-3f)
                    m_handQ[hand] = md.handPose[hand][g];
                for (int i = 0; i < 15; ++i)
                    m_handQ[hand][i] = slerp(m_handQ[hand][i], md.handPose[hand][g][i], k).normalized();
            }
            m_handCustom[hand] += ((own ? 1.f : 0.f) - m_handCustom[hand]) * k;
            if (m_handW[hand] < 1e-3f)
                continue;
            for (int f = 0; f < FINGER_COUNT; ++f)
                for (int s = 0; s < 3; ++s) {
                    const int n = md.human[fingerBone(hand, f, s)];
                    if (n < 0)
                        break;
                    const SFingerAxes& ax = m_fingerAxes[hand][f * 3 + s];
                    Quat               d  = f == FINGER_THUMB ? cur.thumb[s] : Quat::axisAngle(ax.curl, cur.curl[f] * SEG[s]);
                    if (f != FINGER_THUMB && s == 0)
                        d = d * Quat::axisAngle(ax.spread, cur.spread[f] * SPREAD);
                    const int  p    = md.nodes[n].parent;
                    const Quat gp   = p >= 0 ? m_restGlobalRot[p] : Quat{};
                    Quat       want = (gp.conj() * d * gp * md.nodes[n].rest.r).normalized();
                    if (m_handCustom[hand] > 1e-3f)
                        want = slerp(want, m_handQ[hand][f * 3 + s], m_handCustom[hand]).normalized();
                    pose[n].r = m_handW[hand] > 0.999f ? want : slerp(pose[n].r, want, m_handW[hand]).normalized();
                }
        }
    }

    // the head turned toward where the camera looks (yawOff: of that, what the trunk turned already); weight: how much (an
    // emote has the head)
    void CAvatarAnimator::look(const SAvatarMotion& m, std::vector<STRS>& pose, float weight, float yawOff) const {
        const auto& md = *m_model;
        if (!md.humanoid || weight < 1e-3f)
            return;
        const V3 RT = normalize(cross(md.forward, UP));
        Quat     d  = Quat::axisAngle(UP, -std::clamp(m.lookYaw - yawOff, -1.2f, 1.2f) * 0.8f) * Quat::axisAngle(RT, std::clamp(m.lookPitch, -1.3f, 1.3f) * 0.6f);
        if (weight < 1)
            d = slerp(Quat{}, d, weight);
        if (md.human[HB_NECK] >= 0) {
            rotateBone(pose, HB_NECK, slerp(Quat{}, d, 0.4f));
            rotateBone(pose, HB_HEAD, slerp(Quat{}, d, 0.6f));
        } else
            rotateBone(pose, HB_HEAD, d);
    }

    // --- first person

    std::optional<V3> CAvatarAnimator::eyes() const {
        if (!m_model || m_model->eyeHeight <= 0 || m_global.size() != m_model->nodes.size())
            return std::nullopt;
        return m_model->fix.point(m_global[m_model->human[HB_HEAD]].point(m_model->eyes));
    }

    namespace {
        // First person's hands in the camera's frame (x right, y up, -z ahead), from the eye in arm lengths (shoulder to
        // wrist: a big avatar's come as far into the view as a small one's), left then right: each wrist, the way the hand
        // points (to its middle finger) and the way its palm faces
        struct SFpPose {
            V3 at[2], along[2], palm[2];
        };
        // ready: held up in front, low in the view, the palms in and a little down
        constexpr SFpPose FP_READY{{{-0.37f, -0.41f, -0.79f}, {0.36f, -0.40f, -0.80f}}, {{0.3f, 0.25f, -1.f}, {-0.3f, 0.25f, -1.f}}, {{0.85f, -0.45f, 0.f}, {-0.85f, -0.45f, 0.f}}};
        // typing: lower and nearer together, the palms down
        constexpr SFpPose FP_TYPE{{{-0.21f, -0.37f, -0.72f}, {0.21f, -0.37f, -0.72f}}, {{0.25f, -0.05f, -1.f}, {-0.25f, -0.05f, -1.f}}, {{0.15f, -1.f, 0.1f}, {-0.15f, -1.f, 0.1f}}};
        // holding something out there: both reaching for it either side of the crosshair, the palms to it, the fingers up
        constexpr SFpPose FP_HOLD{{{-0.33f, -0.22f, -0.90f}, {0.33f, -0.22f, -0.90f}}, {{0.15f, 0.85f, -0.5f}, {-0.15f, 0.85f, -0.5f}}, {{0.15f, 0.f, -1.f}, {-0.15f, 0.f, -1.f}}};
        // where the elbows go: down, out and back
        constexpr V3 FP_ELBOW[2] = {{-0.7f, -1.f, 0.4f}, {0.7f, -1.f, 0.4f}};
        // A hand making a gesture is held up a little and in, where it shows, turned so the gesture reads side on (a hand
        // pointing away from the eye hides behind its own forearm, or a sleeve): the way it points and its palm faces (the
        // right hand's; the left's mirrored); the neutral hand as ready
        struct SFpShow {
            V3 along, palm;
        };
        constexpr V3      FP_SHOW_AT = {0.30f, -0.27f, -0.80f};
        constexpr SFpShow FP_SHOW[GESTURE_COUNT] = {
            {},
            {{-1.f, 0.15f, -0.35f}, {0.1f, 0.f, 1.f}},  // fist: the knuckles in across the view, the thumb up
            {{0.f, 1.f, -0.3f}, {0.f, 0.f, -1.f}},      // open: up, the palm out
            {{-0.7f, 0.2f, -0.7f}, {-0.2f, -1.f, 0.2f}}, // point: ahead and in
            {{0.f, 1.f, -0.3f}, {0.f, 0.f, -1.f}},      // victory: up, the palm out
            {{0.f, 1.f, -0.3f}, {0.f, 0.f, -1.f}},      // rock'n'roll: the same
            {{-0.7f, 0.15f, -0.7f}, {-0.7f, 0.f, 0.7f}}, // handgun: ahead and in, the thumb up
            {{-1.f, 0.15f, -0.35f}, {0.1f, 0.f, 1.f}},  // thumbs up: as a fist
        };
        // touching: the right hand's fingertip goes to the crosshair, a little under it and to the right (its pointing
        // finger, not the hand, over what it touches), as far ahead as there's room, FP_TOUCH_REACH arm lengths at most
        constexpr V3    FP_TOUCH_AT = {0.035f, -0.06f, 0.f};
        constexpr float FP_TOUCH_REACH = 0.9f;
        // the hands' frame turns up and down with the camera's pitch only so far (looking down they come up the view, in
        // front of the body below it; looking up they stay low), and looking far down, ready hands let go (the arms
        // hang as the walking has them: the body and the legs below in sight): from FP_DOWN_FROM to FP_DOWN_ALL radians down
        constexpr float FP_PITCH_DOWN = 0.65f, FP_PITCH_UP = 0.45f, FP_DOWN_FROM = 0.75f, FP_DOWN_ALL = 1.2f;
        // how quickly the hands go where they're going (radians a second, critically damped); touching quicker
        constexpr float FP_W = 13.f, FP_TOUCH_W = 24.f;
        // how far they fall behind the camera's turns (arm lengths per radian a second) and moves (per m/s), at most
        // FP_LAG_MOST, catching up at FP_LAG_W (radians a second) a little springy (FP_LAG_Z)
        constexpr float FP_TURN_LAG = 0.022f, FP_MOVE_LAG = 0.014f, FP_LAG_MOST = 0.12f, FP_LAG_W = 11.f, FP_LAG_Z = 0.65f;
        // walking, the hands bob (arm lengths: across with each stride, up and down with each step) and swing a little
        // against the legs; running they pump: ahead and up, and back and down
        constexpr float FP_BOB_X = 0.035f, FP_BOB_Y = 0.02f, FP_SWING = 0.06f, FP_PUMP_AHEAD = 0.12f, FP_PUMP_BACK = 0.3f, FP_PUMP_UP = 0.1f;
        // how far the trunk turns from the hips toward where the camera looks, at most (radians): the hands in view reach
        // from shoulders square to the view; and bends over looking down past FP_BEND_FROM (radians), by FP_BEND of that
        constexpr float FP_TWIST = 0.75f, FP_BEND_FROM = 0.35f, FP_BEND = 0.3f;

        // two bones from `from` reaching for `to` (as near as they reach), bending toward `hint`: the first bone's
        // direction, the second's
        std::pair<V3, V3> twoBones(const V3& from, const V3& to, float l1, float l2, const V3& hint) {
            const V3 d    = to - from;
            float    dist = length(d);
            const V3 dir  = dist > 1e-6f ? d / dist : V3{0, -1, 0};
            dist          = std::clamp(dist, std::abs(l1 - l2) + 1e-4f, (l1 + l2) * 0.999f);
            const float c = std::clamp((l1 * l1 + dist * dist - l2 * l2) / (2 * l1 * dist), -1.f, 1.f);
            V3          n = hint - dir * dot(hint, dir);
            n             = length(n) > 1e-6f ? normalize(n) : perpendicular(dir);
            const V3 mid  = from + (dir * c + n * std::sqrt(1 - c * c)) * l1;
            return {normalize(mid - from), normalize(from + dir * dist - mid)};
        }

        // up quickly, down slower: a tap or a poke t seconds after it started
        float bump(float t, float up, float down) {
            return t < 0 ? 0.f : t < up ? minJerk(t / up) : 1.f - minJerk((t - up) / down);
        }

        // x (going v) a step toward `to` as SSpring::to has it
        void springStep(float& x, float& v, float to, float w, float z, float dt) {
            SSpring s{x, v};
            s.to(to, w, z, dt);
            x = s.x, v = s.v;
        }
    }

    V3 SFirstPersonEye::update(const V3& feet, const V3& eyes, float yaw, float dt) {
        // (FP_EYE_STILL: how far the eyes go before the camera goes after them, meters)
        constexpr float FP_EYE_STILL = 0.03f;
        const V3        want = yawTurn(yaw).conj().rotate(eyes - feet);
        if (!live || length(want - held) > 1.5f) // (or put somewhere else)
            held = off = want, v = {}, live = true;
        else if (dt > 0.f) {
            if (const V3 d = want - held; length(d) > FP_EYE_STILL)
                held += d * (1.f - FP_EYE_STILL / length(d));
            dt = std::min(dt, 0.05f);
            springTo(off.x, v.x, held.x, 25.f, dt);
            springTo(off.y, v.y, held.y, 14.f, dt);
            springTo(off.z, v.z, held.z, 25.f, dt);
        }
        if (const float flat = std::hypot(off.x, off.z); flat > reach)
            off.x *= reach / flat, off.z *= reach / flat;
        off.y = std::clamp(off.y, low, std::max(low, high));
        return at(feet, yaw);
    }

    V3 SFirstPersonEye::at(const V3& feet, float yaw) const {
        return feet + yawTurn(yaw).rotate(off);
    }

    void CAvatarAnimator::firstPersonState(const SAvatarMotion& m, float emoteW) {
        const auto& md  = *m_model;
        const bool  can = m.fp.on && md.eyeHeight > 0;
        if (can && !m_gait)
            m_gait = makeGait(md);
        const float dt = std::clamp(m.dt, 0.f, 0.05f);
        m_fpW      = approach(m_fpW, can ? 1.f : 0.f, dt / 0.2f);
        m_fpTrunkW = approach(m_fpTrunkW, can ? 1.f - emoteW : 0.f, dt / 0.25f);
        // (ready, looking far down: let go, the arms as the walking has them)
        const float down = m.fp.hands == FPH_READY ? smoothstep01((-m.fp.pitch - FP_DOWN_FROM) / (FP_DOWN_ALL - FP_DOWN_FROM)) : 0.f;
        m_fpArmsW        = approach(m_fpArmsW, can && m.fp.hands != FPH_DOWN ? (1.f - emoteW) * (1.f - down) : 0.f, dt / 0.25f);
        m_fpGesture      = {-1, -1};
        if (can && m.fp.hands == FPH_TOUCH)
            m_fpGesture[1] = GESTURE_POINT;
        else if (can && m.fp.hands == FPH_HOLD)
            m_fpGesture = {GESTURE_OPEN, GESTURE_OPEN};
        if (m_fpArmsW <= 0.f)
            m_fpLive = false;
        // the camera the hands and the trunk go by: first person's; while it's off (or turned off: going out of it, the
        // view gone behind the avatar), as it was last, going along with the body
        const V3    F    = m.world.dir({0, 0, -1});
        const float body = std::atan2(F.x, -F.z);
        if (can) {
            m_fpViewEye = m.fp.eye, m_fpViewYaw = m.fp.yaw, m_fpViewPitch = m.fp.pitch;
            m_fpEyeAt   = m.world.inverse().point(m.fp.eye);
            m_fpYawFrom = wrapPi(m.fp.yaw - body);
        } else
            m_fpViewEye = m.world.point(m_fpEyeAt), m_fpViewYaw = body + m_fpYawFrom;
    }

    float CAvatarAnimator::firstPersonTrunk(const SAvatarMotion& m, std::vector<STRS>& pose) const {
        if (m_fpTrunkW <= 0.f || !m_model->humanoid)
            return 0.f;
        const auto& md   = *m_model;
        const auto& h    = md.human;
        const float w    = smoothstep01(m_fpTrunkW);
        const V3    F    = m.world.dir({0, 0, -1});
        const float turn = std::clamp(wrapPi(m_fpViewYaw - std::atan2(F.x, -F.z)), -FP_TWIST, FP_TWIST) * w;
        const int   n    = (h[HB_SPINE] >= 0) + (h[HB_CHEST] >= 0) + (h[HB_UPPER_CHEST] >= 0);
        if (!n)
            return 0.f;
        // looking down, bent over toward where it looks, as people looking at their feet are: the eyes out over the body
        const float bend  = std::max(0.f, -m_fpViewPitch - FP_BEND_FROM) * FP_BEND * w;
        const V3    right = normalize((m.world * md.fix).inverse().dir(rightOf(m_fpViewYaw)));
        for (int b : {HB_SPINE, HB_CHEST, HB_UPPER_CHEST})
            rotateBone(pose, b, Quat::axisAngle(right, -bend / (float)n) * Quat::axisAngle(UP, -turn / (float)n));
        return turn;
    }

    void CAvatarAnimator::firstPersonArms(const SAvatarMotion& m, std::vector<STRS>& pose) {
        if (m_fpArmsW <= 0.f || !m_gait)
            return;
        const auto& md = *m_model;
        SGait&      g  = gaitOf(m_gait);
        const float dt = std::clamp(m.dt, 0.f, 0.05f);
        const float A  = std::max(g.body.arm * g.unit, 0.05f); // shoulder to wrist, meters
        const bool  fresh = !m_fpLive;
        m_fpLive          = true;

        // the hands' frame: the camera's, pitched less (but touching and holding, where it points)
        const V3    eye = m_fpViewEye;
        const float yaw = m_fpViewYaw, pitch = m_fpViewPitch, hp = pitch * (pitch < 0 ? FP_PITCH_DOWN : FP_PITCH_UP);
        const V3    right = rightOf(yaw);
        auto        fwdAt = [&](float p) { return V3{std::sin(yaw) * std::cos(p), std::sin(p), -std::cos(yaw) * std::cos(p)}; };
        auto        world = [&](const V3& c, float p) { const V3 f = fwdAt(p); return right * c.x + cross(right, f) * c.y - f * c.z; };
        auto        frame = [&](const V3& w) { const V3 f = fwdAt(hp); return V3{dot(w, right), dot(w, cross(right, f)), -dot(w, f)}; };

        // behind the camera's turns and moves
        {
            V3 lag{};
            if (!fresh && dt > 1e-4f) {
                const float yr = std::clamp(wrapPi(yaw - m_fpYaw) / dt, -6.f, 6.f), pr = std::clamp((pitch - m_fpPitch) / dt, -6.f, 6.f);
                const V3    v  = frame((eye - m_fpEye) * (1.f / dt));
                lag            = V3{-FP_TURN_LAG * yr, -FP_TURN_LAG * pr, 0} - v * (FP_MOVE_LAG * (length(v) < 20.f ? 1.f : 0.f)); // (not a jump across the world)
                for (float* c : {&lag.x, &lag.y, &lag.z})
                    *c = std::clamp(*c, -FP_LAG_MOST, FP_LAG_MOST);
            }
            if (fresh)
                m_fpLag = m_fpLagV = {};
            for (int k = 0; k < 3; ++k)
                springStep((&m_fpLag.x)[k], (&m_fpLagV.x)[k], (&lag.x)[k], FP_LAG_W, FP_LAG_Z, dt);
        }
        m_fpYaw = yaw, m_fpPitch = pitch, m_fpEye = eye;

        // the steps: the walking's, else as fast as it goes
        float mw = 0, run = 0, phase = 0;
        if (m_gaitUsed && g.grounded)
            mw = g.moveW, run = std::clamp(g.runArms, 0.f, 1.f), phase = g.phase;
        else if (m.onGround && !m.flying) {
            mw      = std::clamp(m.speed / 0.7f, 0.f, 1.f);
            run     = m.run && m.speed > 2.f ? 1.f : 0.f;
            m_fpPhase = frac(m_fpPhase + dt * (0.6f + 0.2f * m.speed) * (m.speed > 0.1f ? 1.f : 0.f));
            phase   = m_fpPhase;
        }
        m_fpPoke = m.fp.press ? 0.f : m_fpPoke + dt;

        for (int s = 0; s < 2; ++s) {
            SFpHand&    hd = m_fpHand[s];
            const float sx = s ? 1.f : -1.f;
            const auto  mirror = [&](const V3& v) { return V3{v.x * sx, v.y, v.z}; };
            V3          at = FP_READY.at[s], along = FP_READY.along[s], palm = FP_READY.palm[s];
            float       w  = FP_W;
            const int   gesture = m_fpGesture[s] >= 0 ? m_fpGesture[s] : m_gesture[s];
            switch (m.fp.hands) {
                case FPH_TOUCH:
                    if (s == 1) {
                        // the fingertip to the crosshair, as far as there's room (and the arm reaches); poked as it's pressed
                        const float reach = std::clamp((m.fp.room - 0.03f) / A, 0.35f, FP_TOUCH_REACH) + 0.06f * bump(m_fpPoke, 0.07f, 0.16f);
                        // (from the right, across: from behind, the hand would hide behind its forearm)
                        const V3    tip   = FP_TOUCH_AT + V3{0, 0, -reach};
                        along             = normalize(V3{-0.7f, 0.15f, -0.7f});
                        at                = tip - along * (md.clearance.hand[1] > 0 ? md.clearance.hand[1] / A : 0.35f);
                        palm              = {-0.2f, -1.f, 0.1f};
                        w                 = FP_TOUCH_W;
                    }
                    break;
                case FPH_TYPE:
                    at = FP_TYPE.at[s], along = FP_TYPE.along[s], palm = FP_TYPE.palm[s];
                    if (m.fp.tap == s)
                        hd.tap = 0;
                    else
                        hd.tap += dt;
                    at += V3{0, -0.04f, -0.015f} * bump(hd.tap, 0.04f, 0.1f);
                    break;
                case FPH_HOLD: at = FP_HOLD.at[s], along = FP_HOLD.along[s], palm = FP_HOLD.palm[s]; break;
                default: break;
            }
            if (m.fp.hands == FPH_READY || (m.fp.hands == FPH_TOUCH && s == 0)) {
                if (gesture > GESTURE_NEUTRAL && gesture < GESTURE_COUNT)
                    at = mirror(FP_SHOW_AT), along = mirror(FP_SHOW[gesture].along), palm = mirror(FP_SHOW[gesture].palm);
                // breathing
                at.y += 0.008f * std::sin(TAU * m_time / 4.2f + s);
            }
            // walking: bobbing, swinging a little against the legs (ahead as the other leg is); running, pumping
            const float fs = -std::cos(TAU * (phase + 0.5f * s));
            if (m.fp.hands != FPH_TOUCH || s == 0) {
                const float walk = mw * (1.f - run);
                at += V3{FP_BOB_X * std::sin(TAU * phase), -FP_BOB_Y * 0.5f * (1.f - std::cos(2.f * TAU * phase)), -FP_SWING * fs} * walk;
                at += V3{-sx * 0.06f * std::max(fs, 0.f), FP_PUMP_UP * fs - 0.05f, fs > 0 ? -FP_PUMP_AHEAD * fs : -FP_PUMP_BACK * fs} * (mw * run);
            }
            // a wall or a window near: the hands no further ahead than there's room, lower the less there is
            if (const float most = (m.fp.room - 0.05f) / A; -at.z > most) {
                const float over = -at.z - std::max(most, 0.3f);
                at.z             = -std::max(most, 0.3f);
                at.y -= 0.6f * std::max(over, 0.f);
            }
            const float aim = (m.fp.hands == FPH_TOUCH && s == 1) || m.fp.hands == FPH_HOLD ? pitch : hp;
            if (fresh) {
                hd.at = at, hd.v = {}, hd.along = normalize(along), hd.palm = normalize(palm), hd.pitch = aim, hd.tap = 1;
            } else
                for (int k = 0; k < 3; ++k)
                    springTo((&hd.at.x)[k], (&hd.v.x)[k], (&at.x)[k], w, dt);
            const float k = 1.f - std::exp(-dt * w);
            hd.along      = normalize(lerp(hd.along, normalize(along), k));
            hd.palm       = normalize(lerp(hd.palm, normalize(palm), k));
            hd.pitch += (aim - hd.pitch) * k;
        }

        // the arms to them: from the shoulders as posed (the trunk turned), the elbows down and out
        globals(pose, m_global);
        const M4    toModel = (M4::translation({0, m_lift, 0}) * m.world * md.fix).inverse();
        const float arms    = smoothstep01(m_fpArmsW);
        for (int s = 0; s < 2; ++s) {
            const SFpHand& hd = m_fpHand[s];
            reachArm(pose, s, toModel.point(eye + world((hd.at + m_fpLag) * A, hd.pitch)), toModel.dir(world(FP_ELBOW[s], hd.pitch)), toModel.dir(world(hd.along, hd.pitch)),
                     toModel.dir(world(hd.palm, hd.pitch)), arms);
        }
    }

    // (the shoulder from m_global: globals() first)
    void CAvatarAnimator::reachArm(std::vector<STRS>& pose, int s, const V3& wrist, const V3& elbow, const V3& along, const V3& palm, float w) {
        const auto& md = *m_model;
        const auto& h  = md.human;
        const SGait& g = gaitOf(m_gait);
        const Quat   R = g.rig.facing, Rc = R.conj();
        const int    ua = s ? HB_R_UPPER_ARM : HB_L_UPPER_ARM;
        const auto [u, f] = twoBones(origin(m_global[h[ua]]), wrist, g.body.upper[s] * g.rig.height, g.body.fore[s] * g.rig.height, elbow);
        auto        rig   = [&](const V3& d) { return normalize(Rc.rotate(normalize(d))); };
        CPoser      p(g.body);
        p.arm(s, p.armOf(s, Rc.rotate(u), Rc.rotate(f), rig(along), rig(palm)));
        for (int i = 0; i < 3; ++i) {
            const int b = ua + i, n = h[b];
            if (n < 0)
                break;
            const int  par  = md.nodes[n].parent;
            const Quat want = (R * p.turn[b] * Rc * g.rig.tpose[b]).normalized() * g.rig.restRot[n];
            const Quat up   = par >= 0 ? rotationOf(globalOf(pose, par)) : Quat{};
            const Quat q    = (up.conj() * want).normalized();
            pose[n].r       = w >= 0.999f ? q : slerp(pose[n].r, q, w).normalized();
        }
    }

    // --- attacks

    namespace {
        // the body is the attack's over ATTACK_IN as one starts; a swing after another takes over from it over ATTACK_CROSS
        // (from where its fists are up, when that one's still up); ATTACK_PAUSE after the last one started, the next is
        // the right's again
        constexpr float ATTACK_IN = 0.1f, ATTACK_CROSS = 0.1f, ATTACK_PAUSE = 0.9f;
        // how much of the head's looking where the camera looks an attack takes over (the head goes with the punch)
        constexpr float ATTACK_LOOK = 0.75f;
        // first person, the swing goes where the camera looks, but up and down no further than this (radians)
        constexpr float ATTACK_UP = 0.5f, ATTACK_DOWN = -0.6f;
    }

    const SAvatarAttack& CAvatarAnimator::attackOf(const SSwing& sw) const {
        const auto& md = *m_model;
        return (sw.fp && !md.attacksFirst[sw.hand & 1].anim.channels.empty() ? md.attacksFirst : md.attacks)[sw.hand & 1];
    }

    bool CAvatarAnimator::attack(int hand) {
        if (!m_model || !m_model->humanoid || m_model->attacks[1].anim.channels.empty())
            return false;
        const auto& h = m_model->human;
        for (int b : {HB_HEAD, HB_L_UPPER_ARM, HB_L_LOWER_ARM, HB_L_HAND, HB_R_UPPER_ARM, HB_R_LOWER_ARM, HB_R_HAND})
            if (h[b] < 0)
                return false;
        stopEmote(); // (as moving does)
        if (m_swingNext >= 0) // (one's waiting already)
            return true;
        if (hand < 0)
            hand = m_swingAgo > ATTACK_PAUSE ? 1 : 1 - m_swingLast;
        hand &= 1;
        if (m_swing.t >= 0 && m_swing.t < attackOf(m_swing).next)
            m_swingNext = hand; // (once this one has struck)
        else
            swingStart(hand, m_fpW > 0.5f);
        return true;
    }

    void CAvatarAnimator::swingStart(int hand, bool fp) {
        // after one that's still up: from where this one's fists are up, that one going on under it as it takes over;
        // after one that's letting go, from the start, taking over from it
        const bool up = m_swing.t >= 0 && m_attackW > 0.5f;
        if (m_swing.t >= 0) {
            m_swingWas   = m_swing;
            m_swingCross = 0;
        }
        m_swing     = {0, hand, fp};
        m_swing.t   = up ? attackOf(m_swing).ready : 0.f;
        m_swingLast = hand;
        m_swingAgo  = 0;
        ++m_swings;
    }

    std::string CAvatarAnimator::attackStatus() const {
        static constexpr const char* SIDE[] = {"left", "right"};
        auto arm = [&](int s) {
            for (const SSwing* sw : {&m_swing, &m_swingWas})
                if (sw->t >= 0 && sw->hand == s) {
                    const float c = smoothstep01(m_swingCross), w = m_attackW * (sw == &m_swing ? (m_swingWas.t >= 0 ? c : 1.f) : 1.f - c);
                    return std::format(R"({{"t": {:.3f}, "weight": {:.2f}, "fist": {}, "firstPerson": {}}})", sw->t, w, m_swingFist[s], sw->fp);
                }
            return std::string("null");
        };
        // (the chest's turn from the hips)
        float turn = 0;
        if (m_model && m_model->humanoid && m_global.size() == m_model->nodes.size()) {
            const auto& md    = *m_model;
            const int   chest = md.human[HB_UPPER_CHEST] >= 0 ? md.human[HB_UPPER_CHEST] : md.human[HB_CHEST] >= 0 ? md.human[HB_CHEST] : md.human[HB_SPINE];
            auto        yaw   = [&](int n) {
                const V3 f = md.fix.dir((rotationOf(m_global[n]) * m_restGlobalRot[n].conj()).rotate(md.forward));
                return std::atan2(f.x, -f.z);
            };
            if (chest >= 0)
                turn = wrapPi(yaw(chest) - yaw(md.human[HB_HIPS]));
        }
        return std::format(R"({{"left": {}, "right": {}, "next": {}, "last": {}, "swings": {}, "turn": {:.3f}, "weight": {:.2f}}})", arm(0), arm(1),
                           m_swingNext >= 0 ? std::format("\"{}\"", SIDE[m_swingNext]) : "null", m_swings ? std::format("\"{}\"", SIDE[m_swingLast]) : "null", m_swings,
                           turn, m_attackW);
    }

    void CAvatarAnimator::attackState(const SAvatarMotion& m) {
        const float dt = std::clamp(m.dt, 0.f, 0.05f);
        m_swingAgo += dt;
        m_swingFist = {};
        if (m_swing.t < 0) {
            m_attackW = 0;
            return;
        }
        // (the one asked for next, once this one has struck)
        if (m_swingNext >= 0 && m_swing.t >= attackOf(m_swing).next) {
            swingStart(m_swingNext, m_fpW > 0.5f);
            m_swingNext = -1;
        }
        const SAvatarAttack& a   = attackOf(m_swing);
        const float          dur = a.anim.duration;
        m_swing.t                = std::min(m_swing.t + dt, dur);
        if (m_swingWas.t >= 0)
            m_swingWas.t = std::min(m_swingWas.t + dt, attackOf(m_swingWas).anim.duration);
        m_swingCross = std::min(1.f, m_swingCross + dt / ATTACK_CROSS);
        if (m_swingCross >= 1.f)
            m_swingWas.t = -1;
        // the body: the swing's at once, let go from where it starts letting go to its end (no quicker than it came in)
        const float want = m_swing.t >= dur ? 0.f : m_swing.t <= a.out ? 1.f : 1.f - (m_swing.t - a.out) / (dur - a.out);
        m_attackW        = want > m_attackW ? std::min(want, m_attackW + dt / ATTACK_IN) : std::max(want, m_attackW - dt / ATTACK_IN);
        if (m_swing.t >= dur && m_attackW <= 0.f) {
            m_swing.t = m_swingWas.t = -1;
            m_swingNext              = -1;
            return;
        }
        m_swingFist[0] = m_swingFist[1] = m_swing.t < a.out + 0.05f;
        // its pose, taking over from the one before
        sampleClip(a.anim, m_swing.t, m_attackPose);
        if (m_swingWas.t >= 0) {
            sampleClip(attackOf(m_swingWas).anim, m_swingWas.t, m_attackWas);
            const float c = smoothstep01(m_swingCross);
            for (size_t n = 0; n < m_attackPose.size(); ++n)
                if (m_attackPart[n] != ATTACK_NONE)
                    m_attackPose[n].r = slerp(m_attackWas[n].r, m_attackPose[n].r, c).normalized();
        }
    }

    void CAvatarAnimator::attackTrunk(std::vector<STRS>& pose) const {
        if (m_attackW <= 0.f)
            return;
        const auto& md = *m_model;
        const float w  = smoothstep01(m_attackW);
        for (size_t n = 0; n < pose.size(); ++n)
            if (m_attackPart[n] == ATTACK_TRUNK) {
                // (the clip's turn from rest, in the bone's own frame)
                const Quat q = (pose[n].r * md.nodes[n].rest.r.conj() * m_attackPose[n].r).normalized();
                pose[n].r    = w >= 0.999f ? q : slerp(pose[n].r, q, w).normalized();
            }
    }

    void CAvatarAnimator::attackArms(const SAvatarMotion& m, std::vector<STRS>& pose) {
        if (m_attackW <= 0.f)
            return;
        const auto& md = *m_model;
        const float w  = smoothstep01(m_attackW);
        const bool  fp = m.fp.on && md.eyeHeight > 0;
        // first person: the hands where the clip has them in the view (from the eyes, in arm lengths: whatever the avatar's
        // build), its view the camera's, pitched no further than ATTACK_UP and ATTACK_DOWN
        std::array<V3, 2> reach;
        bool              aim    = false;
        auto              wrists = [&](const SSwing& sw, std::array<V3, 2>& out) {
            const SAvatarAttack& a = attackOf(sw);
            if (a.reach.empty())
                return false;
            const size_t k = (size_t)std::max<ptrdiff_t>(0, std::ranges::upper_bound(a.reachT, sw.t) - a.reachT.begin() - 1), k1 = std::min(k + 1, a.reach.size() - 1);
            const float  dt = a.reachT[k1] - a.reachT[k], u = dt > 0 ? std::clamp((sw.t - a.reachT[k]) / dt, 0.f, 1.f) : 0.f;
            for (int s = 0; s < 2; ++s)
                out[s] = lerp(a.reach[k][s], a.reach[k1][s], u);
            return true;
        };
        if (fp && m_swing.fp && wrists(m_swing, reach)) {
            aim = true;
            if (std::array<V3, 2> was; m_swingWas.t >= 0 && m_swingWas.fp && wrists(m_swingWas, was))
                for (int s = 0; s < 2; ++s)
                    reach[s] = lerp(was[s], reach[s], smoothstep01(m_swingCross));
        }
        if (!m_gait)
            m_gait = makeGait(md);
        const float A       = std::max(gaitOf(m_gait).body.arm * gaitOf(m_gait).unit, 0.05f); // shoulder to wrist, meters
        const M4    toWorld = M4::translation({0, m_lift, 0}) * m.world * md.fix, toModel = toWorld.inverse();
        const float pitch   = std::clamp(m_fpViewPitch, ATTACK_DOWN, ATTACK_UP);
        const V3    fwd{std::sin(m_fpViewYaw) * std::cos(pitch), std::sin(pitch), -std::cos(m_fpViewYaw) * std::cos(pitch)}, right = rightOf(m_fpViewYaw),
            up = cross(right, fwd);
        auto swingArms = [&](float k) {
            for (size_t n = 0; n < pose.size(); ++n)
                if (m_attackPart[n] == ATTACK_ARM)
                    pose[n].r = k >= 0.999f ? m_attackPose[n].r : slerp(pose[n].r, m_attackPose[n].r, k).normalized();
            if (aim)
                for (int s = 0; s < 2; ++s)
                    armTo(pose, s, toModel.point(m_fpViewEye + (right * -reach[s].x + up * reach[s].y + fwd * reach[s].z) * A), k);
        };
        if (!fp) {
            swingArms(w);
            return;
        }
        const std::vector<STRS> under = pose; // (the arms as first person holds them: the swing giving way to them starts from these)
        swingArms(w);
        // the fist no further ahead than there's room (a wall, a window there): the swing giving way, as far as that takes,
        // to the hands as first person holds them (kept short of it already)
        const int   hand = md.human[m_swing.hand ? HB_R_HAND : HB_L_HAND];
        const float most = std::max(m.fp.room - 0.1f, 0.15f);
        const V3    look{std::sin(m_fpViewYaw) * std::cos(m_fpViewPitch), std::sin(m_fpViewPitch), -std::cos(m_fpViewYaw) * std::cos(m_fpViewPitch)};
        auto        ahead = [&] { return dot(toWorld.point(origin(globalOf(pose, hand))) - m_fpViewEye, look); };
        if (hand < 0 || m.fp.room >= 1e8f || ahead() <= most)
            return;
        float lo = 0, hi = w;
        for (int i = 0; i < 10; ++i) {
            const float k = 0.5f * (lo + hi);
            pose          = under;
            swingArms(k);
            (ahead() > most ? hi : lo) = k;
        }
        pose = under;
        swingArms(lo);
    }

    void CAvatarAnimator::armTo(std::vector<STRS>& pose, int s, const V3& wrist, float w) const {
        const auto& md = *m_model;
        const auto& h  = md.human;
        const int   ua = h[s ? HB_R_UPPER_ARM : HB_L_UPPER_ARM], la = h[s ? HB_R_LOWER_ARM : HB_L_LOWER_ARM], hd = h[s ? HB_R_HAND : HB_L_HAND];
        if (ua < 0 || la < 0 || hd < 0 || w <= 0.f)
            return;
        const M4    gu = globalOf(pose, ua), gl = globalOf(pose, la), gh = globalOf(pose, hd);
        const V3    S = origin(gu), E = origin(gl), W = origin(gh);
        const float l1 = length(origin(m_restGlobal[la]) - origin(m_restGlobal[ua])), l2 = length(origin(m_restGlobal[hd]) - origin(m_restGlobal[la]));
        if (l1 < 1e-6f || l2 < 1e-6f || length(E - S) < 1e-6f || length(W - E) < 1e-6f)
            return;
        // (bent the way it's bent: the elbow's way out from the line through the shoulder and the wrist)
        const V3   sw = length(W - S) > 1e-6f ? normalize(W - S) : normalize(E - S);
        const auto [u, f] = twoBones(S, wrist, l1, l2, E - S - sw * dot(E - S, sw));
        const Quat turnU = arc(normalize(E - S), u), turnF = arc(turnU.rotate(normalize(W - E)), f);
        const Quat want[3] = {(turnU * rotationOf(gu)).normalized(), (turnF * turnU * rotationOf(gl)).normalized(), rotationOf(gh)};
        // (each from its own parent as posed by then: a bone between two keeps its own turn)
        const int nodes[3] = {ua, la, hd};
        for (int i = 0; i < 3; ++i) {
            const int  n  = nodes[i], p = md.nodes[n].parent;
            const Quat up = p >= 0 ? rotationOf(globalOf(pose, p)) : Quat{};
            const Quat q  = (up.conj() * want[i]).normalized();
            pose[n].r     = w >= 0.999f ? q : slerp(pose[n].r, q, w).normalized();
        }
    }

    float CAvatarAnimator::random() {
        m_rng ^= m_rng << 13;
        m_rng ^= m_rng >> 17;
        m_rng ^= m_rng << 5;
        return (float)(m_rng >> 8) / 16777216.f;
    }

    // expressions (held, or the gesture's), blinking, where the eyes look, and what that makes of the morphs
    // and materials; how the expressions combine is VRM 1.0's (three-vrm's)
    // what a node pose sets of a node's own, f of the way there
    static void putPose(STRS& d, const SNodePose& p, float f) {
        if (p.set & SNodePose::T)
            d.t = lerp(d.t, p.trs.t, f);
        if (p.set & SNodePose::R)
            d.r = f >= 1 ? p.trs.r : slerp(d.r, p.trs.r, f).normalized();
        if (p.set & SNodePose::S)
            d.s = lerp(d.s, p.trs.s, f);
    }

    // how far a loop is from a to b: there and back every `seconds`, easing in and out at both (VRCFury's keys have
    // flat tangents)
    static float loopWeight(float time, float seconds) {
        const float p = std::fmod(time, seconds) / seconds;
        const float u = p < 0.5f ? 2 * p : 2 - 2 * p;
        return u * u * (3 - 2 * u);
    }

    void CAvatarAnimator::face(const SAvatarMotion& m, std::vector<STRS>& pose) {
        const auto&  md = *m_model;
        const auto&  P  = md.preset;
        const size_t n  = md.expressions.size();
        const float  dt = std::clamp(m.dt, 0.f, 0.1f);
        auto         binary = [&](size_t e, float w) { return md.expressions[e].binary ? (w > 0.5f ? 1.f : 0.f) : w; };

        // the held one, else the face both hands' gestures make together, else that of the hand that made its gesture last
        int   want  = m_held;
        float wantW = m_heldWeight;
        if (want < 0 && md.gestureCombo[m_gesture[0]][m_gesture[1]] != -2) {
            want  = md.gestureCombo[m_gesture[0]][m_gesture[1]];
            wantW = 1;
        } else if (want < 0)
            for (int hand : {m_lastHand, 1 - m_lastHand})
                if (const int f = md.gestureFace[hand][m_gesture[hand]]; f >= 0) {
                    want  = f;
                    wantW = 1;
                    break;
                }
        for (size_t e = 0; e < n; ++e) {
            const float target = (int)e == want ? wantW : 0.f;
            float&      w      = m_exprW[e];
            w                  = target > w ? std::min(target, w + dt / 0.12f) : std::max(target, w - dt / 0.12f);
        }

        auto& out = m_exprOut;
        out       = m_exprW;
        // the emote's faces, under the player's own (but for closing the eyes)
        if (m_emote >= 0) {
            float mine = 0;
            for (size_t e = 0; e < n; ++e)
                mine = std::max(mine, m_exprW[e]);
            for (size_t e = 0; e < n; ++e) {
                const int p = md.expressions[e].preset;
                out[e]      = std::max(out[e], m_emoteFace[e] * (p == EX_BLINK || p == EX_BLINK_L || p == EX_BLINK_R ? 1.f : 1.f - mine));
            }
        }

        // what they block of the blinking, looking and mouth
        float blockBlink = 0, blockLook = 0, blockMouth = 0;
        for (size_t e = 0; e < n; ++e) {
            const float w = binary(e, out[e]);
            if (w <= 0)
                continue;
            const auto& x      = md.expressions[e];
            auto        amount = [&](eOverride o) { return o == OVERRIDE_BLOCK ? 1.f : o == OVERRIDE_BLEND ? w : 0.f; };
            blockBlink += amount(x.overrideBlink);
            blockLook += amount(x.overrideLookAt);
            blockMouth += amount(x.overrideMouth);
        }
        const float blinkM = std::max(0.f, 1.f - blockBlink), lookM = std::max(0.f, 1.f - blockLook), mouthM = std::max(0.f, 1.f - blockMouth);

        auto more = [&](int preset, float w) {
            if (P[preset] >= 0)
                out[P[preset]] = std::max(out[P[preset]], std::clamp(w, 0.f, 1.f));
        };

        // blinking: every few seconds, sometimes twice
        const bool canBlink = P[EX_BLINK] >= 0 || (P[EX_BLINK_L] >= 0 && P[EX_BLINK_R] >= 0);
        if (m_autoBlink && canBlink) {
            constexpr float CLOSE = 0.06f, SHUT = 0.04f, OPEN = 0.1f;
            float           b = 0;
            if (m_blinkT < 0 && (m_blinkIn -= dt) <= 0)
                m_blinkT = 0;
            if (m_blinkT >= 0) {
                m_blinkT += dt;
                if (m_blinkT < CLOSE)
                    b = smoothstep01(m_blinkT / CLOSE);
                else if (m_blinkT < CLOSE + SHUT)
                    b = 1;
                else if (m_blinkT < CLOSE + SHUT + OPEN)
                    b = 1.f - smoothstep01((m_blinkT - CLOSE - SHUT) / OPEN);
                else {
                    m_blinkT     = -1;
                    m_blinkAgain = !m_blinkAgain && random() < 0.1f;
                    m_blinkIn    = m_blinkAgain ? 0.12f : 1.5f + 4.f * random();
                }
            }
            // not over a blink or wink that's held
            float held = 0;
            for (int p : {EX_BLINK, EX_BLINK_L, EX_BLINK_R})
                if (P[p] >= 0)
                    held = std::max(held, out[P[p]]);
            b *= 1.f - held;
            if (P[EX_BLINK] >= 0)
                more(EX_BLINK, b);
            else {
                more(EX_BLINK_L, b);
                more(EX_BLINK_R, b);
            }
        } else
            m_blinkT = -1;

        // the eyes: where the head doesn't turn to (see look()), and glancing about
        if (md.lookAt.type != SLookAt::NONE) {
            if ((m_saccadeIn -= dt) <= 0) {
                m_saccadeIn = 0.6f + 2.4f * random();
                if (random() < 0.4f)
                    m_saccadeYaw = m_saccadePitch = 0;
                else {
                    m_saccadeYaw   = (random() * 2.f - 1.f) * 0.2f;
                    m_saccadePitch = (random() * 2.f - 1.f) * 0.12f;
                }
            }
            constexpr float DEG   = 57.2957795f;
            const auto&     range = md.lookAt.range;
            const float     eyes  = m_emote >= 0 ? m_emoteEyes : 0.f; // an emote's, over the player's
            const float     yaw   = lerpf((m.lookYaw - 0.8f * std::clamp(m.lookYaw, -1.2f, 1.2f)) * DEG, m_emoteYaw, eyes);
            const float     pitch = lerpf((m.lookPitch - 0.6f * std::clamp(m.lookPitch, -1.3f, 1.3f)) * DEG, m_emotePitch, eyes);
            auto            map   = [&](int row, float deg) { return std::min(std::abs(deg), range[row][0]) / range[row][0] * range[row][1]; };
            const float     v     = (pitch >= 0 ? map(3, pitch) : -map(2, pitch)) + m_saccadePitch * range[pitch >= 0 ? 3 : 2][1];
            if (md.lookAt.type == SLookAt::BONES && md.humanoid) {
                const V3 RT = normalize(cross(md.forward, UP));
                for (int eye : {HB_L_EYE, HB_R_EYE}) {
                    // turning towards the nose is the inner range
                    const bool  inward = (eye == HB_L_EYE) == (yaw > 0);
                    const float h      = std::copysign(map(inward ? 0 : 1, yaw), yaw) + m_saccadeYaw * range[1][1];
                    rotateBone(pose, eye, Quat::axisAngle(UP, -h * lookM / DEG) * Quat::axisAngle(RT, v * lookM / DEG));
                }
            } else if (md.lookAt.type == SLookAt::EXPRESSIONS) {
                const float h = std::copysign(map(1, yaw), yaw) + m_saccadeYaw * range[1][1];
                more(EX_LOOK_RIGHT, h);
                more(EX_LOOK_LEFT, -h);
                more(EX_LOOK_UP, v);
                more(EX_LOOK_DOWN, -v);
            }
        }

        // lip sync: the mouth's presets as the voice has them, and the consonants the avatar has, in place of as much of
        // the vowels
        float spoken = 0;
        for (int k = VOWEL_COUNT; k < VISEME_COUNT; ++k)
            if (const int e = md.consonant[k - VOWEL_COUNT]; e >= 0 && m_visemes[k] > 0) {
                out[e] = std::max(out[e], std::clamp(m_visemes[k], 0.f, 1.f));
                spoken += m_visemes[k];
            }
        for (int k = 0; k < VOWEL_COUNT; ++k)
            if (m_visemes[k] > 0)
                more(EX_AA + k, m_visemes[k] * std::max(0.f, 1.f - spoken));

        for (size_t e = 0; e < n; ++e) {
            float     o = binary(e, out[e]);
            const int p = md.expressions[e].preset;
            if (p == EX_BLINK || p == EX_BLINK_L || p == EX_BLINK_R)
                o *= blinkM;
            else if (p >= EX_LOOK_UP && p <= EX_LOOK_RIGHT)
                o *= lookM;
            else if ((p >= EX_AA && p <= EX_OH) || md.expressions[e].viseme)
                o *= mouthM;
            out[e] = o;
        }

        m_morphW = m_shapeBase;
        for (const int t : m_loops) { // a toggle's loop: its shape keys from a to b and back
            const auto& l = md.toggles[t].loop;
            const float w = loopWeight(m_time, l.seconds);
            auto        at = [&](const std::vector<std::pair<int, float>>& v, int morph) {
                const auto it = std::ranges::find(v, morph, &std::pair<int, float>::first);
                return it != v.end() ? it->second : m_shapeBase[morph];
            };
            for (const auto* side : {&l.shapesA, &l.shapesB})
                for (const auto& [mo, _] : *side)
                    m_morphW[mo] = at(l.shapesA, mo) + (at(l.shapesB, mo) - at(l.shapesA, mo)) * w;
        }
        for (size_t e = 0; e < n; ++e)
            if (out[e] > 0)
                for (const auto& b : md.expressions[e].morphs)
                    m_morphW[b.morph] += out[e] * b.weight;
        for (size_t i = 0; i < md.morphs.size(); ++i) {
            const float base = m_shapeBase[i];
            m_morphW[i]      = std::clamp(m_morphW[i], std::min(base, 0.f), std::max(base, 1.f));
        }

        // materials: colors and atlas faces, made again when what they come from changes
        if (m_materials.empty())
            return;
        bool changed = false;
        for (size_t e = 0; e < n && !changed; ++e)
            changed = !md.expressions[e].materials.empty() && std::abs(out[e] - m_matOut[e]) > 1e-4f;
        if (!changed)
            return;
        m_matOut    = out;
        m_materials = md.materials;
        for (size_t e = 0; e < n; ++e) {
            const float o = out[e];
            if (o <= 0)
                continue;
            for (const auto& b : md.expressions[e].materials) {
                SMapMaterial&       mt = m_materials[b.material];
                const SMapMaterial& at = md.materials[b.material];
                switch (b.prop) {
                    case SExpression::MP_COLOR:
                        for (int k = 0; k < 4; ++k)
                            mt.baseColor[k] += (b.value[k] - at.baseColor[k]) * o;
                        break;
                    case SExpression::MP_EMISSIVE:
                        for (int k = 0; k < 3; ++k)
                            mt.emissive[k] += (b.value[k] - at.emissive[k]) * o;
                        break;
                    case SExpression::MP_UV:
                        for (auto [xf, from] : {std::pair{mt.baseXf, at.baseXf}, {mt.emissiveXf, at.emissiveXf}}) {
                            xf[0] += (b.value[0] - from[0]) * o;
                            xf[3] += (b.value[1] - from[3]) * o;
                            xf[4] += (b.value[2] - from[4]) * o;
                            xf[5] += (b.value[3] - from[5]) * o;
                        }
                        break;
                }
            }
        }
    }

    // --- outfit

    namespace {
        // the key of a slider in effect at its value: the last at or below it (the first below all of them)
        size_t sliderKey(const SAvatarSlider& s, float v) {
            size_t k = 0;
            for (size_t i = 1; i < s.keys.size(); ++i)
                if (s.keys[i].at <= v)
                    k = i;
            return k;
        }

        // a 2D slider's keys around (x, y): the grid cell's corners (left bottom, right bottom, left top, right top)
        // and how far across it
        struct SCell {
            const SAvatarSlider::SKey* k[4] = {};
            float                      fx = 0, fy = 0;
        };
        SCell sliderCell(const SAvatarSlider& s, float x, float y) {
            const int   n  = s.grid;
            const float gx = std::clamp((x + 1) * 0.5f * (n - 1), 0.f, (float)(n - 1)), gy = std::clamp((y + 1) * 0.5f * (n - 1), 0.f, (float)(n - 1));
            const int   ix = std::min((int)gx, n - 2), iy = std::min((int)gy, n - 2);
            SCell       c;
            c.fx   = gx - ix;
            c.fy   = gy - iy;
            c.k[0] = &s.keys[iy * n + ix];
            c.k[1] = &s.keys[iy * n + ix + 1];
            c.k[2] = &s.keys[(iy + 1) * n + ix];
            c.k[3] = &s.keys[(iy + 1) * n + ix + 1];
            return c;
        }

        // the key a slider's parts and material variants are as: 1D the last at or below the value, 2D the nearest
        const SAvatarSlider::SKey& sliderOn(const SAvatarSlider& s, float v, float vy) {
            if (!s.grid)
                return s.keys[sliderKey(s, v)];
            const SCell c = sliderCell(s, v, vy);
            return *c.k[(c.fx >= 0.5f ? 1 : 0) + (c.fy >= 0.5f ? 2 : 0)];
        }

        STRS lerpTRS(const STRS& a, const STRS& b, float f) {
            return {lerp(a.t, b.t, f), slerp(a.r, b.r, f).normalized(), lerp(a.s, b.s, f)};
        }
    }

    void CAvatarAnimator::loopPoses(std::vector<STRS>& pose) const {
        const auto& md = *m_model;
        for (const int t : m_loops) {
            const auto& l = md.toggles[t].loop;
            const float w = loopWeight(m_time, l.seconds);
            for (const auto& a : l.posesA) {
                const STRS under = pose[a.node];
                putPose(pose[a.node], a, 1.f);
                if (std::ranges::none_of(l.posesB, [&](const SNodePose& b) { return b.node == a.node; })) {
                    SNodePose back = a;
                    back.trs       = under;
                    putPose(pose[a.node], back, w);
                }
            }
            for (const auto& b : l.posesB)
                putPose(pose[b.node], b, w);
        }
    }

    // nodes a toggle leaves in the world (VRCFury's World Drop): where they were when it turned on, and all under them;
    // and nodes held in the world (MA's World Fixed Object): where their rest pose was when the avatar appeared (MA
    // moves them to a world-fixed root as the avatar is built)
    void CAvatarAnimator::drops(const SAvatarMotion& m) {
        if (m_dropBy.empty())
            return;
        const auto& md = *m_model;
        const M4    W  = m.world * M4::translation({0, m_lift, 0}) * md.fix, Wi = W.inverse();
        std::vector<uint8_t> moved(md.nodes.size(), 0);
        for (size_t n = 0; n < md.nodes.size(); ++n) {
            const int p = md.nodes[n].parent;
            if (p >= 0 && moved[p]) { // under a dropped one: with it
                m_global[n] = m_global[p] * m_pose[n].matrix();
                moved[n]    = 1;
                continue;
            }
            if (m_dropBy[n] < 0)
                continue;
            if (m_dropTake[n]) {
                M4 at = m_global[n];
                if (m_dropBy[n] == DROP_FIXED) {
                    at = M4::identity();
                    for (int k = (int)n; k >= 0; k = md.nodes[k].parent)
                        at = md.nodes[k].rest.matrix() * at;
                }
                m_dropAt[n]   = W * at;
                m_dropTake[n] = 0;
            }
            m_global[n] = Wi * m_dropAt[n];
            moved[n]    = 1;
        }
    }

    void CAvatarAnimator::outfit() {
        const auto&  md    = *m_model;
        const size_t parts = md.parts.size();
        // parts a toggle or a slider shows are hidden unless one of those shows them; one that hides them wins
        std::vector<uint8_t> showable(parts, 0), shownBy(parts, 0), hiddenBy(parts, 0);
        for (size_t t = 0; t < md.toggles.size(); ++t) {
            for (const int p : md.toggles[t].show) {
                showable[p] = 1;
                shownBy[p] |= m_toggles[t];
            }
            if (m_toggles[t])
                for (const int p : md.toggles[t].hide)
                    hiddenBy[p] = 1;
        }
        for (size_t s = 0; s < md.sliders.size(); ++s) {
            const auto& sl = md.sliders[s];
            const auto& on = sliderOn(sl, m_sliders[s], m_slidersY[s]);
            for (const auto& k : sl.keys)
                for (const int p : k.show)
                    showable[p] = 1;
            for (const int p : on.show)
                shownBy[p] = 1;
            for (const int p : on.hide)
                hiddenBy[p] = 1;
        }
        m_shown.resize(parts);
        for (size_t p = 0; p < parts; ++p)
            m_shown[p] = !hiddenBy[p] && (showable[p] ? shownBy[p] : !md.parts[p].hidden);
        for (size_t p = 0; p < parts; ++p)
            if (m_partSet[p] >= 0)
                m_shown[p] = m_partSet[p];

        m_shapeBase.resize(md.morphs.size());
        for (size_t i = 0; i < md.morphs.size(); ++i)
            m_shapeBase[i] = md.morphs[i].rest;
        for (size_t t = 0; t < md.toggles.size(); ++t)
            if (m_toggles[t])
                for (const auto& [m, w] : md.toggles[t].shapes)
                    m_shapeBase[m] = w;
        for (size_t s = 0; s < md.sliders.size(); ++s) { // along the line between the keys on each side
            const auto& sl = md.sliders[s];
            const float v  = m_sliders[s];
            if (sl.grid) { // 2D: between the four keys around it
                const SCell c    = sliderCell(sl, v, m_slidersY[s]);
                const float w[4] = {(1 - c.fx) * (1 - c.fy), c.fx * (1 - c.fy), (1 - c.fx) * c.fy, c.fx * c.fy};
                std::vector<int> ms;
                for (const auto* k : c.k)
                    for (const auto& [m, _] : k->shapes)
                        if (std::ranges::find(ms, m) == ms.end())
                            ms.push_back(m);
                for (const int m : ms) {
                    float x = 0;
                    for (int i = 0; i < 4; ++i) {
                        const auto it = std::ranges::find(c.k[i]->shapes, m, &std::pair<int, float>::first);
                        x += w[i] * (it != c.k[i]->shapes.end() ? it->second : m_shapeBase[m]);
                    }
                    m_shapeBase[m] = x;
                }
                continue;
            }
            const auto& a  = sl.keys[sliderKey(sl, v)];
            const auto* b  = &a;
            for (const auto& k : sl.keys)
                if (k.at > v) {
                    b = &k;
                    break;
                }
            const float f = b->at > a.at ? std::clamp((v - a.at) / (b->at - a.at), 0.f, 1.f) : 0.f;
            for (const auto& [m, w] : a.shapes)
                m_shapeBase[m] = w;
            for (const auto& [m, w] : b->shapes) {
                const auto it = std::ranges::find(a.shapes, m, &std::pair<int, float>::first);
                m_shapeBase[m] = (it != a.shapes.end() ? it->second : m_shapeBase[m]) * (1 - f) + w * f;
            }
        }
        for (size_t i = 0; i < md.morphs.size(); ++i)
            if (!std::isnan(m_shapeSet[i]))
                m_shapeBase[i] = m_shapeSet[i];

        // node poses: the rest, as the toggles that are on and the sliders have it
        const bool posed = std::ranges::any_of(md.toggles, [](const SAvatarToggle& t) { return !t.poses.empty(); }) ||
            std::ranges::any_of(md.sliders, [](const SAvatarSlider& s) { return std::ranges::any_of(s.keys, [](const SAvatarSlider::SKey& k) { return !k.poses.empty(); }); });
        if (!posed)
            m_nodePose.clear();
        else {
            m_nodePose.resize(md.nodes.size());
            for (size_t i = 0; i < md.nodes.size(); ++i)
                m_nodePose[i] = md.nodes[i].rest;
            for (size_t t = 0; t < md.toggles.size(); ++t)
                if (m_toggles[t])
                    for (const auto& p : md.toggles[t].poses)
                        putPose(m_nodePose[p.node], p, 1.f);
            for (size_t s = 0; s < md.sliders.size(); ++s) {
                const auto& sl = md.sliders[s];
                const float v  = m_sliders[s];
                if (sl.grid) { // 2D: between the four keys around it
                    const SCell      c = sliderCell(sl, v, m_slidersY[s]);
                    std::vector<int> ns;
                    for (const auto* k : c.k)
                        for (const auto& p : k->poses)
                            if (std::ranges::find(ns, p.node) == ns.end())
                                ns.push_back(p.node);
                    for (const int n : ns) {
                        auto at = [&](int i) {
                            STRS d = m_nodePose[n];
                            for (const auto& p : c.k[i]->poses)
                                if (p.node == n)
                                    putPose(d, p, 1.f);
                            return d;
                        };
                        m_nodePose[n] = lerpTRS(lerpTRS(at(0), at(1), c.fx), lerpTRS(at(2), at(3), c.fx), c.fy);
                    }
                    continue;
                }
                const auto& a  = sl.keys[sliderKey(sl, v)];
                const auto* b  = &a;
                for (const auto& k : sl.keys)
                    if (k.at > v) {
                        b = &k;
                        break;
                    }
                const float f = b->at > a.at ? std::clamp((v - a.at) / (b->at - a.at), 0.f, 1.f) : 0.f;
                for (const auto& p : a.poses)
                    putPose(m_nodePose[p.node], p, 1.f);
                if (b != &a)
                    for (const auto& p : b->poses)
                        putPose(m_nodePose[p.node], p, f);
            }
        }
        // what plays while its toggle is on, and what stays in the world
        m_loops.clear();
        std::vector<int> was = std::move(m_dropBy);
        m_dropBy.assign(md.nodes.size(), -1);
        bool drops = false;
        for (size_t t = 0; t < md.toggles.size(); ++t) {
            if (!m_toggles[t])
                continue;
            if (md.toggles[t].loop.seconds > 0)
                m_loops.push_back((int)t);
            for (const int n : md.toggles[t].drop)
                if (m_dropBy[n] < 0) {
                    m_dropBy[n] = (int)t;
                    drops       = true;
                }
        }
        for (const int n : md.fixed)
            if (m_dropBy[n] < 0) {
                m_dropBy[n] = DROP_FIXED;
                drops       = true;
            }
        if (!drops)
            m_dropBy.clear();
        else {
            m_dropAt.resize(md.nodes.size(), M4::identity());
            m_dropTake.resize(md.nodes.size(), 0);
            for (size_t n = 0; n < md.nodes.size(); ++n)
                if (m_dropBy[n] >= 0 && (n >= was.size() || was[n] < 0))
                    m_dropTake[n] = 1;
        }

        // material variants: a batch takes the material of the last variant in effect that has one for it
        if (md.variants.empty()) {
            m_batchMat.clear();
            return;
        }
        m_variantOn.assign(md.variants.size(), 0);
        for (size_t t = 0; t < md.toggles.size(); ++t)
            if (m_toggles[t])
                for (const int v : md.toggles[t].variants)
                    m_variantOn[v] = 1;
        for (size_t s = 0; s < md.sliders.size(); ++s)
            for (const int v : sliderOn(md.sliders[s], m_sliders[s], m_slidersY[s]).variants)
                m_variantOn[v] = 1;
        m_batchMat.resize(md.batches.size());
        for (size_t i = 0; i < md.batches.size(); ++i) {
            const auto& b = md.batches[i];
            m_batchMat[i] = b.material;
            if (b.variants > 0 && b.variants < (int)md.variantMaps.size())
                for (const auto& [v, mat] : md.variantMaps[b.variants])
                    if (m_variantOn[v] && mat >= 0 && mat < (int)md.materials.size())
                        m_batchMat[i] = mat;
        }
    }

    void CAvatarAnimator::resetOutfit() {
        if (!m_model)
            return;
        m_toggles.clear();
        for (const auto& t : m_model->toggles)
            m_toggles.push_back(t.on);
        m_sliders.clear();
        m_slidersY.clear();
        for (const auto& s : m_model->sliders) {
            m_sliders.push_back(s.value);
            m_slidersY.push_back(s.valueY);
        }
        m_partSet.assign(m_model->parts.size(), -1);
        m_shapeSet.assign(m_model->morphs.size(), NAN);
        outfit();
    }

    void CAvatarAnimator::setToggle(int toggle, bool on) {
        if (!m_model || toggle < 0 || toggle >= (int)m_toggles.size())
            return;
        const auto& tg = m_model->toggles;
        if (on)
            for (size_t t = 0; t < tg.size(); ++t)
                if (std::ranges::any_of(tg[t].groups, [&](const std::string& g) { return std::ranges::find(tg[toggle].groups, g) != tg[toggle].groups.end(); }))
                    m_toggles[t] = 0;
        m_toggles[toggle] = on;
        outfit();
    }

    void CAvatarAnimator::setSlider(int slider, float value, float valueY) {
        if (!m_model || slider < 0 || slider >= (int)m_sliders.size())
            return;
        const auto& sl = m_model->sliders[slider];
        if (sl.grid) {
            m_sliders[slider]  = std::isnan(value) ? sl.value : std::clamp(value, -1.f, 1.f);
            m_slidersY[slider] = std::isnan(valueY) ? (std::isnan(value) ? sl.valueY : m_slidersY[slider]) : std::clamp(valueY, -1.f, 1.f);
        } else
            m_sliders[slider] = std::isnan(value) ? sl.value : std::clamp(value, 0.f, 1.f);
        outfit();
    }

    float CAvatarAnimator::slider(int slider) const {
        return slider >= 0 && slider < (int)m_sliders.size() ? m_sliders[slider] : 0.f;
    }

    float CAvatarAnimator::sliderY(int slider) const {
        return slider >= 0 && slider < (int)m_slidersY.size() ? m_slidersY[slider] : 0.f;
    }

    void CAvatarAnimator::setPart(int part, int shown) {
        if (!m_model || part < 0 || part >= (int)m_partSet.size())
            return;
        m_partSet[part] = (int8_t)std::clamp(shown, -1, 1);
        outfit();
    }

    void CAvatarAnimator::setShape(int morph, float weight) {
        if (!m_model || morph < 0 || morph >= (int)m_shapeSet.size())
            return;
        m_shapeSet[morph] = std::isnan(weight) ? NAN : std::clamp(weight, 0.f, 1.f);
        outfit();
    }

    float CAvatarAnimator::shape(int morph) const {
        return morph >= 0 && morph < (int)m_shapeBase.size() ? m_shapeBase[morph] : 0.f;
    }

    // --- face

    void CAvatarAnimator::setExpression(int expression, float weight) {
        m_held       = m_model && expression >= 0 && expression < (int)m_model->expressions.size() ? expression : -1;
        m_heldWeight = std::clamp(weight, 0.f, 1.f);
    }

    void CAvatarAnimator::setGesture(int hand, int gesture) {
        hand &= 1;
        m_gesture[hand] = (uint8_t)std::clamp(gesture, 0, GESTURE_COUNT - 1);
        if (m_gesture[hand] != GESTURE_NEUTRAL)
            m_lastHand = hand;
        else if (m_gesture[1 - hand] != GESTURE_NEUTRAL)
            m_lastHand = 1 - hand;
    }

    void CAvatarAnimator::globals(const std::vector<STRS>& pose, std::vector<M4>& out) const {
        const auto& nodes = m_model->nodes;
        out.resize(nodes.size());
        for (size_t i = 0; i < nodes.size(); ++i) {
            const M4 l = pose[i].matrix();
            out[i]     = nodes[i].parent >= 0 ? out[nodes[i].parent] * l : l;
        }
    }

    float CAvatarAnimator::lowestFoot(const std::vector<M4>& g) const {
        const auto& md = *m_model;
        float       y  = 1e30f;
        for (auto [foot, shin] : {std::pair{HB_L_FOOT, HB_L_LOWER_LEG}, {HB_R_FOOT, HB_R_LOWER_LEG}}) {
            const int n = md.human[foot] >= 0 ? md.human[foot] : md.human[shin];
            if (n >= 0)
                y = std::min(y, md.fix.point(origin(g[n])).y);
        }
        return y < 1e29f ? y : 0.f;
    }

    // --- constraints and springs

    void CAvatarAnimator::setPhysics(bool on) {
        if (on && !m_physics)
            m_springLive = false; // from where the animation has them
        m_physics = on;
    }

    M4 CAvatarAnimator::globalOf(const std::vector<STRS>& pose, int node) const {
        M4 g = M4::identity();
        for (int n = node; n >= 0; n = m_model->nodes[n].parent)
            g = pose[n].matrix() * g;
        return g;
    }

    // VRM 1.0's node constraints, as its spec has them
    void CAvatarAnimator::constrain(std::vector<STRS>& pose) const {
        const auto& md = *m_model;
        for (const auto& c : md.constraints) {
            const Quat dstRest = md.nodes[c.node].rest.r, srcRest = md.nodes[c.source].rest.r;
            Quat       target;
            switch (c.type) {
                case SNodeConstraint::ROTATION: target = dstRest * (srcRest.conj() * pose[c.source].r); break;
                case SNodeConstraint::ROLL: {
                    // the source's turn from its rest, in the node's rest space: the part of it around the axis
                    const Quat d = dstRest.conj() * pose[c.source].r * srcRest.conj() * dstRest;
                    target       = dstRest * arc(d.rotate(c.axis), c.axis) * d;
                    break;
                }
                case SNodeConstraint::AIM: {
                    const int  parent = md.nodes[c.node].parent;
                    const Quat up     = parent >= 0 ? rotationOf(globalOf(pose, parent)) : Quat{};
                    const V3   from   = up.rotate(dstRest.rotate(c.axis));
                    const V3   to     = normalize(origin(globalOf(pose, c.source)) - origin(globalOf(pose, c.node)));
                    if (length(to) < 0.5f)
                        continue;
                    target = up.conj() * arc(from, to) * up * dstRest;
                    break;
                }
            }
            pose[c.node].r = slerp(dstRest, target, c.weight);
        }
    }

    namespace {
        constexpr float SPRING_STEP = 1.f / 60; // seconds
    }

    // spring bones, stepped 60 times a second whatever the frame rate: the avatar's move over the frame is spread over
    // the steps, and so is its pose's (a step between two frames goes by where what the springs hang from was by then:
    // in first person the arms turn with the camera, frame by frame); what's shown is a step as far on into the next one
    // as the frame is, from where it all is now (a sleeve on an arm goes with it as the arm goes, not as it was going)
    void CAvatarAnimator::springs(const SAvatarMotion& m) {
        const auto& md = *m_model;
        if (md.springJoints.empty())
            return;
        if (!m_physics) {
            m_springLive = false;
            return;
        }
        constexpr float STEP      = SPRING_STEP;
        constexpr int   MAX_STEPS = 8; // slower than that and it slows down
        const STRS      body      = decompose(m.world * M4::translation({0, m_lift, 0}));
        if (!m_springLive || m.dt > 0.25f || length(body.t - m_springBody.t) > 2.f + 20.f * m.dt || m_springWas.size() != m_pose.size()) {
            // the first frame, or it jumped
            m_springStepAt = body.matrix();
            springPass(SP_START, m_springStepAt * md.fix, 0, m_global);
            m_springAcc  = 0;
            m_springLive = true;
        } else if (m.dt > 0) {
            const float from  = m_springAcc;
            int         steps = (int)((from + m.dt) / STEP + 1e-3f); // a frame of 1/60 s is a step, not most of one
            m_springAcc       = std::max(0.f, from + m.dt - steps * STEP);
            const bool slow   = steps > MAX_STEPS;
            if (slow) {
                steps       = MAX_STEPS;
                m_springAcc = 0;
            }
            for (int s = 1; s <= steps; ++s) {
                // where the avatar was by then, and how it stood
                const float u = slow ? (float)s / steps : std::clamp((s * STEP - from) / m.dt, 0.f, 1.f);
                const STRS  at{lerp(m_springBody.t, body.t, u), slerp(m_springBody.r, body.r, u), lerp(m_springBody.s, body.s, u)};
                const M4    now = at.matrix();
                m_springMove    = now * m_springStepAt.inverse(); // (the last step can be frames back)
                m_springStepAt  = now;
                springPass(SP_STEP, now * md.fix, STEP, u < 1.f ? springPoseAt(u) : m_global);
            }
        }
        const M4 world = body.matrix() * md.fix;
        m_springMove   = body.matrix() * m_springStepAt.inverse(); // (since the last step)
        springPass(SP_SHOW, world, m_springAcc / STEP, m_global);
        m_springBody = body;
        m_springWas  = m_pose;
        const M4 inv = world.inverse();
        for (size_t n = m_springFrom; n < md.nodes.size(); ++n)
            if (m_springOf[n] != -1)
                m_global[n] = inv * m_springGlobal[n];
    }

    // m_global as the springs go by it, u of the way from the last frame's pose to this one's (only what they read of it,
    // and what's above that)
    const std::vector<M4>& CAvatarAnimator::springPoseAt(float u) {
        const auto& nodes = m_model->nodes;
        for (int n : m_springUp) {
            const STRS &a = m_springWas[n], &b = m_pose[n];
            const M4    l = STRS{lerp(a.t, b.t, u), slerp(a.r, b.r, u), lerp(a.s, b.s, u)}.matrix();
            m_springPose[n] = nodes[n].parent >= 0 ? m_springPose[nodes[n].parent] * l : l;
        }
        return m_springPose;
    }

    void CAvatarAnimator::springPass(eSpringPass pass, const M4& world, float t, const std::vector<M4>& pose) {
        const auto& md = *m_model;
        for (size_t s = 0; s < md.springs.size(); ++s)
            if (const int c = md.springs[s].center; c >= 0) {
                m_centerAt[s]  = world * pose[c];
                m_centerInv[s] = m_centerAt[s].inverse();
            }
        for (size_t s = 0; s < md.springs.size(); ++s)
            if (const int c = md.springs[s].carrier; c >= 0) {
                const M4 at      = world * pose[c];
                m_carrierMove[s] = pass == SP_START ? M4::identity() : at * m_carrierAt[s].inverse(); // (since the last step)
                if (pass != SP_SHOW)
                    m_carrierAt[s] = at;
            }
        for (size_t c = 0; c < md.springColliders.size(); ++c) {
            const auto& k   = md.springColliders[c];
            m_colliderAt[c] = {world.point(pose[k.node].point(k.offset)), world.point(pose[k.node].point(k.tail)), k.radius, k.kind, k.disc};
        }
        // (shown: f of a step, of dt seconds, none of it kept)
        const float f = pass == SP_STEP ? 1.f : t, dt = pass == SP_STEP ? t : t * SPRING_STEP;
        for (size_t n = m_springFrom; n < md.nodes.size(); ++n) {
            const int j = m_springOf[n];
            if (j == -1)
                continue;
            const int p     = md.nodes[n].parent;
            const M4  P     = p < 0 ? world : m_springOf[p] != -1 ? m_springGlobal[p] : world * pose[p];
            STRS      local = m_pose[n];
            M4        G     = P * local.matrix();
            if (j >= 0) {
                const auto& J    = md.springJoints[j];
                const auto& sp   = md.springs[J.spring];
                const M4&   C    = m_centerAt[J.spring];
                const V3    O    = origin(G);
                const V3    rest = G.point(J.tail) - O; // where the animation has it point
                const float len  = length(rest);
                const M4    Pi   = P.inverse();
                // its limit, after the swing and after each collider's push, as VRMC_springBone_limit has it
                const Quat L       = J.limit != LIMIT_NONE ? (rotationOf(G) * J.limitFrame).normalized() : Quat{};
                auto       limited = [&](const V3& p) { return springLimit(J, L, O, p); };
                // its length again, within its limit, out of the colliders
                auto pushed = [&](V3 next) {
                    next = limited(O + normalize(next - O) * len);
                    for (int k : sp.colliders) {
                        if (md.springColliders[k].body & J.startsIn)
                            continue; // (one of the body's, that it starts inside of: its limit keeps it out)
                        const auto& c  = m_colliderAt[k];
                        const V3    ab = c.b - c.a;
                        if (c.kind == COLLIDER_PLANE) {
                            // to its side, as far as the bone's radius
                            const V3    n = normalize(ab);
                            const float h = dot(next - c.a, n);
                            if (h < J.radius)
                                next = limited(O + normalize(next + n * (J.radius - h) - O) * len);
                            continue;
                        }
                        if (c.kind == COLLIDER_DISC) {
                            // out of what's within its radius of the nearest point of the disc: off its face, or round
                            // its edge (on it: off the face it's on, its normal's side if right in it)
                            const V3    n = normalize(ab);
                            const float h = dot(next - c.a, n);
                            V3          f = next - c.a - n * h;
                            if (const float fl = length(f); fl > c.disc)
                                f = f * (c.disc / fl);
                            const V3    q = c.a + f;
                            const float r = J.radius + c.radius;
                            if (const float d = length(next - q); d < r)
                                next = limited(O + normalize((d > 1e-7f ? q + (next - q) * (r / d) : q + n * r) - O) * len);
                            continue;
                        }
                        const float ll = dot(ab, ab);
                        const V3    q  = ll > 1e-12f ? c.a + ab * std::clamp(dot(next - c.a, ab) / ll, 0.f, 1.f) : c.a;
                        const float d  = length(next - q);
                        if (c.kind == COLLIDER_INSIDE) {
                            // pulled back in, the bone's radius inside it
                            const float r = std::max(c.radius - J.radius, 0.f);
                            if (d > r)
                                next = limited(O + normalize(q + (next - q) * (r / d) - O) * len);
                            continue;
                        }
                        const float r = J.radius + c.radius;
                        if (d < r && d > 1e-7f)
                            next = limited(O + normalize(q + (next - q) * (r / d) - O) * len); // pushed out, the same length
                    }
                    if (!std::isfinite(next.x) || !std::isfinite(next.y) || !std::isfinite(next.z))
                        next = O + rest;
                    return next;
                };
                // (the pushes needn't leave it where they found it: colliders that overlap push it into each other, or
                // into its limit. What they'd do to it again where it is, so what's shown starts from there: see below)
                auto again = [&](const V3& at) { return Pi.dir(pushed(at) - at); };
                if (pass == SP_START) {
                    m_carried[j] = {};
                    m_tail[j] = m_tailPrev[j] = m_centerInv[J.spring].point(O + rest);
                    m_again[j]                 = again(O + rest);
                    m_springGlobal[n]          = G;
                    continue;
                }
                const V3 cur = C.point(m_tail[j]), prev = C.point(m_tailPrev[j]);
                const V3 byBody = m_springMove.point(cur) - cur;
                // a PhysBone's Immobile (All Motion): carried along with all its carrier does, as much (and where the avatar
                // goes no less than its own immobile); its own swing goes on, the carrying doesn't: the last step's is taken
                // out of how it was going
                const V3 carried = sp.center < 0 && sp.carrier >= 0 ?
                    byBody * std::max(sp.immobile, sp.parentImmobile) + (m_carrierMove[J.spring].point(cur) - cur - byBody) * sp.parentImmobile :
                    V3{};
                V3 next = cur + carried + (cur - prev - m_carried[j]) * ((1 - J.drag) * f) + normalize(rest) * (J.stiffness * dt * J.scale) +
                    J.gravityDir * (J.gravity * dt * J.scale);
                if (sp.center < 0 && sp.carrier < 0)
                    // the drag is of the air, and that moves along with the avatar some: going somewhere at a steady
                    // speed swings it less than starting, stopping and turning do
                    next += byBody * (J.drag * sp.immobile);
                next = pushed(next);
                if (pass == SP_STEP) {
                    m_carried[j]  = carried;
                    m_tailPrev[j] = m_tail[j];
                    m_tail[j]     = m_centerInv[J.spring].point(next);
                    m_again[j]    = again(next);
                } else
                    // shown: less what pushing it again where the step left it would do, less and less on into the next step
                    next = limited(next - P.dir(m_again[j]) * (1 - f));
                // turned from where the animation points it to the tail
                const V3 a = normalize(Pi.dir(rest)), d = normalize(Pi.dir(next - O));
                if (length(a) > 0.5f && length(d) > 0.5f) {
                    local.r = (arc(a, d) * local.r).normalized();
                    G       = P * local.matrix();
                }
            }
            m_springGlobal[n] = G;
        }
    }

    void CAvatarAnimator::update(const SAvatarMotion& m) {
        if (!m_model)
            return;
        const auto& md = *m_model;
        m_time += m.dt;

        // a step down a stair isn't a fall (but coming out of flying off the ground is)
        if (m.flying)
            m_flewOff = true;
        else if (m.onGround)
            m_flewOff = false;
        m_flying        = m.flying;
        m_airTime       = m.onGround || m.flying ? 0.f : m_airTime + m.dt;
        const bool air  = m_airTime > 0.15f || (!m.onGround && !m.flying && (m.vy > 1.f || m_flewOff));
        // a bit of hysteresis between the gaits
        auto       pace = [&] {
            const float up = m_state == ST_IDLE || m_state == ST_AIR ? 0.4f : 0.25f;
            if (m.flying || m.speed < up)
                return ST_IDLE;
            if (m.speed < (m_state == ST_RUN ? 2.6f : 3.f))
                return ST_WALK;
            return ST_RUN;
        };
        const eState st = air ? ST_AIR : pace();

        if (st != m_state || m_first) {
            const SSource src = choose(st, m);
            if (!(src == m_source) || m_first) {
                const bool gaits = (st == ST_WALK || st == ST_RUN) && (m_state == ST_WALK || m_state == ST_RUN);
                if (gaits && src.kind == SRC_CLIP && m_source.kind == SRC_CLIP && md.clips[m_source.clip].duration > 0)
                    m_clipTime = m_clipTime / md.clips[m_source.clip].duration * md.clips[src.clip].duration; // stay in step
                else
                    m_clipTime = 0;
                if (!m_first) {
                    m_from = m_pose;
                    m_fade = 0;
                }
                m_source = src;
            }
            m_state = st;
            m_first = false;
        }
        // moving ends an emote
        if (m_emote >= 0 && m_state != ST_IDLE)
            m_emoteOut = true;
        // a jump that's over while still in the air
        if (m_state == ST_AIR && m_source.kind == SRC_CLIP && !m_source.loop && md.clipFor[CLIP_FALL] >= 0 && m_clipTime >= md.clips[m_source.clip].duration) {
            m_from     = m_pose;
            m_fade     = 0;
            m_source   = {SRC_CLIP, md.clipFor[CLIP_FALL]};
            m_clipTime = 0;
        }

        for (size_t i = 0; i < md.nodes.size(); ++i)
            m_target[i] = m_nodePose.empty() ? md.nodes[i].rest : m_nodePose[i];
        if (!m_loops.empty())
            loopPoses(m_target);
        switch (m_source.kind) {
            case SRC_CLIP: {
                const auto& clip = md.clips[m_source.clip];
                float       rate = 1;
                if (m_state == ST_WALK || m_state == ST_RUN) {
                    const bool  run     = m_source.clip == md.clipFor[CLIP_RUN];
                    const float natural = clip.naturalSpeed > 0 ? clip.naturalSpeed : run ? 4.f * md.height / 1.75f : 1.5f * md.height / 1.75f;
                    rate                = std::clamp(m.speed / std::max(natural, 0.05f), 0.6f, 2.f);
                }
                if (!m_source.frozen)
                    m_clipTime += m.dt * rate;
                if (clip.duration > 0)
                    m_clipTime = m_source.loop ? std::fmod(m_clipTime, clip.duration) : std::min(m_clipTime, clip.duration);
                sampleClip(clip, m_clipTime, m_target);
                break;
            }
            case SRC_PROC: gait(m, m_state == ST_AIR, m_target); break;
            case SRC_REST: break;
        }
        m_gaitUsed = m_source.kind == SRC_PROC;

        if (m_fade < 1.f) {
            m_fade        = std::min(1.f, m_fade + m.dt / 0.2f);
            const float w = smoothstep01(m_fade);
            for (size_t i = 0; i < m_pose.size(); ++i)
                m_pose[i] = {lerp(m_from[i].t, m_target[i].t, w), slerp(m_from[i].r, m_target[i].r, w), lerp(m_from[i].s, m_target[i].s, w)};
        } else
            m_pose = m_target;
        // where the pose changed all at once: what was shown going on as it went (its turns and moves last frame) and
        // settling into the new one (see m_settleRate)
        // (jumping off or landing the body's own move changes all at once, a frame after landing: the hips, k of them,
        // go on as they went, the root's move past what its speed last frame took it and that speed's change taken off
        // them; the speed's after this frame's step)
        int  liftHips = -1;
        V3   liftV;
        auto carry = [&](float k) {
            const int hips = md.human[HB_HIPS];
            const V3 root = origin(m.world);
            // (not when it was put somewhere else)
            if (k <= 0 || hips < 0 || !m_settleRootSet || m.dt <= 1e-4f || m_settleT.size() != m_pose.size() || length(root - m_settleRoot) > 2.f)
                return;
            const V3  rootV = (root - m_settleRoot) * (1.f / m.dt);
            const int up = md.nodes[hips].parent;
            const M4  toLocal = (m.world * md.fix * (up >= 0 ? m_global[up] : M4::identity())).inverse();
            m_settleT[hips] -= toLocal.dir(root - m_settleRoot - m_settleRootV * m.dt) * k;
            liftHips = hips, liftV = liftV + toLocal.dir(rootV - m_settleRootV) * k;
        };
        if (m_settleAsk > 0 && m_settleNow.size() == m_pose.size() && m.dt > 1e-4f) {
            const size_t n = m_pose.size();
            m_settleR.resize(n), m_settleRV.resize(n), m_settleT.resize(n), m_settleTV.resize(n);
            const float was = m_settleWas.size() == n && m_settleDt > 1e-4f ? m_settleDt : 0.f;
            for (size_t i = 0; i < n; ++i) {
                m_settleR[i] = rotationVector(m_settleNow[i].r * m_pose[i].r.conj());
                m_settleT[i] = m_settleNow[i].t - m_pose[i].t;
                V3 rv{}, tv{};
                if (was > 0) {
                    rv = rotationVector(m_settleNow[i].r * m_settleWas[i].r.conj()) * (1.f / was);
                    tv = (m_settleNow[i].t - m_settleWas[i].t) * (1.f / was);
                }
                m_settleRV[i] = length(rv) > 30.f ? rv * (30.f / length(rv)) : rv; // (not what a snap just before would give)
                m_settleTV[i] = length(tv) > 5.f ? tv * (5.f / length(tv)) : tv;
            }
            if (was > 0)
                carry(m_settleLift);
            m_settleLiftWas = m_settleLift;
            m_settleRate    = m_settleAsk;
            m_settleJust = true;
        }
        m_settleAsk  = 0;
        m_settleLift = 0;
        if (m_settleRate > 0 && m_settleR.size() == m_pose.size()) {
            const float dt   = std::clamp(m.dt, 0.f, 0.05f);
            float       left = 0;
            // (the frame after: less how fast the new pose goes, so what's shown goes on at its own speed, not that and
            // the new pose's too)
            if (m_settleFresh && m_settleNew.size() == m_pose.size() && m.dt > 1e-4f) {
                for (size_t i = 0; i < m_pose.size(); ++i) {
                    V3 rv = rotationVector(m_pose[i].r * m_settleNew[i].r.conj()) * (1.f / m.dt), tv = (m_pose[i].t - m_settleNew[i].t) * (1.f / m.dt);
                    m_settleRV[i] -= length(rv) > 30.f ? rv * (30.f / length(rv)) : rv;
                    m_settleTV[i] -= length(tv) > 5.f ? tv * (5.f / length(tv)) : tv;
                }
                if (!m_settleJust)
                    carry(m_settleLiftWas);
            }
            m_settleFresh = m_settleJust;
            if (m_settleJust)
                m_settleNew = m_pose;
            m_settleJust = false;
            for (size_t i = 0; i < m_pose.size(); ++i) {
                for (int k = 0; k < 3; ++k) {
                    springTo((&m_settleR[i].x)[k], (&m_settleRV[i].x)[k], 0.f, m_settleRate, dt);
                    springTo((&m_settleT[i].x)[k], (&m_settleTV[i].x)[k], 0.f, m_settleRate, dt);
                }
                if ((int)i == liftHips)
                    m_settleTV[i] -= liftV;
                m_pose[i].r = (fromRotationVector(m_settleR[i]) * m_pose[i].r).normalized();
                m_pose[i].t += m_settleT[i];
                left = std::max({left, length(m_settleR[i]), length(m_settleRV[i]) * 0.05f, length(m_settleT[i]) * 10.f, length(m_settleTV[i]) * 0.5f});
            }
            if (left < 1e-4f)
                m_settleRate = 0;
        } else
            m_settleRate = 0;
        std::swap(m_settleWas, m_settleNow);
        m_settleNow = m_pose;
        m_settleDt  = m.dt;
        if (const V3 root = origin(m.world); m.dt > 1e-4f) {
            m_settleRootV   = m_settleRootSet && length(root - m_settleRoot) <= 2.f ? (root - m_settleRoot) * (1.f / m.dt) : V3{};
            m_settleRoot    = root;
            m_settleRootSet = true;
        }
        if (m_emote >= 0)
            emotePose(m.dt, m_pose);
        const float emoteW = m_emote >= 0 ? smoothstep01(m_emoteW) : 0.f;
        firstPersonState(m, emoteW);
        attackState(m);
        hands(m.dt, m_pose);
        // (first person: the trunk turned to the camera, the head only the rest of the way; an attack's turn on that, the
        // head going with it more than looking about)
        const float twist = firstPersonTrunk(m, m_pose);
        attackTrunk(m_pose);
        look(m, m_pose, (1.f - emoteW) * (1.f - ATTACK_LOOK * smoothstep01(m_attackW)), twist);
        face(m, m_pose);
        firstPersonArms(m, m_pose);
        attackArms(m, m_pose);
        constrain(m_pose);
        globals(m_pose, m_global);

        // emotes keep the feet on the ground but for jumps and falls; clips have the hips where they want them, and
        // walking puts the feet where they step
        const float ground = m_restFootY - lowestFoot(m_global);
        float       lift   = 0.f;
        if (emoteW > 0)
            lift = lerpf(lift, m_emotes[m_emote]->grounded ? ground : 0.f, emoteW);
        m_lift += (lift - m_lift) * std::min(1.f, m.dt * 15.f);
        springs(m);
        drops(m);

        for (size_t j = 0; j < md.joints.size(); ++j) {
            const M4 s = m_global[md.joints[j].node] * md.joints[j].inverseBind;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c)
                    m_joints[j * 12 + r * 4 + c] = s.m[c * 4 + r];
        }
    }

    std::string CAvatarAnimator::playing() const {
        if (!m_model)
            return "";
        static constexpr const char* STATE[] = {"idle", "walk", "run", "air"};
        if (m_emote >= 0 && !m_emoteOut) {
            const SAvatarEmote& e = *m_emotes[m_emote];
            return std::format("{}: emote {}{}", STATE[m_state], e.name, m_emoteLoop ? " (looping)" : e.hold && m_emoteTime >= e.anim.duration ? " (held)" : "");
        }
        switch (m_source.kind) {
            case SRC_CLIP: return std::format("{}: clip {}{}", STATE[m_state], m_model->clips[m_source.clip].name, m_source.frozen ? " (held)" : "");
            case SRC_PROC: return std::format("{}: procedural", STATE[m_state]);
            case SRC_REST: return std::format("{}: rest pose", STATE[m_state]);
        }
        return "";
    }

    // --- emotes

    int CAvatarAnimator::findEmote(std::string_view name) const {
        if (name.empty())
            return -1;
        for (int loose = 0; loose < 2; ++loose) {
            const std::string want = loose ? normName(name) : lower(std::string(name));
            for (size_t i = 0; i < m_emotes.size(); ++i)
                if ((loose ? normName(m_emotes[i]->name) : lower(m_emotes[i]->name)) == want)
                    return (int)i;
        }
        int        n        = 0;
        const auto [end, e] = std::from_chars(name.data(), name.data() + name.size(), n);
        return e == std::errc{} && end == name.data() + name.size() && n >= 1 && n <= (int)m_emotes.size() ? n - 1 : -1;
    }

    int CAvatarAnimator::addEmote(std::shared_ptr<const SAvatarEmote> emote) {
        if (!m_model || !emote)
            return -1;
        // made for this model: its nodes and expressions are
        const auto& md = *m_model;
        if (std::ranges::any_of(emote->anim.channels, [&](const SAnimChannel& c) { return c.node < 0 || c.node >= (int)md.nodes.size(); }) ||
            std::ranges::any_of(emote->faces, [&](const SAnimChannel& c) { return c.node < 0 || c.node >= (int)md.expressions.size(); }))
            return -1;
        const std::string name = lower(emote->name);
        for (size_t i = 0; i < m_emotes.size(); ++i)
            if (lower(m_emotes[i]->name) == name) {
                m_emotes[i] = std::move(emote);
                return (int)i;
            }
        m_emotes.push_back(std::move(emote));
        return (int)m_emotes.size() - 1;
    }

    void CAvatarAnimator::playEmote(int emote, int loop) {
        if (!m_model || emote < 0 || emote >= (int)m_emotes.size())
            return;
        // one after another: from where the last one has the body
        if (m_emote >= 0 && m_emoteW > 0 && m_emoteNow.size() == m_pose.size()) {
            m_emoteFrom = m_emoteNow;
            m_emoteSwap = 0;
        } else
            m_emoteSwap = 1;
        m_emote     = emote;
        m_emoteTime = 0;
        m_emoteLoop = loop < 0 ? m_emotes[emote]->loop : loop > 0;
        m_emoteOut  = false;
        m_emoteClock = -1;
        ++m_emoteStarts;
    }

    void CAvatarAnimator::stopEmote() {
        if (m_emote >= 0)
            m_emoteOut = true;
        m_emoteClock = -1;
    }

    // the emote's pose, faded in over the pose, and its faces; done, it fades out. Its time goes on with the frames, or
    // is where its sound is heard (setEmoteClock())
    void CAvatarAnimator::emotePose(float dt, std::vector<STRS>& pose) {
        const SAvatarEmote& em  = *m_emotes[m_emote];
        const float         dur = em.anim.duration;
        if (m_emoteClock >= 0 && !m_emoteOut)
            m_emoteTime = (float)(m_emoteLoop && dur > 0 ? std::fmod(m_emoteClock * em.speed, (double)dur) : m_emoteClock * em.speed);
        else
            m_emoteTime += dt * em.speed;
        m_emoteClock = -1;
        if (m_emoteLoop && dur > 0)
            m_emoteTime = std::fmod(m_emoteTime, dur);
        else {
            m_emoteTime = std::min(m_emoteTime, dur);
            if (!em.hold && m_emoteTime >= dur - std::min(0.3f, dur * 0.25f)) // fading out as it ends
                m_emoteOut = true;
        }
        m_emoteW = m_emoteOut ? std::max(0.f, m_emoteW - dt / 0.3f) : std::min(1.f, m_emoteW + dt / 0.25f);
        if (m_emoteOut && m_emoteW <= 0) {
            m_emote    = -1;
            m_emoteOut = false;
            std::ranges::fill(m_emoteFace, 0.f);
            m_emoteEyes = 0;
            return;
        }

        // what it doesn't move keeps the pose under it
        m_emoteNow = pose;
        sampleClip(em.anim, m_emoteTime, m_emoteNow);
        if (m_emoteSwap < 1) {
            m_emoteSwap   = std::min(1.f, m_emoteSwap + dt / 0.25f);
            const float s = smoothstep01(m_emoteSwap);
            for (size_t i = 0; i < m_emoteNow.size(); ++i)
                m_emoteNow[i] = {lerp(m_emoteFrom[i].t, m_emoteNow[i].t, s), slerp(m_emoteFrom[i].r, m_emoteNow[i].r, s), lerp(m_emoteFrom[i].s, m_emoteNow[i].s, s)};
        }
        const float w = smoothstep01(m_emoteW);
        for (size_t i = 0; i < pose.size(); ++i)
            pose[i] = w >= 1 ? m_emoteNow[i] : STRS{lerp(pose[i].t, m_emoteNow[i].t, w), slerp(pose[i].r, m_emoteNow[i].r, w), lerp(pose[i].s, m_emoteNow[i].s, w)};

        std::ranges::fill(m_emoteFace, 0.f);
        for (const auto& f : em.faces) {
            float v[4];
            sampleChannel(f, m_emoteTime, v);
            m_emoteFace[f.node] = std::max(m_emoteFace[f.node], std::clamp(v[0], 0.f, 1.f) * w);
        }
        m_emoteEyes = 0;
        if (!em.eyes.empty()) {
            float v[4];
            sampleChannel(em.eyes[0], m_emoteTime, v);
            // a VRM animation faces +Z, its left +X
            const V3 d   = Quat{v[0], v[1], v[2], v[3]}.normalized().rotate({0, 0, 1});
            m_emoteYaw   = std::atan2(-d.x, d.z) * 57.2957795f;
            m_emotePitch = std::asin(std::clamp(d.y, -1.f, 1.f)) * 57.2957795f;
            m_emoteEyes  = w;
        }
    }

    const char* expressionPresetName(int preset) {
        return preset >= 0 && preset < EX_COUNT ? PRESET_NAMES[preset].data() : "";
    }

    const char* gestureName(int gesture) {
        return gesture >= 0 && gesture < GESTURE_COUNT ? GESTURE_NAMES[gesture].data() : "";
    }

    int gestureFromName(std::string_view name) {
        std::string n;
        for (const char c : name)
            if (std::isalnum((unsigned char)c))
                n += (char)std::tolower((unsigned char)c);
        if (n.size() == 1 && n[0] >= '0' && n[0] < '0' + GESTURE_COUNT)
            return n[0] - '0';
        for (int g = 0; g < GESTURE_COUNT; ++g)
            if (n == GESTURE_NAMES[g])
                return g;
        static const std::map<std::string_view, int> ALIASES = {
            {"none", GESTURE_NEUTRAL},  {"idle", GESTURE_NEUTRAL},       {"handopen", GESTURE_OPEN}, {"fingerpoint", GESTURE_POINT}, {"peace", GESTURE_VICTORY},
            {"rock", GESTURE_ROCK},     {"rockandroll", GESTURE_ROCK},   {"gun", GESTURE_GUN},       {"thumb", GESTURE_THUMBS_UP},   {"thumbs", GESTURE_THUMBS_UP},
        };
        const auto it = ALIASES.find(n);
        return it == ALIASES.end() ? -1 : it->second;
    }
}
