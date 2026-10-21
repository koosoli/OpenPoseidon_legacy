#include "Island.hpp"

#include <algorithm>
#include <limits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace island
{
namespace
{
float Clamp(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

float ClampHeight(float y)
{
    return Clamp(y, kMinHeight, kMaxHeight);
}

float SmoothStep(float t)
{
    t = Clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// ---- value noise ---------------------------------------------------------------
float Hash2(int x, int z, unsigned seed)
{
    unsigned h = static_cast<unsigned>(x) * 374761393u + static_cast<unsigned>(z) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (h & 0xFFFFFF) / static_cast<float>(0x1000000);
}

float ValueNoise(float x, float z, unsigned seed)
{
    const int xi = static_cast<int>(std::floor(x)), zi = static_cast<int>(std::floor(z));
    const float xf = SmoothStep(x - xi), zf = SmoothStep(z - zi);
    const float a = Hash2(xi, zi, seed), b = Hash2(xi + 1, zi, seed);
    const float c = Hash2(xi, zi + 1, seed), d = Hash2(xi + 1, zi + 1, seed);
    return (a + (b - a) * xf) * (1 - zf) + (c + (d - c) * xf) * zf;
}

// 0..1, several octaves; `persistence` is how much each finer octave adds
float Fractal(float x, float z, unsigned seed, int octaves, float persistence)
{
    float sum = 0, amp = 1, norm = 0;
    for (int o = 0; o < octaves; o++)
    {
        sum += ValueNoise(x, z, seed + o * 101u) * amp;
        norm += amp;
        amp *= persistence;
        x *= 2.03f;
        z *= 2.03f;
    }
    return sum / norm;
}

#pragma pack(push, 1)
struct RawObject
{
    float m[12];
    int id;
    char name[kObjectNameLen];
};
struct RawObject3
{
    float m[12];
    char name[kObjectNameLen + 4];
};
#pragma pack(pop)

bool PowerOfTwo(int n)
{
    return n > 0 && (n & (n - 1)) == 0;
}
} // namespace

std::string LowerPath(const std::string& s)
{
    std::string r = s;
    for (char& c : r)
    {
        if (c == '/')
            c = '\\';
        else if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return r;
}

float Brush::Weight(float dist) const
{
    if (radius <= 0 || dist >= radius)
        return 0;
    const float t = 1.0f - dist / radius; // 1 at the centre
    switch (falloff)
    {
        case Falloff::Linear:
            return t;
        case Falloff::Sharp:
            return t * t;
        case Falloff::Flat:
            return 1.0f;
        default:
            return SmoothStep(t);
    }
}

// ---------------------------------------------------------------------------
// making and reading
// ---------------------------------------------------------------------------

void Island::Reset()
{
    _undo.clear();
    _redo.clear();
    _step = Step();
    _editing = false;
    _stepCell.assign(static_cast<size_t>(size) * size, 0);
    _stepTex.assign(static_cast<size_t>(size) * size, 0);
    modified = false;
    MarkAllDirty();
}

void Island::New(int cells, float baseHeight, const std::string& groundTexture)
{
    size = cells;
    cell = 50.0f;
    height.assign(static_cast<size_t>(size) * size, ClampHeight(baseHeight));
    textures.assign(kMaxTextures, std::string());
    textures[0] = "LandText\\mo.pac"; // the sea
    int ground = 0;
    if (!groundTexture.empty())
    {
        textures[1] = groundTexture;
        ground = 1;
    }
    tex.assign(static_cast<size_t>(size) * size, static_cast<uint16_t>(ground));
    objects.clear();
    objectsRevision++;
    path.clear();
    sourceFormat = "new";
    Reset();
}

void Island::Adopt(int cells, std::vector<float>&& heights, std::vector<uint16_t>&& texIndices,
                   std::vector<std::string>&& textureTable, ObjectList&& objs, const std::string& file,
                   const std::string& format, float cellSize)
{
    size = cells;
    cell = cellSize; // 50 m for the OFP formats; an Arma / DayZ world brings its own (GameData::LoadIsland)
    height = std::move(heights);
    tex = std::move(texIndices);
    textures = std::move(textureTable);
    textures.resize(kMaxTextures);
    objects = std::move(objs);
    objectsRevision++;
    height.resize(static_cast<size_t>(size) * size, 0.0f);
    tex.resize(static_cast<size_t>(size) * size, 0);
    for (uint16_t& t : tex)
        if (t >= kMaxTextures)
            t = 0;
    for (Object& o : objects)
    {
        o.above = o.m[10] - SurfaceY(o.X(), o.Z());
        // bridges span the ground instead of standing on it
        const std::string m = LowerPath(o.model);
        const size_t slash = m.find_last_of('\\');
        const std::string stem = m.substr(slash == std::string::npos ? 0 : slash + 1);
        o.fixed = stem.rfind("most", 0) == 0 || stem.find("bridge") != std::string::npos;
    }
    renumbered = UniqueObjectIds();
    path = file;
    sourceFormat = format;
    Reset();
}

bool Island::LoadRaw(const std::string& file, std::string& error)
{
    std::ifstream f(file, std::ios::binary);
    if (!f)
    {
        error = "cannot open the file";
        return false;
    }
    char magic[4] = {};
    int xr = 0, zr = 0;
    f.read(magic, 4);
    f.read(reinterpret_cast<char*>(&xr), 4);
    f.read(reinterpret_cast<char*>(&zr), 4);
    const bool v4 = std::memcmp(magic, "4WVR", 4) == 0, v3 = std::memcmp(magic, "3WVR", 4) == 0;
    if (!f || (!v4 && !v3))
    {
        error = "not a 4WVR / 3WVR island";
        return false;
    }
    if (xr != zr || !PowerOfTwo(xr) || xr < 16 || xr > 1024)
    {
        error = "unsupported size " + std::to_string(xr) + " x " + std::to_string(zr);
        return false;
    }
    const size_t n = static_cast<size_t>(xr) * xr;
    std::vector<int16_t> raw(n);
    std::vector<uint16_t> ti(n);
    f.read(reinterpret_cast<char*>(raw.data()), n * 2);
    f.read(reinterpret_cast<char*>(ti.data()), n * 2);
    std::vector<std::string> table(kMaxTextures);
    for (int i = 0; i < kMaxTextures; i++)
    {
        char name[kTextureNameLen + 1] = {};
        f.read(name, kTextureNameLen);
        table[i] = name;
    }
    if (!f)
    {
        error = "the file ends too early";
        return false;
    }
    std::vector<float> h(n);
    for (size_t i = 0; i < n; i++)
        h[i] = raw[i] * kHeightStep;
    ObjectList objs;
    for (;;)
    {
        Object o;
        if (v4)
        {
            RawObject r;
            f.read(reinterpret_cast<char*>(&r), sizeof(r));
            if (!f || !r.name[0])
                break;
            r.name[sizeof(r.name) - 1] = 0;
            std::memcpy(o.m, r.m, sizeof(o.m));
            o.id = r.id;
            o.model = r.name;
        }
        else
        {
            RawObject3 r;
            f.read(reinterpret_cast<char*>(&r), sizeof(r));
            if (!f || !r.name[0])
                break;
            r.name[sizeof(r.name) - 1] = 0;
            std::memcpy(o.m, r.m, sizeof(o.m));
            o.id = static_cast<int>(objs.size()) + 1;
            o.model = r.name;
        }
        objs.push_back(std::move(o));
    }
    // 4WVR has no cell size: OFP's is 50 m, and the game loads the format at that. An island that is not 50 m
    // a cell -- one converted from an Arma or DayZ world -- keeps it in <file>.cell beside it (see Save).
    float cellSize = 50.0f;
    {
        std::ifstream side(file + ".cell");
        float value = 0;
        if (side >> value && value >= 1.0f && value <= 200.0f)
            cellSize = value;
    }
    Adopt(xr, std::move(h), std::move(ti), std::move(table), std::move(objs), file, v4 ? "4WVR" : "3WVR", cellSize);
    return true;
}

bool Island::Save(const std::string& file, std::string& error)
{
    namespace fs = std::filesystem;
    if (size <= 0)
    {
        error = "nothing to save";
        return false;
    }
    const std::string tmp = file + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
        {
            error = "cannot write " + tmp;
            return false;
        }
        const size_t n = static_cast<size_t>(size) * size;
        f.write("4WVR", 4);
        f.write(reinterpret_cast<const char*>(&size), 4);
        f.write(reinterpret_cast<const char*>(&size), 4);
        // the elevations as the file holds them; the objects are set on that ground
        std::vector<int16_t> raw(n);
        std::vector<float> saved = height;
        for (size_t i = 0; i < n; i++)
        {
            raw[i] = static_cast<int16_t>(std::lround(ClampHeight(height[i]) / kHeightStep));
            height[i] = raw[i] * kHeightStep;
        }
        f.write(reinterpret_cast<const char*>(raw.data()), n * 2);
        f.write(reinterpret_cast<const char*>(tex.data()), n * 2);
        for (int i = 0; i < kMaxTextures; i++)
        {
            char name[kTextureNameLen] = {};
            const std::string& t = i < static_cast<int>(textures.size()) ? textures[i] : std::string();
            std::strncpy(name, t.c_str(), kTextureNameLen - 1);
            f.write(name, kTextureNameLen);
        }
        for (const Object& o : objects)
        {
            if (o.model.empty())
                continue;
            RawObject r = {};
            std::memcpy(r.m, o.m, sizeof(r.m));
            r.m[10] = ObjectY(o);
            r.id = o.id;
            std::strncpy(r.name, o.model.c_str(), kObjectNameLen - 1);
            f.write(reinterpret_cast<const char*>(&r), sizeof(r));
        }
        RawObject end = {};
        f.write(reinterpret_cast<const char*>(&end), sizeof(end));
        height = std::move(saved);
        if (!f)
        {
            error = "writing failed (disk full?)";
            return false;
        }
    }
    std::error_code ec;
    // the cell size beside the island when it is not OFP's 50 m (LoadRaw reads it back). The game loads the format
    // at 50 m whatever this says: such an island is for editing, not yet for playing at its own scale
    if (std::fabs(cell - 50.0f) > 1e-3f)
    {
        std::ofstream side(file + ".cell", std::ios::trunc);
        side << cell << std::endl;
    }
    else
        fs::remove(file + ".cell", ec);
    if (fs::exists(file, ec))
    {
        fs::copy_file(file, file + ".bak", fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            error = "cannot keep a backup of the old file: " + ec.message();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, file, ec);
    if (ec)
    {
        // rename does not replace on every file system
        fs::remove(file, ec);
        fs::rename(tmp, file, ec);
    }
    if (ec)
    {
        error = "cannot put the new file in place: " + ec.message();
        return false;
    }
    path = file;
    sourceFormat = "4WVR";
    modified = false;
    return true;
}

// Objects sharing an id: the first keeps it and the others get new ones after the highest,
// in the order of the ids - unless the first is a forest block, which gives the id up.
// That is what the game does when it reads a binarized island, so missions that name
// objects by id still find them.
int Island::UniqueObjectIds()
{
    int maxId = 0;
    for (const Object& o : objects)
        maxId = std::max(maxId, o.id);
    // The objects in the order of their ids, those of one id in file order. (This was a table with an entry per
    // id up to the highest. OFP islands number their objects from 1 up; a DayZ world's ids are sparse and large,
    // and the table was bigger than the machine.)
    std::vector<int, SystemAllocator<int>> order;
    order.reserve(objects.size());
    for (size_t i = 0; i < objects.size(); i++)
        if (objects[i].id >= 0)
            order.push_back(static_cast<int>(i));
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return objects[a].id < objects[b].id; });
    auto forest = [&](int index)
    {
        const std::string m = LowerPath(objects[index].model);
        const size_t slash = m.find_last_of('\\');
        return m.compare(slash == std::string::npos ? 0 : slash + 1, 3, "les") == 0;
    };
    // New ids go after the highest, as the game gives them. Should the highest already be the largest an id can be
    // (ids are sparse in the later worlds), the free ids from 1 up are used instead: `used` is every id in use, in
    // order, and `gap` only moves up, so two objects never get the same one.
    std::vector<int, SystemAllocator<int>> used;
    used.reserve(order.size());
    for (int index : order)
        if (used.empty() || used.back() != objects[index].id)
            used.push_back(objects[index].id);
    long long next = static_cast<long long>(maxId) + 1;
    int gap = 1, changed = 0;
    auto newId = [&]()
    {
        if (next <= std::numeric_limits<int>::max())
            return static_cast<int>(next++);
        while (std::binary_search(used.begin(), used.end(), gap))
            gap++;
        return gap++;
    };
    for (size_t first = 0; first < order.size();)
    {
        size_t last = first + 1;
        while (last < order.size() && objects[order[last]].id == objects[order[first]].id)
            last++;
        // objects[order[first .. last)] share an id (read before any of them is renumbered below)
        int keeper = order[first];
        for (size_t j = first + 1; j < last; j++)
        {
            int other = order[j];
            if (forest(keeper))
                std::swap(keeper, other);
            objects[other].id = newId();
            changed++;
        }
        first = last;
    }
    return changed;
}

// ---------------------------------------------------------------------------
// reading the terrain
// ---------------------------------------------------------------------------

float Island::H(int x, int z) const
{
    if (size <= 0)
        return 0;
    x = x < 0 ? 0 : (x >= size ? size - 1 : x);
    z = z < 0 ? 0 : (z >= size ? size - 1 : z);
    return height[static_cast<size_t>(z) * size + x];
}

float Island::SurfaceY(float wx, float wz) const
{
    if (size <= 0)
        return 0;
    const float fx = wx / cell, fz = wz / cell;
    const int x = static_cast<int>(std::floor(fx)), z = static_cast<int>(std::floor(fz));
    const float xf = fx - x, zf = fz - z;
    const float y00 = H(x, z), y10 = H(x + 1, z), y01 = H(x, z + 1), y11 = H(x + 1, z + 1);
    if (xf + zf <= 1.0f)
        return y00 + (y10 - y00) * xf + (y01 - y00) * zf;
    return y11 + (y01 - y11) * (1.0f - xf) + (y10 - y11) * (1.0f - zf);
}

void Island::Normal(int x, int z, float n[3]) const
{
    const float dx = H(x + 1, z) - H(x - 1, z), dz = H(x, z + 1) - H(x, z - 1);
    float v[3] = {-dx, 2.0f * cell, -dz};
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    n[0] = v[0] / len;
    n[1] = v[1] / len;
    n[2] = v[2] / len;
}

float Island::MinHeight() const
{
    return height.empty() ? 0 : *std::min_element(height.begin(), height.end());
}

float Island::MaxHeight() const
{
    return height.empty() ? 0 : *std::max_element(height.begin(), height.end());
}

bool Island::Raycast(const float origin[3], const float dir[3], float maxDist, float hit[3]) const
{
    if (size <= 0)
        return false;
    const float step = cell * 0.25f;
    float prevT = 0;
    bool prevAbove = origin[1] > SurfaceY(origin[0], origin[2]);
    if (!prevAbove)
        return false;
    for (float t = step; t <= maxDist; t += step)
    {
        const float x = origin[0] + dir[0] * t, y = origin[1] + dir[1] * t, z = origin[2] + dir[2] * t;
        if (y < SurfaceY(x, z))
        {
            // between prevT (above) and t (below)
            float a = prevT, b = t;
            for (int i = 0; i < 24; i++)
            {
                const float m = (a + b) * 0.5f;
                const float my = origin[1] + dir[1] * m;
                if (my < SurfaceY(origin[0] + dir[0] * m, origin[2] + dir[2] * m))
                    b = m;
                else
                    a = m;
            }
            hit[0] = origin[0] + dir[0] * b;
            hit[2] = origin[2] + dir[2] * b;
            hit[1] = SurfaceY(hit[0], hit[2]);
            return hit[0] >= 0 && hit[2] >= 0 && hit[0] <= WorldSize() && hit[2] <= WorldSize();
        }
        prevT = t;
    }
    return false;
}

// ---------------------------------------------------------------------------
// editing
// ---------------------------------------------------------------------------

void Island::Touch(int x, int z)
{
    _dx0 = std::min(_dx0, x);
    _dz0 = std::min(_dz0, z);
    _dx1 = std::max(_dx1, x);
    _dz1 = std::max(_dz1, z);
}

void Island::MarkAllDirty()
{
    _dx0 = _dz0 = 0;
    _dx1 = _dz1 = size - 1;
}

bool Island::TakeDirty(int& x0, int& z0, int& x1, int& z1)
{
    if (_dx1 < _dx0 || _dz1 < _dz0)
        return false;
    x0 = _dx0;
    z0 = _dz0;
    x1 = _dx1;
    z1 = _dz1;
    _dx0 = _dz0 = 1 << 30;
    _dx1 = _dz1 = -1;
    return true;
}

void Island::BeginEdit(const char* what)
{
    if (_editing)
        EndEdit();
    _editing = true;
    _step = Step();
    _step.what = what ? what : "";
    _texturesAtBegin = textures;
}

void Island::EndEdit()
{
    if (!_editing)
        return;
    _editing = false;
    for (const CellChange& c : _step.heights)
        _stepCell[c.index] = 0;
    for (const TexChange& c : _step.texs)
        _stepTex[c.index] = 0;
    // changes that came back to where they started are not changes
    _step.heights.erase(std::remove_if(_step.heights.begin(), _step.heights.end(),
                                       [](const CellChange& c) { return c.before == c.after; }),
                        _step.heights.end());
    _step.texs.erase(
        std::remove_if(_step.texs.begin(), _step.texs.end(), [](const TexChange& c) { return c.before == c.after; }),
        _step.texs.end());
    if (textures != _texturesAtBegin)
    {
        _step.texturesBefore = _texturesAtBegin;
        _step.texturesAfter = textures;
    }
    if (_step.Empty())
        return;
    _undo.push_back(std::move(_step));
    _step = Step();
    _redo.clear();
    if (_undo.size() > 200)
        _undo.erase(_undo.begin());
    modified = true;
}

void Island::SetHeight(int x, int z, float y)
{
    if (!InRange(x, z))
        return;
    const int i = z * size + x;
    y = ClampHeight(y);
    if (height[i] == y)
        return;
    if (_editing)
    {
        if (_stepCell[i] == 0)
        {
            _step.heights.push_back({i, height[i], y});
            _stepCell[i] = static_cast<int>(_step.heights.size());
        }
        else
        {
            _step.heights[_stepCell[i] - 1].after = y;
        }
    }
    height[i] = y;
    Touch(x, z);
}

void Island::SetTex(int x, int z, uint16_t slot)
{
    if (!InRange(x, z) || slot >= kMaxTextures)
        return;
    const int i = z * size + x;
    if (tex[i] == slot)
        return;
    if (_editing)
    {
        if (_stepTex[i] == 0)
        {
            _step.texs.push_back({i, tex[i], slot});
            _stepTex[i] = static_cast<int>(_step.texs.size());
        }
        else
        {
            _step.texs[_stepTex[i] - 1].after = slot;
        }
    }
    tex[i] = slot;
    Touch(x, z);
}

int Island::NewObjectId() const
{
    int id = 0;
    for (const Object& o : objects)
        id = std::max(id, o.id);
    return id + 1;
}

int Island::AddObject(const Object& o)
{
    objects.push_back(o);
    objectsRevision++;
    const int index = static_cast<int>(objects.size()) - 1;
    if (_editing)
        _step.objs.push_back({ObjectChange::Added, index, Object(), o});
    return index;
}

void Island::RemoveObject(int index)
{
    if (index < 0 || index >= static_cast<int>(objects.size()))
        return;
    if (_editing)
        _step.objs.push_back({ObjectChange::Removed, index, objects[index], Object()});
    objects.erase(objects.begin() + index);
    objectsRevision++;
}

void Island::ChangeObject(int index, const Object& o)
{
    if (index < 0 || index >= static_cast<int>(objects.size()))
        return;
    if (_editing)
    {
        // one entry per object and step: a drag is one change
        bool found = false;
        for (ObjectChange& c : _step.objs)
        {
            if (c.kind == ObjectChange::Changed && c.index == index)
            {
                c.after = o;
                found = true;
                break;
            }
        }
        if (!found)
            _step.objs.push_back({ObjectChange::Changed, index, objects[index], o});
    }
    objects[index] = o;
    objectsRevision++;
}

int Island::TextureSlot(const std::string& name)
{
    const std::string want = LowerPath(name);
    if (want.empty())
        return -1;
    textures.resize(kMaxTextures);
    for (int i = 0; i < kMaxTextures; i++)
        if (LowerPath(textures[i]) == want)
            return i;
    for (int i = 1; i < kMaxTextures; i++)
    {
        if (textures[i].empty())
        {
            textures[i] = name;
            return i;
        }
    }
    return -1;
}

int Island::UsedTextures() const
{
    int n = 0;
    for (const std::string& t : textures)
        n += t.empty() ? 0 : 1;
    return n;
}

void Island::Apply(const Step& s, bool forward)
{
    for (const CellChange& c : s.heights)
    {
        height[c.index] = forward ? c.after : c.before;
        Touch(c.index % size, c.index / size);
    }
    for (const TexChange& c : s.texs)
    {
        tex[c.index] = forward ? c.after : c.before;
        Touch(c.index % size, c.index / size);
    }
    if (!s.texturesBefore.empty())
        textures = forward ? s.texturesAfter : s.texturesBefore;
    if (s.allObjects)
        objects = forward ? s.objectsAfter : s.objectsBefore;
    if (forward)
    {
        for (const ObjectChange& c : s.objs)
        {
            if (c.kind == ObjectChange::Added)
                objects.insert(objects.begin() + std::min<size_t>(c.index, objects.size()), c.after);
            else if (c.kind == ObjectChange::Removed)
                objects.erase(objects.begin() + c.index);
            else
                objects[c.index] = c.after;
        }
    }
    else
    {
        for (auto it = s.objs.rbegin(); it != s.objs.rend(); ++it)
        {
            const ObjectChange& c = *it;
            if (c.kind == ObjectChange::Added)
                objects.erase(objects.begin() + c.index);
            else if (c.kind == ObjectChange::Removed)
                objects.insert(objects.begin() + std::min<size_t>(c.index, objects.size()), c.before);
            else
                objects[c.index] = c.before;
        }
    }
    objectsRevision++;
    modified = true;
}

void Island::Undo()
{
    if (_editing)
        EndEdit();
    if (_undo.empty())
        return;
    Apply(_undo.back(), false);
    _redo.push_back(std::move(_undo.back()));
    _undo.pop_back();
}

void Island::Redo()
{
    if (_editing)
        EndEdit();
    if (_redo.empty())
        return;
    Apply(_redo.back(), true);
    _undo.push_back(std::move(_redo.back()));
    _redo.pop_back();
}

// ---------------------------------------------------------------------------
// tools
// ---------------------------------------------------------------------------

namespace
{
struct Reach
{
    int x0, z0, x1, z1;
};

Reach BrushReach(const Island& is, float wx, float wz, float radius)
{
    Reach r;
    r.x0 = std::max(0, static_cast<int>(std::floor((wx - radius) / is.cell)));
    r.z0 = std::max(0, static_cast<int>(std::floor((wz - radius) / is.cell)));
    r.x1 = std::min(is.size - 1, static_cast<int>(std::ceil((wx + radius) / is.cell)));
    r.z1 = std::min(is.size - 1, static_cast<int>(std::ceil((wz + radius) / is.cell)));
    return r;
}

float Dist(const Island& is, int x, int z, float wx, float wz)
{
    const float dx = x * is.cell - wx, dz = z * is.cell - wz;
    return std::sqrt(dx * dx + dz * dz);
}
} // namespace

void Island::Raise(float wx, float wz, const Brush& b, float metres)
{
    const Reach r = BrushReach(*this, wx, wz, b.radius);
    for (int z = r.z0; z <= r.z1; z++)
        for (int x = r.x0; x <= r.x1; x++)
        {
            const float w = b.Weight(Dist(*this, x, z, wx, wz));
            if (w > 0)
                SetHeight(x, z, H(x, z) + metres * w);
        }
}

void Island::Smooth(float wx, float wz, const Brush& b, float amount)
{
    const Reach r = BrushReach(*this, wx, wz, b.radius);
    const int w = r.x1 - r.x0 + 1, h = r.z1 - r.z0 + 1;
    if (w <= 0 || h <= 0)
        return;
    std::vector<float> avg(static_cast<size_t>(w) * h);
    for (int z = r.z0; z <= r.z1; z++)
        for (int x = r.x0; x <= r.x1; x++)
        {
            float s = 0;
            for (int dz = -1; dz <= 1; dz++)
                for (int dx = -1; dx <= 1; dx++)
                    s += H(x + dx, z + dz);
            avg[static_cast<size_t>(z - r.z0) * w + (x - r.x0)] = s / 9.0f;
        }
    for (int z = r.z0; z <= r.z1; z++)
        for (int x = r.x0; x <= r.x1; x++)
        {
            const float k = b.Weight(Dist(*this, x, z, wx, wz)) * Clamp(amount, 0.0f, 1.0f);
            if (k > 0)
            {
                const float cur = H(x, z);
                SetHeight(x, z, cur + (avg[static_cast<size_t>(z - r.z0) * w + (x - r.x0)] - cur) * k);
            }
        }
}

void Island::Flatten(float wx, float wz, const Brush& b, float target, float amount)
{
    const Reach r = BrushReach(*this, wx, wz, b.radius);
    for (int z = r.z0; z <= r.z1; z++)
        for (int x = r.x0; x <= r.x1; x++)
        {
            const float k = b.Weight(Dist(*this, x, z, wx, wz)) * Clamp(amount, 0.0f, 1.0f);
            if (k > 0)
            {
                const float cur = H(x, z);
                SetHeight(x, z, cur + (target - cur) * k);
            }
        }
}

void Island::Noise(float wx, float wz, const Brush& b, float metres, float scale, unsigned seed)
{
    const Reach r = BrushReach(*this, wx, wz, b.radius);
    const float inv = 1.0f / std::max(scale, 1.0f);
    for (int z = r.z0; z <= r.z1; z++)
        for (int x = r.x0; x <= r.x1; x++)
        {
            const float w = b.Weight(Dist(*this, x, z, wx, wz));
            if (w > 0)
            {
                const float n = Fractal(x * cell * inv, z * cell * inv, seed, 4, 0.5f) * 2.0f - 1.0f;
                SetHeight(x, z, H(x, z) + n * metres * w);
            }
        }
}

void Island::Erode(float wx, float wz, const Brush& b, float amount)
{
    const Reach r = BrushReach(*this, wx, wz, b.radius);
    const float talus = cell * 0.45f; // about 24 degrees: steeper than this slumps
    static const int nx[4] = {1, -1, 0, 0}, nz[4] = {0, 0, 1, -1};
    for (int z = r.z0; z <= r.z1; z++)
        for (int x = r.x0; x <= r.x1; x++)
        {
            const float w = b.Weight(Dist(*this, x, z, wx, wz)) * Clamp(amount, 0.0f, 1.0f);
            if (w <= 0)
                continue;
            int lx = x, lz = z;
            float low = H(x, z);
            for (int k = 0; k < 4; k++)
            {
                const int ax = x + nx[k], az = z + nz[k];
                if (InRange(ax, az) && H(ax, az) < low)
                {
                    low = H(ax, az);
                    lx = ax;
                    lz = az;
                }
            }
            const float d = H(x, z) - low;
            if (d > talus)
            {
                const float move = (d - talus) * 0.5f * w;
                SetHeight(x, z, H(x, z) - move);
                SetHeight(lx, lz, H(lx, lz) + move);
            }
        }
}

void Island::Paint(float wx, float wz, const Brush& b, uint16_t slot)
{
    // a cell is painted when its centre is under the brush
    const float half = cell * 0.5f;
    const Reach r = BrushReach(*this, wx, wz, b.radius + cell);
    for (int z = r.z0; z <= r.z1; z++)
        for (int x = r.x0; x <= r.x1; x++)
        {
            const float dx = x * cell + half - wx, dz = z * cell + half - wz;
            if (dx * dx + dz * dz <= b.radius * b.radius || (b.radius < half && std::fabs(dx) <= half && std::fabs(dz) <= half))
                SetTex(x, z, slot);
        }
}

void Island::Ramp(float x0, float z0, float y0, float x1, float z1, float y1, const Brush& b)
{
    const float sx = x1 - x0, sz = z1 - z0;
    const float len2 = sx * sx + sz * sz;
    if (len2 < 1.0f)
        return;
    Reach r;
    r.x0 = std::max(0, static_cast<int>(std::floor((std::min(x0, x1) - b.radius) / cell)));
    r.z0 = std::max(0, static_cast<int>(std::floor((std::min(z0, z1) - b.radius) / cell)));
    r.x1 = std::min(size - 1, static_cast<int>(std::ceil((std::max(x0, x1) + b.radius) / cell)));
    r.z1 = std::min(size - 1, static_cast<int>(std::ceil((std::max(z0, z1) + b.radius) / cell)));
    for (int z = r.z0; z <= r.z1; z++)
        for (int x = r.x0; x <= r.x1; x++)
        {
            const float px = x * cell - x0, pz = z * cell - z0;
            const float t = Clamp((px * sx + pz * sz) / len2, 0.0f, 1.0f);
            const float cx = px - sx * t, cz = pz - sz * t;
            const float w = b.Weight(std::sqrt(cx * cx + cz * cz));
            if (w > 0)
            {
                // the middle of the strip is exactly the incline; the sides blend into the land
                const float k = Clamp(w * 2.0f, 0.0f, 1.0f);
                const float cur = H(x, z);
                SetHeight(x, z, cur + (y0 + (y1 - y0) * t - cur) * k);
            }
        }
}

void Island::SmoothAll(int passes)
{
    std::vector<float> next(height.size());
    for (int p = 0; p < passes; p++)
    {
        for (int z = 0; z < size; z++)
            for (int x = 0; x < size; x++)
            {
                float s = 0;
                for (int dz = -1; dz <= 1; dz++)
                    for (int dx = -1; dx <= 1; dx++)
                        s += H(x + dx, z + dz);
                next[static_cast<size_t>(z) * size + x] = s / 9.0f;
            }
        for (int z = 0; z < size; z++)
            for (int x = 0; x < size; x++)
                SetHeight(x, z, next[static_cast<size_t>(z) * size + x]);
    }
}

void Island::ScaleAll(float factor)
{
    for (int z = 0; z < size; z++)
        for (int x = 0; x < size; x++)
            SetHeight(x, z, H(x, z) * factor);
}

void Island::OffsetAll(float metres)
{
    for (int z = 0; z < size; z++)
        for (int x = 0; x < size; x++)
            SetHeight(x, z, H(x, z) + metres);
}

void Island::Generate(unsigned seed, float peak, float roughness, float seaDepth)
{
    const float inv = 1.0f / size;
    for (int z = 0; z < size; z++)
        for (int x = 0; x < size; x++)
        {
            const float u = x * inv, v = z * inv;
            float n = Fractal(u * 4.0f, v * 4.0f, seed, 6, Clamp(roughness, 0.2f, 0.8f));
            n = std::pow(Clamp((n - 0.25f) / 0.6f, 0.0f, 1.0f), 1.6f); // flat lowlands, few peaks
            // land in the middle, sea all around; the coast wanders with the noise
            const float dx = u - 0.5f, dz = v - 0.5f;
            const float edge = std::sqrt(dx * dx + dz * dz) * 2.0f +
                               (Fractal(u * 3.0f + 17.0f, v * 3.0f + 5.0f, seed + 77u, 3, 0.5f) - 0.5f) * 0.5f;
            const float land = SmoothStep((0.82f - edge) / 0.3f);
            SetHeight(x, z, land * (2.0f + n * peak) - (1.0f - land) * seaDepth);
        }
}

void Island::ReplaceTexture(uint16_t from, uint16_t to)
{
    for (int z = 0; z < size; z++)
        for (int x = 0; x < size; x++)
            if (tex[static_cast<size_t>(z) * size + x] == from)
                SetTex(x, z, to);
}

// ---------------------------------------------------------------------------
// height maps
// ---------------------------------------------------------------------------

bool Island::ExportHeightmap(const std::string& file, float& low, float& high, std::string& error) const
{
    low = MinHeight();
    high = MaxHeight();
    if (high - low < 0.01f)
        high = low + 1.0f;
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f)
    {
        error = "cannot write the file";
        return false;
    }
    f << "P5\n# island height map: 0 = " << low << " m, 65535 = " << high << " m\n" << size << " " << size << "\n65535\n";
    std::vector<uint8_t> row(static_cast<size_t>(size) * 2);
    for (int z = size - 1; z >= 0; z--) // north at the top
    {
        for (int x = 0; x < size; x++)
        {
            const int v = static_cast<int>(std::lround((H(x, z) - low) / (high - low) * 65535.0f));
            const int c = v < 0 ? 0 : (v > 65535 ? 65535 : v);
            row[x * 2] = static_cast<uint8_t>(c >> 8);
            row[x * 2 + 1] = static_cast<uint8_t>(c & 255);
        }
        f.write(reinterpret_cast<const char*>(row.data()), row.size());
    }
    return static_cast<bool>(f);
}

void Island::ApplyHeights(const float* values, int w, int h, float low, float high, bool metres)
{
    if (!values || w < 2 || h < 2 || size < 2)
        return;
    auto at = [&](int x, int y)
    {
        x = x < 0 ? 0 : (x >= w ? w - 1 : x);
        y = y < 0 ? 0 : (y >= h ? h - 1 : y);
        return values[static_cast<size_t>(y) * w + x];
    };
    for (int z = 0; z < size; z++)
        for (int x = 0; x < size; x++)
        {
            const float px = x * (w - 1) / static_cast<float>(size - 1);
            const float py = (size - 1 - z) * (h - 1) / static_cast<float>(size - 1);
            const int ix = static_cast<int>(px), iy = static_cast<int>(py);
            const float fx = px - ix, fy = py - iy;
            const float v = (at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx) * (1 - fy) +
                            (at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx) * fy;
            SetHeight(x, z, metres ? v : low + v * (high - low));
        }
}

// ---------------------------------------------------------------------------
// texturing by rule
// ---------------------------------------------------------------------------

AutoTextureResult Island::AutoTexture(const AutoTextureRules& rules, const std::vector<std::string>& available,
                                      const Brush* brush, float wx, float wz)
{
    AutoTextureResult res;
    if (size <= 0 || rules.grass.empty())
        return res;
    const std::string folder = LowerPath(rules.folder);

    // ---- what the set has: its plain textures and which of them have transitions between them
    std::vector<std::string> codes;
    auto codeId = [&](const std::string& c) -> int
    {
        for (size_t i = 0; i < codes.size(); i++)
            if (codes[i] == c)
                return static_cast<int>(i);
        codes.push_back(c);
        return static_cast<int>(codes.size()) - 1;
    };
    struct Transition
    {
        int c[4];
    };
    std::vector<Transition> transitions;
    std::vector<std::string> names; // stems of every file of the set, sorted
    auto consider = [&](const std::string& name)
    {
        if (name.rfind(folder, 0) != 0)
            return;
        const size_t dot = name.find_last_of('.');
        if (dot == std::string::npos || dot <= folder.size())
            return;
        const std::string stem = name.substr(folder.size(), dot - folder.size());
        if (stem.find('\\') != std::string::npos)
            return;
        if (stem.size() == 2)
            codeId(stem);
        else if (stem.size() == 8)
            transitions.push_back({{codeId(stem.substr(0, 2)), codeId(stem.substr(2, 2)), codeId(stem.substr(4, 2)), codeId(stem.substr(6, 2))}});
        else
            return;
        names.push_back(stem);
    };
    {
        // the set's files sit together in the sorted list
        auto it = std::lower_bound(available.begin(), available.end(), folder);
        for (; it != available.end() && it->rfind(folder, 0) == 0; ++it)
            consider(*it);
        for (const std::string& tname : textures)
            if (!tname.empty())
                consider(LowerPath(tname));
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
    }
    auto exists = [&](const std::string& stem) { return std::binary_search(names.begin(), names.end(), stem); };
    // the file a stem of the set is in
    auto fileOf = [&](const std::string& stem) -> std::string
    {
        for (const char* ext : {".paa", ".pac"})
        {
            const std::string name = folder + stem + ext;
            if (std::binary_search(available.begin(), available.end(), name))
                return name;
            for (const std::string& tname : textures)
                if (!tname.empty() && LowerPath(tname) == name)
                    return name;
        }
        return std::string();
    };

    // ---- the kinds of ground and their textures
    enum Kind
    {
        Sea,
        Beach,
        Shore,
        Grass,
        GrassAlt,
        Hill,
        Rock,
        Peak,
        Forest,
        NKinds
    };
    std::string roleCode[NKinds];
    roleCode[Beach] = LowerPath(rules.beach);
    roleCode[Shore] = LowerPath(rules.shore);
    roleCode[Grass] = LowerPath(rules.grass);
    roleCode[GrassAlt] = LowerPath(rules.grassAlt);
    roleCode[Hill] = LowerPath(rules.hill);
    roleCode[Rock] = LowerPath(rules.rock);
    roleCode[Peak] = LowerPath(rules.peak);
    roleCode[Forest] = LowerPath(rules.forest);
    // what is not given falls back on its neighbour in the list
    auto orElse = [&](Kind k, Kind other)
    {
        if (roleCode[k].empty())
            roleCode[k] = roleCode[other];
    };
    orElse(Beach, Grass);
    orElse(Shore, Beach);
    orElse(GrassAlt, Grass);
    orElse(Hill, Grass);
    orElse(Rock, Hill);
    orElse(Peak, Rock);
    orElse(Forest, Grass);
    roleCode[Sea] = roleCode[Beach]; // the shallows are beach
    int roleId[NKinds];
    for (int k = 0; k < NKinds; k++)
        roleId[k] = codeId(roleCode[k]);

    // which codes have a transition between them, and the way from one to another
    const int nc = static_cast<int>(codes.size());
    std::vector<uint8_t> joined(static_cast<size_t>(nc) * nc, 0);
    for (const Transition& tr : transitions)
        for (int a = 0; a < 4; a++)
            for (int b = 0; b < 4; b++)
                joined[static_cast<size_t>(tr.c[a]) * nc + tr.c[b]] = 1;
    for (int c = 0; c < nc; c++)
        joined[static_cast<size_t>(c) * nc + c] = 1;
    // toward[a * nc + b]: the code to put next to a on the way to b (-1: no way)
    std::vector<int> toward(static_cast<size_t>(nc) * nc, -1);
    for (int b = 0; b < nc; b++)
    {
        std::vector<int> dist(nc, -1), queue;
        dist[b] = 0;
        queue.push_back(b);
        for (size_t q = 0; q < queue.size(); q++)
        {
            const int cur = queue[q];
            for (int n = 0; n < nc; n++)
                if (dist[n] < 0 && joined[static_cast<size_t>(cur) * nc + n])
                {
                    dist[n] = dist[cur] + 1;
                    toward[static_cast<size_t>(n) * nc + b] = cur; // from n, step to cur
                    queue.push_back(n);
                }
        }
    }

    // ---- forest blocks stand on their own floor
    std::vector<uint8_t> forestCell;
    if (rules.forestFloor && !rules.forest.empty())
    {
        forestCell.assign(static_cast<size_t>(size) * size, 0);
        for (const Object& o : objects)
        {
            const std::string m = LowerPath(o.model);
            const size_t slash = m.find_last_of('\\');
            if (m.compare(slash == std::string::npos ? 0 : slash + 1, 3, "les") != 0)
                continue;
            const int cx = static_cast<int>(o.X() / cell), cz = static_cast<int>(o.Z() / cell);
            if (InRange(cx, cz))
                forestCell[static_cast<size_t>(cz) * size + cx] = 1;
        }
    }
    auto inForest = [&](int x, int z) { return !forestCell.empty() && InRange(x, z) && forestCell[static_cast<size_t>(z) * size + x]; };

    const float wob = Clamp(rules.wobble, 0.0f, 1.0f);
    const bool hasShore = !rules.shore.empty();
    auto kindAt = [&](int x, int z) -> Kind
    {
        const float h = H(x, z);
        if (rules.seaTexture && h < rules.seaBelow)
            return Sea;
        if (h > -1.0f && (inForest(x, z) || inForest(x - 1, z) || inForest(x, z - 1) || inForest(x - 1, z - 1)))
            return Forest;
        // borders wander: what the rules see is pushed about by slow noise
        const float n = Fractal(x * 0.11f, z * 0.11f, rules.seed, 4, 0.55f) * 2.0f - 1.0f;
        const float fine = Fractal(x * 0.47f, z * 0.47f, rules.seed + 31u, 2, 0.5f) * 2.0f - 1.0f;
        const float low = h + (n * 1.2f + fine * 0.6f) * wob;
        if (low < rules.beachTop)
            return Beach;
        float nrm[3];
        Normal(x, z, nrm);
        const float slope = std::acos(Clamp(nrm[1], -1.0f, 1.0f)) * 57.29578f;
        if (slope + (n * 5.0f + fine * 3.0f) * wob > rules.rockSlope)
            return Rock;
        if (hasShore && low < rules.shoreTop)
            return Shore;
        const float hh = h + (n * 28.0f + fine * 9.0f) * wob;
        if (hh > rules.peakFrom)
            return Peak;
        if (hh > rules.hillFrom)
            return Hill;
        const float p = Fractal(x * 0.19f + 40.0f, z * 0.19f + 7.0f, rules.seed + 5u, 3, 0.5f);
        return p < Clamp(rules.patches, 0.0f, 1.0f) * 0.85f ? GrassAlt : Grass;
    };

    // ---- the corners in reach (and a few around them, for the steps between kinds)
    const int margin = rules.stepThrough ? 4 : 0;
    int cx0 = 0, cz0 = 0, cx1 = size - 1, cz1 = size - 1; // cells
    if (brush)
    {
        cx0 = std::max(0, static_cast<int>(std::floor((wx - brush->radius) / cell)) - 1);
        cz0 = std::max(0, static_cast<int>(std::floor((wz - brush->radius) / cell)) - 1);
        cx1 = std::min(size - 1, static_cast<int>(std::ceil((wx + brush->radius) / cell)));
        cz1 = std::min(size - 1, static_cast<int>(std::ceil((wz + brush->radius) / cell)));
    }
    const int vx0 = cx0 - margin, vz0 = cz0 - margin;
    const int vw = cx1 - cx0 + 2 + margin * 2, vh = cz1 - cz0 + 2 + margin * 2;
    std::vector<int16_t> id(static_cast<size_t>(vw) * vh);
    std::vector<uint8_t> sea(static_cast<size_t>(vw) * vh, 0), fixed(static_cast<size_t>(vw) * vh, 0);
    for (int z = 0; z < vh; z++)
        for (int x = 0; x < vw; x++)
        {
            const Kind k = kindAt(vx0 + x, vz0 + z);
            const size_t i = static_cast<size_t>(z) * vw + x;
            id[i] = static_cast<int16_t>(roleId[k]);
            sea[i] = k == Sea;
            fixed[i] = k == Forest; // the floor stays under the trees: its neighbours give way
        }

    // ---- where two kinds meet that the set has no transition for, the kinds between them go in
    if (rules.stepThrough)
    {
        std::vector<std::pair<size_t, int16_t>> changes;
        for (int pass = 0; pass < 8; pass++)
        {
            changes.clear();
            for (int z = 0; z < vh; z++)
                for (int x = 0; x < vw; x++)
                {
                    const size_t i = static_cast<size_t>(z) * vw + x;
                    if (fixed[i])
                        continue;
                    const int c = id[i];
                    for (int dz = -1; dz <= 1; dz++)
                    {
                        bool done = false;
                        for (int dx = -1; dx <= 1; dx++)
                        {
                            const int ax = x + dx, az = z + dz;
                            if ((dx == 0 && dz == 0) || ax < 0 || az < 0 || ax >= vw || az >= vh)
                                continue;
                            const int d = id[static_cast<size_t>(az) * vw + ax];
                            if (d == c || joined[static_cast<size_t>(c) * nc + d])
                                continue;
                            const int step = toward[static_cast<size_t>(c) * nc + d];
                            if (step < 0 || step == c)
                                continue;
                            changes.emplace_back(i, static_cast<int16_t>(step));
                            done = true;
                            break;
                        }
                        if (done)
                            break;
                    }
                }
            if (changes.empty())
                break;
            for (const auto& ch : changes)
                id[ch.first] = ch.second;
        }
    }

    // ---- a texture for every cell
    std::vector<std::pair<std::string, int>> slotOf; // stem -> slot (-1: no such file, -2: table full)
    auto slotFor = [&](const std::string& stem) -> int
    {
        for (const auto& s : slotOf)
            if (s.first == stem)
                return s.second;
        const std::string name = fileOf(stem);
        int slot = -1;
        if (!name.empty())
        {
            slot = TextureSlot(name);
            if (slot < 0)
                slot = -2;
        }
        slotOf.emplace_back(stem, slot);
        return slot;
    };
    const float half = cell * 0.5f;
    for (int z = cz0; z <= cz1; z++)
        for (int x = cx0; x <= cx1; x++)
        {
            if (brush)
            {
                const float dx = x * cell + half - wx, dz = z * cell + half - wz;
                if (dx * dx + dz * dz > brush->radius * brush->radius)
                    continue;
            }
            res.cells++;
            const size_t i0 = static_cast<size_t>(z - vz0) * vw + (x - vx0);
            const size_t corner[4] = {i0, i0 + 1, i0 + vw, i0 + vw + 1}; // SW, SE, NW, NE
            int c[4];
            bool allSea = true;
            for (int k = 0; k < 4; k++)
            {
                c[k] = id[corner[k]];
                allSea = allSea && sea[corner[k]];
            }
            int slot = -1;
            if (allSea)
            {
                slot = 0;
            }
            else if (c[0] == c[1] && c[0] == c[2] && c[0] == c[3])
            {
                slot = slotFor(codes[c[0]]);
            }
            else
            {
                auto stemOf = [&](const int v[4]) { return codes[v[0]] + codes[v[1]] + codes[v[2]] + codes[v[3]]; };
                std::string stem = stemOf(c);
                bool found = exists(stem);
                if (!found)
                {
                    // nearly: one of its kinds taken for another one of them
                    for (int from = 0; from < 4 && !found; from++)
                        for (int to = 0; to < 4 && !found; to++)
                        {
                            if (c[from] == c[to])
                                continue;
                            int v[4];
                            for (int k = 0; k < 4; k++)
                                v[k] = c[k] == c[from] ? c[to] : c[k];
                            if (v[0] == v[1] && v[0] == v[2] && v[0] == v[3])
                                continue; // that would be no transition at all: last resort below
                            const std::string s = stemOf(v);
                            if (exists(s))
                            {
                                stem = s;
                                found = true;
                            }
                        }
                }
                if (found)
                {
                    slot = slotFor(stem);
                    if (slot >= 0)
                        res.transitions++;
                }
                if (slot == -1)
                {
                    // the commonest corner, plain
                    int best = 0, bestCount = 0;
                    for (int a = 0; a < 4; a++)
                    {
                        int count = 0;
                        for (int b = 0; b < 4; b++)
                            count += c[b] == c[a] ? 1 : 0;
                        if (count > bestCount)
                        {
                            bestCount = count;
                            best = a;
                        }
                    }
                    slot = slotFor(codes[c[best]]);
                    res.noTransition++;
                }
            }
            if (slot == -2)
            {
                res.tableFull++;
                continue;
            }
            if (slot < 0)
                continue; // the set has no such plain texture: left as it is
            if (tex[static_cast<size_t>(z) * size + x] != slot)
            {
                SetTex(x, z, static_cast<uint16_t>(slot));
                res.changed++;
            }
        }
    return res;
}

void Island::RenameTexture(int slot, const std::string& name)
{
    if (slot < 0 || slot >= kMaxTextures)
        return;
    textures.resize(kMaxTextures);
    textures[slot] = name;
}

void Island::ReplaceObjects(ObjectList&& now)
{
    if (_editing)
    {
        if (!_step.allObjects)
        {
            _step.allObjects = true;
            _step.objectsBefore = objects;
        }
        _step.objectsAfter = now;
    }
    objects = std::move(now);
    objectsRevision++;
}
} // namespace island
