#pragma once

// The island being edited: a square grid of elevations (one per cell corner), one
// ground texture per cell, the texture table and the objects standing on it.
//
// World layout as the game has it: x east, z north, y up; cell (x, z) spans
// [x*cell, (x+1)*cell] x [z*cell, (z+1)*cell] and its elevation point is its
// south-west corner.  Sea level is 0.
//
// Saved as 4WVR, which Cold War Assault / Operation Flashpoint and Open Malprave
// load directly: elevations as 16-bit steps of 4.5 cm, at most 512 textures.

#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

namespace island
{
constexpr float kHeightStep = 0.045f;                  // one unit of a stored elevation
constexpr float kMaxHeight = 32767 * kHeightStep;      // 1474.5 m
constexpr float kMinHeight = -32768 * kHeightStep;
constexpr int kMaxTextures = 512;
constexpr int kTextureNameLen = 32;                    // in the file, with the terminator
constexpr int kObjectNameLen = 76;

struct Object
{
    int id = 0;
    std::string model; // e.g. data3d\dum01.p3d
    // as in the file: aside, up, direction (the columns of its orientation), then position
    float m[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    // Height above the ground under it.  The position's y is worked out from this when
    // the island is saved or drawn, so objects follow the terrain as it is shaped.
    float above = 0;
    // A bridge: it keeps its height above the sea (m[10]) whatever the ground does.
    bool fixed = false;

    float X() const { return m[9]; }
    float Z() const { return m[11]; }
};

// The objects of an island, in one block as a vector holds them. The engine library this program links replaces
// operator new with its own allocator, which gives out at most 256 MiB in one piece (a sanity limit for the game):
// about 2.7 million objects. Arma 2's Chernarus has a million, DayZ's ChernarusPlus three, and a list near the limit
// could be read but not grown. The list takes its block from the system instead, so its size is the machine's.
template <class T> struct SystemAllocator
{
    using value_type = T;
    SystemAllocator() = default;
    template <class U> SystemAllocator(const SystemAllocator<U>&) {}
    T* allocate(size_t n)
    {
        // (malloc(0) may give null without being out of memory: ask for a byte)
        void* p = n <= static_cast<size_t>(-1) / sizeof(T) ? std::malloc(n ? n * sizeof(T) : 1) : nullptr;
        if (!p)
            throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, size_t) { std::free(p); }
    template <class U> bool operator==(const SystemAllocator<U>&) const { return true; }
    template <class U> bool operator!=(const SystemAllocator<U>&) const { return false; }
};
using ObjectList = std::vector<Object, SystemAllocator<Object>>;

enum class Falloff
{
    Smooth,
    Linear,
    Sharp,
    Flat
};

struct Brush
{
    float radius = 150.0f; // metres
    float strength = 0.5f; // 0..1, what it means depends on the tool
    Falloff falloff = Falloff::Smooth;
    // weight 0..1 at `dist` metres from the centre
    float Weight(float dist) const;
};

// Texturing by rule (the "auto texture" tool): which plain texture of a set goes where.
// A set is a folder of ground textures named the game's way: two letters for a plain
// texture ("pi.paa"), eight for a transition between up to four of them, one pair per
// corner of the cell in the order south-west, south-east, north-west, north-east.
struct AutoTextureRules
{
    std::string folder; // "eden\\" (with the separator)
    // codes of the set's plain textures; empty = not used
    std::string beach, shore, grass, grassAlt, hill, rock, peak, forest;
    bool seaTexture = false;  // cells wholly below seaBelow get the sea texture (slot 0); the
    float seaBelow = -6.0f;   // game's own islands carry the beach on over the sea bed instead
    float beachTop = 2.5f;    // beach up to here
    float shoreTop = 8.0f;    // shore texture up to here (when the set has one)
    float hillFrom = 120.0f;  // hill texture above this
    float peakFrom = 320.0f;  // peak texture above this
    float rockSlope = 28.0f;  // degrees: steeper ground is rock
    float wobble = 0.5f;      // 0..1: how much the borders wander instead of following contours
    float patches = 0.35f;    // 0..1: share of the grass that is the alternative grass
    bool forestFloor = true;  // forest texture under forest blocks
    bool stepThrough = true;  // where two kinds have no transition, put the ones between them in
    unsigned seed = 1;
    // hills and peaks at a share of the island's height
    void FitHeights(float highest)
    {
        hillFrom = highest > 40.0f ? highest * 0.45f : 1e9f;
        peakFrom = highest > 40.0f ? highest * 0.75f : 1e9f;
    }
};

struct AutoTextureResult
{
    int cells = 0;        // cells looked at
    int changed = 0;
    int transitions = 0;  // cells that got a transition texture
    int noTransition = 0; // mixed cells the set has no transition for (the commonest corner was used)
    int tableFull = 0;    // cells left alone because all 512 slots are taken
};

class Island
{
  public:
    int size = 0;       // cells per side (a power of two, 16..1024)
    float cell = 50.0f; // metres
    std::vector<float> height;         // size * size, index z * size + x
    std::vector<uint16_t> tex;         // texture slot of each cell
    std::vector<std::string> textures; // the table: kMaxTextures slots, empty = unused; slot 0 is the sea
    ObjectList objects;
    std::string path;         // where it was loaded from / last saved to
    std::string sourceFormat; // "4WVR", "OPRW v3" ...
    // Binarized islands may give several objects one id; the plain format may not.  This many
    // were given new ids when the island was read (the same ones the game gives them).
    int renumbered = 0;
    bool modified = false;
    // goes up whenever the objects change in any way: what draws them watches it
    unsigned objectsRevision = 0;

