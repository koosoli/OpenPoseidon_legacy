#include "Builders.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace island
{
namespace
{
const float kPi = 3.14159265f;

std::string Stem(const std::string& model)
{
    const std::string m = LowerPath(model);
    const size_t slash = m.find_last_of('\\');
    const size_t from = slash == std::string::npos ? 0 : slash + 1;
    const size_t dot = m.find_last_of('.');
    return m.substr(from, dot == std::string::npos || dot < from ? std::string::npos : dot - from);
}

std::string Folder(const std::string& model)
{
    const std::string m = LowerPath(model);
    const size_t slash = m.find_last_of('\\');
    return slash == std::string::npos ? std::string() : m.substr(0, slash + 1);
}

float Wrap(float a)
{
    while (a > kPi)
        a -= 2.0f * kPi;
    while (a < -kPi)
        a += 2.0f * kPi;
    return a;
}

// model space (x aside, z forward) to the map, for an object at (px, pz) turned by yaw
void ToMap(float px, float pz, float yaw, const float local[2], float out[2])
{
    const float c = std::cos(yaw), s = std::sin(yaw);
    out[0] = px + c * local[0] + s * local[1];
    out[1] = pz - s * local[0] + c * local[1];
}

float AngleOf(const float d[2]) // radians from north (+z), towards east (+x)
{
    return std::atan2(d[0], d[1]);
}

void SetYaw(Object& o, float yaw)
{
    const float c = std::cos(yaw), s = std::sin(yaw);
    const float m[9] = {c, 0, -s, 0, 1, 0, s, 0, c};
    std::memcpy(o.m, m, sizeof(m));
}

bool HasDigit(const std::string& s)
{
    for (char c : s)
        if (c >= '0' && c <= '9')
            return true;
    return false;
}
} // namespace

// ---------------------------------------------------------------------------
// roads
// ---------------------------------------------------------------------------

void Roads::Find(const GameData& game, const Island& is)
{
    _found = true;
    _families.clear();
    // candidates: the game's models with a number in the name (asfaltka25, silnice10 50 ...)
    // and every model the island already uses as a road
    std::set<std::string> names;
    for (const std::string& m : game.Models())
    {
        const std::string stem = Stem(m);
        if (HasDigit(stem) && stem.rfind("kr_", 0) != 0)
            names.insert(m);
    }
    for (const Object& o : is.objects)
        if (HasDigit(Stem(o.model)))
            names.insert(LowerPath(o.model));

    std::map<std::string, RoadFamily> families;
    for (const std::string& name : names)
    {
        const std::string stem = Stem(name);
        if (stem.rfind("kr_", 0) == 0 || stem.rfind("most", 0) == 0)
            continue; // crossings and bridges have their own ways
        const ModelShape& s = game.Model(name);
        if (!s.road)
            continue;
        RoadPiece p;
        p.model = name;
        const float lb[2] = {s.lb[0], s.lb[2]}, pb[2] = {s.pb[0], s.pb[2]}, le[2] = {s.le[0], s.le[2]}, pe[2] = {s.pe[0], s.pe[2]};
        for (int k = 0; k < 2; k++)
        {
            p.begin[k] = (lb[k] + pb[k]) * 0.5f;
            p.end[k] = (le[k] + pe[k]) * 0.5f;
            p.corner[0][k] = lb[k];
            p.corner[1][k] = pb[k];
            p.corner[2][k] = pe[k];
            p.corner[3][k] = le[k];
        }
        const float along[2] = {p.end[0] - p.begin[0], p.end[1] - p.begin[1]};
        p.length = std::sqrt(along[0] * along[0] + along[1] * along[1]);
        p.width = std::sqrt((pb[0] - lb[0]) * (pb[0] - lb[0]) + (pb[1] - lb[1]) * (pb[1] - lb[1]));
        if (p.length < 1.0f || p.width < 0.5f)
            continue;
        // the way through is square to each edge, pointing from begin to end
        auto square = [&](const float a[2], const float b[2], float out[2])
        {
            float d[2] = {-(b[1] - a[1]), b[0] - a[0]};
            if (d[0] * along[0] + d[1] * along[1] < 0)
            {
                d[0] = -d[0];
                d[1] = -d[1];
            }
            const float len = std::sqrt(d[0] * d[0] + d[1] * d[1]);
            out[0] = d[0] / len;
            out[1] = d[1] / len;
        };
        square(lb, pb, p.beginDir);
        square(le, pe, p.endDir);
        p.turn = Wrap(AngleOf(p.endDir) - AngleOf(p.beginDir)) * 180.0f / kPi;
        p.cap = stem.find("konec") != std::string::npos || stem.find("end") != std::string::npos;

        // a family: the same folder, the same first letters, the same width
        char key[160];
        snprintf(key, sizeof(key), "%s%s  %.1f m wide", Folder(name).c_str(), stem.substr(0, 3).c_str(), p.width);
        RoadFamily& f = families[key];
        f.name = key;
        f.width = p.width;
        if (p.cap)
        {
            f.hasCap = true;
            f.capPiece = p;
        }
        else if (std::fabs(p.turn) < 0.5f)
        {
            f.straights.push_back(p);
        }
        else if (std::fabs(p.turn) < 46.0f)
        {
            f.curves.push_back(p);
        }
    }
    for (auto& kv : families)
    {
        RoadFamily& f = kv.second;
        if (f.straights.empty())
            continue;
        std::sort(f.straights.begin(), f.straights.end(), [](const RoadPiece& a, const RoadPiece& b) { return a.length > b.length; });
        // gentlest first: the longest for its turn
        std::sort(f.curves.begin(), f.curves.end(), [](const RoadPiece& a, const RoadPiece& b)
                  { return a.length / std::fabs(a.turn) > b.length / std::fabs(b.turn); });
        _families.push_back(f);
    }
}

Object Roads::MakeObject(const PlacedRoad& placed, int id)
{
    Object o;
    o.id = id;
    o.model = placed.piece ? placed.piece->model : std::string();
    SetYaw(o, placed.yaw);
    o.m[9] = placed.x;
    o.m[10] = 0;
    o.m[11] = placed.z;
    o.above = 0; // a road lies on the ground
    return o;
}

std::vector<PlacedRoad> Roads::Lay(const RoadFamily& family, const std::vector<std::array<float, 2>>& path, bool caps,
                                   const float* heading) const
{
    std::vector<PlacedRoad> out;
    if (path.size() < 2 || family.straights.empty())
        return out;
    // the path by distance along it
    std::vector<float> at(path.size(), 0.0f);
    for (size_t i = 1; i < path.size(); i++)
    {
        const float dx = path[i][0] - path[i - 1][0], dz = path[i][1] - path[i - 1][1];
        at[i] = at[i - 1] + std::sqrt(dx * dx + dz * dz);
    }
    const float total = at.back();
    if (total < 1.0f)
        return out;
    auto pointAt = [&](float s, float p[2])
    {
        s = std::clamp(s, 0.0f, total);
        size_t i = 1;
        while (i + 1 < path.size() && at[i] < s)
            i++;
        const float seg = at[i] - at[i - 1];
        const float t = seg > 1e-4f ? (s - at[i - 1]) / seg : 0.0f;
        p[0] = path[i - 1][0] + (path[i][0] - path[i - 1][0]) * t;
        p[1] = path[i - 1][1] + (path[i][1] - path[i - 1][1]) * t;
    };

    float pos[2] = {path[0][0], path[0][1]};
    float dir = heading ? *heading : std::atan2(path[1][0] - path[0][0], path[1][1] - path[0][1]);

    // puts a piece down at the cursor, through from begin to end or the other way
    auto place = [&](const RoadPiece& piece, bool reversed)
    {
        float in[2], inDir[2], outPt[2], outDir[2];
        for (int k = 0; k < 2; k++)
        {
            in[k] = reversed ? piece.end[k] : piece.begin[k];
            inDir[k] = reversed ? -piece.endDir[k] : piece.beginDir[k];
            outPt[k] = reversed ? piece.begin[k] : piece.end[k];
            outDir[k] = reversed ? -piece.beginDir[k] : piece.endDir[k];
        }
        PlacedRoad pr;
        pr.piece = &piece;
        pr.reversed = reversed;
        pr.yaw = dir - AngleOf(inDir);
        float origin[2] = {0, 0}, entry[2];
        ToMap(0, 0, pr.yaw, in, entry);
        pr.x = pos[0] - entry[0];
        pr.z = pos[1] - entry[1];
        (void)origin;
        for (int c = 0; c < 4; c++)
            ToMap(pr.x, pr.z, pr.yaw, piece.corner[c], pr.outline[c]);
        float next[2];
        ToMap(pr.x, pr.z, pr.yaw, outPt, next);
        pos[0] = next[0];
        pos[1] = next[1];
        dir = AngleOf(outDir) + pr.yaw;
        out.push_back(pr);
    };

    if (caps && family.hasCap)
        place(family.capPiece, false); // closed at its begin: the road leaves by its end

    const float shortest = family.straights.back().length;
    const float endRoom = caps && family.hasCap ? family.capPiece.length : 0.0f;
    float progress = 0.0f; // how far along the path the cursor has come
    for (int guard = 0; guard < 20000; guard++)
    {
        // where on the path is the cursor now?  Only ever forwards.
        {
            float best = 1e30f, bestS = progress;
            for (float s = progress; s <= std::min(total, progress + 80.0f); s += 1.0f)
            {
                float p[2];
                pointAt(s, p);
                const float d = (p[0] - pos[0]) * (p[0] - pos[0]) + (p[1] - pos[1]) * (p[1] - pos[1]);
                if (d < best)
                {
                    best = d;
                    bestS = s;
                }
            }
            progress = bestS;
        }
        const float left = total - progress - endRoom;
        if (left < shortest * 0.5f)
            break;
        // steer for a point a little way ahead
        float aim[2];
        const float ahead = std::min(std::max(18.0f, family.width * 3.0f), left + endRoom);
        pointAt(progress + ahead, aim);
        const float tx = aim[0] - pos[0], tz = aim[1] - pos[1];
        const float dist = std::sqrt(tx * tx + tz * tz);
        if (dist < 0.5f)
            break;
        const float off = Wrap(std::atan2(tx, tz) - dir); // + = the aim is to the right
        // the curve that would take us there: 1 / radius
        const float want = 2.0f * std::sin(off) / dist;

        const RoadPiece* pick = nullptr;
        bool reversed = false;
        if (!family.curves.empty())
        {
            const RoadPiece& gentlest = family.curves.front();
            const float gentle = std::fabs(gentlest.turn) * kPi / 180.0f / gentlest.length; // its 1 / radius
            if (std::fabs(want) > gentle * 0.5f)
            {
                // the curve whose bend is nearest to what is wanted
                float bestErr = 1e30f;
                for (const RoadPiece& c : family.curves)
                {
                    const float bend = std::fabs(c.turn) * kPi / 180.0f / c.length;
                    const float err = std::fabs(bend - std::fabs(want));
                    if (err < bestErr)
                    {
                        bestErr = err;
                        pick = &c;
                    }
                }
                // through it the way that turns to the side wanted
                reversed = (pick->turn > 0) != (want > 0);
            }
        }
        if (!pick)
        {
            // straight on: the longest piece that fits what is left and what lies straight ahead
            const float straightAhead = std::fabs(off) < 0.02f ? left : std::min(left, 14.0f);
            pick = &family.straights.back();
            for (const RoadPiece& s : family.straights)
                if (s.length <= straightAhead + 0.5f)
                {
                    pick = &s;
                    break;
                }
        }
        place(*pick, reversed);
    }
    if (caps && family.hasCap && !out.empty())
        place(family.capPiece, true); // entered by its open end
    return out;
}

bool Roads::OpenEnd(const Island& is, const GameData& game, float x, float z, float reach, float at[2], float& heading) const
{
    struct End
    {
        float p[2];
        float out; // the way a road carries on from here
        int object;
    };
    std::vector<End> ends;
    for (size_t i = 0; i < is.objects.size(); i++)
    {
        const Object& o = is.objects[i];
        if (std::fabs(o.X() - x) > reach + 120.0f || std::fabs(o.Z() - z) > reach + 120.0f)
            continue;
        const ModelShape& s = game.Model(o.model);
        if (!s.road)
            continue;
        const float yaw = std::atan2(o.m[6], o.m[8]);
        const float begin[2] = {(s.lb[0] + s.pb[0]) * 0.5f, (s.lb[2] + s.pb[2]) * 0.5f};
        const float end[2] = {(s.le[0] + s.pe[0]) * 0.5f, (s.le[2] + s.pe[2]) * 0.5f};
        const float along[2] = {end[0] - begin[0], end[1] - begin[1]};
        End b, e;
        ToMap(o.X(), o.Z(), yaw, begin, b.p);
        ToMap(o.X(), o.Z(), yaw, end, e.p);
        const float fwd = AngleOf(along) + yaw;
        b.out = fwd + kPi; // leaving by the begin is going backwards
        e.out = fwd;
        b.object = e.object = static_cast<int>(i);
        ends.push_back(b);
        ends.push_back(e);
    }
    bool found = false;
    float best = reach * reach;
    for (size_t i = 0; i < ends.size(); i++)
    {
        const float dx = ends[i].p[0] - x, dz = ends[i].p[1] - z;
        const float d = dx * dx + dz * dz;
        if (d > best)
            continue;
        bool open = true;
        for (size_t j = 0; j < ends.size() && open; j++)
        {
            if (ends[j].object == ends[i].object)
                continue;
            const float ex = ends[j].p[0] - ends[i].p[0], ez = ends[j].p[1] - ends[i].p[1];
            open = ex * ex + ez * ez > 1.0f;
        }
        if (!open)
            continue;
        best = d;
        at[0] = ends[i].p[0];
        at[1] = ends[i].p[1];
        heading = Wrap(ends[i].out);
        found = true;
    }
    return found;
}

// ---------------------------------------------------------------------------
// forests
// ---------------------------------------------------------------------------

bool IsForestBlock(const std::string& model)
{
    const std::string stem = Stem(model);
    if (stem.rfind("les ", 0) != 0 && stem.rfind("les_", 0) != 0)
        return false; // "leseni" is scaffolding
    return stem.find("strom") == std::string::npos && stem.find("mlaz") == std::string::npos && stem.find("geom") == std::string::npos;
}

std::vector<ForestKind> FindForests(const GameData& game)
{
    std::vector<ForestKind> out;
    const std::vector<std::string>& models = game.Models();
    for (const std::string& m : models)
    {
        const std::string stem = Stem(m);
        if (!IsForestBlock(m) || stem.size() < 4 || stem.compare(stem.size() - 3, 3, "_t1") != 0)
            continue;
        const std::string base = m.substr(0, m.size() - 7); // without "_t1.p3d"
        const std::string other = base + "_t2.p3d";
        if (!std::binary_search(models.begin(), models.end(), other))
            continue;
        // its corner block: the "trojuhelnik" of the same folder whose name starts most like it
        std::string corner;
        size_t bestScore = 0;
        for (const std::string& c : models)
        {
            if (Folder(c) != Folder(m) || !IsForestBlock(c) || Stem(c).find("troj") == std::string::npos)
                continue;
            const std::string cs = Stem(c);
            size_t common = 0;
            while (common < cs.size() && common < stem.size() && cs[common] == stem[common])
                common++;
            // "pruchozi" (walk-through) forests go with walk-through corners, when there is one
            const bool through = stem.find("pruchozi") != std::string::npos || stem.find("pruhozi") != std::string::npos;
            const bool cornerThrough = cs.find("pruchozi") != std::string::npos || cs.find("pruhozi") != std::string::npos;
            const size_t score = common * 2 + (through == cornerThrough ? 1 : 0);
            // of two as good, the plainer name
            if (score > bestScore || (score == bestScore && !corner.empty() && cs.size() < Stem(corner).size()))
            {
                bestScore = score;
                corner = c;
            }
        }
        const ModelShape& s1 = game.Model(m);
        const ModelShape& s2 = game.Model(other);
        if (!s1.valid || !s2.valid)
            continue;
        ForestKind k;
        k.name = base;
        k.half1 = m;
        k.half2 = other;
        k.corner = corner;
        // the half blocks are the two halves of one model cut apart: each sits where its own
        // middle was in the whole
        k.off1[0] = s1.centre[0];
        k.off1[1] = s1.centre[2];
        k.off2[0] = s2.centre[0];
        k.off2[1] = s2.centre[2];
        k.above1 = s1.centre[1];
        k.above2 = s2.centre[1];
        if (!corner.empty())
            k.aboveCorner = game.Model(corner).centre[1];
        out.push_back(k);
    }
    return out;
}

int SetForestCells(Island& is, const ForestKind& kind, const std::vector<std::array<int, 2>>& cells, bool plant, bool corners)
{
    if (is.size <= 0 || cells.empty())
        return 0;
    const int n = is.size;
    // what stands where now
    std::vector<uint8_t> forest(static_cast<size_t>(n) * n, 0);
    for (const Object& o : is.objects)
    {
        if (!IsForestBlock(o.model))
            continue;
        const int cx = static_cast<int>(o.X() / is.cell), cz = static_cast<int>(o.Z() / is.cell);
        if (!is.InRange(cx, cz))
            continue;
        const std::string stem = Stem(o.model);
        if (stem.find("troj") == std::string::npos)
            forest[static_cast<size_t>(cz) * n + cx] = 1;
    }
    // the cells that change, and around them the ones whose corner blocks may
    std::vector<uint8_t> touched(static_cast<size_t>(n) * n, 0);
    int changed = 0;
    for (const std::array<int, 2>& c : cells)
    {
        if (!is.InRange(c[0], c[1]))
            continue;
        uint8_t& f = forest[static_cast<size_t>(c[1]) * n + c[0]];
        if ((f != 0) == plant)
            continue;
        f = plant ? 2 : 0; // 2: to be planted
        changed++;
        for (int dz = -1; dz <= 1; dz++)
            for (int dx = -1; dx <= 1; dx++)
                if (is.InRange(c[0] + dx, c[1] + dz))
                    touched[static_cast<size_t>(c[1] + dz) * n + c[0] + dx] = 1;
    }
    if (!changed)
        return 0;
    auto isForest = [&](int x, int z) { return is.InRange(x, z) && forest[static_cast<size_t>(z) * n + x] != 0; };

    // out with the blocks of cleared cells and every corner block near a change; the rest stays
    std::vector<Object> now;
    now.reserve(is.objects.size() + static_cast<size_t>(changed) * 2);
    int nextId = 0;
    for (const Object& o : is.objects)
    {
        nextId = std::max(nextId, o.id);
        if (IsForestBlock(o.model))
        {
            const int cx = static_cast<int>(o.X() / is.cell), cz = static_cast<int>(o.Z() / is.cell);
            if (is.InRange(cx, cz) && touched[static_cast<size_t>(cz) * n + cx])
            {
                const bool cornerBlock = Stem(o.model).find("troj") != std::string::npos;
                if (cornerBlock || forest[static_cast<size_t>(cz) * n + cx] == 0)
                    continue;
            }
        }
        now.push_back(o);
    }
    auto add = [&](const std::string& model, float x, float z, float yaw, float above)
    {
        Object o;
        o.id = ++nextId;
        o.model = model;
        SetYaw(o, yaw);
        o.m[9] = x;
        o.m[11] = z;
        o.above = above;
        now.push_back(std::move(o));
    };
    const float half = is.cell * 0.5f;
    for (int z = 0; z < n; z++)
        for (int x = 0; x < n; x++)
        {
            const size_t i = static_cast<size_t>(z) * n + x;
            if (!touched[i])
                continue;
            const float mx = x * is.cell + half, mz = z * is.cell + half;
            if (forest[i] == 2)
            {
                add(kind.half1, mx + kind.off1[0], mz + kind.off1[1], 0.0f, kind.above1);
                add(kind.half2, mx + kind.off2[0], mz + kind.off2[1], 0.0f, kind.above2);
            }
            else if (forest[i] == 0 && corners && !kind.corner.empty())
            {
                // forest on two sides next to each other and on no other: a corner block,
                // turned to face them (north and east: a quarter turn, and so on round)
                const bool N = isForest(x, z + 1), E = isForest(x + 1, z), S = isForest(x, z - 1), W = isForest(x - 1, z);
                float yaw = -1;
                if (N && E && !S && !W)
                    yaw = 90;
                else if (E && S && !W && !N)
                    yaw = 180;
                else if (S && W && !N && !E)
                    yaw = 270;
                else if (W && N && !E && !S)
                    yaw = 0;
                if (yaw >= 0)
                    add(kind.corner, mx, mz, yaw * kPi / 180.0f, kind.aboveCorner);
            }
        }
    is.ReplaceObjects(std::move(now));
    return changed;
}

int PaintForest(Island& is, const ForestKind& kind, float wx, float wz, float radius, bool plant, bool corners)
{
    std::vector<std::array<int, 2>> cells;
    const float half = is.cell * 0.5f;
    const int x0 = static_cast<int>(std::floor((wx - radius) / is.cell)), x1 = static_cast<int>(std::ceil((wx + radius) / is.cell));
    const int z0 = static_cast<int>(std::floor((wz - radius) / is.cell)), z1 = static_cast<int>(std::ceil((wz + radius) / is.cell));
    for (int z = z0; z <= z1; z++)
        for (int x = x0; x <= x1; x++)
        {
            const float dx = x * is.cell + half - wx, dz = z * is.cell + half - wz;
            const bool under = dx * dx + dz * dz <= radius * radius || (std::fabs(dx) <= half && std::fabs(dz) <= half);
            // not in the sea
            if (under && is.InRange(x, z) && (!plant || is.SurfaceY(x * is.cell + half, z * is.cell + half) > 1.0f))
                cells.push_back({x, z});
        }
    return SetForestCells(is, kind, cells, plant, corners);
}

// ---------------------------------------------------------------------------
// bridges
// ---------------------------------------------------------------------------

std::vector<BridgeKind> FindBridges(const GameData& game)
{
    std::vector<BridgeKind> out;
    for (const std::string& m : game.Models())
    {
        const std::string stem = Stem(m);
        if (stem.rfind("most", 0) != 0 && stem.find("bridge") == std::string::npos)
            continue;
        const ModelShape& s = game.Model(m);
        if (!s.valid)
            continue;
        BridgeKind k;
        k.model = m;
        if (s.road)
        {
            // the way across is from its begin edge to its end edge, the deck at their height
            const float bx = (s.lb[0] + s.pb[0]) * 0.5f, bz = (s.lb[2] + s.pb[2]) * 0.5f;
            const float ex = (s.le[0] + s.pe[0]) * 0.5f, ez = (s.le[2] + s.pe[2]) * 0.5f;
            k.length = std::sqrt((ex - bx) * (ex - bx) + (ez - bz) * (ez - bz));
            k.deck = (s.lb[1] + s.le[1]) * 0.5f;
        }
        else
        {
            k.length = s.bmax[2] - s.bmin[2];
            k.deck = 0.0f;
        }
        if (k.length > 2.0f)
            out.push_back(k);
    }
    // the ones the game can drive over first
    std::stable_sort(out.begin(), out.end(), [](const BridgeKind& a, const BridgeKind& b) { return a.deck > b.deck; });
    return out;
}

int BuildBridge(Island& is, const BridgeKind& kind, float x0, float z0, float x1, float z1, float deckLevel)
{
    const float dx = x1 - x0, dz = z1 - z0;
    const float span = std::sqrt(dx * dx + dz * dz);
    if (span < 1.0f || kind.length < 1.0f)
        return 0;
    const int count = std::max(1, static_cast<int>(std::lround(span / kind.length)));
    const float ux = dx / span, uz = dz / span;
    const float yaw = std::atan2(ux, uz);
    // the sections in a row, the row centred on the span
    const float start = (span - count * kind.length) * 0.5f;
    int id = is.NewObjectId();
    for (int i = 0; i < count; i++)
    {
        const float along = start + (i + 0.5f) * kind.length;
        Object o;
        o.id = id++;
        o.model = kind.model;
        SetYaw(o, yaw);
        o.m[9] = x0 + ux * along;
        o.m[11] = z0 + uz * along;
        // a bridge keeps its height above the sea whatever the ground under it does
        o.fixed = true;
        o.m[10] = deckLevel - kind.deck;
        o.above = o.m[10] - is.SurfaceY(o.m[9], o.m[11]);
        is.AddObject(o);
    }
    return count;
}

// ---------------------------------------------------------------------------
// regions
// ---------------------------------------------------------------------------

namespace
{
const char kRegionMark[8] = {'I', 'E', 'R', 'E', 'G', 'I', 'O', 'N'};
}

bool ExportRegion(const Island& is, int x0, int z0, int x1, int z1, const std::string& file, std::string& error)
{
    if (x0 > x1)
        std::swap(x0, x1);
    if (z0 > z1)
        std::swap(z0, z1);
    x0 = std::max(0, x0);
    z0 = std::max(0, z0);
    x1 = std::min(is.size - 1, x1);
    z1 = std::min(is.size - 1, z1);
    const int w = x1 - x0 + 1, h = z1 - z0 + 1;
    if (is.size <= 0 || w < 1 || h < 1)
    {
        error = "no region";
        return false;
    }
    // an island of a size the games take, big enough for the region and the row of
    // elevations along its far edges
    int size = 32;
    while (size < std::max(w, h) + 1 && size < 1024)
        size *= 2;
    Island out;
    out.New(size, 0.0f, std::string());
    out.textures = is.textures;
    for (int z = 0; z < size; z++)
        for (int x = 0; x < size; x++)
        {
            // beyond the region the land carries on level from its edge
            const int sx = x0 + std::min(x, w), sz = z0 + std::min(z, h);
            out.height[static_cast<size_t>(z) * size + x] = is.H(sx, sz);
            out.tex[static_cast<size_t>(z) * size + x] =
                (x < w && z < h) ? is.tex[static_cast<size_t>(z0 + z) * is.size + (x0 + x)] : static_cast<uint16_t>(0);
        }
    const float minX = x0 * is.cell, minZ = z0 * is.cell, maxX = (x1 + 1) * is.cell, maxZ = (z1 + 1) * is.cell;
    for (const Object& o : is.objects)
    {
        if (o.X() < minX || o.X() >= maxX || o.Z() < minZ || o.Z() >= maxZ)
            continue;
        Object c = o;
        c.m[9] -= minX;
        c.m[11] -= minZ;
        out.objects.push_back(c);
    }
    if (!out.Save(file, error))
        return false;
    std::error_code ec;
    std::filesystem::remove(file + ".bak", ec);
    // after the end of the island proper, where the games do not look: which part is the region
    std::ofstream f(file, std::ios::binary | std::ios::app);
    const int32_t v[4] = {w, h, x0, z0};
    f.write(kRegionMark, sizeof(kRegionMark));
    f.write(reinterpret_cast<const char*>(v), sizeof(v));
    if (!f)
    {
        error = "cannot finish writing " + file;
        return false;
    }
    return true;
}

RegionInfo ReadRegionInfo(const std::string& file, const Island& loaded)
{
    RegionInfo info;
    info.width = info.height = loaded.size;
    std::ifstream f(file, std::ios::binary | std::ios::ate);
    const std::streamoff size = f ? static_cast<std::streamoff>(f.tellg()) : 0;
    if (size < 24)
        return info;
    char mark[8];
    int32_t v[4];
    f.seekg(size - 24);
    f.read(mark, 8);
    f.read(reinterpret_cast<char*>(v), sizeof(v));
    if (f && std::memcmp(mark, kRegionMark, 8) == 0 && v[0] >= 1 && v[1] >= 1 && v[0] <= loaded.size && v[1] <= loaded.size)
    {
        info.marked = true;
        info.width = v[0];
        info.height = v[1];
        info.fromX = v[2];
        info.fromZ = v[3];
    }
    return info;
}

void ImportRegion(Island& is, const Island& src, const RegionInfo& info, int dx, int dz, const ImportOptions& opt)
{
    if (is.size <= 0 || src.size <= 0)
        return;
    const int w = std::min(info.width, src.size), h = std::min(info.height, src.size);
    if (opt.heights)
    {
        // the elevation points: one more each way than there are cells
        for (int z = 0; z <= h; z++)
            for (int x = 0; x <= w; x++)
                if (is.InRange(dx + x, dz + z))
                    is.SetHeight(dx + x, dz + z, src.H(x, z));
    }
    if (opt.textures)
    {
        std::vector<int> slot(kMaxTextures, -2); // -2 not looked up yet, -1 no room
        for (int z = 0; z < h; z++)
            for (int x = 0; x < w; x++)
            {
                if (!is.InRange(dx + x, dz + z))
                    continue;
                const uint16_t s = src.tex[static_cast<size_t>(z) * src.size + x];
                if (slot[s] == -2)
                    slot[s] = s == 0 ? 0 : (src.textures[s].empty() ? -1 : is.TextureSlot(src.textures[s]));
                if (slot[s] >= 0)
                    is.SetTex(dx + x, dz + z, static_cast<uint16_t>(slot[s]));
            }
    }
    if (opt.objects)
    {
        const float minX = dx * is.cell, minZ = dz * is.cell, maxX = (dx + w) * is.cell, maxZ = (dz + h) * is.cell;
        std::vector<Object> now;
        now.reserve(is.objects.size() + src.objects.size());
        int id = 0;
        for (const Object& o : is.objects)
        {
            id = std::max(id, o.id);
            if (opt.clearObjects && o.X() >= minX && o.X() < maxX && o.Z() >= minZ && o.Z() < maxZ)
                continue;
            now.push_back(o);
        }
        for (const Object& o : src.objects)
        {
            if (o.X() < 0 || o.Z() < 0 || o.X() >= w * src.cell || o.Z() >= h * src.cell)
                continue;
            Object c = o;
            c.id = ++id;
            c.m[9] += minX;
            c.m[11] += minZ;
            if (c.X() < 0 || c.Z() < 0 || c.X() >= is.WorldSize() || c.Z() >= is.WorldSize())
                continue;
            now.push_back(c);
        }
        is.ReplaceObjects(std::move(now));
    }
}
} // namespace island
