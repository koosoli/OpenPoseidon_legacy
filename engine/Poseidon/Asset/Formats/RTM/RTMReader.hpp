#pragma once

#include <Poseidon/Foundation/Algorithms/Lzo1x.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace Poseidon::Asset::Formats
{

static constexpr int MAGIC_LEN     = 8;
static constexpr int BONE_NAME_LEN = 32;
static constexpr int MATRIX_FLOATS = 12; // 3x4 stored as 12 floats

enum class RTMVersion { Unknown, V100, V101 };

struct BoneTransform
{
    // [aside(3) up(3) dir(3) pos(3)] = Matrix3T + Vector3T
    float m[12];
    void  setIdentity() { std::memset(m, 0, sizeof(m)); m[0] = m[4] = m[8] = 1.0f; }
    float tx() const { return m[9]; }
    float ty() const { return m[10]; }
    float tz() const { return m[11]; }
};

struct RTMPhase
{
    float                    time = 0.0f;
    std::vector<BoneTransform> transforms; // one per bone
};

struct RTMAnimation
{
    RTMVersion               version = RTMVersion::Unknown;
    float                    stepX = 0.0f, stepY = 0.0f, stepZ = 0.0f;
    std::vector<std::string> boneNames;
    std::vector<RTMPhase>    phases;
    int                      boneCount() const { return static_cast<int>(boneNames.size()); }
    int                      phaseCount() const { return static_cast<int>(phases.size()); }
};

// Read header only (magic, step, bone names, phase/bone counts) — no phase data.
// Useful for lazy loading: get metadata without reading all keyframes.
template<typename Stream>
bool readRTMHeader(Stream& in, RTMAnimation& anim)
{
    anim = RTMAnimation{};
    char magic[MAGIC_LEN + 1] = {};
    in.read(magic, MAGIC_LEN);
    if (in.fail())
        return false;

    if (std::strcmp(magic, "RTM_0100") == 0)
    {
        anim.version = RTMVersion::V100;
        in.read((char*)&anim.stepZ, sizeof(float));
    }
    else if (std::strcmp(magic, "RTM_0101") == 0)
    {
        anim.version = RTMVersion::V101;
        in.read((char*)&anim.stepX, sizeof(float));
        in.read((char*)&anim.stepY, sizeof(float));
        in.read((char*)&anim.stepZ, sizeof(float));
    }
    else
        return false;

    int32_t nAnim = 0, nSel = 0;
    in.read((char*)&nAnim, sizeof(int32_t));
    in.read((char*)&nSel, sizeof(int32_t));
    if (in.fail() || nAnim < 0 || nSel < 0)
        return false;
    if (nAnim > 100000 || nSel > 1000)
        return false;

    anim.boneNames.resize(nSel);
    for (int i = 0; i < nSel; i++)
    {
        // One extra byte the read never touches guarantees a NUL terminator: the
        // wire name is exactly BONE_NAME_LEN bytes with no required terminator, so
        // the tolower scan and the std::string assignment below would otherwise run
        // off the buffer when all BONE_NAME_LEN bytes are non-zero.
        char name[BONE_NAME_LEN + 1] = {};
        in.read(name, BONE_NAME_LEN);
        if (in.fail())
            return false;
        for (char* p = name; *p; ++p)
            *p = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
        anim.boneNames[i] = name;
    }
    anim.phases.resize(nAnim);
    return true;
}

// Read phase data from stream (call after readRTMHeader, stream positioned at first phase).
template<typename Stream>
bool readRTMPhases(Stream& in, RTMAnimation& anim)
{
    int nSel  = anim.boneCount();
    int nAnim = anim.phaseCount();
    for (int p = 0; p < nAnim; p++)
    {
        auto& phase = anim.phases[p];
        in.read((char*)&phase.time, sizeof(float));
        if (in.fail())
            return false;
        phase.transforms.resize(nSel);
        for (int s = 0; s < nSel; s++)
        {
            char name[BONE_NAME_LEN]; // per-phase bone name (redundant in format, skip)
            in.read(name, BONE_NAME_LEN);
            in.read((char*)phase.transforms[s].m, sizeof(float) * MATRIX_FLOATS);
            if (in.fail())
                return false;
        }
    }
    return true;
}

template<typename Stream>
bool readRTM(Stream& in, RTMAnimation& anim)
{
    if (!readRTMHeader(in, anim))
        return false;
    return readRTMPhases(in, anim);
}

inline bool readRTMFromMemory(const uint8_t* data, size_t size, RTMAnimation& anim)
{
    std::string       buf(reinterpret_cast<const char*>(data), size);
    std::istringstream iss(std::move(buf), std::ios::binary);
    return readRTM(iss, anim);
}

inline bool readRTMFromFile(const char* path, RTMAnimation& anim)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.good())
        return false;
    return readRTM(file, anim);
}


