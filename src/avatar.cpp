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

            for (int b = 0; b < HB_COUNT; ++b)
                nc.has[b] = h[b] >= 0 && b != HB_L_EYE && b != HB_R_EYE;
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

            void addJoint(int node, int spring, SSpringJoint j, const V3& tail, float meters) {
                if (length(tail) * metersAt(node) < 1e-4f)
                    return; // no length to point with: it stays as the animation has it
                j.node   = node;
                j.spring = spring;
                j.tail   = tail;
                j.length = length(tail) * metersAt(node);
                j.radius = std::max(meters, 0.f);
                model.springJoints.push_back(j);
                claimed[node] = true;
            }

            // a bone and all under it swing, as VRM 0.x's bone groups have it: each toward its first child, the
            // last ones toward a made-up end `leaf` model units on (0: as long as the bone before)
            void addTree(int root, int spring, const SSpringJoint& params, float radius, const std::set<int>& ignore, float leaf, bool skinOnly) {
                auto skip = [&](int n) { return claimed[n] || human[n] || ignore.contains(n) || (skinOnly && !skinJoint[n]); };
                std::vector<int> stack{root};
                while (!stack.empty()) {
                    const int n = stack.back();
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
                    addJoint(n, spring, params, tail, radius);
                    for (auto c = kids[n].rbegin(); c != kids[n].rend(); ++c)
                        stack.push_back(*c);
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
            // bones in)}, or a plane {"name", "node", "offset", "normal": [x, y, z]} (they keep to where it points)], in the node's units;
            // "springs": [{"name", "bones": [roots], "ignore": [bones], "stiffness", "drag", "gravity", "gravityDir": [x, y, z],
            // "radius", "center": bone, "immobile", "colliders": [names; "body" for the ones made for the body]}]: each root and all under
            // it swing, as VRM 0.x has it. A spring that doesn't name colliders keeps out of all of the file's, or the body's.
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
                        const int    k      = c.get("normal") ? addCollider(node, o, o + normalize(jvec(c.get("normal"), {0, 1, 0})), 0, COLLIDER_PLANE) :
                                                                addCollider(node, o, jvec(c.get("tail"), o), (float)jnum(c.get("radius"), 0.05) * metersAt(node),
                                                                            inside && inside->type == SJson::J_BOOL && inside->num != 0 ? COLLIDER_INSIDE : COLLIDER_OUTSIDE);
                        named[lower(std::string(jstr(c.get("name"))))].push_back(k);
                        all.push_back(k);
                    }
                for (const auto& s : list) {
                    SSpring sp;
                    sp.name     = jstr(s.get("name"));
                    sp.immobile = std::clamp((float)jnum(s.get("immobile"), immobile()), 0.f, 1.f);
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
                    const float radius = (float)jnum(s.get("radius"), 0.02);
                    const int   index  = (int)model.springs.size();
                    model.springs.push_back(std::move(sp));
                    if (const auto* bones = jarr(s.get("bones")))
                        for (const auto& b : *bones) {
                            if (const int n = nodeNamed(jstr(&b)); n >= 0)
                                addTree(n, index, p, radius * metersAt(n), ignore, 0.07f, false);
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
            // ("emotes": [{"name", "file", "clip", "loop", "hold", "grounded", "speed"}]: a file by where the settings file is,
            // else a clip of the model's) and the request's files; a later one of a name replaces an earlier one
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
                        const std::string name(jstr(x.get("name")));
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
                            if (!file.empty())
                                files.push_back(std::format("{} ({})", e->name, e->from));
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
        b.clips();
        b.expressions();
        b.outfit();
        b.springs();
        b.constraints();
        b.emotes();
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

    // --- animation

    void CAvatarAnimator::reset(std::shared_ptr<const SAvatarModel> model) {
        m_model = std::move(model);
        m_pose.clear();
        m_joints.clear();
        m_source   = {};
        m_state    = ST_IDLE;
        m_fade     = 1;
        m_clipTime = m_phase = m_time = m_crouch = m_run = m_amp = m_airTime = m_lift = 0;
        m_first    = true;
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
        m_springGlobal.clear();
        m_centerAt.clear();
        m_centerInv.clear();
        m_colliderModel.clear();
        m_colliderAt.clear();
        m_springAcc  = 0;
        m_springLive = false;
        m_emotes.clear();
        m_emote     = -1;
        m_emoteTime = m_emoteW = 0;
        m_emoteLoop = m_emoteOut = false;
        m_emoteSwap = 1;
        m_emoteNow.clear();
        m_emoteFrom.clear();
        m_emoteFace.clear();
        m_emoteEyes = 0;
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
        m_springGlobal.assign(md.nodes.size(), M4::identity());
        m_centerAt.assign(md.springs.size(), M4::identity());
        m_centerInv = m_centerAt;
        m_colliderModel.assign(md.springColliders.size(), {});
        m_colliderAt = m_colliderModel;
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
            {{0.15f, 0.12f, 0.16f, 0.2f, 0.24f}, {0.05f, 0, -0.05f, -0.1f}, 0.15f, {}, -1}, // neutral
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
            const int          g    = em && em->gesture[hand] >= 0 ? em->gesture[hand] : m_gesture[hand];
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

    void CAvatarAnimator::procedural(eState st, const SAvatarMotion& m, std::vector<STRS>& pose) {
        const auto& md = *m_model;
        const auto& h  = md.human;
        const V3    F = md.forward, RT = normalize(cross(F, UP));
        const float k = std::min(1.f, m.dt * 8.f);
        const bool  air = st == ST_AIR;

        m_crouch += ((m.crouched && !air ? 1.f : 0.f) - m_crouch) * k;
        m_run += (std::clamp((m.speed - 2.f) / 3.f, 0.f, 1.f) - m_run) * k;
        m_amp += ((air ? 0.f : std::clamp(m.speed / 1.5f, 0.f, 1.f)) - m_amp) * k;
        const float cycle = std::max(0.3f, md.height * lerpf(0.86f, 1.37f, m_run)); // meters per stride pair
        m_phase           = std::fmod(m_phase + TAU * (air ? 0.f : m.speed) * m.dt / cycle, TAU);

        const float A = lerpf(0.4f, 0.75f, m_run) * m_amp, K = lerpf(0.5f, 1.3f, m_run) * m_amp, swing = lerpf(0.35f, 0.6f, m_run) * m_amp;
        const float elbow = lerpf(0.15f, lerpf(0.25f, 1.4f, m_run), m_amp), lean = lerpf(0.03f, 0.18f, m_run) * m_amp + 0.3f * m_crouch;
        const float s = std::sin(m_phase), c = std::cos(m_phase);
        const float breathe = 0.015f * std::sin(m_time * 1.7f) * (1.f - m_amp);

        for (int side = 0; side < 2; ++side) {
            const float sgn = side == 0 ? 1.f : -1.f;
            const int   leg = side == 0 ? HB_L_UPPER_LEG : HB_R_UPPER_LEG;
            float       thigh, knee;
            if (air) {
                thigh = side == 0 ? 0.5f : -0.15f;
                knee  = side == 0 ? 0.9f : 0.5f;
            } else {
                thigh = sgn * A * s + 0.75f * m_crouch;
                knee  = 0.05f + K * std::max(0.f, sgn * c) + 1.3f * m_crouch;
            }
            rotateBone(pose, leg, Quat::axisAngle(RT, thigh));
            rotateBone(pose, leg + 1, Quat::axisAngle(RT, -knee));
            rotateBone(pose, leg + 2, Quat::axisAngle(RT, -0.7f * (thigh - knee)));

            // arms: from the rest pose (T or A) down along the body, then swing
            const int ua = side == 0 ? HB_L_UPPER_ARM : HB_R_UPPER_ARM;
            const V3  restDir = normalize(origin(m_restGlobal[h[ua + 1]]) - origin(m_restGlobal[h[ua]]));
            const V3  outward = side == 0 ? -RT : RT;
            const V3  target  = air ? normalize(outward * 0.8f - UP * 0.6f) : normalize(outward * 0.2f - UP + F * 0.05f);
            const Quat down   = arc(restDir, target);
            const float armSwing = air ? 0.f : -sgn * swing * s;
            rotateBone(pose, ua, Quat::axisAngle(RT, armSwing) * down);
            rotateBone(pose, ua + 1, Quat::axisAngle(down.conj().rotate(RT), air ? 0.4f : elbow));
        }

        // lean into the stride, twist against the hips, breathe
        const int spine = h[HB_SPINE] >= 0 ? HB_SPINE : -1, chest = h[HB_CHEST] >= 0 ? HB_CHEST : h[HB_UPPER_CHEST] >= 0 ? HB_UPPER_CHEST : -1;
        const Quat torso = Quat::axisAngle(UP, -0.12f * A * s) * Quat::axisAngle(RT, -lean + breathe);
        if (spine >= 0 && chest >= 0) {
            rotateBone(pose, spine, slerp(Quat{}, torso, 0.5f));
            rotateBone(pose, chest, slerp(Quat{}, torso, 0.5f));
        } else if (spine >= 0 || chest >= 0)
            rotateBone(pose, spine >= 0 ? spine : chest, torso);
    }

    // the head turned toward where the camera looks; weight: how much (an emote has the head)
    void CAvatarAnimator::look(const SAvatarMotion& m, std::vector<STRS>& pose, float weight) const {
        const auto& md = *m_model;
        if (!md.humanoid || weight < 1e-3f)
            return;
        const V3 RT = normalize(cross(md.forward, UP));
        Quat     d  = Quat::axisAngle(UP, -std::clamp(m.lookYaw, -1.2f, 1.2f) * 0.8f) * Quat::axisAngle(RT, std::clamp(m.lookPitch, -1.3f, 1.3f) * 0.6f);
        if (weight < 1)
            d = slerp(Quat{}, d, weight);
        if (md.human[HB_NECK] >= 0) {
            rotateBone(pose, HB_NECK, slerp(Quat{}, d, 0.4f));
            rotateBone(pose, HB_HEAD, slerp(Quat{}, d, 0.6f));
        } else
            rotateBone(pose, HB_HEAD, d);
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

    // spring bones, stepped 60 times a second whatever the frame rate: the avatar's move over the frame is spread
    // over the steps, and what's shown is that far on into the next one
    void CAvatarAnimator::springs(const SAvatarMotion& m) {
        const auto& md = *m_model;
        if (md.springJoints.empty())
            return;
        if (!m_physics) {
            m_springLive = false;
            return;
        }
        constexpr float STEP      = 1.f / 60;
        constexpr int   MAX_STEPS = 8; // slower than that and it slows down
        const STRS      body      = decompose(m.world * M4::translation({0, m_lift, 0}));
        for (size_t c = 0; c < md.springColliders.size(); ++c) {
            const auto& k      = md.springColliders[c];
            m_colliderModel[c] = {m_global[k.node].point(k.offset), m_global[k.node].point(k.tail), k.radius, k.kind};
        }
        if (!m_springLive || m.dt > 0.25f || length(body.t - m_springBody.t) > 2.f + 20.f * m.dt) {
            // the first frame, or it jumped
            m_springStepAt = body.matrix();
            springPass(SP_START, m_springStepAt * md.fix, 0);
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
                // where the avatar was by then
                const float u = slow ? (float)s / steps : std::clamp((s * STEP - from) / m.dt, 0.f, 1.f);
                const STRS  at{lerp(m_springBody.t, body.t, u), slerp(m_springBody.r, body.r, u), lerp(m_springBody.s, body.s, u)};
                const M4    now = at.matrix();
                m_springMove    = now * m_springStepAt.inverse(); // (the last step can be frames back)
                m_springStepAt  = now;
                springPass(SP_STEP, now * md.fix, STEP);
            }
        }
        const M4 world = body.matrix() * md.fix;
        springPass(SP_SHOW, world, m_springAcc / STEP);
        m_springBody = body;
        const M4 inv = world.inverse();
        for (size_t n = m_springFrom; n < md.nodes.size(); ++n)
            if (m_springOf[n] != -1)
                m_global[n] = inv * m_springGlobal[n];
    }

    void CAvatarAnimator::springPass(eSpringPass pass, const M4& world, float t) {
        const auto& md = *m_model;
        for (size_t s = 0; s < md.springs.size(); ++s)
            if (const int c = md.springs[s].center; c >= 0) {
                m_centerAt[s]  = world * m_global[c];
                m_centerInv[s] = m_centerAt[s].inverse();
            }
        if (pass == SP_STEP)
            for (size_t c = 0; c < m_colliderModel.size(); ++c)
                m_colliderAt[c] = {world.point(m_colliderModel[c].a), world.point(m_colliderModel[c].b), m_colliderModel[c].radius, m_colliderModel[c].kind};
        for (size_t n = m_springFrom; n < md.nodes.size(); ++n) {
            const int j = m_springOf[n];
            if (j == -1)
                continue;
            const int p     = md.nodes[n].parent;
            const M4  P     = p < 0 ? world : m_springOf[p] != -1 ? m_springGlobal[p] : world * m_global[p];
            STRS      local = m_pose[n];
            M4        G     = P * local.matrix();
            if (j >= 0) {
                const auto& J    = md.springJoints[j];
                const M4&   C    = m_centerAt[J.spring];
                const V3    O    = origin(G);
                const V3    rest = G.point(J.tail) - O; // where the animation has it point
                if (pass == SP_START) {
                    m_tail[j] = m_tailPrev[j] = m_centerInv[J.spring].point(O + rest);
                    m_springGlobal[n]          = G;
                    continue;
                }
                V3 to;
                if (pass == SP_STEP) {
                    const float len = length(rest);
                    const V3    cur = C.point(m_tail[j]), prev = C.point(m_tailPrev[j]);
                    V3 next = cur + (cur - prev) * (1 - J.drag) + normalize(rest) * (J.stiffness * t * J.scale) + J.gravityDir * (J.gravity * t * J.scale);
                    if (const auto& sp = md.springs[J.spring]; sp.center < 0)
                        // the drag is of the air, and that moves along with the avatar some: going somewhere at a steady
                        // speed swings it less than starting, stopping and turning do
                        next += (m_springMove.point(cur) - cur) * (J.drag * sp.immobile);
                    next    = O + normalize(next - O) * len;
                    for (int k : md.springs[J.spring].colliders) {
                        const auto& c  = m_colliderAt[k];
                        const V3    ab = c.b - c.a;
                        if (c.kind == COLLIDER_PLANE) {
                            // to its side, as far as the bone's radius
                            const V3    n = normalize(ab);
                            const float h = dot(next - c.a, n);
                            if (h < J.radius)
                                next = O + normalize(next + n * (J.radius - h) - O) * len;
                            continue;
                        }
                        const float ll = dot(ab, ab);
                        const V3    q  = ll > 1e-12f ? c.a + ab * std::clamp(dot(next - c.a, ab) / ll, 0.f, 1.f) : c.a;
                        const float d  = length(next - q);
                        if (c.kind == COLLIDER_INSIDE) {
                            // pulled back in, the bone's radius inside it
                            const float r = std::max(c.radius - J.radius, 0.f);
                            if (d > r)
                                next = O + normalize(q + (next - q) * (r / d) - O) * len;
                            continue;
                        }
                        const float r = J.radius + c.radius;
                        if (d < r && d > 1e-7f)
                            next = O + normalize(q + (next - q) * (r / d) - O) * len; // pushed out, the same length
                    }
                    if (!std::isfinite(next.x) || !std::isfinite(next.y) || !std::isfinite(next.z))
                        next = O + rest;
                    m_tailPrev[j] = m_tail[j];
                    m_tail[j]     = m_centerInv[J.spring].point(next);
                    to            = next;
                } else
                    to = C.point(m_tail[j] + (m_tail[j] - m_tailPrev[j]) * t);
                // turned from where the animation points it to the tail
                const M4 Pi = P.inverse();
                const V3 a = normalize(Pi.dir(rest)), d = normalize(Pi.dir(to - O));
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

        // a step down a stair isn't a fall
        m_airTime       = m.onGround || m.flying ? 0.f : m_airTime + m.dt;
        const bool air  = m_airTime > 0.15f || (!m.onGround && !m.flying && m.vy > 1.f);
        // a bit of hysteresis between the gaits
        auto       gait = [&] {
            const float up = m_state == ST_IDLE || m_state == ST_AIR ? 0.4f : 0.25f;
            if (m.flying || m.speed < up)
                return ST_IDLE;
            if (m.speed < (m_state == ST_RUN ? 2.6f : 3.f))
                return ST_WALK;
            return ST_RUN;
        };
        const eState st = air ? ST_AIR : gait();

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
            case SRC_PROC: procedural(m_state, m, m_target); break;
            case SRC_REST: break;
        }

        if (m_fade < 1.f) {
            m_fade        = std::min(1.f, m_fade + m.dt / 0.2f);
            const float w = smoothstep01(m_fade);
            for (size_t i = 0; i < m_pose.size(); ++i)
                m_pose[i] = {lerp(m_from[i].t, m_target[i].t, w), slerp(m_from[i].r, m_target[i].r, w), lerp(m_from[i].s, m_target[i].s, w)};
        } else
            m_pose = m_target;
        if (m_emote >= 0)
            emotePose(m.dt, m_pose);
        const float emoteW = m_emote >= 0 ? smoothstep01(m_emoteW) : 0.f;
        hands(m.dt, m_pose);
        look(m, m_pose, 1.f - emoteW);
        face(m, m_pose);
        constrain(m_pose);
        globals(m_pose, m_global);

        // procedural poses keep the feet on the ground (and crouch by bending the knees), and so do emotes but
        // for jumps and falls; clips have the hips where they want them
        const float ground = m_restFootY - lowestFoot(m_global);
        float       lift   = m_source.kind == SRC_PROC && m_state != ST_AIR ? ground : 0.f;
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
    }

    void CAvatarAnimator::stopEmote() {
        if (m_emote >= 0)
            m_emoteOut = true;
    }

    // the emote's pose, faded in over the pose, and its faces; done, it fades out
    void CAvatarAnimator::emotePose(float dt, std::vector<STRS>& pose) {
        const SAvatarEmote& em  = *m_emotes[m_emote];
        const float         dur = em.anim.duration;
        m_emoteTime += dt * em.speed;
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
