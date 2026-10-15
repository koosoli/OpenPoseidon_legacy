// Offline pose check for Arma men (ODOL 73 + binarised moves), built from the repo's own
// readers: poses LOD 0 of a man model with a move (middle phase) and reports the mean
// stretch of the mesh's edges -- near 0 when the pose is read right, metres when not.
//
//   clang++ -std=c++20 -O2 -w -Iengine -Iengine/Poseidon -I<spdlog>/include -I<fmt>/include \
//     -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY -include cstring -include cstdint \
//     scripts/arma3-pc/offline/posescore.cpp engine/Poseidon/Asset/Formats/BISBinaryStream.cpp \
//     engine/Poseidon/IO/Streams/SsCompress.cpp -o posescore
//   posescore b_soldier_01.p3d AmovPercMwlkSlowWpstDf.rtm ...
//
// The model and the moves are extracted from the PBOs first (any PBO tool); nothing of
// Bohemia's data is in the repository. The environment switches of ConvertBMTRToRTM
// (BMTR_FLAT, BMTR_QUAT_CONJUGATE, BMTR_V5_HIERARCHICAL, ...) apply.
//
// Measured 2026-10-15 on b_soldier_01 (bounding centre 0.305, 0.836, -0.401) with a pistol
// walk, a kneeling walk, prone-to-stand and a prone idle: "stored" 4.3-4.9, "source"
// 0.08-0.12 -- the engine had posed Arma 3 men around the stored centre.
#include <Poseidon/Asset/Formats/P3D/Odol73.hpp>
#include <Poseidon/Asset/Formats/RTM/RTMReader.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

using namespace Poseidon::Asset::Formats::P3D;
namespace F = Poseidon::Asset::Formats;

namespace
{
std::string Low(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::vector<char> LoadBytes(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
struct V3
{
    float x, y, z;
};
V3 Apply(const std::array<float, 12>& m, V3 v)
{
    return {m[0] * v.x + m[3] * v.y + m[6] * v.z + m[9], m[1] * v.x + m[4] * v.y + m[7] * v.z + m[10],
            m[2] * v.x + m[5] * v.y + m[8] * v.z + m[11]};
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: posescore man.p3d move.rtm...\n");
        return 1;
    }
    const auto data = LoadBytes(argv[1]);
    Poseidon::QIStream in(data.data(), data.size());
    F::BinaryReader reader(in);
    const Odol73StaticModel model = ReadOdol73StaticModel(reader, static_cast<int>(data.size()));
    const auto& bones = model.directory.bones;
    const auto& lod = model.lods[0];
    const auto& pos = lod.rest.positions;
    const auto& refs = lod.rest.vertexBoneRefs;
    const auto& remap = lod.header.subSkeletonsToSkeleton;
    const auto bc = model.directory.storedBoundingCenter;
    std::printf("%zu bones, %zu vertices, bounding centre %.3f %.3f %.3f\n", bones.size(), pos.size(), bc.x, bc.y,
                bc.z);

    // skin: (bone, weight) per vertex; index-first pairs, sub-skeleton remapped (as ODOLLoader)
    const uint32_t limit = remap.empty() ? static_cast<uint32_t>(bones.size()) : static_cast<uint32_t>(remap.size());
    std::vector<std::vector<std::pair<int, float>>> skin(pos.size());
    for (size_t v = 0; v < pos.size() && v < refs.size(); v++)
    {
        float sum = 0;
        for (int p = 0; p < std::min(refs[v].count, 4); p++)
        {
            const uint32_t i = refs[v].data[static_cast<size_t>(p) * 2];
            const uint8_t w = refs[v].data[static_cast<size_t>(p) * 2 + 1];
            if (i >= limit || !w)
                continue;
            const int b = remap.empty() ? static_cast<int>(i) : remap[i];
            if (b < 0 || b >= static_cast<int>(bones.size()))
                continue;
            skin[v].push_back({b, static_cast<float>(w)});
            sum += w;
        }
        for (auto& s : skin[v])
            s.second /= sum > 0 ? sum : 1;
    }
    // the skeleton's parents, as the ODOL loader registers them for the move conversion
    for (const auto& b : bones)
        F::BmtrSkeletonParents().emplace(Low(b.name), Low(b.parent));
    std::set<std::pair<uint32_t, uint32_t>> edges;
    for (const auto& f : lod.polygons.faces)
        for (size_t k = 0; k < f.size(); k++)
        {
            uint32_t a = f[k], b = f[(k + 1) % f.size()];
            if (a > b)
                std::swap(a, b);
            if (b < pos.size())
                edges.insert({a, b});
        }

    for (int ai = 2; ai < argc; ai++)
    {
        const auto move = LoadBytes(argv[ai]);
        std::string rtm;
        if (!F::ConvertBMTRToRTM(reinterpret_cast<const uint8_t*>(move.data()), move.size(), rtm))
        {
            std::printf("%s: not converted\n", argv[ai]);
            continue;
        }
        int32_t phases, count;
        std::memcpy(&phases, rtm.data() + 20, 4);
        std::memcpy(&count, rtm.data() + 24, 4);
        size_t at = 28 + static_cast<size_t>(count) * 32 + static_cast<size_t>(phases / 2) * (4 + count * 80) + 4;
        std::map<std::string, std::array<float, 12>> byName;
        for (int b = 0; b < count; b++)
        {
            char name[33] = {};
            std::memcpy(name, rtm.data() + at, 32);
            std::array<float, 12> m;
            std::memcpy(m.data(), rtm.data() + at + 32, 48);
            byName[Low(name)] = m;
            at += 80;
        }
        std::vector<std::array<float, 12>> mats(bones.size(), {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0});
        for (size_t b = 0; b < bones.size(); b++)
            if (const auto it = byName.find(Low(bones[b].name)); it != byName.end())
                mats[b] = it->second;
        std::printf("%s (%d phases)\n", argv[ai], phases);
        for (int around = 0; around < 2; around++)
        {
            std::vector<V3> src(pos.size()), out(pos.size());
            for (size_t v = 0; v < pos.size(); v++)
            {
                src[v] = {pos[v].x, pos[v].y, pos[v].z};
                if (around)
                    src[v] = {src[v].x + bc.x, src[v].y + bc.y, src[v].z + bc.z};
                V3 acc{0, 0, 0};
                for (const auto& [b, w] : skin[v])
                {
                    const V3 q = Apply(mats[static_cast<size_t>(b)], src[v]);
                    acc = {acc.x + q.x * w, acc.y + q.y * w, acc.z + q.z * w};
                }
                out[v] = skin[v].empty() ? src[v] : acc;
            }
            std::vector<double> stretch;
            for (const auto& [a, b] : edges)
            {
                if (skin[a].empty() || skin[b].empty())
                    continue;
                const auto len = [](V3 p, V3 q) {
                    return std::sqrt((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y) + (p.z - q.z) * (p.z - q.z));
                };
                const double l0 = len(src[a], src[b]);
                if (l0 > 1e-4)
                    stretch.push_back(std::fabs(len(out[a], out[b]) - l0) / l0);
            }
            double mean = 0;
            for (double s : stretch)
                mean += s;
            std::sort(stretch.begin(), stretch.end());
            std::printf("  posed around the %s centre: mean edge stretch %.3f, median %.3f\n",
                        around ? "source (bounding)" : "stored", stretch.empty() ? 0 : mean / stretch.size(),
                        stretch.empty() ? 0 : stretch[stretch.size() / 2]);
        }
    }
}