// Binarised animation ("BMTR", Arma 2 / Operation Arrowhead), converted to the plain
// RTM_0101 layout the readers above take. Layout, as read from the Arma 2 data:
//
//   char[4] "BMTR"; u32 version (3); u8 flag; float step[3]; u32 phases; u32 (unused);
//   u32 bones; u32 bone names; asciiz name[bones];
//   u32 phases; float time[phases]          -- an "array" (see below)
//   per phase: u32 bones; Transform t[bones] -- an "array"
//   Transform = int16 quaternion x,y,z,w (1.0 = 16384) + half-float position x,y,z
//
// An array whose payload is 1024 bytes or more is LZO1X-compressed, else raw -- the
// same rule as the Arma 2 model format. The quaternion becomes the bone's 3x3
// orientation (columns aside, up, dir) and the position its translation, i.e. the
// 3x4 matrix a plain RTM stores. BMTR_QUAT_CONJUGATE=0 (environment) keeps the
// stored rotation unconjugated -- a switch for checking the handedness by eye.
namespace detail
{
inline float HalfToFloat(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1;
    const int exp = (h >> 10) & 0x1f;
    const uint32_t mant = h & 0x3ff;
    float v;
    if (exp == 0)
        v = std::ldexp(static_cast<float>(mant), -24);
    else if (exp == 31)
        v = mant ? NAN : INFINITY;
    else
        v = std::ldexp(static_cast<float>(mant | 0x400), exp - 25);
    return sign ? -v : v;
}

struct BmtrCursor
{
    const uint8_t* data;
    size_t size;
    size_t at = 0;
    bool ok = true;
    bool need(size_t n)
    {
        if (at + n > size)
            ok = false;
        return ok;
    }
    uint32_t u32()
    {
        if (!need(4))
            return 0;
        uint32_t v;
        std::memcpy(&v, data + at, 4);
        at += 4;
        return v;
    }
    float f32()
    {
        const uint32_t b = u32();
        float f;
        std::memcpy(&f, &b, 4);
        return f;
    }
    uint8_t u8()
    {
        if (!need(1))
            return 0;
        return data[at++];
    }
    std::string asciiz()
    {
        std::string s;
        while (ok && at < size && data[at])
            s.push_back(static_cast<char>(data[at++]));
        if (at < size)
            at++;
        else
            ok = false;
        return s;
    }
    bool array(size_t bytes, std::vector<uint8_t>& out)
    {
        out.resize(bytes);
        if (bytes == 0)
            return true;
        if (bytes < 1024)
        {
            if (!need(bytes))
                return false;
            std::memcpy(out.data(), data + at, bytes);
            at += bytes;
            return true;
        }
        size_t consumed = 0;
        const size_t got = Poseidon::Foundation::Lzo1x::Decompress(data + at, size - at, out.data(), out.size(), &consumed);
        if (got != bytes || consumed == 0)
        {
            ok = false;
            return false;
        }
        at += consumed;
        return true;
    }
};
} // namespace detail

// Bone -> parent (lower case) of the skinned skeletons seen so far. The ODOL loader fills it
// from an Arma 2 man model's skeleton (OFP2_ManSkeleton) before that model's moves are
// loaded; BMTR conversion needs it, because a binarised move stores each bone relative to
// its parent (see ConvertBMTRToRTM).
inline std::map<std::string, std::string>& BmtrSkeletonParents()
{
    static std::map<std::string, std::string> parents;
    return parents;
}

inline bool IsBMTR(const uint8_t* data, size_t size)
{
    return size >= 8 && std::memcmp(data, "BMTR", 4) == 0;
}

