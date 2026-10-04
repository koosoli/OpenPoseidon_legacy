#pragma once

#include <array>
#include <cmath>
#include <string_view>

namespace Poseidon
{
// Original retail CWA Data3D/BISCamel/O/O_WP character census. A filename is
// necessary, not sufficient: the live model's rig, palettes, points, proxies,
// anatomical hulls and support are admitted separately. No addon prefix rule.
inline constexpr std::array<std::string_view, 38> StockCorpseModels = {{
    "data3d\\mc civil.p3d", "data3d\\mc civil2.p3d", "data3d\\mc civil2stary.p3d",
    "data3d\\mc civil3.p3d", "data3d\\mc crewg.p3d", "data3d\\mc pilote2.p3d",
    "data3d\\mc pilotw2.p3d", "data3d\\mc saboteur.p3d", "data3d\\mc saboteurday.p3d",
    "data3d\\mc snipere2.p3d", "data3d\\mc sniperw2.p3d", "data3d\\mc specnas2.p3d",
    "data3d\\mc tankistae2.p3d", "data3d\\mc tankistaw2.p3d", "data3d\\mc vojake2.p3d",
    "data3d\\mc vojake2_guba.p3d", "data3d\\mc vojakeo2.p3d", "data3d\\mc vojakg2.p3d",
    "data3d\\mc vojakg3.p3d", "data3d\\mc vojakgo2.p3d", "data3d\\mc vojakw2.p3d",
    "data3d\\mc vojakwo2.p3d", "data3d\\angelina.p3d",
    "biscamel\\biscamelpilot.p3d", "biscamel\\biscamelpilot2.p3d",
    "o\\char\\char01.p3d", "o\\char\\char02.p3d", "o\\char\\char03.p3d",
    "o\\char\\char04.p3d", "o\\char\\char05.p3d", "o\\char\\char06.p3d",
    "o\\char\\char07.p3d", "o\\char\\civilistka01a.p3d", "o\\char\\civilistka01b.p3d",
    "o\\char\\civilistka02a.p3d", "o\\char\\civilistka02b.p3d", "o\\char\\civilistka02c.p3d",
    "o_wp\\mc_specg.p3d"
}};

inline constexpr std::array<std::string_view, 25> StockCorpseBodyBones = {{
    "pchodidlo", "lchodidlo", "pprsty", "lprsty", "lholen", "pholen", "pstehno", "lstehno",
    "pzadek", "lzadek", "bricho", "zebra", "hrudnik", "krk", "prameno", "lrameno", "hlava",
    "pbiceps", "lbiceps", "ploket", "lloket", "roura", "zbran", "pruka", "lruka"
}};
inline constexpr std::array<std::string_view, 8> StockCorpseFaceBones = {{
    "%face_eyelid", "%face_lip", "%face_lmouth", "%face_mmouth",
    "%face_rmouth", "%face_lbrow", "%face_mbrow", "%face_rbrow"
}};

inline bool StockCorpseModelNameEqual(std::string_view actual, std::string_view expected)
{
    if (actual.size() != expected.size()) return false;
    for (std::size_t i = 0; i < actual.size(); ++i)
    {
        char c = actual[i];
        if (c == '/') c = '\\';
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != expected[i]) return false;
    }
    return true;
}

inline bool StockCorpseModelKnown(std::string_view model)
{
    for (auto expected : StockCorpseModels)
        if (StockCorpseModelNameEqual(model, expected)) return true;
    return false;
}

// These source assets have all15 positive graphical shared boundaries. Other
// stock files still require the nine physical joint boundaries, but must not
// pretend that missing toe/ankle selections measured a knee hinge direction.
inline bool StockCorpseHasAuthoredHingeEndpoints(std::string_view model)
{
    for (std::size_t i = 0; i < 22; ++i)
        if (StockCorpseModelNameEqual(model, StockCorpseModels[i])) return true;
    return StockCorpseModelNameEqual(model, "o_wp\\mc_specg.p3d");
}

inline int StockCorpseAnatomicalPart(std::string_view bone)
{
    if (bone == "pzadek" || bone == "lzadek" || bone == "bricho") return 0;
    if (bone == "zebra" || bone == "hrudnik" || bone == "krk" || bone == "prameno" || bone == "lrameno") return 1;
    if (bone == "hlava") return 2;
    if (bone == "pbiceps") return 3;
    if (bone == "ploket" || bone == "pruka") return 4;
    if (bone == "lbiceps") return 5;
    if (bone == "lloket" || bone == "lruka") return 6;
    if (bone == "pstehno") return 7;
    if (bone == "pholen" || bone == "pchodidlo" || bone == "pprsty") return 8;
    if (bone == "lstehno") return 9;
    if (bone == "lholen" || bone == "lchodidlo" || bone == "lprsty") return 10;
    return -1;
}

// Equipment helper deltas are exactly the already-owned body part deltas.
// A normalized native proxy blend remains rigid if all influences share one
// solver part. Keep its captured AnimateProxyMatrix rather than rebuilding it.
inline int StockCorpseProxyPart(std::string_view bone)
{
    if (bone == "roura") return 1;
    if (bone == "zbran") return 4;
    return StockCorpseAnatomicalPart(bone);
}

template<class BoneAt, class WeightAt, class PartForBone>
int StockCorpseRigidProxyBone(int count, BoneAt boneAt, WeightAt weightAt, PartForBone partForBone)
{
    if (count <= 0 || count > 25) return -1;
    int representative = -1, part = -1;
    float total = 0;
    for (int w = 0; w < count; ++w)
    {
        const int bone = boneAt(w);
        const float weight = weightAt(w);
        if (bone < 0 || bone >= 25 || !std::isfinite(weight) || weight <= 0 || weight > 1) return -1;
        const int current = partForBone(bone);
        if (current < 0 || current >= 11 || (part >= 0 && current != part)) return -1;
        if (representative < 0) { representative = bone; part = current; }
        total += weight;
    }
    return std::isfinite(total) && std::abs(total-1) <= 1e-5f ? representative : -1;
}

// Names are matched as a set in each range. The RTM corpus contains different
// orderings; production resolves actual indices by name. The callback compares
// immediately so Skeleton::GetBone's temporary RString cannot leave a dangling
// string_view. Only the eight implemented head synthetic bones may follow25.
template<class NameEquals>
const char* StockCorpseSkeletonRefusal(int count, NameEquals nameEquals)
{
    if (count != 33) return "complete-stock-33-bone-palette-required";
    std::array<bool, 25> bodySeen{};
    std::array<bool, 8> faceSeen{};
    for (int bone = 0; bone < 33; ++bone)
    {
        bool found = false;
        if (bone < 25)
            for (std::size_t name = 0; name < StockCorpseBodyBones.size(); ++name)
                if (nameEquals(bone, StockCorpseBodyBones[name]))
                {
                    if (bodySeen[name]) return "duplicate-stock-body-bone";
                    bodySeen[name] = found = true; break;
                }
        if (bone >= 25)
            for (std::size_t name = 0; name < StockCorpseFaceBones.size(); ++name)
                if (nameEquals(bone, StockCorpseFaceBones[name]))
                {
                    if (faceSeen[name]) return "duplicate-stock-face-bone";
                    faceSeen[name] = found = true; break;
                }
        if (!found) return bone < 25 ? "unknown-stock-body-bone" : "unsupported-synthetic-bone";
    }
    return nullptr;
}
}
