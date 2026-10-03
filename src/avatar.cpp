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
        constexpr float MAX_CLIP = 900.f; // seconds
        constexpr int   DROP_FIXED = 1 << 30; // m_dropBy: dropped by no toggle

        // --- JSON reader (cgltf leaves the VRM extensions as text)

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

        // shortest rotation from a to b
        Quat arc(const V3& a, const V3& b) {
            const float d = dot(a, b);
            if (d < -0.9999f)
                return Quat::axisAngle(perpendicular(a), TAU * 0.5f);
            const V3 c = cross(a, b);
            return Quat{c.x, c.y, c.z, 1.f + d}.normalized();
        }

        // rotation part, ignoring scale and mirroring
        Quat rotationOf(const M4& m) {
            V3 x{m.m[0], m.m[1], m.m[2]};
            const V3 y{m.m[4], m.m[5], m.m[6]}, z{m.m[8], m.m[9], m.m[10]};
            if (m.det3() < 0)
                x = -x;
            return Quat::fromBasis(normalize(x), normalize(y), normalize(z));
        }

        // p turned into J's VRMC_springBone_limit about O (L: limitFrame, world space)
        V3 springLimit(const SSpringJoint& J, const Quat& L, const V3& O, const V3& p) {
            V3 d = L.conj().rotate(normalize(p - O));
            switch (J.limit) {
                case LIMIT_NONE: return p;
                case LIMIT_CONE: {
                    // within limitA of +y
                    if (const float c = std::cos(J.limitA); d.y < c) {
                        const float side = d.x * d.x + d.z * d.z, s = std::sqrt(std::max(0.f, 1.f - c * c));
                        d                = side <= 1e-8f ? V3{0, c, s} : V3{d.x * s / std::sqrt(side), c, d.z * s / std::sqrt(side)}; // opposite y: toward +z
                    }
                    break;
                }
                case LIMIT_HINGE: {
                    // yz plane, within limitA of +y
                    const float l = std::sqrt(d.y * d.y + d.z * d.z);
                    d             = l <= 1e-4f ? V3{0, 1, 0} : V3{0, d.y / l, d.z / l};
                    if (const float c = std::cos(J.limitA); d.y < c)
                        d = {0, c, (d.z < 0 ? -1.f : 1.f) * std::sqrt(std::max(0.f, 1.f - c * c))};
                    break;
                }
                case LIMIT_SPHERICAL: {
                    // pitch round x, yaw toward x, each clamped
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

        // --- humanoid bones from names

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
            std::string key;      // no sides, numbers, rig prefixes
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
            int      bone; // K_CENTER: eHumanBone, else index in the limb
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

        // VRM or Unity bone name; VRM 1.0 thumbs start at the metacarpal
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

        // eFinger, -1 none, -2 palm (non-thumb metacarpal)
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

        // loose: "vrc.v_aa" and "V_AA" are both "vaa"
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

        // VRM 1.0 / 0.x preset name, -1 = none
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

        // per preset, shape key sets to try (space-separated, :weight)
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

        // unanimated nodes keep their pose
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

        // whether a finger turns 5 degrees or more
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

        // exact, then loose name; -1 = none
        int clipNamed(const std::vector<SAnimClip>& clips, std::string_view name) {
            for (int loose = 0; loose < 2 && !name.empty(); ++loose) {
                const std::string want = loose ? normName(name) : lower(std::string(name));
                for (size_t i = 0; i < clips.size(); ++i)
                    if ((loose ? normName(clips[i].name) : lower(clips[i].name)) == want)
                        return (int)i;
            }
            return -1;
        }

        // rest facing, model space: from legs or arms, else hips to head
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

        // --- emotes: humanoid clips as T pose turns in a body frame (+X left, +Y up, +Z forward)

        constexpr float PI = TAU * 0.5f;

        // humanoid rest pose
        struct SRig {
            const SAvatarModel*        md = nullptr;
            std::vector<M4>            restGlobal;
            std::vector<Quat>          restRot; // per node
            Quat                       facing;  // body frame -> model space
            std::array<Quat, HB_COUNT> tpose{}; // rest -> T pose per bone, model space
            V3                         hips;    // at rest, model space
            float                      height = 1; // hips above ankles, model units
        };

        // parent in the humanoid hierarchy
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

            // T pose: arms sideways, legs down
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
            // others take their limb parent's turn, else none
            for (int b = 0; b < HB_COUNT; ++b)
                if (!own[b])
                    r.tpose[b] = humanParent(b) >= 0 && b != HB_L_SHOULDER && b != HB_R_SHOULDER && humanParent(b) > HB_HEAD ? r.tpose[humanParent(b)] : Quat{};
            return r;
        }

        // per frame: bone turns from the T pose, hips offset in hips heights; faces' x = weight
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

        SNormClip canonical(const SAnimClip& clip, const SRig& rig, const std::atomic<bool>& cancel) {
            const auto& md = *rig.md;
            const auto& h  = md.human;
            SNormClip   nc;
            nc.name     = clip.name;
            nc.duration = clip.duration;
            // sample at the keys if few and linear, else 60 Hz
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

            // VRMA bones without curves stay free (upper-body clips leave legs walking)
            std::vector<bool> turned(md.nodes.size(), md.humanFrom != "VRMA");
            for (const auto& c : clip.channels)
                if (c.path == PATH_R)
                    turned[c.node] = true;
            for (int b = 0; b < HB_COUNT; ++b)
                nc.has[b] = h[b] >= 0 && b != HB_L_EYE && b != HB_R_EYE && turned[h[b]];
            const Quat         Rc = rig.facing.conj();
            std::vector<STRS>  pose(md.nodes.size());
            std::vector<M4>    g(md.nodes.size());
            std::array<Quat, HB_COUNT> undo; // inverse of rest -> T pose
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

        // body frame, hips heights: hips at (0, 1, 0), ankles at y = 0
        struct SBody {
            std::array<V3, HB_COUNT>   at{}; // at rest
            std::array<bool, HB_COUNT> has{};
            float                      upper[2]{}, fore[2]{}, thigh[2]{}, shin[2]{}; // 0 = left
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

        // turn taking t0 to t, and h0 (perpendicular to t0) nearest h
        Quat frameTo(const V3& t0, const V3& h0, const V3& t, const V3& h) {
            V3 hp = h - t * dot(h, t);
            if (length(hp) < 1e-5f)
                return arc(t0, t);
            hp = normalize(hp);
            return (Quat::fromBasis(t, hp, cross(t, hp)) * Quat::fromBasis(t0, h0, cross(t0, h0)).conj()).normalized();
        }

        // hinge axis after the shortest turn of a limb from t0 to u
        V3 hingeAt(const V3& t0, const V3& h0, const V3& u) {
            V3 a = arc(t0, u).rotate(h0);
            a    = a - u * dot(a, u);
            return length(a) > 1e-5f ? normalize(a) : perpendicular(u);
        }

        V3 towards(const V3& a, const V3& b, float w) {
            return slerp(Quat{}, arc(normalize(a), normalize(b)), w).rotate(normalize(a));
        }

        float ramp(float t, float a, float b) {
            return smoothstep01((t - a) / (b - a));
        }

        // chest frame; twist about u from the natural hinge; zero `along`: hand straight
        struct SArm {
            V3    u{0, -1, 0};
            float bend = 0, twist = 0;
            V3    along, palm;
        };

        class CPoser {
          public:
            std::array<Quat, HB_COUNT> turn; // turns from the T pose, body frame
            std::array<bool, HB_COUNT> set;
            V3                         move; // hips offset from rest

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

            void body(const Quat& q, const V3& moved = {}) {
                m_local[HB_HIPS] = q;
                move             = moved;
                torso();
            }

            // turn relative to the bone below
            void bend(int bone, const Quat& q) {
                m_local[bone] = q;
                torso();
            }

            // upper arm along u, forearm along f, chest frame
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

            // posed joint position so far, body frame
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

            // unset bones turn with their parent
            Quat turnAt(int b) const {
                while (!set[b] && humanParent(b) >= 0)
                    b = humanParent(b);
                return turn[b];
            }

            const SBody& measure() const {
                return m_b;
            }

            // target in arm lengths from mid-shoulders, chest frame; elbow toward `elbow`
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

            // blends direction, bend and twist: slerping would swing the forearm through the body
            void arm(int side, const SArm& a, const SArm& b, float w) {
                float twist = b.twist - a.twist;
                twist -= TAU * std::round(twist / TAU);
                const SArm m{towards(a.u, b.u, w), a.bend + (b.bend - a.bend) * w, a.twist + twist * w, {}, {}};
                const auto [ua, la] = armBones(side, m);
                const Quat ha = handOf(side, a, armBones(side, a).second), hb = handOf(side, b, armBones(side, b).second);
                apply(side, armFinish(side, ua, la, slerp(ha, hb, w)));
            }

            // thigh along `thigh` (hips' frame), knee bend, foot pitch
            void leg(int side, const V3& thigh, float knee, float pitch = 0) {
                const V3   t0{0, -1, 0}, h0{1, 0, 0};
                const V3   u = normalize(thigh), a = hingeAt(t0, h0, u);
                const Quat hips = turn[HB_HIPS], ul = frameTo(t0, h0, u, a), ll = Quat::axisAngle(a, knee) * ul;
                legTurns(side, {hips * ul, hips * ll, hips * ll * Quat::axisAngle(h0, pitch)});
            }

            // ankle at a point (body frame), knee forward, flat unless pitched
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

            // eases the target in near full reach (SOFT) so the knee doesn't snap
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
                // knee hinge normal to the bend plane, valid when straight
                V3 d = ankle - hip;
                d    = length(d) > 1e-6f ? normalize(d) : V3{0, -1, 0};
                V3 n = knee - d * dot(knee, d);
                n    = length(n) > 1e-6f ? normalize(n) : perpendicular(d);
                const V3   a = normalize(cross(n, d));
                const Quat t = frameTo(t0, h0, u, a), l = Quat::axisAngle(a, std::acos(std::clamp(dot(u, f), -1.f, 1.f))) * t;
                legTurns(side, {t, l, foot});
            }

            void toes(int side, const Quat& toes) {
                const int b = side ? HB_R_TOES : HB_L_TOES;
                turn[b]     = toes.normalized();
                set[b]      = true;
            }

            // rest ankle, on the ground
            V3 ankle(int side) const {
                const int ul = side ? HB_R_UPPER_LEG : HB_L_UPPER_LEG;
                return m_b.has[ul + 2] ? V3{m_b.at[ul + 2].x, 0, m_b.at[ul + 2].z} : V3{m_b.at[ul].x, 0, m_b.at[ul].z};
            }

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

            // chest frame; forearm before the hand's twist
            std::pair<Quat, Quat> armBones(int side, const SArm& a) const {
                const V3   t0{side ? -1.f : 1.f, 0, 0}, h0{0, side ? 1.f : -1.f, 0};
                const V3   u     = normalize(a.u);
                const V3   hinge = Quat::axisAngle(u, a.twist).rotate(hingeAt(t0, h0, u));
                const Quat ua    = frameTo(t0, h0, u, hinge);
                return {ua, Quat::axisAngle(hinge, a.bend) * ua};
            }

            // relative to the forearm; straight unless `along` is set
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

            // forearm takes half the hand's twist
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

        // --- built in emotes: look-alikes of VRChat's

        // seconds; to < 0 = held to the end
        struct SFaceKey {
            std::string_view name;
            float            weight = 1, from = 0, to = -1;
        };

        struct SBuiltin {
            std::string_view        name;
            float                   duration;
            bool                    loop, hold, grounded;
            std::array<int8_t, 2>   gesture; // left, right; -1 = the player's
            std::array<SFaceKey, 2> faces;   // unnamed = unused
            void (*pose)(CPoser&, float t);
        };

        SArm restArm(int side) {
            const float sx = side ? -1.f : 1.f;
            return {normalize(V3{sx * 0.12f, -1, 0.03f}), 0.25f, 0, {}, {}};
        }

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
            const float open = 0.5f + 0.5f * std::cos(TAU * 3 * (t - 0.35f)); // 3 claps a second
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
            // step touch: right out, left follows, left back, right follows
            constexpr float S = 0.22f;
            float           off[2]{};
            const int       moving = beat == 0 || beat == 3 ? 1 : 0;
            switch (beat) {
                case 0: off[1] = -S * f; break;
                case 1: off[1] = -S, off[0] = -S * f; break;
                case 2: off[1] = -S, off[0] = -S * (1 - f); break;
                default: off[1] = -S * (1 - f); break;
            }
            // bob on the beat; hips between the feet
            const float bob  = 0.5f + 0.5f * std::cos(TAU * q);
            const float snap = smoothstep01(std::min(1.f, (q - (float)beat) / 0.35f)); // the arm's moves are quick
            const bool  high = beat % 2 == 0;                                      // pointing up, else down across
            const float lean = high ? snap : 1 - snap;
            p.body(Quat::axisAngle(UP, -0.25f * lean + 0.1f) * Quat::axisAngle({0, 0, 1}, 0.06f * (lean - 0.5f)),
                   {(off[0] + off[1]) * 0.5f, -0.06f * bob, 0});
            for (int s = 0; s < 2; ++s)
                p.step(s, p.ankle(s) + V3{off[s], s == moving ? 0.06f * std::sin(PI * f) : 0.f, 0});
            // right arm up-out, then down across; left hand on hip
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
            // right foot scuffs: back, forward, home
            struct SFootKey {
                float t;
                V3    at; // from the rest ankle
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
                // knees give; lying down, soles on the ground
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

        // with the face channels the avatar has
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

        // by node name (non-humanoids): moves and scales only where rests match
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

        // from a file or folder; `clip` picks one by name
        std::vector<std::shared_ptr<SAvatarEmote>> emotesFromFile(const std::string& file, const SAvatarModel& target, const std::atomic<bool>& cancel,
                                                                  std::vector<std::string>& log, std::string& error, std::string_view clip = {});

        bool attackClips(const void* bytes, size_t size, const std::string& what, const SAvatarModel& target, const std::atomic<bool>& cancel,
                         std::array<SAvatarAttack, 2>& out, std::string& error);

        // built in punches (tools/blend2vrma.py)
        constexpr unsigned char ATTACK_VRMA[] = {
#embed "../assets/attack.vrma"
        };
        constexpr unsigned char ATTACK_FIRST_VRMA[] = {
#embed "../assets/attack-first-person.vrma"
        };

        bool gaitClip(const void* bytes, size_t size, const std::string& what, const std::atomic<bool>& cancel, SGaitClip& out, std::string& error);

        // built in walk and run (tools/blender/h3d_walk.py)
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
            std::vector<bool>         inScene;   // by our node
            std::vector<int>          depth;
            std::vector<bool>         skinJoint; // used by a skin
            std::vector<uint32_t>     skinBase;  // glTF skin -> first of its joints
            std::vector<M4>           restGlobal;
            std::map<std::tuple<int, int, int>, std::vector<uint32_t>> batches; // by material, part, variant map
            int                       part = 0;                                    // of the mesh being read
            bool                      vrm = false;
            size_t                    skippedDraco = 0, skippedOther = 0;
            SJson                     vrmJson;                  // the VRM extension
            int                       vrmVersion = 0;           // 0 none, 1 VRM 0.x, 2 VRM 1.0
            std::vector<std::vector<SMorphDelta>> targets;      // current mesh's morph targets
            SJson                     settings;                 // {} = none
            std::string               settingsName;
            bool                      emoteSource = false;      // read only for its clips
            std::string               emotesMade;               // for the log

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
                        // a NaN would spread through the skeleton
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
                for (size_t i = 0; i < data->nodes_count; ++i) // left: nodes in a parent cycle
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
                // a skin without joints would index out of range
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
                        q[big] += 255 - total; // sum exactly 255
                        for (int k = 0; k < 4; ++k) {
                            v.joints[k]  = (uint16_t)(skinBase[skin] + j[k]);
                            v.weights[k] = (uint8_t)std::clamp(q[k], 0, 255);
                        }
                    }
                    model.vertices.push_back(v);
                }
                // KHR_materials_variants map, shared when identical
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

            // extras.targetNames, else the primitives' (older UniVRM)
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

            // own part from extras' "hypr3d_part" (unity2hypr3d's Mesh Cutter splits)
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
                    // VRMA names bones the VRM 1.0 way
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

            // settings "humanoid" overrides the rig
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

            // unmapped fingers: by name under each hand, else by position
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
                    // next: the child named as this finger, else the only child
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

                    // by position: thumb nearest the wrist, then index the most forward
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

            // then cut fingers where their chain breaks
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
                // the limbs must be chains
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
                // lowest point, or settings "floor" (MA Floor Adjuster)
                const float floor = (float)jnum(settings.get("floor"), b.min.y);
                model.fix    = M4::trs({-p.x, -floor * s, -p.z}, turn, {s, s, s});
                model.scale  = s;
                model.height = height * s;
            }

            // heel, ball, toe contacts from each foot's skinned mesh, else guessed
            void footShapes() {
                const auto& h = model.human;
                if (!model.humanoid)
                    return;
                std::vector<int8_t> side(model.nodes.size(), -1); // -1 = none
                for (int s = 0; s < 2; ++s)
                    if (const int f = h[s ? HB_R_FOOT : HB_L_FOOT]; f >= 0)
                        side[f] = (int8_t)s;
                for (size_t i = 0; i < model.nodes.size(); ++i) // parents come first
                    if (const int p = model.nodes[i].parent; side[i] < 0 && p >= 0)
                        side[i] = side[p];
                std::vector<M4> skin(model.joints.size());
                for (size_t j = 0; j < skin.size(); ++j)
                    skin[j] = restGlobal[model.joints[j].node] * model.joints[j].inverseBind;
                std::array<std::vector<V3>, 2> pts;
                for (const auto& v : model.vertices) {
                    int most = 0; // heaviest joint
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
                    // default: foot 0.15 x height, ankle 1/4 from the heel
                    const float len = 0.15f * std::max(model.height, 0.3f), up = std::max(ankle.y, 0.02f);
                    fs.heel = {0, -up, 0.25f * len};
                    fs.ball = {0, -up, -0.5f * len};
                    fs.toe  = {0, -up, -0.75f * len};
                    if (pts[s].size() < 12)
                        continue;
                    float low = 1e30f;
                    for (const V3& p : pts[s])
                        low = std::min(low, p.y);
                    // sole: lowest 1-2 cm, back (+z) to front
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
                    // sole height at heel and ball (high heels stand on both)
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

            // SBodyClearance from the shown rest skin: torso below the chest, arm thickness
            void bodyClearance() {
                const auto& h = model.human;
                if (!model.humanoid || h[HB_HIPS] < 0)
                    return;
                // -1 head, 0 body, 1 + 3 * side + k arm (upper arm, forearm, hand)
                std::vector<int8_t> part(model.nodes.size(), 0);
                const int           head = h[HB_NECK] >= 0 ? h[HB_NECK] : h[HB_HEAD];
                for (const auto& nc : model.constraints)
                    if (nc.type != SNodeConstraint::ROLL && nc.node >= 0)
                        part[nc.node] = -1;
                for (size_t i = 0; i < model.nodes.size(); ++i) { // parents come first
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
                // a part skinned only to a hand is a held prop (a microphone), not the hand
                auto handOnly = [&](uint32_t v) {
                    const auto& vx = model.vertices[v];
                    for (int k = 0; k < 4; ++k)
                        if (vx.weights[k] >= 250) {
                            const int pt = part[model.joints[vx.joints[k]].node];
                            return pt == 3 || pt == 6; // hands
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
                    arm[s]      = finite(bone[s][0]) && finite(bone[s][1]) && finite(bone[s][2]) && finite(handDir[s]); // broken files
                }
                const V3 hips = model.fix.point(restPos(h[HB_HIPS]));
                if ((!arm[0] && !arm[1]) || !std::isfinite(hips.x) || !std::isfinite(hips.y) || !std::isfinite(hips.z))
                    return;
                const int   chest = h[HB_CHEST] >= 0 ? h[HB_CHEST] : h[HB_SPINE];
                const float top = chest >= 0 ? model.fix.point(restPos(chest)).y : hips.y + 0.15f * model.height, far = 0.25f * model.height;
                std::vector<V3>       body;
                std::array<std::vector<V3>, 6> limb; // per arm and bone
                for (size_t v = 0; v < model.vertices.size(); ++v) {
                    const auto& vx   = model.vertices[v];
                    int         most = 0; // heaviest joint
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
                        continue; // broken file
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

            // first person: batches minus the head's triangles (fpFirst), and the eyes
            void firstPerson() {
                const auto& h = model.human;
                if (!model.humanoid || h[HB_HEAD] < 0)
                    return;
                for (int b : {HB_L_UPPER_ARM, HB_L_LOWER_ARM, HB_L_HAND, HB_R_UPPER_ARM, HB_R_LOWER_ARM, HB_R_HAND})
                    if (h[b] < 0)
                        return;
                const int            headN = h[HB_HEAD];
                std::vector<uint8_t> headNode(model.nodes.size(), 0);
                for (size_t i = 0; i < model.nodes.size(); ++i) // parents come first
                    headNode[i] = (int)i == headN || (model.nodes[i].parent >= 0 && headNode[model.nodes[i].parent]);
                std::vector<uint16_t> headW(model.vertices.size(), 0); // head's weight, of 255
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
                        continue; // left drawn whole
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

                // eyes between the eye bones, if in the face
                std::vector<M4> skin(model.joints.size());
                for (size_t j = 0; j < skin.size(); ++j)
                    skin[j] = restGlobal[model.joints[j].node] * model.joints[j].inverseBind;
                const V3 headAt = model.fix.point(restPos(headN));
                float    top    = headAt.y;
                std::vector<V3> face; // head skin near the joint, avatar space
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
                    // guess: 45% up the head, just behind the face's front
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
                        // sorted, within MAX_CLIP (baked at 60 Hz)
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
                    if (!emoteSource) // emotes keep root motion
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
                    model.clipFor[CLIP_WALK] = unmatched[0]; // lone unnamed clip: likely a walk
            }

            // removes a clip's forward drift: the player moves the avatar
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
                const V3 back     = pg.inverse().dir(flat); // drift, in the channel's space
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

            // morphs of a VRM 0.x mesh / VRM 1.0 node, by target index
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

            // VRM 0.x: weights 0..100; Unity material values (sRGB, V flipped)
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

            // VRM 1.0: weights 0..1, linear colors, glTF UVs
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

            // name is key after stripping face-part prefixes ("eye_smile")
            static bool bareIs(std::string_view name, std::string_view key) {
                static constexpr std::string_view PARTS[] = {"expression", "blendshape", "eyebrows", "eyebrow", "eyeblow", "mouth", "brows", "brow", "face",
                                                             "eyes",       "lips",       "fcl",      "all",     "mth",     "brw",   "eye",   "lip",  "bs"};
                for (const std::string_view part : PARTS)
                    if (name.size() > part.size() && name.starts_with(part) && (name.substr(part.size()) == key || bareIs(name.substr(part.size()), key)))
                        return true;
                return false;
            }

            // first alternative with a matching key; pass 2 strips face-part prefixes
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

            // AVATAR.hypr3d.json beside the model (see the README)
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

            // settings names the model lacks
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

            // settings expressions; those that set "blink"
            std::vector<int> mine;
            std::set<int>    ownBlink;

            // settings "expressions" (README); existing ones keep what isn't given
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

            // names -> KHR_materials_variants indices
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

            // "transforms": {"node": {"t", "r", "s"}}, each optional
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

            // settings "hidden", "fixed", "toggles", "sliders" (README)
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
                        // at most one per group starts on
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
                        // "axes": 2: [x, y] values, an n x n "grid" of keys
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
                // custom ones named as a preset ("Surprised": not a VRM 0.x preset)
                for (int i = 0; i < (int)defined; ++i) {
                    auto& e = model.expressions[i];
                    if (e.preset >= 0)
                        continue;
                    if (const int p = presetOf(e.name); p >= 0 && P[p] < 0) {
                        P[p]     = i;
                        e.preset = p;
                    }
                }

                // missing or empty presets from shape key names
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
                // unless VRM 1.0 or the settings file set it, emotions blend out blinking
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

                // consonants (PP, FF, SS, CH): settings "visemes", else "vrc.v_pp"-style shape keys
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

                // gesture faces, as on most VRChat avatars
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
                // or settings "gestures" (README)
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

            // JSON glTF node index -> ours, -1 = none
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

            // exact, then loose name; -1 = none
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
            std::vector<bool>             human;   // humanoid bone or above one: never swings

            int addCollider(int node, const V3& offset, const V3& tail, float meters, eColliderKind kind = COLLIDER_OUTSIDE) {
                model.springColliders.push_back({node, offset, tail, std::max(meters, 0.f), kind});
                return (int)model.springColliders.size() - 1;
            }

            // j.limitFrame: relative to a frame with y along the bone
            void addJoint(int node, int spring, SSpringJoint j, const V3& tail, float meters) {
                if (length(tail) * metersAt(node) < 1e-4f)
                    return; // zero length: left to the animation
                j.node   = node;
                j.spring = spring;
                j.tail   = tail;
                j.length = length(tail) * metersAt(node);
                j.radius = std::max(meters, 0.f);
                if (j.limit != LIMIT_NONE) {
                    // shortest turn from y to the bone; opposite: half a turn round x
                    const V3 d   = normalize(tail);
                    const Quat y = 1.f + d.y < 1e-6f ? Quat{1, 0, 0, 0} : Quat{d.z, 0, -d.x, 1.f + d.y}.normalized();
                    j.limitFrame = (y * j.limitFrame).normalized();
                }
                model.springJoints.push_back(j);
                claimed[node] = true;
            }

            // VRMC_springBone_limit's cone, hinge or spherical limit, radians
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

            // root and all under it swing (VRM 0.x bone groups); `leaf`: leaf tail length (0: the bone before's)
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

            // VRM 0.x secondaryAnimation, in Unity's axes (z flipped)
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

            // VRMC_springBone, with VRMC_springBone_extended_collider shapes
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

            // body colliders for springs without their own
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
                    auto across = [&](int part, int bone, const V3& off, float half, float r) {
                        if (const int n = h[bone]; n >= 0)
                            bodyParts[part].push_back(addCollider(n, inNode(n, restPos(n) + (off - RT * half) * u), inNode(n, restPos(n) + (off + RT * half) * u), r * m));
                    };
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

            // settings "colliders" and "springs" (README); springs naming none use the file's, else the body's
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
                            // older hypr3d reads a disc as a sphere of "radius"
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
                    std::vector<float> radii; // PhysBone radius curve, per depth
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

            // no springs given: hair, skirts, tails... by bone name
            void heuristicSprings() {
                struct SKind {
                    const char*                   name;
                    std::vector<std::string_view> words; // word prefixes; non-ASCII: substrings
                    float                         stiffness, drag, gravity, radius; // for a 1.6 m avatar
                    int                           body;
                };
                static const SKind KINDS[] = {
                    // gravity keeps a ponytail from streaming out level at a run
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
                // unweighted roots and branching bones stay; the chains under them swing
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

            // settings "immobile": how little world motion swings centerless springs
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
                // drop springs without joints
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
                // limited bones skip body colliders their tail starts in: the limit keeps them out
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
                // carrier: a root's parent, unless a spring moves it or its ancestors
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

            // VRMC_node_constraint, each ordered after those it depends on
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
                // depends on its source and on the source's and node's ancestors
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

            // built in, own non-locomotion clips, settings "emotes", the request's; later names win
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

            // request's attack, else settings "attack", else built in
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

            // settings "walk", else the built in walk and run
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

            // settings "hands": gesture finger poses from a VRMA
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
            std::vector<std::string> quiet; // discarded
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

            // VRMA expressions: nodes whose translation x is the weight
            std::vector<std::pair<int, std::string>> faceNodes;
            if (const SJson* ex = sb.vrmJson.get("expressions"))
                for (const char* kind : {"preset", "custom"})
                    if (const SJson* group = ex->get(kind); group && group->type == SJson::J_OBJ)
                        for (const auto& [name, v] : group->obj)
                            if (const double n = jnum(v.get("node"), -1); n >= 0 && n < (double)sb.nodeIndex.size() && sb.nodeIndex[(size_t)n] >= 0)
                                faceNodes.push_back({sb.nodeIndex[(size_t)n], name});

            // lookAt node
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

        // --- attacks: a right-arm punch, mirrored for the left

        // per node (CAvatarAnimator::m_attackPart): trunk turns added on, arms as animated
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

        // mirror across the body's middle (+X is left)
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

        // swings from an in-memory VRMA right-arm punch, timed by its extras' "markers"
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
            // wrists from the eyes per clip time (arm lengths, body frame); mirrored for the left
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
            // markers by clip name
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

        // one stride from an in-memory VRMA at GAIT_STEPS steps, with mean turns
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

        // at stride phase 0..1
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
        b.humanoid(); // before materials: detects VRM
        if (b.vrm)
            for (auto& m : b.mats.materials)
                m.unlit = false; // MToon falls back to unlit; lit fits better
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
        b.bodyClearance(); // after outfit() and constraints()
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
                // widest body radius down to the thighs (skirt hem, else hips)
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
        // partial success: failures only logged
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

    // own or an ancestor's name: "Jacket" is every mesh under it
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

    // "name" on any mesh, or "mesh node/name" if no shape key is called that
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

    // --- walking: procedural for humanoids without clips (dynamic similarity, Alexander 1976)

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

        // -0.5..0.5, the short way round
        float phaseDiff(float d) {
            return d - std::round(d);
        }

        float approach(float from, float to, float step) {
            return from < to ? std::min(from + step, to) : std::max(from - step, to);
        }

        // minimum-jerk ease 0..1
        float minJerk(float s) {
            s = std::clamp(s, 0.f, 1.f);
            return s * s * s * (10.f + s * (-15.f + 6.f * s));
        }

        // Catmull-Rom through keys (t 0..1), flat ends
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

        // radians, toes down > 0: heel strike, flat, heel rise
        constexpr SCurveKey WALK_STANCE_PITCH[] = {{0, -0.26f}, {0.12f, -0.03f}, {0.2f, 0}, {0.5f, 0}, {0.75f, 0.24f}, {1, 0.9f}};
        constexpr SCurveKey RUN_STANCE_PITCH[]  = {{0, -0.13f}, {0.1f, 0}, {0.4f, 0}, {0.7f, 0.32f}, {1, 0.95f}};
        // toes down at lift-off, up to clear the ground, heel first
        constexpr SCurveKey WALK_SWING_PITCH[] = {{0, 0.9f}, {0.2f, 0.4f}, {0.42f, 0.03f}, {0.62f, -0.1f}, {0.85f, -0.16f}, {1, -0.26f}};
        constexpr SCurveKey RUN_SWING_PITCH[]  = {{0, 0.95f}, {0.25f, 0.75f}, {0.5f, 0.38f}, {0.75f, 0.02f}, {0.9f, -0.08f}, {1, -0.13f}};
        // leg lengths
        constexpr SCurveKey WALK_LIFT[]  = {{0, 0}, {0.15f, 0.07f}, {0.3f, 0.065f}, {0.5f, 0.035f}, {0.75f, 0.025f}, {0.9f, 0.015f}, {1, 0}};
        constexpr SCurveKey RUN_LIFT[]   = {{0, 0}, {0.2f, 0.2f}, {0.38f, 0.27f}, {0.55f, 0.22f}, {0.75f, 0.11f}, {0.9f, 0.04f}, {1, 0}};
        constexpr SCurveKey STAND_LIFT[] = {{0, 0}, {0.4f, 0.05f}, {0.75f, 0.035f}, {1, 0}};
        // radians, per gait data (Perry and Burnfield)
        constexpr SCurveKey WALK_STANCE_KNEE[] = {{0.4f, 0.1f}, {0.65f, 0.14f}, {0.8f, 0.24f}, {0.9f, 0.42f}, {1, 0.66f}};
        constexpr SCurveKey WALK_SWING_KNEE[]  = {{0, 0.66f}, {0.12f, 0.87f}, {0.25f, 1.f}, {0.33f, 1.05f}, {0.45f, 0.93f}, {0.6f, 0.68f}, {0.75f, 0.35f}, {0.88f, 0.14f}, {1, 0.07f}};
        // minimum heel rise late in stance
        constexpr SCurveKey WALK_HEEL_LEAST[] = {{0.5f, 0}, {0.8f, 0.1f}, {1, 0.5f}};

        // pelvis turn off a planted foot (radians), then its ball pivot rate (radians/s)
        constexpr float HIP_TURN   = 0.5f;
        constexpr float PIVOT_RATE = 5.f;
        // m: no pivot where the heel would swing over higher ground (a stair behind)
        constexpr float PIVOT_CLEAR = 0.03f;
        // max pelvis turn speed (rad/s) and its acceleration (rad/s²)
        constexpr float HIP_RATE = 6.f, HIP_ACCEL = 50.f;
        // turning round people slow, turn, then go: velocity scaled down past GO_ON off the pelvis's facing
        constexpr float GO_ON = 0.35f, GO_OFF = 1.8f, GO_LEAST = 0.25f, GO_GIVE = 0.5f, RUN_GO = 0.4f;
        // turning far: at least TURN_CADENCE strides/s; no slowing while the pelvis turns over CADENCE_TURN radians/s
        constexpr float TURN_CADENCE = 1.1f, CADENCE_TURN = 2.f;
        // planted foot out from its hip past OUT_EASY m hurries the steps, up to OUT_MOST x
        constexpr float OUT_EASY = 0.06f, OUT_HURRY = 6.f, OUT_MOST = 1.3f;
        // max swing foot offset out from / in under its hip, meters
        constexpr float SWING_OUT = 0.05f, SWING_IN = 0.03f;
        // how fast the body's turning speeds up or slows (rad/s²; a quick turn reaches 7 rad/s in about 1/9 s)
        constexpr float TURN_ACCEL = 60.f;
        // foot plan turn rate (radians/s); max landing spot speed over the body's (m/s)
        constexpr float TURN_PLAN = 10.f, RETARGET = 2.5f;
        // legs' body following the player: rate (1/s), accels (m/s²), catch-up, lags (m), wall brake, overshoot (m)
        constexpr float BODY_K = 8.f, BODY_WALK_ACCEL = 7.f, BODY_RUN_ACCEL = 10.f, BODY_CATCH_UP = 1.f, BODY_CATCH_UP_MOST = 0.03f, BODY_LAG_SOFT = 0.3f,
                        BODY_LAG = 0.5f, BODY_WALL = 40.f, BODY_REST = 0.2f;
        // back and forth: keeps facing its travel above WAY_GOING m/s
        constexpr float WAY_GOING = 0.3f;
        // head leads a turn: fraction, max radians
        constexpr float HEAD_LEAD = 0.6f, HEAD_LEAD_MOST = 0.7f;
        // max trunk twist from the pelvis (rad) and how fast it gets there (1/s)
        constexpr float TRUNK_TWIST = 0.6f, TRUNK_W = 14.f;
        // 1/s, critically damped
        constexpr float ARM_PUSH_W = 40.f;
        // gait clip speeds (m/s) for GAIT_LEG m legs; swing scale limits
        constexpr float GAIT_WALK = 1.5f, GAIT_RUN = 4.5f, GAIT_LEG = 0.8f, GAIT_LEAST = 0.35f, GAIT_MOST = 1.2f;
        // clip arm clearance per stride step; 1 in GAIT_NEED_EVERY refreshed a frame
        constexpr int GAIT_NEED_STEPS = 24, GAIT_NEED_EVERY = 8;
        // radians/s, radians/s²: limits keep the foot from flicking
        constexpr float FOOT_ROLL = 16.f, FOOT_ROLL_ACCEL = 400.f;
        // max change in a swinging foot's vertical speed for the knee fold (m/s²; a steady walk needs at most 90)
        constexpr float RAISE_ACCEL = 120.f;
        // how fast the steps may slow (strides/s per s) as the body slows; the feet keep stepping
        constexpr float CADENCE_DROP = 0.4f;
        // max shift (m) of a foot across a stair edge onto one tread
        constexpr float STAIR_SHIFT = 0.16f, STAIR_ROOM = 0.03f; // clearance, meters
        // how hard (m/s²) and how fast (m/s) the pelvis lowers going down, so the legs reach
        constexpr float LOWER_ACCEL = 60.f, LOWER_SPEED = 3.f;
        // radians; ground off a line by more (m) isn't a slope
        constexpr float SLOPE_MOST = 0.45f, SLOPE_EVEN = 0.012f;

        // jump poses by progress `ju`: 0 take-off at JUMP_UP m/s, 1/2 top, 1 landing; radians
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
        constexpr SCurveKey JUMP_ARM[]        = {{0, 0.8f}, {0.5f, 0.4f}, {1, 0.3f}};   // from standing
        constexpr SCurveKey JUMP_ARM_OUT[]    = {{0, 0.3f}, {0.5f, 0.55f}, {1, 0.6f}};
        constexpr SCurveKey JUMP_ARM_AHEAD[]  = {{0, 0.6f}, {1, 0.35f}};                 // running: opposite the lead leg
        constexpr SCurveKey JUMP_ARM_BACK[]   = {{0, -0.55f}, {1, -0.2f}};              // the lead leg's side
        constexpr SCurveKey JUMP_ARM_RUN_OUT[]  = {{0, 0.2f}, {1, 0.45f}};
        constexpr SCurveKey JUMP_ARM_RUN_BEND[] = {{0, 1.25f}, {1, 0.7f}};
        constexpr SCurveKey JUMP_LEAN[]       = {{0, 0.05f}, {0.5f, -0.02f}, {1, 0.08f}}; // trunk ahead of the pelvis
        constexpr SCurveKey JUMP_RUN_LEAN[]   = {{0, 0.18f}, {0.5f, 0.08f}, {1, 0.12f}};
        // flying: lies flatter the faster, leans with acceleration, banks into turns; FLY_TURN in radians/s
        constexpr float FLY_LIE = 1.3f, FLY_LIE_SPEED = 5.5f, FLY_PUSH = 0.03f, FLY_PUSH_MOST = 0.35f, FLY_BRAKE_MOST = 0.5f, FLY_BANK = 0.7f;
        constexpr float FLY_TURN = 4.f, FLY_TURN_ACCEL = 25.f;
        // hover bob, meters
        constexpr float FLY_BOB = 0.022f;
        // radians/s; damping 1 = critical
        constexpr float AIR_LEG_W = 11.f, AIR_LEG_Z = 0.7f, FLY_LEG_W = 8.f, FLY_LEG_Z = 0.55f, AIR_ARM_W = 9.f, AIR_ARM_Z = 0.7f;
        constexpr float FLY_BODY_W = 6.5f, FLY_BODY_Z = 0.7f;
        // settle rates (1/s) into a new pose at take-off, landing and flight start
        constexpr float SETTLE_AIR = 18.f, SETTLE_LAND = 20.f, SETTLE_FLY = 10.f;
        // landing: the hips sink on at this fraction of the impact speed
        constexpr float LAND_GIVE = 0.5f;

        // player's velocity t seconds on, as the plugin moves it toward `wish`
        V3 keysAhead(const V3& cv, const V3& wish, const SAvatarMotion& m, float t) {
            const V3    c{cv.x, 0, cv.z}, d = V3{wish.x, 0, wish.z} - c;
            const float most = (dot(wish, c) < 0 ? m.turnBack : length(wish) < length(c) ? m.decel : m.accel) * t, dl = length(d);
            return c + (dl > most ? d * (most / dl) : d);
        }

        // one dt step of the legs' body chasing the player's: see BODY_K, GO_ON
        void follow(V3& p, V3& v, V3& a, const V3& cp, const V3& cv, const V3& lead, const V3& wish, bool wall, float most, float dt, float go = 1.f) {
            const V3 off{cp.x - p.x, 0, cp.z - p.z};
            // past BODY_LAG_SOFT behind, up to twice as hard by BODY_LAG
            const float soft = BODY_LAG_SOFT + 0.06f * length(V3{cv.x, 0, cv.z}), hard = soft + BODY_LAG - BODY_LAG_SOFT;
            const float far  = smoothstep01((length(off) - soft) / (hard - soft));
            most *= 1.f + far;
            // stopped just past the player, keys released: stays put
            const bool still = length(wish) < 0.05f && length(V3{cv.x, 0, cv.z}) < 0.05f && length(off) < 2.f * BODY_REST;
            V3         back  = still ? V3{} : off * BODY_CATCH_UP;
            if (const float bl = length(back), cap = 0.03f + (BODY_CATCH_UP_MOST + 0.15f * far) * length(cv); bl > cap)
                back = back * (cap / bl);
            // critically damped push toward the player's velocity a moment ahead; `go` holds back
            V3 want = (V3{lead.x, 0, lead.z} + back) * lerpf(go, 1.f, smoothstep01(far / GO_GIVE));
            V3 push = (want - v) * BODY_K;
            push.y  = 0;
            if (const float pl = length(push); pl > most)
                push = push * (most / pl);
            // brake within the gap at walls or pushing on; released, stop within BODY_REST
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
            // clamp the lag and the overshoot
            if (const V3 o{cp.x - p.x, 0, cp.z - p.z}; length(o) > (dot(o, v) >= 0 ? hard : hard + 2.f * BODY_REST)) {
                const V3 u = o / length(o);
                p += o * (1.f - hard / length(o));
                if (const float lag = dot(cv - v, u); lag > 0)
                    v += u * lag;
            }
        }

        // critically damped spring, w in radians/s
        void springTo(float& x, float& v, float to, float w, float dt) {
            const float d = x - to, e = std::exp(-w * dt), k = v + w * d;
            x = to + (d + k * dt) * e;
            v = (v - w * k * dt) * e;
        }

        struct SSpring {
            float x = 0, v = 0;
            // damping ratio z (1 = critical), substepped for stability
            void to(float to, float w, float z, float dt) {
                const int   n = std::max(1, (int)std::ceil(dt * 240.f));
                const float h = dt / n;
                for (int i = 0; i < n; ++i) {
                    v += (-(x - to) * w * w - 2.f * z * w * v) * h;
                    x += v * h;
                }
            }
        };

        // rotation vector (axis * angle, short way round) and back
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

        // plugin yaw: 0 faces -Z, right positive
        Quat yawTurn(float yaw) {
            return Quat::axisAngle(UP, -yaw);
        }
        V3 forwardOf(float yaw) {
            return {std::sin(yaw), 0, -std::cos(yaw)};
        }
        V3 rightOf(float yaw) {
            return {std::cos(yaw), 0, std::sin(yaw)};
        }

        // toes down > 0, about the left axis
        Quat footPitch(float pitch) {
            return Quat::axisAngle({1, 0, 0}, -pitch);
        }

        // ankle for a foot at `at` pitched about its heel (< 0) or ball / toe tip (> 0)
        V3 ankleOver(const V3& at, float yaw, float pitch, const SFootShape& fs, float ankleH, bool toes) {
            V3 pivot = pitch < 0 ? fs.heel : toes ? fs.ball : fs.toe;
            pivot.x  = 0;
            return at + yawTurn(yaw).rotate(V3{0, ankleH, 0} + pivot - footPitch(pitch).rotate(pivot));
        }

        // fraction of a stride each foot is down
        float dutyFactor(float v, float run) {
            return lerpf(std::clamp(0.64f - 0.03f * v, 0.56f, 0.66f), std::clamp(0.5f - 0.035f * v, 0.27f, 0.42f), run);
        }

        struct SGaitFoot {
            bool  swing = false; // in the air
            bool  timed = false; // own step, not the gait's
            V3    at;            // ground point under the ankle
            float yaw   = 0;
            V3    from, to;      // step ends, on the ground
            float fromYaw = 0, toYaw = 0;
            float fromOut = 0, toOut = 0; // out from the pelvis, meters
            float fromPitch = 0;
            float s         = 0; // step progress 0..1
            float start     = 0; // gait phase at lift-off
            float rate      = 0; // own step: s per second
            float phase     = 0; // gait phase last frame
            float pitch     = 0; // as drawn
            bool  locked    = false; // landing spot fixed
            float spin      = 0;     // ball pivot speed / PIVOT_RATE
            float strain    = 0;     // hip-ankle distance / reach
            float heel      = 0;     // drawn pitch last frame
            float roll = 0, rollFrom = 0; // pitch rate, last frame's
            float over      = -1;    // past the leg's reach, m
            float tight     = 0;     // pelvis drop needed, of max
            float liftKnee  = 0;
            float landed    = -1;    // gait time it landed
            float fold = 0, foldV = 0; // swing knee fold 0..1
            float raise = 0, raiseV = 0, raiseTo = -1; // fold raise m, speed, target (-1 new)
            float gaitW     = 1;     // gait step 1 / own step 0, eased
            bool  wait      = false; // waits for its gait lift
            float toY = 0, toYV = 0; // eased landing height
            bool  toYSet = false;
            // ground pitch under the foot (toes down > 0); swing: lift-off's to landing's
            float gp = 0, fromGp = 0, toGp = 0;
            float knee      = 0;     // hip to ankle, m, as drawn
            V3    ankle;             // world, as drawn
        };

        // per-model walking state
        struct SGait {
            SRig              rig;
            SBody             body;
            std::vector<int>  own; // humanoid bone per node, -1 = none
            std::vector<Quat> scratch;
            M4                hipsToLocal = M4::identity(); // model space -> the hips' parent's
            M4                fixInv      = M4::identity(); // avatar space -> model space
            Quat              toFrame;                      // avatar-space turns -> body frame
            std::array<Quat, 2> footUntilt;                 // undoes leg straightening's foot tilt
            float             unit = 1;                     // meters per body frame unit
            float             leg  = 0.8f;                  // hip joint height over the soles, m
            float             hipH = 0.9f;                  // pelvis bone height, m
            std::array<float, 2> thigh{0.4f, 0.4f}, shin{0.4f, 0.4f}, ankleH{0.07f, 0.07f}; // meters
            std::array<float, 2> ahead{};                   // rest ankle forward offset, m
            std::array<V3, 2>    hipAt;                     // hip joints from the pelvis, avatar space
            float             stance = 0.08f;               // half the rest ankle spacing, m
            float             armOut = 0.12f;               // arm abduction clearing the hips
            std::array<SFootShape, 2> foot;
            std::array<bool, 2>       toes{};       // has a toes bone
            std::array<float, 2>      toesUp{1, 1}; // landing toe lift vs bare foot
            // push-off ankle travel ahead and up, m (walk, run)
            std::array<float, 2> pushAhead{}, pushUp{};

            bool      live = false, moving = false, wasAir = false;
            float     phase = 0; // left lands at 0, right at 0.5
            // body swing phase offset: catches up after phase jumps, critically damped
            float     phaseOff = 0, phaseOffV = 0;
            float     run = 0, crouch = 0, moveW = 0, moveWV = 0;
            float     runArms = 0, runArmsV = 0; // eased `run` for arms and trunk
            float     stride = 0, cadence = 0, duty = 0.6f, swingTime = 0.4f;
            SGaitFoot feet[2];
            float     groundY = 0, groundV = 0; // ground under the feet, sprung
            float     groundRate = 0, lastSupport = NAN; // climb rate (last 1/6 s), last support
            float     slope = 0; // ground slope along travel
            float     dip = 0, dipV = 0;        // landing knee give
            float     lowered = 0, loweredV = 0; // pelvis lowering for reach, speed
            float     hurry   = 1;              // step speed-up (sharp turns)
            float     hurrySmooth = 1;
            float     rollW = 0;                // standing: pelvis onto a leg (+ right)
            float     airVy = 0;                // fastest fall this airtime
            float     pause = 0;                // standing: until the next step
            float     time  = 0;
            V3        lastP, lastVel, accel; // lastP: the player's body
            V3        bodyP, bodyV, bodyA;   // legs' body, following the player
            V3        lastVelC;              // player's velocity last frame
            float     wall = 0;              // wall stop countdown
            float     lastYaw = 0, yawRate = 0;
            float     velTurn = 0; // travel turn rate, radians/s
            float     wishTurn = 0; // wish turn rate (camera), radians/s
            V3        lastWish;
            float     turnLeft = 0; // turnBody's remaining turn, eased
            float     forward = 1; // 1 ahead .. -1 backward
            float     hipLag  = 0, hipLagV = 0; // pelvis yaw lag, turn rate
            float     twist = 0, twistV = 0;   // trunk twist from the pelvis
            std::array<float, 2> armClear{}; // skirt clearance per arm, radians
            std::array<float, 2> armPush{}, armPushV{}; // current swing's, eased
            std::array<float, 2> clipClear{}, clipClearV{}, clipPush{}, clipPushV{}; // same for the gait clips' arms
            std::array<std::array<float, GAIT_NEED_STEPS>, 2> clipNeed{}; // per stride step
            unsigned             clipTick = 0;
            V3        pelvis; // world, as drawn
            // airborne pose springs (JUMP_*, FLY_*)
            struct SAirLeg {
                SSpring hip, knee, out, toes; // thigh ahead, knee bend, leg out, toes down
            };
            struct SAirArm {
                SSpring ahead, out, bend; // as armAt: ahead, out, bend
            };
            std::array<SAirLeg, 2> airLeg{};
            std::array<SAirArm, 2> airArm{};
            SSpring   airPitch, airRoll, airYaw, airLean; // pelvis pitch, roll, yaw; trunk lean
            SSpring   flyE;                   // flying, eased (0..1)
            int       airLead = -1;           // leading leg at takeoff, -1 none
            float     airRun  = 0;            // takeoff speed: 0 standing, 1 running
            bool      grounded = true;        // last frame
            std::array<V3, 2> armWas{};       // (ahead, out, bend) last frame
            float     leanWas = 0;            // grounded lean last frame
        };

        SGait& gaitOf(const std::shared_ptr<void>& p) {
            return *static_cast<SGait*>(p.get());
        }

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
                g.footUntilt[s] = (g.rig.facing.conj() * g.rig.tpose[ul + 2].conj() * g.rig.facing).normalized();
                // heel strike: ankle swing-back limited to a bare foot's 1.6 cm (heels land flatter)
                const float land = -through(WALK_STANCE_PITCH, 0);
                const float back = g.ankleH[s] * std::sin(land) - std::max(0.f, g.foot[s].heel.z) * (1.f - std::cos(land));
                g.toesUp[s]      = std::clamp(0.016f / std::max(back, 1e-3f), 0.25f, 1.f);
            }
            g.stance = std::max(g.stance, 0.02f);
            g.hipH   = hips.y - sole;
            g.leg    = std::max(0.2f, 0.5f * (at(HB_L_UPPER_LEG).y + at(HB_R_UPPER_LEG).y) - sole);
            // hands clear the hip joints by 9% of the leg (skirt, coat)
            const float shoulders = 0.5f * std::abs(at(HB_L_UPPER_ARM).x - at(HB_R_UPPER_ARM).x);
            const float hipsHalf  = 0.5f * std::abs(g.hipAt[0].x - g.hipAt[1].x);
            const float arm       = 0.5f * (g.body.upper[0] + g.body.fore[0] + g.body.upper[1] + g.body.fore[1]) * g.unit;
            g.armOut = std::clamp(std::asin(std::clamp((hipsHalf + 0.09f * g.leg - shoulders) / std::max(arm, 0.1f), 0.f, 0.6f)), 0.1f, 0.45f);
            return gp;
        }

        // applies a CPoser-frame pose: turns the `set` bones, moves the hips
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
        // turning right round: toward the rear foot's side, so the feet don't cross
        if (std::abs(err) < 2.3f)
            m_turnSide = 0;
        else {
            if (!m_turnSide && m_gait && m_gaitUsed)
                if (const SGait& g = gaitOf(m_gait); g.moving && !g.wasAir) {
                    const V3    F = forwardOf(yaw);
                    auto        along = [&](const SGaitFoot& ft) { return dot(ft.swing ? ft.to : ft.at, F); };
                    const float d     = along(g.feet[0]) - along(g.feet[1]); // > 0: left foot ahead
                    if (std::abs(d) > 0.05f)
                        m_turnSide = d > 0 ? 1 : -1;
                }
            // from standing or feet level: the current turn direction, else the short way; until round
            if (!m_turnSide)
                m_turnSide = std::abs(m_turnRate) > 0.5f ? (m_turnRate > 0 ? 1 : -1) : err > 0 ? 1 : -1;
            if (m_turnSide && (err > 0) != (m_turnSide > 0))
                err += m_turnSide > 0 ? TAU : -TAU;
        }
        // ~1/9 s to close the gap, capped by `most` and braking distance
        if (std::abs(wrapPi(yaw - m_turnYaw)) > 1e-3f) // yaw set elsewhere: restart
            m_turnRate = 0;
        const float accel = m_flying ? FLY_TURN_ACCEL : TURN_ACCEL, most = m_flying ? FLY_TURN : speed > 3.f ? 5.f : 7.f, need = std::abs(err);
        m_turnRate = approach(m_turnRate, std::copysign(std::min({need * (m_flying ? 4.f : 9.f), most, std::sqrt(2.f * accel * need)}), err), accel * dt);
        m_turnYaw        = wrapPi(yaw + m_turnRate * dt);
        m_turnLeft       = err - m_turnRate * dt;
        m_turnKnown      = true;
        return m_turnYaw;
    }

    std::optional<float> CAvatarAnimator::wayToFace(const SAvatarMotion& m, bool thirdPerson, float bodyYaw, float keysYaw) {
        // taps back and forth within ALT_BACK s keep the facing until one way is held ALT_HOLD s
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
        const V3 to = keysAhead(m.vel, w, m, WAY_AHEAD * std::clamp((3.f - length(V3{m.vel.x, 0, m.vel.z})) / 1.5f, 0.2f, 1.f));
        return length(to) > 0.05f ? std::atan2(to.x, -to.z) : std::atan2(dir.x, -dir.z);
    }

    void CAvatarAnimator::gait(const SAvatarMotion& m, bool air, std::vector<STRS>& pose) {
        const auto& md = *m_model;
        if (!m_gait)
            m_gait = makeGait(md);
        SGait&      g   = gaitOf(m_gait);
        const float dt  = std::clamp(m.dt, 0.f, 0.05f);
        const V3    Pc  = origin(m.world);
        const V3    Fw  = m.world.dir({0, 0, -1});
        const float yaw = std::atan2(Fw.x, -Fw.z);
        const V3    F = forwardOf(yaw), Rt = rightOf(yaw);
        V3          velC{m.vel.x, 0, m.vel.z};
        if (length(velC) < 1e-4f && m.speed > 1e-3f)
            velC = F * m.speed;
        const bool grounded = !air && !m.flying;
        g.time += dt;
        // yawIn(t): body yaw t s on, from turnBody's turn, else the current rate briefly
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

        // fresh: first frame, back from a clip, or teleported
        const V3   moved = Pc - g.lastP, unexplained = moved - V3{velC.x, m.vy, velC.z} * dt;
        const bool fresh = !g.live || !m_gaitUsed || length(V3{unexplained.x, 0, unexplained.z}) > 0.5f || std::abs(unexplained.y) > 1.f;
        // takeoff and landing switch pose: settle into it
        if (!fresh && grounded != g.grounded)
            m_settleAsk = std::max(m_settleAsk, grounded ? SETTLE_LAND : m.flying ? SETTLE_FLY : SETTLE_AIR);
        const bool leaving = !grounded && (g.grounded || fresh);
        g.grounded         = grounded;
        // carried body: follows the player's (BODY_K) no harder than legs push, never into a wall
        const float bodyMost = lerpf(BODY_WALK_ACCEL, BODY_RUN_ACCEL, m.run ? 1.f : g.run); // Shift: run accel from the start
        // hit something: velocity changed faster than the plugin accelerates
        g.wall     = length(velC - g.lastVelC) > 1.5f * std::max({m.accel, m.decel, m.turnBack}) * dt + 0.05f ? 0.25f : std::max(0.f, g.wall - dt);
        g.lastVelC = velC;
        // hy: last frame's pelvis yaw from the body's (unwrapped); legs are planned from it
        const float lagNow = fresh ? 0.f : g.hipLag - wrapPi(yaw - g.lastYaw), hy = yaw + lagNow;
        // hipIn(stand, t): pelvis yaw t s on, toward yawIn(t) within HIP_TURN of the standing foot
        auto        hipIn  = [&](int stand, float t) {
            const float want = yawIn(t) - hy;
            const auto& o    = g.feet[stand];
            float       f    = wrapPi((o.swing ? o.toYaw : o.yaw) - hy);
            if (const float err = want - f; !o.swing && std::abs(err) > 0.3f)
                f += std::copysign(std::min(std::abs(err) - 0.3f, PIVOT_RATE * t), err);
            const float to = f + std::clamp(want - f, -HIP_TURN, HIP_TURN), r = std::clamp(g.hipLagV * (to >= 0 ? 1.f : -1.f), 0.f, HIP_RATE);
            const float t1 = std::min((HIP_RATE - r) / HIP_ACCEL, t);
            return hy + std::copysign(std::min(std::abs(to), r * t1 + 0.5f * HIP_ACCEL * t1 * t1 + HIP_RATE * (t - t1)), to);
        };
        // how far p is out on foot s's side of the pelvis
        auto outOf = [&](int s, const V3& p) { return dot(V3{p.x - g.pelvis.x, 0, p.z - g.pelvis.z}, rightOf(hy)) * (s ? 1.f : -1.f); };
        // share of the wanted velocity the legs take while the pelvis turns (GO_ON)
        auto goFor = [&](const V3& to, float hipYaw, float bodyYaw) {
            if (length(V3{to.x, 0, to.z}) < 0.05f)
                return 1.f;
            const float wy = std::atan2(to.x, -to.z), off = std::abs(wrapPi(wy - hipYaw));
            const float turning = smoothstep01((off - std::abs(wrapPi(wy - bodyYaw)) - 0.3f) / 0.6f);
            const float along   = smoothstep01((std::cos(off) - std::cos(GO_OFF)) / (std::cos(GO_ON) - std::cos(GO_OFF)));
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
        if (fresh || !grounded)
            g.slope = 0;
        else if (v > 0.3f && dt > 1e-4f)
            g.slope += (std::clamp((Pc.y - g.lastP.y) / dt / v, -1.f, 1.f) - g.slope) * (1.f - std::exp(-dt * 8.f));
        else
            g.slope *= std::exp(-dt * 2.f);
        const V3 way = v > 0.3f ? vel / v : F;
        // ground under p, near the slope-extended feet height
        auto groundAt = [&](V3 p) {
            const float near = P.y + g.slope * dot(V3{p.x - P.x, 0, p.z - P.z}, way);
            p.y              = near;
            if (m.ground)
                if (const auto y = m.ground(p.x, p.z, near); y && std::isfinite(*y) && std::abs(*y - near) < 0.8f)
                    p.y = *y;
            return p;
        };
        // ground under a foot (heel, ankle, middle, toe): along one line, else the highest
        struct SUnder {
            float y = 0, lo = 0, hi = 0, pitch = 0;
            bool  even = true; // line holds STAIR_ROOM past heel and toe
            int   fit  = 3;    // 3 even + room, 2 even, 1 heel over an edge, 0 across
        };
        auto under = [&](V3 p, float fyaw, int s) {
            const V3    f    = forwardOf(fyaw);
            const float back = std::max(g.foot[s].heel.z, 0.f), toe = std::max(-g.foot[s].toe.z, 0.01f);
            const float yb = groundAt(p - f * (back + STAIR_ROOM)).y, yh = groundAt(p - f * back).y, ya = groundAt(p).y;
            const float ym = groundAt(p + f * (0.5f * toe)).y, yt = groundAt(p + f * toe).y, yf = groundAt(p + f * (toe + 0.5f * STAIR_ROOM)).y;
            const float k = (yt - yh) / (back + toe); // rise per meter
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
        // footing: shifted along the foot (up to STAIR_SHIFT) onto one tread, else on the edge
        auto footing = [&](V3 p, float fyaw, int s) {
            const V3 f = forwardOf(fyaw);
            const auto u0 = under(p, fyaw, s);
            if (u0.even || u0.hi - u0.lo < 0.06f)
                return V3{p.x, u0.y, p.z};
            V3  best{p.x, u0.y, p.z};
            int fit = u0.fit;
            for (float d = 0.02f; d <= STAIR_SHIFT + 1e-4f && fit < 3; d += 0.02f)
                for (const float sg : {1.f, -1.f})
                    if (const V3 q = p + f * (sg * d); fit < 3)
                        if (const auto u = under(q, fyaw, s); u.fit > fit)
                            best = {q.x, u.y, q.z}, fit = u.fit;
            return best;
        };
        // ankle over a foot spot, tilted to ground pitch gp
        auto ankleAt = [&](const V3& at, float fyaw, float pitch, float gp, int s) {
            return at + yawTurn(fyaw).rotate(footPitch(gp).rotate(ankleOver({}, 0, pitch, g.foot[s], g.ankleH[s], g.toes[s])));
        };
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
        // a key press jumps the wish: not a curve
        if (length(m.wish) > 0.3f && length(g.lastWish) > 0.3f && dt > 1e-4f) {
            const float turned = wrapPi(std::atan2(m.wish.x, -m.wish.z) - std::atan2(g.lastWish.x, -g.lastWish.z));
            g.wishTurn += ((std::abs(turned) < 0.3f ? std::clamp(turned / dt, -6.f, 6.f) : 0.f) - g.wishTurn) * (1.f - std::exp(-dt * 12.f));
        } else
            g.wishTurn = 0;
        g.lastWish = m.wish;
        if (dt > 1e-4f)
            g.accel += ((vel - g.lastVel) / dt - g.accel) * (1.f - std::exp(-dt * 6.f));
        g.lastP = Pc, g.lastVel = vel, g.lastYaw = yaw;

        const float froude = v * v / (GRAVITY * L);
        g.run    = approach(g.run, (m.run && v > 1.f) || froude > 0.8f ? 1.f : 0.f, dt * 4.f);
        g.crouch = approach(g.crouch, m.crouched && !m.flying ? 1.f : 0.f, dt * 5.f);

        // pelvis height: walking, highest over the standing leg; running, lowest under load
        const float bob = L * (0.01f + 0.008f * std::min(v, 2.5f));
        const float hWalk = g.hipH - L * 0.0275f, aWalk = 0.7f * bob;
        const float hRun = g.hipH - L * 0.065f, aRun = L * (0.03f + 0.005f * std::min(v, 7.f));
        const float hCrouch = 0.36f * g.hipH * g.crouch;

        // stride, cadence and duty factor: the stance is capped by the legs' span (reach ahead + behind + foot roll);
        // past it walking shortens steps, running shortens contact
        const float ahead_ = v > 0.05f ? dot(vel / v, forwardOf(hy)) : 1.f; // forward share vs the pelvis, -1 backward
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
            // reach along slope sl from hip height h: x² + (h - sl·x)² = reach²
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
        // 0..1: pelvis far from the body's yaw
        const float turnFar = length(m.wish) > 0.1f ? smoothstep01((std::abs(wrapPi(yawIn(0.15f) - hy)) - 0.6f) / 0.8f) : 0.f;
        // strides per second; drops slowly (CADENCE_DROP) reversing or turning far
        float cadence = std::max(v / std::max(stride, 1e-3f), lerpf(lerpf(0.8f, 1.3f, g.run), TURN_CADENCE, turnFar) * std::sqrt(0.9f / L));
        if (g.moving)
            cadence = std::max(cadence, g.cadence - dt * (dot(m.wish, vel) < 0 || std::abs(g.turnLeft) > 1.f || std::abs(g.hipLagV) > CADENCE_TURN ? CADENCE_DROP : 3.f));
        stride              = v / cadence;
        ahead               = lerpf(0.5f * beta * stride, std::clamp(ahead, 0.f, 0.5f * beta * stride), frontal);
        g.stride = stride, g.cadence = cadence, g.duty = beta, g.swingTime = (1.f - beta) / cadence;

        // a landing foot stays on its own side of the other, within a side step
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
        // landing(s, left): spot `ahead` of the body `left` s on, its yaw, how far out from the pelvis
        auto        landing = [&](int s, float left) {
            // simulate both bodies until then
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
            // within HIP_TURN of the pelvis at landing: sharp turns take steps
            const float ply  = hipIn(1 - s, left);
            const float ly   = ply + std::clamp(yawIn(left) - ply, -HIP_TURN, HIP_TURN);
            const V3    rt   = rightOf(ply + (ly - ply) * (1.f - smoothstep01(std::abs(g.slope) / 0.2f)));
            const float side = lerpf(wide * 0.9f, wide * 0.35f, g.run) * (s ? 1.f : -1.f);
            // a foot that will pivot lands offset by its heel swing, so the ankle ends under the hip
            const float rem = yawIn(left + g.duty / std::max(cadence, 0.1f)) - ly;
            const float piv = std::abs(rem) > 0.3f ? std::copysign(std::min(std::abs(rem) - 0.3f, PIVOT_RATE * g.duty / std::max(cadence, 0.1f)), rem) : 0.f;
            const V3    ball{0, 0, g.foot[s].ball.z}, place = uncross(s, at + fwd * stepAhead + rt * side, left);
            const V3    swung = place + yawTurn(ly + piv).rotate(ball) - yawTurn(ly).rotate(ball);
            const V3    to    = footing(std::abs(piv) > 0.05f && under(swung, ly, s).even && under(swung, ly + piv, s).even ? swung : place, ly, s);
            return std::tuple{to, ly, dot(V3{to.x - at.x, 0, to.z - at.z}, rt) * (s ? 1.f : -1.f)};
        };

        if (!grounded) {
            // in the air: at takeoff note the leading leg and the speed
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
            // landed moving: the stride goes on from the first foot down, not stopping dead
            g.wasAir = false;
            for (int s = 0; s < 2; ++s) {
                auto& ft = g.feet[s];
                ft       = {.at = footing(ft.ankle, yaw, s), .yaw = yaw, .pitch = ft.pitch, .heel = ft.pitch, .knee = ft.knee, .ankle = ft.ankle};
            }
            g.groundY = P.y, g.groundV = g.groundRate = 0, g.lastSupport = NAN;
            const bool going = v > 0.8f && length(m.wish) > 0.1f;
            if (going && beta < 0.5f) { // running; walking restarts from standing
                // first foot's phase from how far the body is past it
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
            m_settleLift = LAND_GIVE;
            if (!m.flying)
                g.dipV += std::clamp(-g.airVy - 1.5f, 0.f, 10.f) * (going ? 0.1f : 0.16f);
        }

        if (grounded) {
            // starting: the rear foot lifts first
            if (!g.moving && (v > 0.25f || turnFar > 0.5f)) {
                g.moving = true;
                // a far turn: the foot on its side leads
                int lead = g.feet[0].swing ? 0 : g.feet[1].swing ? 1 : -1;
                if (lead < 0)
                    lead = turnKnown && std::abs(m_turnLeft) > 1.f ? (m_turnLeft > 0 ? 1 : 0) : dot(g.feet[0].at - g.feet[1].at, vel) < 0 ? 0 : 1;
                auto& ft = g.feet[lead];
                const float ph = ft.swing ? beta + ft.s * (1.f - beta) : beta, was = g.phase;
                g.phase        = frac(ph - 0.5f * lead);
                g.phaseOff     = phaseDiff(g.phaseOff + was - g.phase);
                ft.phase       = ft.swing ? ph : beta - 1e-4f;
                ft.start       = beta;
                ft.timed = ft.wait = false; // becomes a gait step, eased
                g.feet[1 - lead].phase = frac(ph + 0.5f);
            } else if (g.moving && v < 0.12f && length(velC) < 0.3f && length(m.wish) < 0.1f) {
                // stopping: a foot in the air lands beside the other
                g.moving = false;
                for (auto& ft : g.feet)
                    if (ft.swing) {
                        ft.timed = true;
                        ft.rate  = 1.f / std::max(0.14f, (1.f - ft.s) * g.swingTime) * (1.f - ft.s);
                    }
            }

            if (g.moving) {
                // strained: planted foot nearly out of reach; leaving: body moving off it sideways or strained
                auto strained = [&](int s) { return std::max(0.f, (g.feet[s].tight - 0.55f) / 0.25f) + std::max(0.f, g.feet[s].over / 0.015f); };
                auto leaving = [&](int s) {
                    const auto& ft = g.feet[s];
                    const V3    d{P.x - ft.at.x, 0, P.z - ft.at.z};
                    return !ft.swing && dot(d, vel) > 0 && (std::abs(dot(d, rightOf(ft.yaw))) > 0.35f * length(d) || strained(s) > 0);
                };
                // hurry while a planted foot is left behind, out of reach or out to its side
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
                // body swing at the eased pace, or arms and pelvis jerk
                g.phaseOff = phaseDiff(g.phaseOff + cadence * dt * (g.hurrySmooth - g.hurry));
                // walking, a foot the body turned or moved from lifts early
                int   lift = -1;
                float need = 0;
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
                        // a standing step overtaken by the gait: land where it says, then wait for the stance
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
                        // lifts when its stance ends, not just before landing again
                        if (!ft.wait && ph >= beta && ph < 0.92f && ft.phase <= ph) {
                            ft.swing = true, ft.timed = false, ft.locked = false, ft.gaitW = 1;
                            ft.from = ft.at, ft.fromYaw = ft.yaw, ft.fromPitch = ft.pitch, ft.liftKnee = ft.knee, ft.fromOut = outOf(s, ft.at);
                            ft.start = ph, ft.s = 0;
                            std::tie(ft.to, ft.toYaw, ft.toOut) = landing(s, swingLeft(ph));
                        }
                    } else if (ph < ft.phase - 0.5f) {
                        // phase wrapped: landed
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
                            // uncross in flight, eased, so locking doesn't jump
                            ft.to += (uncross(s, ft.to, swingLeft(ph)) - ft.to) * (1.f - std::exp(-dt * 25.f));
                            ft.locked = ft.s > 0.75f;
                        } else { // locked: big changes only
                            // retarget past 6 cm; uncross eased (the other foot's reference jumps)
                            if (const auto [to, ly, lo] = landing(s, swingLeft(ph)); length(to - ft.to) > 0.06f)
                                ft.to += (to - ft.to) * (smoothstep01((length(to - ft.to) - 0.06f) / 0.04f) * (1.f - std::exp(-dt * 8.f)));
                            ft.to += (uncross(s, ft.to, swingLeft(ph)) - ft.to) * (1.f - std::exp(-dt * 25.f));
                        }
                        // locked: onto one tread, not across an edge; moves at most RETARGET over body speed
                        if (ft.locked) {
                            const V3 fl = footing(ft.to, ft.toYaw, s);
                            ft.to.x = fl.x, ft.to.z = fl.z;
                        }
                        if (V3 d{ft.to.x - was.x, 0, ft.to.z - was.z}; length(d) > (RETARGET + v) * dt)
                            ft.to = was + d * ((RETARGET + v) * dt / length(d));
                        ft.to.y = footY(ft.to, ft.toYaw, s);
                    }
                    ft.phase = ph;
                }
            } else {
                // standing: a foot off its spot steps back, the furthest first
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
                    if (ft.s < 0.7f) { // follows the body, then comes down
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
        // eased so the gait doesn't start or stop at once
        springTo(g.moveW, g.moveWV, g.moving ? std::clamp(v / 0.7f, 0.f, 1.f) : 0.f, 14.f, dt);
        g.moveW        = std::clamp(g.moveW, 0.f, 1.f);
        const float mw = g.moveW;

        // planted feet pivot on the ball as the body turns; the pelvis turns as far as they allow
        if (grounded) {
            const float from = g.moving ? 0.3f : HIP_TURN;
            for (int s = 0; s < 2; ++s) {
                auto& ft   = g.feet[s];
                float rate = 0;
                // via the pelvis, so it unwraps the right way
                if (const float need = -lagNow - wrapPi(ft.yaw - hy), over = std::abs(need) - (ft.spin > 0.05f ? 0.03f : from); !ft.swing && over > 0) {
                    const float turn = std::copysign(std::min(over, PIVOT_RATE * dt), need);
                    const V3    ball{0, 0, g.foot[s].ball.z}, heel{0, 0, std::max(g.foot[s].heel.z, 0.f)};
                    const V3    at = ft.at + yawTurn(ft.yaw).rotate(ball) - yawTurn(ft.yaw + turn).rotate(ball);
                    // no pivot swinging the heel into a step (PIVOT_CLEAR): it steps instead
                    if (groundAt(at + yawTurn(ft.yaw + turn).rotate(heel)).y < ft.at.y + std::tan(ft.gp) * heel.z + PIVOT_CLEAR) {
                        ft.at  = at;
                        ft.yaw = wrapPi(ft.yaw + turn);
                        rate   = std::abs(turn) / std::max(dt, 1e-4f) / PIVOT_RATE;
                    }
                }
                ft.spin += (rate - ft.spin) * (1.f - std::exp(-dt * 15.f));
            }
            // pelvis lag range the planted feet allow
            float lo = -1e9f, hi = 1e9f;
            for (const auto& ft : g.feet)
                if (!ft.swing) {
                    const float c = lagNow + wrapPi(ft.yaw - hy);
                    lo            = std::max(lo, c - HIP_TURN);
                    hi            = std::min(hi, c + HIP_TURN);
                }
            // within HIP_RATE / HIP_ACCEL: no snap round when a foot lifts
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

        const V3 Rp = rightOf(yaw + g.hipLag);

        // planted feet roll heel to toe (backwards the reverse, sideways flat)
        const float roll = g.forward >= 0 ? g.forward : 0.4f * g.forward;
        std::array<V3, 2>   ankleW;
        std::array<Quat, 2> footW;
        float               support = 0; // the ground under them
        auto toesUp = [&](int s, float pitch) { return pitch < 0 ? pitch * g.toesUp[s] : pitch; };
        // pitch toward `want`, rate-limited (FOOT_ROLL, FOOT_ROLL_ACCEL)
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
                continue; // air pose below
            if (!ft.swing) {
                const float u = std::clamp(frac(g.phase + 0.5f * s) / beta, 0.f, 1.f);
                pitch         = g.moving ? toesUp(s, lerpf(through(WALK_STANCE_PITCH, u), through(RUN_STANCE_PITCH, u), g.run) * mw * roll) : 0.f;
                if (!g.moving)
                    pitch = ft.pitch * std::exp(-dt * 14.f);
                if (ft.spin > 1e-3f) // on its ball
                    pitch = std::max(pitch, 0.15f * ft.spin);
                support += at.y * 0.5f;
                ft.gp += (under(at, fy, s).pitch - ft.gp) * (1.f - std::exp(-dt * 20.f));
            } else {
                // running, the foot still moves ahead as it lands; gw blends the gait's step with its own
                const float gw = smoothstep01(ft.gaitW);
                const float u = ft.s, e = lerpf(minJerk(u), u * u * (3.f - 2.f * u) + 0.6f * u * u * (u - 1.f), g.run * gw);
                // landing height eased onto a new stair, exact at touchdown
                if (!ft.toYSet)
                    ft.toY = ft.to.y, ft.toYV = 0, ft.toYSet = true, ft.fromGp = ft.gp;
                else
                    springTo(ft.toY, ft.toYV, ft.to.y, 30.f, dt);
                ft.toGp = under(ft.to, ft.toYaw, s).pitch;
                ft.gp   = lerpf(ft.fromGp, ft.toGp, e);
                V3 to = ft.to;
                to.y  = lerpf(ft.toY, ft.to.y, smoothstep01((ft.s - 0.8f) / 0.2f));
                at    = lerp(ft.from, to, e);
                // keep it under its hip as the pelvis turns (SWING_OUT / SWING_IN), not onto higher ground
                {
                    const float sd = s ? 1.f : -1.f, ends = lerpf(ft.fromOut, ft.toOut, ft.s), hipOut = std::abs(g.hipAt[s].x);
                    const float was = dot(V3{at.x - g.pelvis.x, 0, at.z - g.pelvis.z}, Rp) * sd;
                    const float lim = std::clamp(was, std::min(ends, hipOut) - SWING_IN, std::max(ends, hipOut) + SWING_OUT);
                    if (const V3 in = at + Rp * (sd * (lim - was) * smoothstep01(ft.s / 0.25f) * smoothstep01((1.f - ft.s) / 0.25f));
                        std::abs(lim - was) > 1e-4f && under(in, ft.fromYaw + wrapPi(ft.toYaw - ft.fromYaw) * e, s).hi < std::max(ft.from.y, to.y) + 0.02f)
                        at.x = in.x, at.z = in.z;
                }
                // step up: up then over; step down: over then down; clear edges on the way
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
            pitch     = rollTo(ft, pitch);
            ft.pitch  = pitch;
            ankleW[s] = ankleAt(at, fy, pitch, ft.gp, s);
            footW[s]  = yawTurn(fy) * footPitch(pitch + ft.gp);
        }

        // pelvis ground: mean along the heading (a flight's line); springy, tracking the climb rate
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
        {
            constexpr float W = 11.f;
            g.dipV += (-g.dip * W * W - 2.f * 0.75f * W * g.dipV) * dt;
            g.dip = std::clamp(g.dip + g.dipV * dt, -0.03f * L, 0.3f * L);
        }

        // pelvis: over the body, bobbing and swaying
        springTo(g.phaseOff, g.phaseOffV, 0.f, 15.f, dt);
        const float phL = g.phase + g.phaseOff, mid = 4.f * PI * (phL - beta * 0.5f);
        const float gaitY = lerpf(hWalk + aWalk * std::cos(mid), hRun - aRun * std::cos(mid), g.run);
        float       rollW = 0; // standing: onto one leg
        if (grounded && !g.moving) {
            // standing: weight onto the planted foot while the other steps, else a slow sway
            for (int s = 0; s < 2; ++s)
                if (g.feet[s].swing)
                    rollW += dot(g.feet[1 - s].at - P, Rp) * 0.55f * std::sin(PI * std::min(1.f, g.feet[s].s * 1.2f));
            rollW += L * 0.012f * std::sin(TAU * g.time / 6.5f) * (1.f - mw);
        }
        // eased: no jump onto a far-out foot when stopping
        g.rollW += (rollW - g.rollW) * (1.f - std::exp(-dt * 12.f));
        rollW = g.rollW;
        // eased walk/run blend for arms and trunk
        springTo(g.runArms, g.runArmsV, g.run, 12.f, dt);
        const float ra = std::clamp(g.runArms, 0.f, 1.f);
        // styled: the walk/run clips (SGaitClip) drive hips, trunk, head and arms at the body's phase; turns, lean,
        // crouch and arm clearance stay procedural
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
        // clip turn of bone b, or of the nearest lower bone the clip has
        auto sAt = [&](const std::array<Quat, HB_COUNT>& T, int b) {
            for (; b > HB_HIPS && !sHas[b]; --b)
                ;
            return T[b];
        };
        // sway, m, + = right (the clip's: hips heights, + = left)
        const float lat = lerpf(-mw * L * 0.028f * (1.f - 0.65f * g.run) * std::cos(2.f * PI * (phL - beta * 0.5f)), -mw * (sMove.x - sMeanMove.x) * g.unit, sw) +
            rollW;
        V3          pelvis = P + Rp * lat;
        pelvis.y  = (grounded ? g.groundY : P.y) + lerpf(g.hipH - 0.006f * L, gaitY, mw) - hCrouch - g.dip;

        // in the air (JUMP_*, FLY_*): springs toward the pose, from the grounded pose at takeoff
        g.flyE.to(m.flying ? 1.f : 0.f, 7.f, 1.f, dt);
        const float fe = smoothstep01(std::clamp(g.flyE.x, 0.f, 1.f));
        float       ju = 0.5f, hover = 0;
        if (!grounded) {
            if (leaving) {
                const Quat hq = yawTurn(yaw);
                const V3   was = fresh ? P + UP * g.hipH : g.pelvis;
                for (int s = 0; s < 2; ++s) {
                    auto&       al = g.airLeg[s];
                    const auto& ft = g.feet[s];
                    const V3    d  = hq.conj().rotate(ft.ankle - was - hq.rotate(g.hipAt[s])); // avatar space: ahead -z, right +x
                    const float t = g.thigh[s], sh = g.shin[s], dist = std::clamp(length(d), std::abs(t - sh) + 1e-3f, t + sh);
                    const float knee = std::acos(std::clamp((dist * dist - t * t - sh * sh) / (2.f * t * sh), -1.f, 1.f));
                    const float hip  = std::atan2(-d.z, -d.y) + std::asin(std::clamp(sh * std::sin(knee) / dist, -1.f, 1.f));
                    al.hip = {hip, 0}, al.knee = {knee, 0}, al.out = {std::asin(std::clamp(d.x * (s ? 1.f : -1.f) / dist, -1.f, 1.f)), 0};
                    al.toes     = {ft.pitch + hip - knee, 0};
                    g.airArm[s] = {{g.armWas[s].x, 0}, {g.armWas[s].y, 0}, {g.armWas[s].z, 0}};
                }
                g.airLean  = {g.leanWas, 0};
                g.airPitch = g.airRoll = g.airYaw = {};
                // a jump lifts the body over a moment
                m_settleLift = m.vy > 1.f ? 1.f : 0.f;
            }
            // a jump or a fall
            ju            = std::clamp(0.5f - m.vy / (2.f * JUMP_UP), 0.f, 1.f);
            const float r = g.airLead >= 0 ? g.airRun : 0.f;
            const float u = dot(vel, F), side = dot(vel, Rt), vy = m.vy, sp = std::sqrt(u * u + side * side + vy * vy);
            const float cruise = 1.f - std::exp(-std::max(u, 0.f) / FLY_LIE_SPEED), fast = std::clamp((u - 9.f) / 6.f, 0.f, 1.f);
            const float climb = std::clamp(vy / 6.f, 0.f, 1.f) * (1.f - cruise), sink = std::clamp(-vy / 6.f, 0.f, 1.f) * (1.f - cruise);
            const float aF = dot(g.accel, F), aR = dot(g.accel, Rt);
            hover          = std::exp(-sp / 2.f);
            float pitchTo  = u >= 0 ? FLY_LIE * cruise * u / std::sqrt(u * u + std::max(vy, 0.f) * std::max(vy, 0.f) + 1e-4f) : -0.35f * (1.f - std::exp(u / 3.f));
            pitchTo += std::clamp(FLY_PUSH * aF, -FLY_BRAKE_MOST, FLY_PUSH_MOST) + 0.2f * std::clamp(-vy / 8.f, 0.f, 1.f) * cruise;
            float rollTo = std::clamp(std::atan(length(vel) * g.velTurn / GRAVITY) * 0.9f + 0.06f * side + 0.02f * aR, -FLY_BANK, FLY_BANK) +
                0.035f * std::sin(TAU * 0.21f * g.time) * hover;
            // first person: barely lie down or bank (camera in the eyes)
            if (m.fp.on)
                pitchTo *= 0.25f, rollTo *= 0.5f;
            const float lw = lerpf(AIR_LEG_W, FLY_LEG_W, fe), lz = lerpf(AIR_LEG_Z, FLY_LEG_Z, fe);
            for (int s = 0; s < 2; ++s) {
                // jump: legs tucked; from a run, a stride in the air
                const bool lead = s == g.airLead;
                float hipJ  = lerpf(through(JUMP_HIP, ju) + (s ? 0.04f : -0.02f), through(lead ? JUMP_LEAD_HIP : JUMP_TRAIL_HIP, ju), r);
                float kneeJ = lerpf(through(JUMP_KNEE, ju) + (s ? 0.08f : 0.f), through(lead ? JUMP_LEAD_KNEE : JUMP_TRAIL_KNEE, ju), r);
                float toesJ = lerpf(through(JUMP_TOES, ju), through(lead ? JUMP_LEAD_TOES : JUMP_TRAIL_TOES, ju), r);
                // flying legs: per-mode poses, treading and flutter
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
                // air arms likewise, out on a bank's high side
                const float aheadJ = lerpf(through(JUMP_ARM, ju), through(lead ? JUMP_ARM_BACK : JUMP_ARM_AHEAD, ju), r);
                const float outJ   = lerpf(through(JUMP_ARM_OUT, ju), through(JUMP_ARM_RUN_OUT, ju), r);
                const float bendJ  = lerpf(0.4f, through(JUMP_ARM_RUN_BEND, ju), r);
                float aheadF = lerpf(lerpf(lerpf(0.12f, lerpf(-0.35f, -0.55f, fast), cruise), -0.15f, climb), 0.25f, sink);
                float outF   = lerpf(lerpf(lerpf(0.5f, lerpf(0.32f, 0.16f, fast), cruise), 0.28f, climb), 0.6f, sink);
                float bendF  = lerpf(lerpf(lerpf(0.5f, lerpf(0.25f, 0.12f, fast), cruise), 0.3f, climb), 0.45f, sink);
                aheadF += 0.06f * std::sin(TAU * 0.27f * g.time + s * 2.1f) * hover + std::clamp(-0.012f * aF, 0.f, 0.45f);
                outF += 0.05f * std::sin(TAU * 0.19f * g.time + s) * hover + std::clamp(-0.006f * aF, 0.f, 0.25f);
                const float up = g.airRoll.x * (s ? -1.f : 1.f); // this arm's side rolled up
                outF += up > 0 ? 0.3f * up : 0.12f * up;
                auto& aa = g.airArm[s];
                aa.ahead.to(lerpf(aheadJ, aheadF, fe), AIR_ARM_W, AIR_ARM_Z, dt);
                aa.out.to(lerpf(outJ, outF, fe), AIR_ARM_W, AIR_ARM_Z, dt);
                aa.bend.to(lerpf(bendJ, bendF, fe), AIR_ARM_W, AIR_ARM_Z, dt);
                aa.out.x  = std::clamp(aa.out.x, 0.f, 1.4f);
                aa.bend.x = std::clamp(aa.bend.x, 0.f, 2.4f);
            }
            // air trunk: jump lean, or flying (curled hovering, arched cruising)
            const float leanJ = lerpf(through(JUMP_LEAN, ju), through(JUMP_RUN_LEAN, ju), r);
            const float leanF = lerpf(lerpf(lerpf(0.06f, lerpf(-0.15f, -0.22f, fast), cruise), -0.05f, climb), 0.1f, sink);
            g.airLean.to(lerpf(leanJ, leanF, fe), AIR_ARM_W, AIR_ARM_Z, dt);
            g.airPitch.to(pitchTo * fe, FLY_BODY_W, FLY_BODY_Z, dt);
            g.airRoll.to(rollTo * fe, FLY_BODY_W * 0.8f, FLY_BODY_Z, dt);
            g.airYaw.to(0.05f * std::sin(TAU * 0.13f * g.time + 1.f) * hover * fe, 3.f, 1.f, dt);
        } else
            g.airPitch = g.airRoll = g.airYaw = {};

        // pelvis turns (frame above: +x left, +y up, +z ahead): with the stride, into turns
        const float fwdA  = dot(g.accel, F);
        // lean by the sideways push, not by turning on the spot
        const float leanMost = lerpf(0.1f, 0.2f, g.run);
        const float lean     = mw * std::clamp(std::atan(dot(g.accel, Rp) / GRAVITY) * 0.6f, -leanMost, leanMost);
        float yawP = -mw * lerpf(0.06f + 0.02f * std::min(v, 2.f), 0.1f, g.run) * std::cos(TAU * phL);
        // standing, the weight-bearing hip is higher
        float rollP = mw * 0.07f * std::sin(TAU * (phL + 0.05f)) + lean - (1.f - mw) * 0.6f * rollW / L;
        float tiltP = mw * lerpf(0.03f, 0.09f, g.run) + 0.25f * g.crouch;
        if (sw > 0) { // clip: about its mean, which tilts ahead
            const V3 d = rotationVector(sMean[HB_HIPS].conj() * sT[HB_HIPS]), m0 = rotationVector(sMean[HB_HIPS]);
            yawP       = lerpf(yawP, mw * d.y, sw);
            rollP      = lerpf(rollP, mw * d.z + lean - (1.f - mw) * 0.6f * rollW / L, sw);
            tiltP      = lerpf(tiltP, mw * (m0.x + d.x) + 0.25f * g.crouch, sw);
        }
        const float lagP  = -g.hipLag; // frame above: left > 0
        const Quat  hipsQ = Quat::axisAngle(UP, yawP + lagP + g.airYaw.x) * Quat::axisAngle({0, 0, 1}, rollP + g.airRoll.x) *
            Quat::axisAngle({1, 0, 0}, tiltP + g.airPitch.x);
        pelvis += Rp * (std::sin(lean) * g.hipH * 0.5f);

        // the pelvis no higher than the planted legs reach
        const M4   toAvatar = m.world.inverse();
        const Quat bodyQ    = yawTurn(yaw);
        const Quat hipsW    = bodyQ * (g.toFrame.conj() * hipsQ * g.toFrame); // frame above -> avatar -> world
        // walking ahead, knees follow WALK_STANCE_KNEE / WALK_SWING_KNEE: the heel rises late in stance
        const float kneeW    = (1.f - g.run) * mw * std::clamp(2.f * g.forward, 0.f, 1.f);
        auto        stanceU  = [&](int s) { return std::clamp(frac(g.phase + 0.5f * s) / beta, 0.f, 1.f); };
        auto        heelUp   = [&](int s) { return kneeW > 0.01f && !g.feet[s].swing && stanceU(s) >= 0.4f; };
        auto        heelMost = [&](int s) { // heel limit this frame (FOOT_ROLL)
            return approach(g.feet[s].heel, 1.15f, FOOT_ROLL * dt);
        };
        auto        kneeAt   = [&](int s, float bend) { // hip-ankle distance at that bend
            const float a = g.thigh[s], b = g.shin[s];
            return std::min(std::sqrt(a * a + b * b + 2.f * a * b * std::cos(bend)), 0.995f * (a + b));
        };
        if (!grounded)
            g.lowered = g.loweredV = 0;
        else {
            // a landing foot counts as it nears touchdown
            float lower = 0;
            for (int s = 0; s < 2; ++s) {
                const auto& ft = g.feet[s];
                const float w  = ft.swing ? smoothstep01(ft.gaitW) * smoothstep01((ft.s - 0.5f) / 0.5f) : 1.f;
                if (w <= 0)
                    continue;
                const V3 ankle = ft.swing ? ankleAt(ft.to, ft.toYaw, toesUp(s, lerpf(through(WALK_SWING_PITCH, 1), through(RUN_SWING_PITCH, 1), g.run)), ft.toGp, s)
                    : heelUp(s)           ? ankleAt(ft.at, ft.yaw, lerpf(ft.pitch, std::max(ft.pitch, heelMost(s)), kneeW), ft.gp, s)
                                          : ankleW[s];
                // hip at landing
                const V3    hip   = pelvis + hipsW.rotate(g.hipAt[s]) + (ft.swing ? vel * ((1.f - ft.s) * g.swingTime) : V3{});
                const float reach = 0.995f * (g.thigh[s] + g.shin[s]);
                const float dx    = length(V3{hip.x - ankle.x, 0, hip.z - ankle.z});
                const float up    = std::sqrt(std::max(0.f, reach * reach - dx * dx));
                lower             = std::max(lower, w * (hip.y - ankle.y - up));
                if (!ft.swing)
                    g.feet[s].tight = (hip.y - ankle.y - up) / (0.12f * L);
            }
            // lowering limit: what knees bend well, more on slopes and stairs; feet out of reach are dragged
            float below = 0;
            for (const auto& ft : g.feet) { // only feet ahead or under
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
                // heel up: least pitch (from WALK_HEEL_LEAST) bringing the ankle within knee reach
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
                // lower the pelvis for the rest
                const float reach = 0.995f * (g.thigh[s] + g.shin[s]), dx = length(V3{hip.x - ankleW[s].x, 0, hip.z - ankleW[s].z});
                const float over = hip.y - ankleW[s].y - std::sqrt(std::max(0.f, reach * reach - dx * dx)), room = lowest - lower;
                ft.tight         = (lower + over) / (0.12f * L);
                if (over > 0 && room > 0) {
                    pelvis.y -= std::min(over, room);
                    lower += std::min(over, room);
                }
            }
            // lowered: down fast (LOWER_ACCEL, LOWER_SPEED), up critically damped so steps don't hop
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
                // drag the foot, but not into a stair riser
                if (const V3 pull = d * ((most - dx) / std::max(dx, 1e-4f)); dx > most + 1e-4f) {
                    const auto u = under(ft.at + pull, ft.yaw, s);
                    if (u.y < ft.at.y + 0.02f || (u.even && u.y < ft.at.y + 0.1f)) {
                        const V3 by = pull + V3{0, u.even && std::abs(u.y - ft.at.y) < 0.1f ? u.y - ft.at.y : 0.f, 0};
                        ft.at += by;
                        ankleW[s] += by;
                    }
                }
            }
            // a swinging foot rises as the knee folds
            for (int s = 0; s < 2; ++s) {
                auto& ft = g.feet[s];
                springTo(ft.fold, ft.foldV, ft.timed ? 0.f : kneeW, 20.f, dt);
                if (!ft.swing) {
                    ft.raise = ft.raiseV = 0, ft.raiseTo = -1;
                    continue;
                }
                const V3    hip    = pelvis + hipsW.rotate(g.hipAt[s]);
                V3&         at     = ankleW[s];
                // from the lift-off bend; less in quick steps
                const float want   = lerpf(ft.liftKnee, kneeAt(s, through(WALK_SWING_KNEE, ft.s) / g.hurrySmooth), smoothstep01(ft.s / 0.2f));
                const float across = length(V3{at.x - hip.x, 0, at.z - hip.z});
                // a foot far behind doesn't kick up to the hip
                const float y = want > across ? hip.y - std::sqrt(want * want - across * across) : hip.y, most = 0.12f * L;
                // raise: rate-limited (RAISE_ACCEL), braking for 0 and `most`; else a cut-short step stops dead
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

        // in the air: legs from the air springs; landing a jump, the pelvis shifts to put the lower foot down
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
                ft.pitch = std::asin(std::clamp(-footW[s].rotate({0, 0, -1}).y, -1.f, 1.f)); // toes down, ready to land
                ft.roll  = 0;
                ft.yaw   = yaw;
            }
        } else
            for (int s = 0; s < 2; ++s)
                g.feet[s].ankle = ankleW[s];
        g.pelvis = pelvis;
        for (int s = 0; s < 2; ++s) {
            g.feet[s].knee = length(ankleW[s] - pelvis - hipsW.rotate(g.hipAt[s]));
            g.feet[s].heel = g.feet[s].pitch;
        }

        // into the frame above
        auto frameOf = [&](const V3& world) { return g.rig.facing.conj().rotate(g.fixInv.point(toAvatar.point(world)) - g.rig.hips) / g.rig.height + UP; };
        auto turnOf  = [&](const Quat& world) { return (g.toFrame * bodyQ.conj() * world * g.toFrame.conj()).normalized(); };

        CPoser p(g.body);
        p.body(hipsQ, frameOf(pelvis) - UP);
        // trunk: twisted against the pelvis, leaning ahead, breathing; in the air from the air spring
        const float accelLean = std::clamp(std::atan(fwdA / GRAVITY) * 0.6f, -0.12f, 0.3f) * std::min(1.f, mw + 0.5f);
        const float trunkLean = (grounded ? mw * lerpf(0.055f, 0.1f, ra) + accelLean + 0.3f * g.crouch - 0.6f * tiltP : g.airLean.x) +
            0.012f * std::sin(TAU * g.time / 4.f);
        if (grounded)
            g.leanWas = trunkLean - 0.012f * std::sin(TAU * g.time / 4.f);
        // trunk turns back from the pelvis lag, soft-limited to TRUNK_TWIST
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
        // clip trunk bone by bone, plus the walking's twist, lean, crouch and breathing
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
        // head level along the heading, at most HEAD_MOST from the trunk, leading into turns (HEAD_LEAD)
        constexpr float HEAD_MOST = 0.9f;
        const float     headLead = grounded ? std::clamp(g.turnLeft * HEAD_LEAD, -HEAD_LEAD_MOST, HEAD_LEAD_MOST) : 0.f;
        Quat            level    = slerp(Quat{}, trunkDone.conj() * Quat::axisAngle(UP, -headLead), 0.85f);
        if (const float turned = 2.f * std::acos(std::clamp(std::abs(level.w), 0.f, 1.f)); turned > HEAD_MOST)
            level = slerp(Quat{}, level, HEAD_MOST / turned);
        Quat neck = slerp(Quat{}, level, 0.45f), head = slerp(Quat{}, level, 0.55f);
        if (sw > 0) {
            // clip head: leads the turn too, at most HEAD_MOST from the trunk
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

        // arms swing against the legs, slightly after; elbows bend more running
        const float armAmp = mw * lerpf(0.16f + 0.1f * std::min(v, 2.f), 0.55f, ra);
        // armAt: upper arm and forearm (chest frame) for swing and out angles
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
        // into: how far a posed arm sinks into the rest body round the hips, m; sleeves count half
        const SBodyClearance& cl = md.clearance;
        auto into = [&](int s, const std::pair<V3, V3>& arm) {
            const SBody& b   = p.measure();
            const Quat   top = p.turnAt(HB_UPPER_CHEST), hq = p.turnAt(HB_HIPS);
            const V3     hip = UP + p.move, elbow = p.jointAt(s ? HB_R_UPPER_ARM : HB_L_UPPER_ARM) + top.rotate(arm.first) * b.upper[s];
            const V3     fore = top.rotate(arm.second);
            float        most = -1e30f;
            auto         at   = [&](const V3& q, float r) { // arm point (frame above), skin r m
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
        // least `out` clearing the body
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
        // clip arm (upper arm, forearm, hand, palm) in its chest frame; turnedOut swings it out
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
        // least turn out clearing the body, up to 1.2 rad (a clip made in a wide skirt may need most)
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
                // clip arm: spread fitted to armOut, plus stride clearance, plus a damped push for this swing
                const V3    um   = clipArm(s, sMean)[0];
                const float base = g.armOut - std::asin(std::clamp(sx * um.x, -1.f, 1.f));
                // need at GAIT_NEED_STEPS points of the stride, a few re-sampled a frame
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
            if (grounded) // air arms start from these
                g.armWas[s] = lerp(V3{aheadOf(s, swing), out, bendOf(s, swing)}, clipWas, sw);
            if (cl.measured) {
                // held out to clear a skirt over the whole swing; in slower than out
                float cycle = 0;
                for (float w : {-1.f, -0.75f, -0.5f, -0.25f, 0.f, 0.25f, 0.5f, 0.75f, 1.f})
                    if (armAmp > 0.01f || w == 0.f)
                        cycle = std::max(cycle, clearOut(s, w * armAmp, out) - out);
                g.armClear[s] = approach(g.armClear[s], cycle, dt * (cycle > g.armClear[s] ? 2.f : 0.6f));
                // plus the current swing's, damped: the outline is sampled every 5°
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

        // legs to the feet, knees over the toes
        for (int s = 0; s < 2; ++s) {
            const Quat foot  = turnOf(footW[s]);
            const Quat level = turnOf(yawTurn(std::atan2(footW[s].rotate({0, 0, -1}).x, -footW[s].rotate({0, 0, -1}).z)));
            const V3   knee  = (grounded ? level : hipsQ).rotate({0, 0, 1}) + (grounded ? Quat{} : hipsQ).rotate(V3{s ? -0.12f : 0.12f, 0, 0});
            p.legTo(s, frameOf(ankleW[s]), foot * g.footUntilt[s], knee);
            if (h[s ? HB_R_TOES : HB_L_TOES] >= 0) // toes flat as the heel rises
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
        // knee bend, degrees, from hip-ankle distance
        auto kneeBend = [&](int s) {
            const float a = g.thigh[s], b = g.shin[s], d = std::clamp(g.feet[s].knee, std::abs(a - b), a + b);
            return 180.f - std::acos(std::clamp((a * a + b * b - d * d) / (2 * a * b), -1.f, 1.f)) * 180.f / PI;
        };
        for (int s = 0; s < 2; ++s) {
            const auto& ft = g.feet[s];
            feet += std::format(R"({}{{"planted": {}, "step": {:.3f}, "pitch": {:.3f}, "yaw": {:.1f}, "strain": {:.2f}, "tight": {:.2f}, "knee": {:.0f}, "at": [{:.3f}, {:.3f}, {:.3f}], "ankle": [{:.3f}, {:.3f}, {:.3f}]}})",
                                s ? ", " : "", !ft.swing, ft.swing ? ft.s : 0.f, ft.pitch, ft.yaw * 180.f / PI, ft.swing ? 0.f : ft.strain, ft.swing ? 0.f : ft.tight, kneeBend(s), ft.at.x, ft.at.y, ft.at.z, ft.ankle.x, ft.ankle.y, ft.ankle.z);
        }
        // in the air; angles in degrees
        std::string air = "null";
        if (!g.grounded) {
            constexpr float D = 180.f / PI;
            air = std::format(R"({{"lead": {}, "run": {:.2f}, "fly": {:.2f}, "pitch": {:.1f}, "roll": {:.1f}, "legs": [[{:.0f}, {:.0f}], [{:.0f}, {:.0f}]], "arms": [[{:.0f}, {:.0f}], [{:.0f}, {:.0f}]]}})",
                              g.airLead, g.airRun, g.flyE.x, g.airPitch.x * D, g.airRoll.x * D, g.airLeg[0].hip.x * D, g.airLeg[0].knee.x * D, g.airLeg[1].hip.x * D,
                              g.airLeg[1].knee.x * D, g.airArm[0].ahead.x * D, g.airArm[0].out.x * D, g.airArm[1].ahead.x * D, g.airArm[1].out.x * D);
        }
        // carried body position and velocity, x z
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
        m_matOut.assign(md.expressions.size(), -1.f); // -1: forces a first update
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
        // body part per node the attack clips animate
        m_attackPart.assign(md.nodes.size(), ATTACK_NONE);
        for (int b = 0; b < HB_COUNT; ++b)
            if (const int n = md.human[b]; n >= 0)
                for (const auto* set : {&md.attacks, &md.attacksFirst})
                    for (const auto& at : *set)
                        for (const auto& c : at.anim.channels)
                            if (c.node == n && c.path == PATH_R && (attackArmBone(b) || attackTrunkBone(b)))
                                m_attackPart[n] = attackArmBone(b) ? ATTACK_ARM : ATTACK_TRUNK;
        m_attackPose = m_attackWas = m_pose;

        // spring-moved nodes: joint index, -2 below a joint, -1 none
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
        // nodes the springs read, and their ancestors, for springPoseAt()
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

    // `d`: model-space rotation, applied relative to the parent's rest orientation
    void CAvatarAnimator::rotateBone(std::vector<STRS>& pose, int bone, const Quat& d) const {
        const int n = m_model->human[bone];
        if (n < 0)
            return;
        const int  p  = m_model->nodes[n].parent;
        const Quat gp = p >= 0 ? m_restGlobalRot[p] : Quat{};
        pose[n].r     = (gp.conj() * d * gp * pose[n].r).normalized();
    }

    namespace {
        // VRChat gesture hand poses
        struct SHandPreset {
            std::array<float, FINGER_COUNT>     curl;   // thumb..little: 1 = a fist
            std::array<float, FINGER_COUNT - 1> spread; // index..little, toward the thumb
            float                               fold;   // the thumb's first bone, across the palm
            std::array<float, 3>                aim;    // other thumb bones' aim (along, across, palm); 0 curls
            int                                 hold;   // finger under the thumb, -1 none
        };
        constexpr SHandPreset HAND_POSES[GESTURE_COUNT] = {
            {{0.25f, 0.3f, 0.36f, 0.42f, 0.48f}, {0.06f, 0, -0.06f, -0.12f}, 0.2f, {}, -1}, // neutral: relaxed
            {{0, 1, 1, 1, 1}, {0, 0, 0, 0}, 0, {}, FINGER_INDEX},                           // fist
            {{0, 0, 0, 0, 0}, {0.5f, 0.1f, -0.3f, -0.6f}, -0.2f, {0.65f, 0.75f, -0.05f}, -1}, // open
            {{0, 0, 1, 1, 1}, {0, 0, 0, 0}, 0, {}, FINGER_MIDDLE},                          // point
            {{0, 0, 0, 1, 1}, {0.6f, -0.3f, 0, 0}, 0, {}, FINGER_RING},                     // victory
            {{0, 0, 1, 1, 0}, {0.2f, 0, 0, -0.3f}, 0, {}, FINGER_MIDDLE},                   // rock'n'roll
            {{0, 0, 1, 1, 1}, {0, 0, 0, 0}, -0.2f, {0.4f, 1, -0.1f}, -1},                   // handgun
            {{0, 1, 1, 1, 1}, {0, 0, 0, 0}, -0.2f, {0.3f, 1, -0.15f}, -1},                  // thumbs up
        };
        constexpr float DEG = 0.0174532925f;
        // turn of each bone at 1 (thumb: first by fold, others by curl)
        constexpr float SEG[3] = {80 * DEG, 95 * DEG, 60 * DEG}, THUMB[3] = {45 * DEG, 35 * DEG, 60 * DEG}, SPREAD = 12 * DEG;
    }

    // finger bend axes from the rest hand, then each gesture's thumb pose
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

            // thumb: preset turn, or laid over the fingers it holds
            auto whole = [&](int f) { return bone(f, 0) >= 0 && bone(f, 1) >= 0 && bone(f, 2) >= 0; };
            auto joint = [&](int f, int s) { // rest root, two joints, tip
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
                    // first thumb joint: L0 from the root, L1 from the target, toward the palm
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
                for (int s = 0; s < 3; ++s) { // each relative to its parent
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

    // fingers to the gesture, eased; neutral keeps the animation's own
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
            // the avatar's own gesture pose, eased in over the preset
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

    // head toward the camera look less yawOff (the trunk's turn); weight < 1 under an emote
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
        // first-person hand poses in the camera frame (x right, y up, -z ahead), arm lengths from the eye
        struct SFpPose {
            V3 at[2], along[2], palm[2];
        };
        // ready: up in front, low, palms in
        constexpr SFpPose FP_READY{{{-0.37f, -0.41f, -0.79f}, {0.36f, -0.40f, -0.80f}}, {{0.3f, 0.25f, -1.f}, {-0.3f, 0.25f, -1.f}}, {{0.85f, -0.45f, 0.f}, {-0.85f, -0.45f, 0.f}}};
        // typing: lower and nearer together, the palms down
        constexpr SFpPose FP_TYPE{{{-0.21f, -0.37f, -0.72f}, {0.21f, -0.37f, -0.72f}}, {{0.25f, -0.05f, -1.f}, {-0.25f, -0.05f, -1.f}}, {{0.15f, -1.f, 0.1f}, {-0.15f, -1.f, 0.1f}}};
        // holding: either side of the crosshair, palms in, fingers up
        constexpr SFpPose FP_HOLD{{{-0.33f, -0.22f, -0.90f}, {0.33f, -0.22f, -0.90f}}, {{0.15f, 0.85f, -0.5f}, {-0.15f, 0.85f, -0.5f}}, {{0.15f, 0.f, -1.f}, {-0.15f, 0.f, -1.f}}};
        // where the elbows go: down, out and back
        constexpr V3 FP_ELBOW[2] = {{-0.7f, -1.f, 0.4f}, {0.7f, -1.f, 0.4f}};
        // a gesturing hand shows side on, raised and in, or the forearm would hide it; right hand, left mirrored
        struct SFpShow {
            V3 along, palm;
        };
        constexpr V3      FP_SHOW_AT = {0.30f, -0.27f, -0.80f};
        constexpr SFpShow FP_SHOW[GESTURE_COUNT] = {
            {},
            {{-1.f, 0.15f, -0.35f}, {0.1f, 0.f, 1.f}},  // fist: knuckles across, thumb up
            {{0.f, 1.f, -0.3f}, {0.f, 0.f, -1.f}},      // open: up, palm out
            {{-0.7f, 0.2f, -0.7f}, {-0.2f, -1.f, 0.2f}}, // point: ahead and in
            {{0.f, 1.f, -0.3f}, {0.f, 0.f, -1.f}},      // victory: up, palm out
            {{0.f, 1.f, -0.3f}, {0.f, 0.f, -1.f}},      // rock'n'roll: the same
            {{-0.7f, 0.15f, -0.7f}, {-0.7f, 0.f, 0.7f}}, // handgun: ahead and in, thumb up
            {{-1.f, 0.15f, -0.35f}, {0.1f, 0.f, 1.f}},  // thumbs up: as a fist
        };
        // touching: the right fingertip just below-right of the crosshair, as far ahead as there's room
        constexpr V3    FP_TOUCH_AT = {0.035f, -0.06f, 0.f};
        constexpr float FP_TOUCH_REACH = 0.9f;
        // the hands' frame pitches less than the camera; looking far down, ready hands drop
        constexpr float FP_PITCH_DOWN = 0.65f, FP_PITCH_UP = 0.45f, FP_DOWN_FROM = 0.75f, FP_DOWN_ALL = 1.2f;
        // hand spring rates, rad/s; touching quicker
        constexpr float FP_W = 13.f, FP_TOUCH_W = 24.f;
        // hand lag behind camera turns and moves, capped, springing back
        constexpr float FP_TURN_LAG = 0.022f, FP_MOVE_LAG = 0.014f, FP_LAG_MOST = 0.12f, FP_LAG_W = 11.f, FP_LAG_Z = 0.65f;
        // walking bob and swing, running pump (arm lengths)
        constexpr float FP_BOB_X = 0.035f, FP_BOB_Y = 0.02f, FP_SWING = 0.06f, FP_PUMP_AHEAD = 0.12f, FP_PUMP_BACK = 0.3f, FP_PUMP_UP = 0.1f;
        // trunk turn toward the look, and bend looking down
        constexpr float FP_TWIST = 0.75f, FP_BEND_FROM = 0.35f, FP_BEND = 0.3f;

        // two-bone IK toward `to`, bending toward `hint`: both bones' directions
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

        // one SSpring::to step
        void springStep(float& x, float& v, float to, float w, float z, float dt) {
            SSpring s{x, v};
            s.to(to, w, z, dt);
            x = s.x, v = s.v;
        }
    }

    V3 SFirstPersonEye::update(const V3& feet, const V3& eyes, float yaw, float dt) {
        // dead zone the camera ignores, m
        constexpr float FP_EYE_STILL = 0.03f;
        const V3        want = yawTurn(yaw).conj().rotate(eyes - feet);
        if (!live || length(want - held) > 1.5f) // or teleported
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
        // ready, looking far down: the walking's arms
        const float down = m.fp.hands == FPH_READY ? smoothstep01((-m.fp.pitch - FP_DOWN_FROM) / (FP_DOWN_ALL - FP_DOWN_FROM)) : 0.f;
        m_fpArmsW        = approach(m_fpArmsW, can && m.fp.hands != FPH_DOWN ? (1.f - emoteW) * (1.f - down) : 0.f, dt / 0.25f);
        m_fpGesture      = {-1, -1};
        if (can && m.fp.hands == FPH_TOUCH)
            m_fpGesture[1] = GESTURE_POINT;
        else if (can && m.fp.hands == FPH_HOLD)
            m_fpGesture = {GESTURE_OPEN, GESTURE_OPEN};
        if (m_fpArmsW <= 0.f)
            m_fpLive = false;
        // view the hands and trunk follow: first person's, else the last one carried with the body
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
        // looking down, bend over to see the feet
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

        // hands' frame: the camera's, pitched less unless touching or holding
        const V3    eye = m_fpViewEye;
        const float yaw = m_fpViewYaw, pitch = m_fpViewPitch, hp = pitch * (pitch < 0 ? FP_PITCH_DOWN : FP_PITCH_UP);
        const V3    right = rightOf(yaw);
        auto        fwdAt = [&](float p) { return V3{std::sin(yaw) * std::cos(p), std::sin(p), -std::cos(yaw) * std::cos(p)}; };
        auto        world = [&](const V3& c, float p) { const V3 f = fwdAt(p); return right * c.x + cross(right, f) * c.y - f * c.z; };
        auto        frame = [&](const V3& w) { const V3 f = fwdAt(hp); return V3{dot(w, right), dot(w, cross(right, f)), -dot(w, f)}; };

        // lag behind camera turns and moves
        {
            V3 lag{};
            if (!fresh && dt > 1e-4f) {
                const float yr = std::clamp(wrapPi(yaw - m_fpYaw) / dt, -6.f, 6.f), pr = std::clamp((pitch - m_fpPitch) / dt, -6.f, 6.f);
                const V3    v  = frame((eye - m_fpEye) * (1.f / dt));
                lag            = V3{-FP_TURN_LAG * yr, -FP_TURN_LAG * pr, 0} - v * (FP_MOVE_LAG * (length(v) < 20.f ? 1.f : 0.f)); // not a teleport
                for (float* c : {&lag.x, &lag.y, &lag.z})
                    *c = std::clamp(*c, -FP_LAG_MOST, FP_LAG_MOST);
            }
            if (fresh)
                m_fpLag = m_fpLagV = {};
            for (int k = 0; k < 3; ++k)
                springStep((&m_fpLag.x)[k], (&m_fpLagV.x)[k], (&lag.x)[k], FP_LAG_W, FP_LAG_Z, dt);
        }
        m_fpYaw = yaw, m_fpPitch = pitch, m_fpEye = eye;

        // step phase: the gait's, else from the speed
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
                        // fingertip to the crosshair as room and reach allow; pokes on press
                        const float reach = std::clamp((m.fp.room - 0.03f) / A, 0.35f, FP_TOUCH_REACH) + 0.06f * bump(m_fpPoke, 0.07f, 0.16f);
                        // from the right: from behind, the forearm would hide the hand
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
            // walk bob and swing; run pump
            const float fs = -std::cos(TAU * (phase + 0.5f * s));
            if (m.fp.hands != FPH_TOUCH || s == 0) {
                const float walk = mw * (1.f - run);
                at += V3{FP_BOB_X * std::sin(TAU * phase), -FP_BOB_Y * 0.5f * (1.f - std::cos(2.f * TAU * phase)), -FP_SWING * fs} * walk;
                at += V3{-sx * 0.06f * std::max(fs, 0.f), FP_PUMP_UP * fs - 0.05f, fs > 0 ? -FP_PUMP_AHEAD * fs : -FP_PUMP_BACK * fs} * (mw * run);
            }
            // near a wall: hands no further ahead than the room, and lower
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

        // arms reach them, elbows down and out
        globals(pose, m_global);
        const M4    toModel = (M4::translation({0, m_lift, 0}) * m.world * md.fix).inverse();
        const float arms    = smoothstep01(m_fpArmsW);
        for (int s = 0; s < 2; ++s) {
            const SFpHand& hd = m_fpHand[s];
            reachArm(pose, s, toModel.point(eye + world((hd.at + m_fpLag) * A, hd.pitch)), toModel.dir(world(FP_ELBOW[s], hd.pitch)), toModel.dir(world(hd.along, hd.pitch)),
                     toModel.dir(world(hd.palm, hd.pitch)), arms);
        }
    }

    // needs m_global (globals() first)
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
        // attack blend-in, swing crossfade, and the pause after which the right hand leads again
        constexpr float ATTACK_IN = 0.1f, ATTACK_CROSS = 0.1f, ATTACK_PAUSE = 0.9f;
        // share of the head's look an attack overrides
        constexpr float ATTACK_LOOK = 0.75f;
        // first-person swing pitch limits, radians
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
        stopEmote();
        if (m_swingNext >= 0) // already queued
            return true;
        if (hand < 0)
            hand = m_swingAgo > ATTACK_PAUSE ? 1 : 1 - m_swingLast;
        hand &= 1;
        if (m_swing.t >= 0 && m_swing.t < attackOf(m_swing).next)
            m_swingNext = hand; // after this one strikes
        else
            swingStart(hand, m_fpW > 0.5f);
        return true;
    }

    void CAvatarAnimator::swingStart(int hand, bool fp) {
        // after a swing still up, start at `ready` and crossfade; else from the start
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
        // chest yaw from the hips
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
        // weight: 1 until `out`, then to 0 by the end, rate-limited (ATTACK_IN)
        const float want = m_swing.t >= dur ? 0.f : m_swing.t <= a.out ? 1.f : 1.f - (m_swing.t - a.out) / (dur - a.out);
        m_attackW        = want > m_attackW ? std::min(want, m_attackW + dt / ATTACK_IN) : std::max(want, m_attackW - dt / ATTACK_IN);
        if (m_swing.t >= dur && m_attackW <= 0.f) {
            m_swing.t = m_swingWas.t = -1;
            m_swingNext              = -1;
            return;
        }
        m_swingFist[0] = m_swingFist[1] = m_swing.t < a.out + 0.05f;
        // pose, crossfaded from the previous swing
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
                // clip turn from rest, in the bone's frame
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
        // first person: wrists from the clip's reach in the view (arm lengths from the eyes), pitch clamped
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
        const std::vector<STRS> under = pose; // first-person arms
        swingArms(w);
        // fist no further ahead than the room: bisect the swing weight toward the first-person arms
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
        // bend the way it's bent
        const V3   sw = length(W - S) > 1e-6f ? normalize(W - S) : normalize(E - S);
        const auto [u, f] = twoBones(S, wrist, l1, l2, E - S - sw * dot(E - S, sw));
        const Quat turnU = arc(normalize(E - S), u), turnF = arc(turnU.rotate(normalize(W - E)), f);
        const Quat want[3] = {(turnU * rotationOf(gu)).normalized(), (turnF * turnU * rotationOf(gl)).normalized(), rotationOf(gh)};
        // each relative to its parent as posed so far
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

    // face(): expressions, blinks and eye look into morphs and materials, combined as VRM 1.0 (three-vrm)
    // putPose: applies what a node pose sets, f of the way
    static void putPose(STRS& d, const SNodePose& p, float f) {
        if (p.set & SNodePose::T)
            d.t = lerp(d.t, p.trs.t, f);
        if (p.set & SNodePose::R)
            d.r = f >= 1 ? p.trs.r : slerp(d.r, p.trs.r, f).normalized();
        if (p.set & SNodePose::S)
            d.s = lerp(d.s, p.trs.s, f);
    }

    // loop weight a->b->a every `seconds`, eased (VRCFury keys have flat tangents)
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

        // held expression, else the hands' gesture combo, else the last gesturing hand's face
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
        // emote faces under the player's (blinks excepted)
        if (m_emote >= 0) {
            float mine = 0;
            for (size_t e = 0; e < n; ++e)
                mine = std::max(mine, m_exprW[e]);
            for (size_t e = 0; e < n; ++e) {
                const int p = md.expressions[e].preset;
                out[e]      = std::max(out[e], m_emoteFace[e] * (p == EX_BLINK || p == EX_BLINK_L || p == EX_BLINK_R ? 1.f : 1.f - mine));
            }
        }

        // VRM overrides: how much they block blink, look and mouth
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

        // eyes: the look the head doesn't take, plus saccades
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

        // lip sync: consonant visemes replace as much of the vowels
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
        for (const int t : m_loops) { // toggle loops: shape keys a->b->a
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

        // materials: rebuilt when expression weights change
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
        // slider key at v: the last at or below it (else the first)
        size_t sliderKey(const SAvatarSlider& s, float v) {
            size_t k = 0;
            for (size_t i = 1; i < s.keys.size(); ++i)
                if (s.keys[i].at <= v)
                    k = i;
            return k;
        }

        // 2D slider: corner keys of the grid cell around (x, y), and the position in it
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

        // key whose parts and variants apply: 1D the last at or below, 2D the nearest
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

    // dropped nodes (VRCFury World Drop) stay where toggled on; MA World Fixed Objects at their first rest pose
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
        // parts a toggle or slider can show stay hidden unless shown; hiding wins
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
        for (size_t s = 0; s < md.sliders.size(); ++s) { // 1D: between neighbouring keys
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

        // node poses: rest, then active toggles and sliders
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
        // active loops and world drops
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

        // material variants: the last active variant mapping a batch wins
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
            m_springLive = false; // restart from the animated pose
        m_physics = on;
    }

    M4 CAvatarAnimator::globalOf(const std::vector<STRS>& pose, int node) const {
        M4 g = M4::identity();
        for (int n = node; n >= 0; n = m_model->nodes[n].parent)
            g = pose[n].matrix() * g;
        return g;
    }

    // VRM 1.0 node constraints
    void CAvatarAnimator::constrain(std::vector<STRS>& pose) const {
        const auto& md = *m_model;
        for (const auto& c : md.constraints) {
            const Quat dstRest = md.nodes[c.node].rest.r, srcRest = md.nodes[c.source].rest.r;
            Quat       target;
            switch (c.type) {
                case SNodeConstraint::ROTATION: target = dstRest * (srcRest.conj() * pose[c.source].r); break;
                case SNodeConstraint::ROLL: {
                    // source turn from rest in the node's rest space, around the axis only
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

    // springs step at 60 Hz, interpolating the avatar's move and pose; the shown pose is a partial step from now
    void CAvatarAnimator::springs(const SAvatarMotion& m) {
        const auto& md = *m_model;
        if (md.springJoints.empty())
            return;
        if (!m_physics) {
            m_springLive = false;
            return;
        }
        constexpr float STEP      = SPRING_STEP;
        constexpr int   MAX_STEPS = 8; // below 7.5 fps springs run slow
        const STRS      body      = decompose(m.world * M4::translation({0, m_lift, 0}));
        if (!m_springLive || m.dt > 0.25f || length(body.t - m_springBody.t) > 2.f + 20.f * m.dt || m_springWas.size() != m_pose.size()) {
            // the first frame, or it jumped
            m_springStepAt = body.matrix();
            springPass(SP_START, m_springStepAt * md.fix, 0, m_global);
            m_springAcc  = 0;
            m_springLive = true;
        } else if (m.dt > 0) {
            const float from  = m_springAcc;
            int         steps = (int)((from + m.dt) / STEP + 1e-3f); // a 1/60 s frame is one full step
            m_springAcc       = std::max(0.f, from + m.dt - steps * STEP);
            const bool slow   = steps > MAX_STEPS;
            if (slow) {
                steps       = MAX_STEPS;
                m_springAcc = 0;
            }
            for (int s = 1; s <= steps; ++s) {
                // avatar transform at this step
                const float u = slow ? (float)s / steps : std::clamp((s * STEP - from) / m.dt, 0.f, 1.f);
                const STRS  at{lerp(m_springBody.t, body.t, u), slerp(m_springBody.r, body.r, u), lerp(m_springBody.s, body.s, u)};
                const M4    now = at.matrix();
                m_springMove    = now * m_springStepAt.inverse(); // may be frames back
                m_springStepAt  = now;
                springPass(SP_STEP, now * md.fix, STEP, u < 1.f ? springPoseAt(u) : m_global);
            }
        }
        const M4 world = body.matrix() * md.fix;
        m_springMove   = body.matrix() * m_springStepAt.inverse(); // since the last step
        springPass(SP_SHOW, world, m_springAcc / STEP, m_global);
        m_springBody = body;
        m_springWas  = m_pose;
        const M4 inv = world.inverse();
        for (size_t n = m_springFrom; n < md.nodes.size(); ++n)
            if (m_springOf[n] != -1)
                m_global[n] = inv * m_springGlobal[n];
    }

    // m_global u of the way from last frame's pose, for the nodes the springs read
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
                m_carrierMove[s] = pass == SP_START ? M4::identity() : at * m_carrierAt[s].inverse(); // since the last step
                if (pass != SP_SHOW)
                    m_carrierAt[s] = at;
            }
        for (size_t c = 0; c < md.springColliders.size(); ++c) {
            const auto& k   = md.springColliders[c];
            m_colliderAt[c] = {world.point(pose[k.node].point(k.offset)), world.point(pose[k.node].point(k.tail)), k.radius, k.kind, k.disc};
        }
        // SP_SHOW: a fraction f of a step, not kept
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
                // VRMC_springBone_limit, after the swing and each collider push
                const Quat L       = J.limit != LIMIT_NONE ? (rotationOf(G) * J.limitFrame).normalized() : Quat{};
                auto       limited = [&](const V3& p) { return springLimit(J, L, O, p); };
                // back to its length, within the limit, out of colliders
                auto pushed = [&](V3 next) {
                    next = limited(O + normalize(next - O) * len);
                    for (int k : sp.colliders) {
                        if (md.springColliders[k].body & J.startsIn)
                            continue; // starts inside: the limit keeps it out
                        const auto& c  = m_colliderAt[k];
                        const V3    ab = c.b - c.a;
                        if (c.kind == COLLIDER_PLANE) {
                            // onto its positive side, by the bone radius
                            const V3    n = normalize(ab);
                            const float h = dot(next - c.a, n);
                            if (h < J.radius)
                                next = limited(O + normalize(next + n * (J.radius - h) - O) * len);
                            continue;
                        }
                        if (c.kind == COLLIDER_DISC) {
                            // out of the disc's radius: off its face or round its edge
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
                // pushes can move it again (overlapping colliders, the limit): `again` is taken off the shown pose
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
                // PhysBone Immobile (All Motion): moves with its carrier; the swing excludes the last step's carry
                const V3 carried = sp.center < 0 && sp.carrier >= 0 ?
                    byBody * std::max(sp.immobile, sp.parentImmobile) + (m_carrierMove[J.spring].point(cur) - cur - byBody) * sp.parentImmobile :
                    V3{};
                V3 next = cur + carried + (cur - prev - m_carried[j]) * ((1 - J.drag) * f) + normalize(rest) * (J.stiffness * dt * J.scale) +
                    J.gravityDir * (J.gravity * dt * J.scale);
                if (sp.center < 0 && sp.carrier < 0)
                    // drag against air moving partly with the avatar: steady motion swings it less
                    next += byBody * (J.drag * sp.immobile);
                next = pushed(next);
                if (pass == SP_STEP) {
                    m_carried[j]  = carried;
                    m_tailPrev[j] = m_tail[j];
                    m_tail[j]     = m_centerInv[J.spring].point(next);
                    m_again[j]    = again(next);
                } else
                    // shown: less the re-push (`again`), fading over the step
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
        // where the pose changed at once, the shown pose keeps its motion and settles in (m_settleRate); carry(k) does
        // the same for the root at takeoff and landing
        int  liftHips = -1;
        V3   liftV;
        auto carry = [&](float k) {
            const int hips = md.human[HB_HIPS];
            const V3 root = origin(m.world);
            // not after a teleport
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
                m_settleRV[i] = length(rv) > 30.f ? rv * (30.f / length(rv)) : rv; // ignore a snap just before
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
            // the frame after: take off the new pose's own velocity
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
        // first person: trunk to the camera, head the rest; an attack turns on top
        const float twist = firstPersonTrunk(m, m_pose);
        attackTrunk(m_pose);
        look(m, m_pose, (1.f - emoteW) * (1.f - ATTACK_LOOK * smoothstep01(m_attackW)), twist);
        face(m, m_pose);
        firstPersonArms(m, m_pose);
        attackArms(m, m_pose);
        constrain(m_pose);
        globals(m_pose, m_global);

        // grounded emotes keep the feet down; clips and walking place them themselves
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
        // made for this model: indices in range
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
        // replacing an emote: blend from its pose
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

    // emote pose faded over the pose, plus faces; time from the frames or its sound (setEmoteClock())
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