inline bool ConvertBMTRToRTM(const uint8_t* data, size_t size, std::string& rtm)
{
    if (!IsBMTR(data, size))
        return false;
    detail::BmtrCursor c{data, size};
    c.at = 4;
    const uint32_t version = c.u32();
    if (version < 3 || version > 5)
        return false;
    (void)c.u8();
    const float step[3] = {c.f32(), c.f32(), c.f32()};
    const uint32_t phases = c.u32();
    (void)c.u32();
    const uint32_t bones = c.u32();
    const uint32_t names = c.u32();
    if (!c.ok || phases > 100000 || bones > 1000 || names != bones)
        return false;
    std::vector<std::string> boneNames(bones);
    for (auto& n : boneNames)
        n = c.asciiz();
    const uint32_t timeCount = c.u32();
    if (!c.ok || timeCount != phases)
        return false;
    std::vector<uint8_t> raw;
    if (!c.array(static_cast<size_t>(phases) * 4, raw))
        return false;
    std::vector<float> times(phases);
    if (phases)
        std::memcpy(times.data(), raw.data(), phases * 4);

    // The stored rotation is the inverse of the matrix a plain RTM holds: checked on the
    // data, where translation = pivot - R * pivot only holds for the conjugate (the head
    // of AidlPercMstpSrasWlnrDnon_idleSteady01 matches its pivot to a millimetre).
    static const bool conjugate = [] {
        const char* e = std::getenv("BMTR_QUAT_CONJUGATE");
        return !(e && *e == '0');
    }();

    const auto put = [&rtm](const void* p, size_t n) { rtm.append(static_cast<const char*>(p), n); };
    rtm.clear();
    rtm.reserve(32 + bones * 32 + static_cast<size_t>(phases) * (4 + bones * 80));
    put("RTM_0101", 8);
    put(step, sizeof(step));
    const int32_t nAnim = static_cast<int32_t>(phases), nSel = static_cast<int32_t>(bones);
    put(&nAnim, 4);
    put(&nSel, 4);
    for (const std::string& n : boneNames)
    {
        char name[BONE_NAME_LEN] = {};
        std::memcpy(name, n.data(), std::min<size_t>(n.size(), BONE_NAME_LEN - 1));
        put(name, BONE_NAME_LEN);
    }
    // What a BMTR transform is, measured on Arma 2's own data (a USMC soldier, ODOL 48,
    // posed by AmovPercMstpSrasWrflDnon and AmovPercMwlkSrasWrflDf):
    //  - the quaternion is the conjugate of the RTM orientation (see above), and the
    //    data is mirrored in x against the model (S M S, S = diag(-1, 1, 1));
    //  - each bone is LOCAL: a rotation about its own rest joint (R p + t, so t = J - R J),
    //    applied before its parent's -- world = parent * local, down the skeleton;
    //  - the root (pelvis) holds its height above the ground instead (about 0.98 m
    //    standing), so its translation is that height less the rest height.
    // Composed this way every joint closes (mean edge stretch 0.17 against 2.5-5 for the
    // plain reading, which tore the soldier apart in game). Without a known skeleton
    // (BmtrSkeletonParents empty for these bones) the transforms stay as stored.
    const auto lower = [](std::string v)
    {
        for (char& ch : v)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return v;
    };
    const auto& parents = BmtrSkeletonParents();
    std::vector<int> parentIndex(bones, -1);
    bool hierarchical = false;
    {
        std::map<std::string, int> index;
        for (uint32_t b = 0; b < bones; b++)
            index[lower(boneNames[b])] = static_cast<int>(b);
        for (uint32_t b = 0; b < bones; b++)
        {
            const auto it = parents.find(lower(boneNames[b]));
            if (it == parents.end())
                continue;
            hierarchical = true;
            if (const auto p = index.find(it->second); p != index.end())
                parentIndex[b] = p->second;
        }
    }
    static const bool flat = [] {
        const char* e = std::getenv("BMTR_FLAT");
        return e && *e == '1';
    }();
    if (flat)
        hierarchical = false;
    constexpr float kRootRestHeight = 0.98f;

    for (uint32_t p = 0; p < phases; p++)
    {
        const uint32_t count = c.u32();
        if (!c.ok || count != bones || !c.array(static_cast<size_t>(count) * 14, raw))
            return false;
        put(&times[p], 4);
        std::vector<std::array<float, 12>> local(bones);
        for (uint32_t b = 0; b < bones; b++)
        {
            const uint8_t* t = raw.data() + static_cast<size_t>(b) * 14;
            int16_t q[4];
            uint16_t h[3];
            std::memcpy(q, t, 8);
            std::memcpy(h, t + 8, 6);
            float x = q[0] / 16384.0f, y = q[1] / 16384.0f, z = q[2] / 16384.0f, w = q[3] / 16384.0f;
            const float len = std::sqrt(x * x + y * y + z * z + w * w);
            if (len > 1e-6f)
            {
                x /= len;
                y /= len;
                z /= len;
                w /= len;
            }
            if (conjugate)
            {
                x = -x;
                y = -y;
                z = -z;
            }
            float* m = local[b].data();
            // columns of the rotation matrix: aside, up, dir
            m[0] = 1 - 2 * (y * y + z * z);
            m[1] = 2 * (x * y + w * z);
            m[2] = 2 * (x * z - w * y);
            m[3] = 2 * (x * y - w * z);
            m[4] = 1 - 2 * (x * x + z * z);
            m[5] = 2 * (y * z + w * x);
            m[6] = 2 * (x * z + w * y);
            m[7] = 2 * (y * z - w * x);
            m[8] = 1 - 2 * (x * x + y * y);
            m[9] = detail::HalfToFloat(h[0]);
            m[10] = detail::HalfToFloat(h[1]);
            m[11] = detail::HalfToFloat(h[2]);
            if (hierarchical)
            {
                // mirror in x: S M S
                m[1] = -m[1];
                m[2] = -m[2];
                m[3] = -m[3];
                m[6] = -m[6];
                m[9] = -m[9];
                if (parentIndex[b] < 0 && parents.count(lower(boneNames[b])) && parents.at(lower(boneNames[b])).empty())
                {
                    // the root: its height above the ground, not a joint offset
                    m[9] = 0.0f;
                    m[10] -= kRootRestHeight;
                    m[11] = 0.0f;
                }
            }
        }
        std::vector<std::array<float, 12>> world = local;
        if (hierarchical)
        {
            std::vector<int> state(bones, 0); // 0 todo, 1 busy, 2 done
            std::vector<uint32_t> stack;
            for (uint32_t root = 0; root < bones; root++)
            {
                if (state[root] == 2)
                    continue;
                // walk up to the first resolved ancestor, then resolve back down
                stack.clear();
                uint32_t b = root;
                while (state[b] == 0)
                {
                    state[b] = 1;
                    stack.push_back(b);
                    if (parentIndex[b] < 0 || state[static_cast<uint32_t>(parentIndex[b])] != 0)
                        break;
                    b = static_cast<uint32_t>(parentIndex[b]);
                }
                for (auto it = stack.rbegin(); it != stack.rend(); ++it)
                {
                    const uint32_t bone = *it;
                    const int parent = parentIndex[bone];
                    if (parent >= 0 && state[static_cast<uint32_t>(parent)] == 2)
                    {
                        const auto& A = world[static_cast<uint32_t>(parent)];
                        const auto& B = local[bone];
                        auto& C = world[bone];
                        for (int col = 0; col < 4; col++)
                            for (int row = 0; row < 3; row++)
                            {
                                float v = 0;
                                for (int k = 0; k < 3; k++)
                                    v += A[static_cast<size_t>(k * 3 + row)] * B[static_cast<size_t>(col * 3 + k)];
                                if (col == 3)
                                    v += A[static_cast<size_t>(9 + row)];
                                C[static_cast<size_t>(col * 3 + row)] = v;
                            }
                    }
                    state[bone] = 2;
                }
            }
        }
        // The engine loads every move "reversed" (AnimationRTPhase::LoadFromRTM conjugates it
        // by diag(-1, 1, -1)), as an OFP soldier is a turned model. An Arma 2 soldier is not
        // turned (reversed=0, Arma1ModelClass), so the moves leave here conjugated once
        // already and arrive as they were posed. OP_A2_MAN_REVERSED=1 keeps the old pair.
        static const bool manReversed = [] {
            const char* e = std::getenv("OP_A2_MAN_REVERSED");
            return e && *e == '1';
        }();
        if (hierarchical && !manReversed)
            for (auto& m : world)
            {
                m[1] = -m[1];
                m[3] = -m[3];
                m[5] = -m[5];
                m[7] = -m[7];
                m[9] = -m[9];
                m[11] = -m[11];
            }
        for (uint32_t b = 0; b < bones; b++)
        {
            char name[BONE_NAME_LEN] = {};
            std::memcpy(name, boneNames[b].data(), std::min<size_t>(boneNames[b].size(), BONE_NAME_LEN - 1));
            put(name, BONE_NAME_LEN);
            put(world[b].data(), sizeof(float) * 12);
        }
    }
    return c.ok;
}

} // namespace Poseidon::Asset::Formats
