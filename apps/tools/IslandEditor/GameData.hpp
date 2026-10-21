#pragma once

// The game folder the island belongs to (Cold War Assault / Operation Flashpoint or
// Open Malprave): its archives are mounted so ground textures and model names can be
// read straight from them, without unpacking anything.

#include "Island.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace island
{
struct Image
{
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    bool Valid() const { return width > 0 && height > 0 && !rgba.empty(); }
};

// The plain textures of one folder that follows the game's naming (two letters each,
// eight for the transitions between them): what the auto texture tool works with.
struct TextureSet
{
    struct Plain
    {
        std::string code; // "pi"
        float rgb[3] = {0.5f, 0.5f, 0.5f}; // its average colour, once looked up
    };
    std::string folder; // "eden\\"
    std::vector<Plain> plains;
    int transitions = 0;
    bool coloursKnown = false;
    int stockState = 0; // 0 not looked at, 1 learnt (stock holds the roles), 2 nothing to learn from
    AutoTextureRules stock;
};

// What the editor needs to know of a model (.p3d).
struct ModelShape
{
    bool valid = false;
    float bmin[3] = {}, bmax[3] = {};   // its box, in the model's own space
    float centre[3] = {};               // the point the game puts at the object's position
    // a road piece: the corners where the next pieces join (left / right, begin / end)
    bool road = false;
    float lb[3] = {}, pb[3] = {}, le[3] = {}, pe[3] = {};
    // the triangles of its first level of detail, for drawing: x y z per corner
    std::vector<float> triangles;
};

// One level of detail of a model as it is drawn: its triangles, by texture.
struct ModelLod
{
    struct Vertex
    {
        float pos[3], normal[3], uv[2];
        // how it sits on the land: 0 as the model has it, 1 on the ground itself, 2 at its own
        // height above the ground under it (trees of a forest block on a slope)
        uint8_t land = 0;
    };
    struct Part
    {
        std::string texture; // lower case; empty = none
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices; // three to the triangle
    };
    std::vector<Part> parts;
    int vertices = 0, triangles = 0;
    bool Empty() const { return parts.empty(); }
};

struct ModelMesh
{
    bool valid = false;
    ModelLod fine, coarse; // coarse is empty when the model has one level only
    bool onSurface = false; // every corner lies on the ground: a road
    float centre[3] = {};   // as ModelShape::centre
};

class GameData
{
  public:
    // Mounts every archive of DTA, AddOns and Res under `folder`.  False when none is found.
    bool Mount(const std::string& folder);
    const std::string& Folder() const { return _folder; }
    int ArchiveCount() const { return _archives; }

    // "landtext\pi.pac" as the island names it: from the archives, or a loose file under the folder
    Image LoadTexture(const std::string& name) const;
    // "data3d\silnice25.p3d": read once, then kept.  Never null; `valid` says whether it was found.
    const ModelShape& Model(const std::string& name) const;
    // "ODOL" (binarized), "MLOD" (not), or empty when it cannot be read
    std::string ModelFormat(const std::string& name) const;
    // the same model for drawing; read with it
    const ModelMesh& Mesh(const std::string& name) const;
    // A folder whose sub-folders are looked in first (an addon being made, not packed yet).
    void AddLooseFolder(const std::string& folder);
    // every texture and every model the archives hold, lower case, sorted
    const std::vector<std::string>& Textures() const { return _textures; }
    const std::vector<std::string>& Models() const { return _models; }

    // The texture sets found in the archives.
    std::vector<TextureSet>& Sets() { return _sets; }
    // the set most of the island's cells use, -1 when none of them does
    int SetOf(const Island& is) const;
    // fills in the average colours of a set's plain textures (reads them once)
    void LoadSetColours(TextureSet& set) const;
    // which plain texture is beach, grass, rock ...: by colour.  Only a first guess.
    static void GuessRoles(const TextureSet& set, AutoTextureRules& rules);
    // The same from how an island uses the set's plain textures: at what heights, on what
    // slopes, under forests.  False when the island does not use enough of the set to tell.
    // `report` (optional) gets one line per texture.
    static bool LearnRoles(const Island& is, const TextureSet& set, AutoTextureRules& rules, std::string* report = nullptr);
    // The best there is for `is`: learnt from the island itself, else from the game's own
    // island of that set (Worlds\eden.wrp for "eden\\"), else guessed by colour.
    // Returns how: 0 learnt from the island, 1 from the game's island, 2 by colour.
    int RolesFor(const Island& is, TextureSet& set, AutoTextureRules& rules);

    // Any island file: 4WVR / 3WVR, or the binarized OPRW the game ships.
    static bool LoadIsland(const std::string& file, Island& out, std::string& error);

  private:
    std::string _folder;
    int _archives = 0;
    std::vector<std::string> _textures, _models;
    std::vector<std::string> _loose;
    std::vector<TextureSet> _sets;
    bool ReadFile(const std::string& name, std::vector<char>& bytes) const;
    mutable std::map<std::string, ModelShape> _models3d;
    mutable std::map<std::string, ModelMesh> _meshes;
};
} // namespace island