    // ---- making and reading ---------------------------------------------------
    void New(int cells, float baseHeight, const std::string& groundTexture);
    // 4WVR / 3WVR.  The binarized formats (OPRW) are read by the application, which has
    // the game's decompression at hand, and handed over through Adopt.
    bool LoadRaw(const std::string& file, std::string& error);
    void Adopt(int cells, std::vector<float>&& heights, std::vector<uint16_t>&& texIndices,
               std::vector<std::string>&& textureTable, ObjectList&& objs, const std::string& file,
               const std::string& format, float cellSize = 50.0f);
    // Always 4WVR.  An existing file is kept beside it as <name>.bak first.
    bool Save(const std::string& file, std::string& error);

    // ---- reading the terrain --------------------------------------------------
    bool InRange(int x, int z) const { return x >= 0 && z >= 0 && x < size && z < size; }
    float H(int x, int z) const; // clamped at the edges
    float WorldSize() const { return size * cell; }
    // the ground as the game draws it: two triangles per cell, split from (x+1, z) to (x, z+1)
    float SurfaceY(float wx, float wz) const;
    void Normal(int x, int z, float n[3]) const;
    float MinHeight() const;
    float MaxHeight() const;
    float ObjectY(const Object& o) const { return o.fixed ? o.m[10] : SurfaceY(o.X(), o.Z()) + o.above; }
    // where a ray from `origin` along `dir` (unit) first meets the ground; false if it never does
    bool Raycast(const float origin[3], const float dir[3], float maxDist, float hit[3]) const;

    // ---- editing --------------------------------------------------------------
    // Changes between BeginEdit and EndEdit are one step for Undo.
    void BeginEdit(const char* what);
    void EndEdit();
    bool Editing() const { return _editing; }
    void SetHeight(int x, int z, float y);
    void SetTex(int x, int z, uint16_t slot);
    int AddObject(const Object& o); // returns its index
    void RemoveObject(int index);
    void ChangeObject(int index, const Object& o);
    int NewObjectId() const;
    // slot of `name` in the table, adding it if there is room; -1 when the table is full
    int TextureSlot(const std::string& name);
    int UsedTextures() const;

