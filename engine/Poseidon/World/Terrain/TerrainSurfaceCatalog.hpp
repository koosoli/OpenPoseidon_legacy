#pragma once

// The map's OWN clutter definitions, read from Arma-generation configs.
//
// Real Virtuality chooses ground clutter by SURFACE, through three config classes
// joined by two keys:
//
//   CfgSurfaces           files=<texture glob>  ->  character=<character class>
//   CfgSurfaceCharacters  names[] + probability[]  ->  clutter class names
//   CfgWorlds >> <world> >> class clutter        ->  model, scale, wind response
//
// Two things make this its own reader rather than an addition to the global
// `Pars` tree.
//
// First, `ParamFile::SerializeBin` rejects every Arma-generation `config.bin`:
// it reads the version word and refuses anything below 2, and those configs are
// version 0 (the modern container, with an enum-table offset at 0x0c). So none of
// this is visible to the engine today even though the data is sitting in mounted
// archives. `Asset::Config::ArmaRapReader` already reads that exact container --
// it is what parses Arma RVMATs -- so the format work is done.
//
// Second, and the reason this stays a SIDE table: merging a modern config into
// `Pars` would merge the whole thing, CfgVehicles and CfgAmmo included, into a
// running game's config tree. On an Arma 3 install that is 487 archives. This
// reads three classes and keeps them to itself.
//
// Operation Flashpoint content defines none of these classes -- measured zero
// occurrences of CfgSurfaceCharacters, `character=` or `class clutter` across the
// whole retail install and the Demo package -- so an OFP world simply gets an
// empty catalogue, which is the structural half of "no Arma grass on OFP".

#include <Poseidon/Foundation/Strings/RString.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Poseidon
{

class TerrainSurfaceCatalog
{
  public:
    // One clutter variety a surface character can grow.
    struct Clutter
    {
        RStringB name;   // the clutter class, e.g. "TK_GrassDry"
        RStringB model;  // its p3d, e.g. "ca\plants_E\Clutter\c_GrassDesert_GroupSoft_EP1.p3d"
        float probability = 0.0f;
        float scaleMin = 1.0f;
        float scaleMax = 1.0f;
        float affectedByWind = 0.0f;
    };

    // Parse one raP config and merge what it defines. Safe to call repeatedly:
    // Arma 3 splits CfgSurfaces/CfgSurfaceCharacters into map_data and the world's
    // own addon, so the catalogue is only complete once every relevant archive has
    // been offered. Returns false when the bytes are not a readable raP container.
    bool AddConfig(const std::vector<uint8_t>& bytes);

    // The character class a terrain surface texture resolves to, or empty.
    // `surfaceTexture` may be a full path; only the basename without extension is
    // matched, which is what the `files=` globs are written against.
    RStringB Character(const char* surfaceTexture) const;

    // Does the catalogue KNOW this surface at all, whatever it says about it?
    //
    // An empty `Character` has two very different meanings and callers must be
    // able to tell them apart: "no config we read describes this texture", where a
    // caller may reasonably guess from the name, and "the map declares it and says
    // `character = "Empty"`", i.e. this surface grows nothing -- which is the
    // majority, 8 of Takistan's 14 surfaces. Guessing over the second is how sand
    // grows grass.
    bool Declares(const char* surfaceTexture) const;

    // The clutter list for a character class, empty when unknown, with each entry's
    // model resolved. By value because the join happens here: a character names its
    // clutter classes, and those classes are defined on the world -- on Arma 3 in a
    // DIFFERENT archive (map_data's CAWorld) from the world that inherits them, so
    // neither order of arrival can be assumed and the two halves are only married at
    // lookup. Entries whose class was never defined keep an empty model rather than
    // being dropped, so a missing archive is visible instead of silent.
    std::vector<Clutter> ClutterFor(const char* character) const;

    // Sum of a character's probabilities. Deliberately NOT normalised: the authored
    // numbers do not sum to one (Takistan's grass character totals 0.94, its desert
    // character 0.12) because the remainder is bare ground. That is coverage, and it
    // is what makes one surface read as a field and another as scree.
    float Coverage(const char* character) const;

    bool Empty() const { return _surfaces.empty() && _characters.empty(); }
    size_t SurfaceCount() const { return _surfaces.size(); }
    size_t CharacterCount() const { return _characters.size(); }
    size_t ClutterCount() const { return _clutter.size(); }

  private:
    struct Surface
    {
        RStringB files;     // the glob, e.g. "tk_trava_*"
        RStringB character; // may be empty: most surfaces grow nothing
    };
    struct CharacterEntry
    {
        RStringB name;
        std::vector<Clutter> clutter;
    };

    std::vector<Surface> _surfaces;
    std::vector<CharacterEntry> _characters;
    // Clutter classes by name, before they are attached to a character. The world's
    // `class clutter` and the characters that reference it can arrive from different
    // archives and in either order, so resolution is deferred to lookup time.
    std::vector<Clutter> _clutter;
};

} // namespace Poseidon