    bool CanUndo() const { return !_undo.empty(); }
    bool CanRedo() const { return !_redo.empty(); }
    const char* UndoName() const { return _undo.empty() ? "" : _undo.back().what.c_str(); }
    const char* RedoName() const { return _redo.empty() ? "" : _redo.back().what.c_str(); }
    void Undo();
    void Redo();

    // The cells whose elevations or textures changed since the last call (for redrawing):
    // false when nothing did.
    bool TakeDirty(int& x0, int& z0, int& x1, int& z1);
    void MarkAllDirty();

    // ---- tools (call between BeginEdit / EndEdit) -----------------------------
    void Raise(float wx, float wz, const Brush& b, float metres);              // + up, - down
    void Smooth(float wx, float wz, const Brush& b, float amount);             // 0..1 per call
    void Flatten(float wx, float wz, const Brush& b, float target, float amount);
    void Noise(float wx, float wz, const Brush& b, float metres, float scale, unsigned seed);
    void Erode(float wx, float wz, const Brush& b, float amount);              // steep slopes slump
    void Paint(float wx, float wz, const Brush& b, uint16_t slot);
    // a straight incline of the brush's width from one point to another, at their heights
    void Ramp(float x0, float z0, float y0, float x1, float z1, float y1, const Brush& b);
    // whole island
    void SmoothAll(int passes);
    void ScaleAll(float factor);
    void OffsetAll(float metres);
    // fractal hills falling away to the sea at the edges
    void Generate(unsigned seed, float peak, float roughness, float seaDepth);
    void ReplaceTexture(uint16_t from, uint16_t to);

    // 16-bit greyscale PGM (P5), north at the top: 0 = lowest, 65535 = highest of the range given
    bool ExportHeightmap(const std::string& file, float& low, float& high, std::string& error) const;
    // Elevations from a picture of any size, stretched over the whole island, north at the
    // top: `values` are 0..1 and stand for low..high metres, or are metres already.
    void ApplyHeights(const float* values, int w, int h, float low, float high, bool metres);

    // `available`: every texture file there is (lower case, sorted) - the game's list.
    // With a brush only the cells under it are textured.
    AutoTextureResult AutoTexture(const AutoTextureRules& rules, const std::vector<std::string>& available,
                                  const Brush* brush = nullptr, float wx = 0, float wz = 0);
    void RenameTexture(int slot, const std::string& name);
    // every object at once (one undo step however many change)
    void ReplaceObjects(ObjectList&& now);

  private:
    struct CellChange
    {
        int index;
        float before, after;
    };
    struct TexChange
    {
        int index;
        uint16_t before, after;
    };
    struct ObjectChange
    {
        enum Kind
        {
            Added,
            Removed,
            Changed
        } kind;
        int index;
        Object before, after;
    };
    struct Step
    {
        std::string what;
        std::vector<CellChange> heights;
        std::vector<TexChange> texs;
        std::vector<ObjectChange> objs;
        std::vector<std::string> texturesBefore; // the table, when the step changed it
        std::vector<std::string> texturesAfter;
        bool allObjects = false; // ReplaceObjects: the whole list before and after
        ObjectList objectsBefore, objectsAfter;
        bool Empty() const { return heights.empty() && texs.empty() && objs.empty() && texturesBefore.empty() && !allObjects; }
    };
    void Apply(const Step& s, bool forward);
    void Touch(int x, int z);
    int UniqueObjectIds();
    void Reset();

    std::vector<Step> _undo, _redo;
    Step _step;
    bool _editing = false;
    std::vector<int> _stepCell; // per elevation point: 1 + its place in _step.heights, 0 = not yet in the step
    std::vector<int> _stepTex;
    std::vector<std::string> _texturesAtBegin;
    int _dx0 = 1 << 30, _dz0 = 1 << 30, _dx1 = -1, _dz1 = -1;
};

// "landtext\pi.pac" and "LandText\PI.PAC" are the same file
std::string LowerPath(const std::string& s);
} // namespace island
