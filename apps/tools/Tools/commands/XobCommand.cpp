#include "XobCommand.hpp"

#include <Poseidon/Asset/Formats/Enfusion/EbinWorld.hpp>
#include <Poseidon/Asset/Formats/Enfusion/PakArchive.hpp>
#include <Poseidon/Asset/Formats/Enfusion/ResourceDatabase.hpp>
#include <Poseidon/Asset/Formats/Enfusion/XobCollision.hpp>
#include <Poseidon/Asset/Formats/Enfusion/XobCollisionGeometry.hpp>
#include <Poseidon/Asset/Formats/Enfusion/XobModel.hpp>
#include <Poseidon/Asset/Formats/Material/EmatMaterialAdapter.hpp>
#include <Poseidon/Asset/Formats/Material/EmatSource.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODLoader.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODWriter.hpp>
#include <Poseidon/Graphics/Textures/EddsReader.hpp>
#include <Poseidon/Graphics/Textures/Image.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/Graphics/Textures/PAAEncoder.hpp>
#include <Poseidon/Graphics/Textures/PixelFormat.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ARF-003 -- Arma Reforger's `.xob` render geometry into this engine's model
// pipeline, and its worlds' object placements into a manifest a world exporter can
// consume.
//
// Nothing here invents a format. The read side is XobModel.hpp (15,809 / 15,809
// files close their byte accounting), PakArchive.hpp (16 / 16 archives close) and
// EbinWorld.hpp (103 / 103 `.ent` files close, 1,230,897 placements on Everon); the
// write side is MLODWriter.hpp. This command is the wiring between them plus the
// three things that wiring needs and none of them provide: which LOD to take, which
// winding to write it in, and which model a placement's prefab GUID refers to.
//
// Three measured facts decide those, and each one is a mistake that a parse-success
// check does not catch:
//
//  1. LOD ORDER. XobModel.hpp's header comment says the coarsest LOD sits first and
//     thresholds "run strictly descending, LOD0 always 0.5". The descending part is
//     right and the coarsest-first part is not: measured on
//     Assets/Vegetation/Tree/Picea_Abies/t_picea_abies_3f.xob the five LODs run
//     0.5 / 0.15 / 0.02 / 0.005 / 0.001 with 3,216 / 1,188 / 336 / 98 / 6 triangles,
//     so threshold 0.5 is the FINEST and it is index 0. Selecting the minimum
//     threshold -- which "coarsest first" invites -- yields the 6-triangle billboard
//     and every check short of looking at the triangle count still passes. The
//     converter therefore orders every graphical LOD by DESCENDING threshold and
//     emits the full ladder, rather than collapsing the source to either extreme.
//
//  2. WINDING. Measured over the whole of LOD0 on three corpus models (1,500 + 457 +
//     3,216 triangles): the cross product of the first two edges of an `.xob` index
//     triple points ALONG the mean stored vertex normal, unanimously (1.0000 in all
//     three). ODOLLoader.hpp:206-215 records the opposite convention as measured for
//     ODOL -- cross opposite the stored normal -- and the ODOL loader does not
//     reverse winding on load while MLODLoader.hpp:384 does. So an MLOD file whose
//     on-disk cross points ALONG the normal reaches the IR with the same handedness
//     an ODOL file does, and `.xob` triples are already in that order. They are
//     therefore written VERBATIM at every graphical LOD. `--flip-winding` exists because the ODOL premise
//     rests on a packed-normal sign no render has confirmed, not because anything
//     measured here is uncertain.
//
//     THAT PREMISE IS NOW MEASURED, AND IT IS WRONG IN AN ABSOLUTE SENSE. See fact 6.
//
//  6. ORIENTATION IS INVERTED, BOTH HALVES OF IT. Everything fact 2 says is a
//     RELATIVE measurement -- the cross of an edge pair against the model's own
//     stored normal. Relative agreement is preserved by inverting BOTH, so it cannot
//     detect an inversion of both, and that is exactly what the corpus has.
//
//     The absolute test the programme never ran: does a stored normal point AWAY from
//     the model's centroid? On a convex closed solid the answer is unambiguous.
//     Measured on the written tree (`reforger/everon/models`), LOD0, mean of the three
//     corner normals against the face centroid minus the point-cloud centroid:
//
//       graniteboulder_01.p3d   5,156 faces   5,156 outward   0 inward
//       graniteboulder_02.p3d   3,732 faces   3,732 outward   0 inward
//       granitebeachstone_01    3,874 faces   3,874 outward   0 inward
//       granitebeachstone_02    3,526 faces   3,526 outward   0 inward
//       granitebeachstone_03    2,298 faces   2,298 outward   0 inward
//
//     `.xob` normals are OUTWARD. This engine's are INWARD, and three independent
//     places say so: `MeshBuild.cpp:43-44` negates the Shape normal on the way to the
//     GPU and `gfx3d/shader3d.wgsl:226` declares that result "world space, outward";
//     `MLODLoader.hpp:384` and `ShapeAdapter.cpp:230` copy an MLOD normal verbatim, so
//     the on-disk sign IS the Shape sign; and `Odol73.hpp:1004` decodes ODOL with a
//     deliberate NEGATIVE scale whose comment says dropping it "would flip every normal
//     in the model". `UnpackXobDirection` (XobModel.hpp:115-117) uses a POSITIVE one.
//
//     So both halves are inverted, and they are inverted TOGETHER, which is why every
//     relative check still closes:
//       * the GPU normal ends up pointing INTO the surface, so `ndotl` collapses to 0
//         on every sunlit face and lifts on faces turned away from the sun. Exteriors
//         go flat/ambient, interiors light up.
//       * the on-disk cross points outward, `MLODLoader.hpp:384`'s swap makes the IR
//         cross point INWARD -- the reverse of the native convention -- so with
//         `cull_mode: Back` the FRONT faces are culled and a rock shows only the inside
//         of its far shell.
//
//     `--fix-orientation` (env `XOB_FIX_ORIENTATION=1`) applies BOTH corrections at
//     once: it negates the written normal and swaps corners 0/1. Applying either alone
//     trades one wrong for the other, which is why it is one flag and not two, and why
//     `--flip-winding` on its own is NOT the fix.
//
//     Default OFF. The evidence above is strong but the "Shape normals are inward"
//     anchor is an inference from three code sites, not from a render, and turning it
//     on rewrites every model in the tree. Re-export the rocks alone and A/B them.
//
//     The measurement itself is no longer optional: every conversion now reports
//     `Orientation:` with the outward/inward split, so the number that would have
//     caught this is in the log of every run.
//
//  7. THE MODEL ORIGIN IS MOVED TO THE BOUNDING-BOX CENTRE, AND THAT IS THE SINK.
//     A converted `.p3d` carried no named properties at all (until facts 7 and 8 below
//     started writing `autocenter` and `map`), and `autocenter` is one the engine
//     looks for. Absent, it defaults ON in BOTH places that implement it:
//     `Model.cpp:64-108` subtracts the bbox centre from every vertex of every LOD (and
//     records the offset nowhere -- `ShapeAdapter.cpp:723-745` restores
//     `_boundingCenter` only in the `sourceFormat == "ODOL"` branch), and
//     `ShapeLOD.cpp:413-441` does the same again from `_autoCenter`, which
//     `ShapeLOD.cpp:1198-1202` clears only when the property says so.
//
//     The placement is not compensated for it. `LandSave.cpp` passes
//     `preserveAuthoredElevation = true` for OPRW25, so `Landscape.cpp:2650-2662` --
//     the one place that puts the AUTHORED origin (`PositionModelToWorld(-BoundingCenter())`)
//     on the surface -- does not run. So the world transform positions the recentred
//     vertex space, and the model drops by its own bbox-centre Y.
//
//     Measured on the written tree, LOD0 point table, Y only:
//
//       shophouse_e_2i01t.p3d   min -5.454  max +15.115  centre +4.831
//       house_prefab_2i02t.p3d  min -4.167  max +11.591  centre +3.712
//       farmhouse_e_1l01.p3d    min -4.504  max +11.254  centre +3.375
//       garage_e_01.p3d         min -1.207  max  +3.343  centre +1.068
//       doghouse_01.p3d         min -0.000  max  +0.939  centre +0.470
//       glass_greenhouse_01_*   min -0.220  max  +0.220  centre +0.000
//       birdhouse_01.p3d        min -0.215  max  +0.226  centre +0.005
//
//     The negative minima are foundation skirts, which is what an origin at ground
//     level looks like -- and the measured placements agree: over 304,035 sampled
//     `ev_DALL.wrp` objects the median of (object Y - terrain Y) is -0.004 m. The
//     authored Y already puts the ORIGIN on the ground, so recentring buries a house
//     by 3-5 m and leaves a birdhouse alone. That is "lots of buildings are partly in
//     the ground", including which ones.
//
//     `--fix-origin` (env `XOB_FIX_ORIGIN=1`) writes `autocenter=0` on every LOD.
//     Both implementations honour it, and `ShapeLOD.cpp:1203`'s second `CalculateMinMax`
//     runs the `-_boundingCenter` branch, so a shape that was already recentred is put
//     back. Default OFF, and independent of `--fix-orientation`: they are separate
//     defects and each deserves its own A/B.
//
//  8. THE MODEL HAS NO CLASS, SO THE ENGINE GUESSES ONE FROM ITS NAME. `map` is the
//     named property Real Virtuality classifies a static object by (`ResolveMapTypeProperty`,
//     ShapeLOD.cpp), and that class is not a map symbol only: it is the gate for wind
//     sway and canopy shading (Object.cpp GCurrentFoliageKind, EngineWgpu
//     WGR_INSTANCE_CANOPY_*). Absent, the engine falls back to MapHide unless the path
//     says `vegetation` and the basename says `t_`/`b_`. The converter KNOWS the class --
//     it is in the pak path -- so it now writes it (`MapPropertyForXob`: rock / tree /
//     bush / house / wall / hide) on every LOD, next to `autocenter`. Default ON,
//     `--no-map-property` for the A/B. The engine-side proof is `WGR_SWAY_GATE_DUMP=2`:
//     one `SwayGate: kind=... property='...' model=...` line per model whose gate is
//     non-zero, and a granite model in that list is the bug.
//
//  9. THE MODEL HAS NO GEOMETRY LOD, SO THE PLAYER WALKS THROUGH IT (COL-001). Every
//     converted `.p3d` carried visual LODs only, and the collision log said so for
//     every Everon model: `collision: model=... geometry lod=-1 ... mass=0.0
//     passable=1`. The engine collides with a Geometry LOD (resolution 1e13) whose
//     faces form convex `ComponentXX` selections, and skips the object entirely when
//     its `#Mass#` total is under 10 kg (Object::IsPassable).
//
//     The `.xob` carries the answer in its COLL chunk -- the authored physics
//     shapes, decoded in XobCollision.hpp: boxes, capsules, cylinders, spheres, convex
//     hulls and triangle meshes, each on a layer preset ("Building", "Tree",
//     "FireView", "Foliage", ...). Of the 1,132 models Everon places, 1,074 carry a
//     shape on a character-blocking layer; the 58 that do not are decals, posters,
//     litter, bushes and plants -- walk-through in Reforger as well, so they get no
//     geometry LOD here either, on purpose. XobCollisionGeometry.hpp turns the
//     blocking shapes into components: primitives and hulls one each, meshes
//     decomposed (near-convex pieces whole, the rest as per-panel slabs, so a house
//     stays enterable). Mass is by class: house 5000, wall 2000, rock 1000, tree 500,
//     bush 200, everything else 1000 kg. Default ON, `--no-geometry` for the A/B; the
//     model line reports `geo <N> comps` and the engine-side proof is
//     `WGR_COLLISION_STATS=1` showing `passable=0` on the Everon houses.
//
//  3. PREFAB INHERITANCE. A world placement names a prefab GUID, not a model. The
//     `.et` prefab it resolves to often carries no mesh of its own:
//     Prefabs/Rocks/Granite/Granite_BeachCluster_02_Fucus.et -- the single most
//     placed prefab on Everon at 71,319 instances -- only overrides two materials
//     and inherits its `Object` from Granite_BeachCluster_02.et. Stopping at the
//     first `.et` loses the corpus's largest object outright.
//
//  4. WHERE THE ALBEDO GOES. A `.xob` part names an `.emat`, and an `.emat` is a
//     shader description, not an image. Written into the MLOD MATERIAL field alone it
//     resolves to nothing: `ParseRvMaterialFile` cannot read an `.emat`, so
//     `ResolveMaterialBaseColour` caches the miss, the face texture is empty, and the
//     section reaches the GPU with `texture_id == 0` -- the "section draws UNTEXTURED
//     (fallback=none)" warning, and a white tree.
//
//     `--textures` closes that chain in the oldest and bluntest way the format has:
//     the material's `BCRMap` is decoded out of the `.pak`, folded with its
//     `OpacityMap`, written as a `.paa` and named in the MLOD TEXTURE field. That is
//     the field `ShapeAdapter` hands to `GlobLoadTexture`, so no engine change is
//     needed and no material file has to exist for the albedo to bind.
//
//     The alpha is not a detail. `ClassifyGpuSection` takes a section's cutout
//     decision from the FACE TEXTURE's alpha class and from nothing else, so a leaf
//     card whose albedo arrives through the material slot with no face texture is
//     drawn as an opaque rectangle -- visibly worse than the white it replaced. The
//     `.paa` is therefore DXT5 with its top mip thresholded to 0/255, which is what
//     `ClassifyAlpha` needs to answer Cutout (partial alpha under 2%) rather than
//     Blend. DXT1 is used when the material names no `OpacityMap`, because a DXT1
//     PAA reports `IsAlpha() == false` and short-circuits to Opaque.
//
//  5. WHY THE FACE TEXTURE IS NOT ENOUGH. A face texture is ONE image, and it is the
//     only thing `--textures` alone can deliver. Everything else a surface has --
//     first of all its normal map -- reaches the renderer through a MATERIAL, and the
//     material every section named was that same unreadable `.emat`. So Everon
//     rendered with zero normal maps and zero specular maps on every object in the
//     world; what looked textured was albedo and nothing else.
//
//     `--normals --material-rvmat` closes that: the `NMOMap` is decoded, repacked into
//     the DXT5nm convention the renderer samples (`vec2(sample.a, sample.g)`), written
//     as a `.paa`, and named in a small Super `.rvmat` written beside it -- which is
//     what the section then names instead of the `.emat`. The engine change that would
//     otherwise be needed is none: `Super` Stage1 is already the measured normal slot.
//
//     Two things that look like details and are not. The source packing is DETECTED,
//     not assumed -- the corpus contains both BC5-style (X in red) and DXT5nm-style
//     (X in alpha) normals, and binding one as the other lights every surface wrongly
//     without ever erroring. And the layer maps of a `MatPBRMulti` are excluded from
//     the normal slot on purpose: `NMO_1..4` are shared one-metre library tiles in
//     their own tiled UV frame, not the object's own normal, which is `GlobalNMOMap`.
//
//     Since 2026-08-16 the material slot names the `.emat` ITSELF, deployed: `--textures`
//     writes each baked material, parent chain folded in and its texture keys rewritten
//     to the `.paa` files above, at `<texture prefix>\<source path lower-cased>.emat`
//     (`TextureBaker::WriteEmat`, default on, `--no-material-emat` to disable). The
//     engine reads an `.emat` as a material already (LoadTranslatedMaterial); it could
//     only never OPEN one, because the path was inside a `.pak`. The engine also looks
//     under that same prefix when a raw `Assets/...emat` path fails (`EmatDeployRoots`),
//     so the models already installed pick the deployed file up without a re-export.
//     The `.rvmat` route is kept for the A/B and is otherwise superseded.
//
//     BCR alpha, once and for all: it is ROUGHNESS. Measured 99.8% partial alpha on an
//     opaque prop's MLOD BCR and 100% partial on a grass polyplane BCR -- no cutout is
//     100% partial. Foliage opacity is the `OpacityMap` (463 of 468 MatPBRTreeCrown files
//     name one once inheritance is resolved), and it is folded into the `_ca` alpha here.
//
// The whole-model bounding box in metres is reported for every conversion because a
// unit-scale mistake is the failure mode that survives every structural check: the
// geometry parses, round-trips and draws, at 100x the size.

using namespace Poseidon::Asset::Formats::Enfusion;
namespace Fmt = Poseidon::Asset::Formats;

namespace PoseidonTools
{
namespace
{

// ---------------------------------------------------------------------------
// Small text helpers
// ---------------------------------------------------------------------------

std::string LowerCopy(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool EndsWith(const std::string& text, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}

bool IsHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

//! Enfusion writes asset ids as 16 uppercase hex digits. Everything that keys on one
//! -- the world's placements, an `.et`'s `ID` line, a `{GUID}path` reference -- has
//! to agree on the case or the maps silently miss.
std::string UpperGuid(const char* begin, size_t length)
{
    std::string out(begin, length);
    for (char& c : out)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// ---------------------------------------------------------------------------
// Every `.pak` under a root, opened once, with a case-insensitive path index
// ---------------------------------------------------------------------------

class PakSet
{
  public:
    bool Open(const std::string& root)
    {
        std::vector<std::string> paths;
        std::error_code ec;
        if (std::filesystem::is_regular_file(root, ec))
        {
            paths.push_back(root);
        }
        else
        {
            for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec))
            {
                if (ec)
                    break;
                if (it->is_regular_file(ec) && LowerCopy(it->path().extension().string()) == ".pak")
                    paths.push_back(it->path().string());
            }
        }
        std::sort(paths.begin(), paths.end());
        if (paths.empty())
        {
            _error = "no .pak files under " + root;
            return false;
        }

        // Sized up front: the index holds archive/entry indices rather than
        // pointers, so a later push_back could not invalidate it, but the archives
        // still have to exist before anything indexes into them.
        _archives.resize(paths.size());
        for (size_t i = 0; i < paths.size(); ++i)
        {
            if (!_archives[i].Open(paths[i]))
            {
                std::cerr << "  cannot open " << paths[i] << ": " << _archives[i].Error() << "\n";
                continue;
            }
            const auto& entries = _archives[i].Entries();
            for (size_t e = 0; e < entries.size(); ++e)
                _index.emplace(LowerCopy(entries[e].path), std::pair<size_t, size_t>{i, e});
        }
        if (_index.empty())
        {
            _error = "no readable entries under " + root;
            return false;
        }
        return true;
    }

    size_t ArchiveCount() const { return _archives.size(); }
    size_t EntryCount() const { return _index.size(); }
    const std::string& Error() const { return _error; }
    const std::vector<PakArchive>& Archives() const { return _archives; }

    bool Read(const std::string& virtualPath, std::vector<uint8_t>& out) const
    {
        const auto it = _index.find(LowerCopy(virtualPath));
        if (it == _index.end())
            return false;
        const PakArchive& archive = _archives[it->second.first];
        return archive.Read(archive.Entries()[it->second.second], out);
    }

    bool Has(const std::string& virtualPath) const { return _index.count(LowerCopy(virtualPath)) != 0; }

    //! The stored spelling of a path, which is what a virtual path should be derived
    //! from -- the index is lowercased and the archives are not.
    const std::string* StoredPath(const std::string& virtualPath) const
    {
        const auto it = _index.find(LowerCopy(virtualPath));
        if (it == _index.end())
            return nullptr;
        return &_archives[it->second.first].Entries()[it->second.second].path;
    }

  private:
    std::vector<PakArchive> _archives;
    std::unordered_map<std::string, std::pair<size_t, size_t>> _index;
    std::string _error;
};

// ---------------------------------------------------------------------------
// GUID -> resource path
// ---------------------------------------------------------------------------

// Two independent sources, exactly as the Python reference census established them:
//
//   * every `{<16 hex>}<path>` reference in any text-ish resource names ANOTHER
//     resource, which is the only source that covers non-text assets such as `.xob`;
//   * every `.et`'s own `ID "<16 hex>"` line.
//
// They are kept in SEPARATE maps and references are consulted first, which the
// reference implementation could not do because it merged them into one dictionary
// with order-dependent precedence. The distinction is real: on
// Prefabs/Rocks/Granite/Granite_BeachCluster_02_Fucus.et the `ID` line reads
// 50D67C36F5DCBE17 while the world places it as {5A403FBC1467033F}, so the `ID` line
// is the entity id and NOT the resource GUID a placement carries. Folding the two
// together means an `ID` collision can shadow a correct reference.
struct ResourceMap
{
    std::unordered_map<std::string, std::string> byReference;
    std::unordered_map<std::string, std::string> byOwnId;
    //! From `resourceDatabase.rdb`, the addon's own GUID index. Authoritative and
    //! complete where the two scanned tiers are opportunistic, so it is consulted
    //! LAST only because the scanned spellings are the ones the rest of the
    //! pipeline has always used; anything they miss lands here.
    std::unordered_map<std::string, std::string> byDatabase;
    size_t filesScanned = 0;
    size_t filesUnreadable = 0;
    size_t databasesRead = 0;
    size_t databasesRefused = 0;

    const std::string* Find(const std::string& guid) const
    {
        const auto ref = byReference.find(guid);
        if (ref != byReference.end())
            return &ref->second;
        const auto own = byOwnId.find(guid);
        if (own != byOwnId.end())
            return &own->second;
        const auto db = byDatabase.find(guid);
        if (db != byDatabase.end())
            return &db->second;
        return nullptr;
    }

    size_t Size() const { return byReference.size() + byOwnId.size() + byDatabase.size(); }
};

//! Loads every `resourceDatabase.rdb` under `root` into `map.byDatabase`.
//!
//! They sit loose beside each addon's `.pak`, not inside it, so this walks the
//! filesystem rather than the PakSet. A database that does not close is REFUSED
//! whole: a partial walk of this format yields plausible paths for the wrong
//! GUIDs, which is worse than the gap it would fill.
void LoadResourceDatabases(const std::string& root, ResourceMap& map)
{
    std::error_code ec;
    if (!std::filesystem::exists(root, ec))
        return;
    if (!std::filesystem::is_directory(root, ec))
        return;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec))
    {
        if (ec)
            break;
        if (!entry.is_regular_file(ec))
            continue;
        if (LowerCopy(entry.path().filename().string()) != "resourcedatabase.rdb")
            continue;
        std::ifstream file(entry.path(), std::ios::binary);
        if (!file)
            continue;
        std::vector<uint8_t> blob((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const ResourceDatabase db = ReadResourceDatabase(blob.data(), blob.size());
        if (!db.closes())
        {
            map.databasesRefused++;
            std::cout << "  refused " << entry.path().string() << ": "
                      << (db.error.empty() ? "walk did not consume the file exactly" : db.error) << "\n";
            continue;
        }
        map.databasesRead++;
        for (const auto& [guid, path] : db.byGuid)
            map.byDatabase.emplace(guid, path);
    }
}

//! Extensions whose contents are Enfusion's brace syntax rather than a binary blob.
bool IsTextResource(const std::string& lowerPath)
{
    static const char* kTexty[] = {".et",      ".emat", ".conf",     ".layout", ".gamemat", ".ct", ".physmat",
                                   ".vhcsurf", ".terr", ".imageset", ".styles", ".ptc",     ".bt", ".c"};
    for (const char* ext : kTexty)
        if (EndsWith(lowerPath, ext))
            return true;
    return false;
}

//! Collects every `{<16 hex>}<path>` reference in a blob. First spelling wins, so a
//! GUID that appears in several files resolves to one path deterministically.
void ScanReferences(const std::vector<uint8_t>& blob, ResourceMap& map)
{
    const char* data = reinterpret_cast<const char*>(blob.data());
    const size_t size = blob.size();
    for (size_t i = 0; i + 18 <= size; ++i)
    {
        if (data[i] != '{' || data[i + 17] != '}')
            continue;
        bool hex = true;
        for (size_t k = 1; k <= 16 && hex; ++k)
            hex = IsHexDigit(data[i + k]);
        if (!hex)
            continue;
        size_t end = i + 18;
        while (end < size)
        {
            const char c = data[end];
            if (c == '"' || c == '\'' || c == '{' || c == '}' || static_cast<unsigned char>(c) <= ' ')
                break;
            ++end;
        }
        if (end > i + 18)
            map.byReference.emplace(UpperGuid(data + i + 1, 16), std::string(data + i + 18, end - i - 18));
        i = end - 1;
    }
}

//! The `ID "<16 hex>"` line an `.et` carries. Line-anchored: the same 16-hex shape
//! occurs inside brace references on almost every other line of the same file.
void ScanOwnId(const std::vector<uint8_t>& blob, const std::string& path, ResourceMap& map)
{
    const char* data = reinterpret_cast<const char*>(blob.data());
    const size_t size = blob.size();
    size_t at = 0;
    while (at < size)
    {
        size_t lineEnd = at;
        while (lineEnd < size && data[lineEnd] != '\n')
            ++lineEnd;
        size_t cursor = at;
        while (cursor < lineEnd && static_cast<unsigned char>(data[cursor]) <= ' ')
            ++cursor;
        if (cursor + 21 <= lineEnd && data[cursor] == 'I' && data[cursor + 1] == 'D' &&
            static_cast<unsigned char>(data[cursor + 2]) <= ' ')
        {
            size_t quote = cursor + 3;
            while (quote < lineEnd && static_cast<unsigned char>(data[quote]) <= ' ')
                ++quote;
            if (quote + 17 < lineEnd && data[quote] == '"' && data[quote + 17] == '"')
            {
                bool hex = true;
                for (size_t k = 1; k <= 16 && hex; ++k)
                    hex = IsHexDigit(data[quote + k]);
                if (hex)
                {
                    map.byOwnId[UpperGuid(data + quote + 1, 16)] = path;
                    return; // the ID line is the second line of every `.et`; one per file
                }
            }
        }
        at = lineEnd + 1;
    }
}

ResourceMap BuildResourceMap(const PakSet& paks)
{
    ResourceMap map;
    for (const PakArchive& archive : paks.Archives())
    {
        if (!archive.IsOpen())
            continue;
        for (const PakEntry& entry : archive.Entries())
        {
            const std::string lower = LowerCopy(entry.path);
            if (!IsTextResource(lower))
                continue;
            std::vector<uint8_t> blob;
            if (!archive.Read(entry, blob))
            {
                map.filesUnreadable++;
                continue;
            }
            map.filesScanned++;
            if (EndsWith(lower, ".et"))
                ScanOwnId(blob, entry.path, map);
            ScanReferences(blob, map);
        }
    }
    return map;
}

bool LoadResourceMapCache(const std::string& path, ResourceMap& map)
{
    std::ifstream file(path);
    if (!file)
        return false;
    std::string line;
    while (std::getline(file, line))
    {
        const size_t tab1 = line.find('\t');
        if (tab1 == std::string::npos)
            continue;
        const size_t tab2 = line.find('\t', tab1 + 1);
        if (tab2 == std::string::npos)
            continue;
        const std::string guid = line.substr(tab1 + 1, tab2 - tab1 - 1);
        std::string value = line.substr(tab2 + 1);
        if (!value.empty() && value.back() == '\r')
            value.pop_back();
        if (line[0] == 'R')
            map.byReference.emplace(guid, std::move(value));
        else
            map.byOwnId.emplace(guid, std::move(value));
    }
    return map.Size() != 0;
}

void SaveResourceMapCache(const std::string& path, const ResourceMap& map)
{
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, ec);
    std::ofstream file(path, std::ios::trunc);
    if (!file)
        return;
    for (const auto& [guid, value] : map.byReference)
        file << "R\t" << guid << '\t' << value << '\n';
    for (const auto& [guid, value] : map.byOwnId)
        file << "S\t" << guid << '\t' << value << '\n';
    // The database tier is cached too. A cache written without it silently
    // reintroduces the 562 unresolved prefabs on the next run, and that run would
    // look identical to a correct one.
    for (const auto& [guid, value] : map.byDatabase)
        file << "D\t" << guid << '\t' << value << '\n';
}

// ---------------------------------------------------------------------------
// `.et` prefab -> `.xob`
// ---------------------------------------------------------------------------

//! Why this is a line scanner and not ParseEmat: the mesh reference sits two blocks
//! deep (`components { MeshObject "{..}" { Object "{..}path.xob" } }`) and ParseEmat
//! folds every token below the top level into the enclosing property's value list,
//! so the key `Object` stops being a key. ParseEmat IS used for the header, which is
//! where the four measured header forms and the parent reference live.
std::string FindObjectReference(const std::vector<uint8_t>& blob)
{
    const char* data = reinterpret_cast<const char*>(blob.data());
    const size_t size = blob.size();
    size_t at = 0;
    while (at < size)
    {
        size_t lineEnd = at;
        while (lineEnd < size && data[lineEnd] != '\n')
            ++lineEnd;
        size_t cursor = at;
        while (cursor < lineEnd && static_cast<unsigned char>(data[cursor]) <= ' ')
            ++cursor;
        // Keyed on the exact property name. `m_sPhaseModel` in a
        // SCR_BaseDestructionPhase block names a `.xob` too -- the stump the tree
        // becomes when destroyed -- and a scan for "any .xob on any line" picks that
        // one up on every destructible prefab in the corpus.
        if (cursor + 7 <= lineEnd && std::memcmp(data + cursor, "Object", 6) == 0 &&
            static_cast<unsigned char>(data[cursor + 6]) <= ' ')
        {
            const size_t open = std::string_view(data + cursor, lineEnd - cursor).find('"');
            if (open != std::string_view::npos)
            {
                const size_t begin = cursor + open + 1;
                size_t end = begin;
                while (end < lineEnd && data[end] != '"')
                    ++end;
                std::string guid, path;
                Poseidon::Asset::Material::Detail::SplitGuid(std::string(data + begin, end - begin), guid, path);
                if (EndsWith(LowerCopy(path), ".xob"))
                    return path;
            }
        }
        at = lineEnd + 1;
    }
    return {};
}

struct PrefabResolution
{
    std::string xobPath;
    std::string via;    //!< the `.et` the mesh was finally found in
    int depth = 0;      //!< 0 when the placed prefab carried its own mesh
    std::string reason; //!< empty on success; names what failed
};

//! Follows a prefab's inheritance chain until a `MeshObject`'s `Object` appears.
//!
//! `depthLimit` is 8 for the same reason ResolveEmatInheritance uses 8: it bounds a
//! cycle in authored data rather than expressing an expected depth. Everon's top 25
//! prefabs resolve at depth 0 or 1.
PrefabResolution ResolvePrefabModel(const PakSet& paks, const std::string& etPath, int depthLimit = 8)
{
    PrefabResolution out;
    std::string current = etPath;
    std::vector<std::string> seen;
    for (int depth = 0; depth <= depthLimit; ++depth)
    {
        for (const std::string& before : seen)
        {
            if (LowerCopy(before) == LowerCopy(current))
            {
                out.reason = "prefab inheritance cycle at " + current;
                return out;
            }
        }
        seen.push_back(current);

        std::vector<uint8_t> blob;
        if (!paks.Read(current, blob))
        {
            out.reason = "prefab not in any .pak: " + current;
            return out;
        }
        const std::string object = FindObjectReference(blob);
        if (!object.empty())
        {
            out.xobPath = object;
            out.via = current;
            out.depth = depth;
            return out;
        }
        const auto parsed = Poseidon::Asset::Material::ParseEmat(
            std::string_view(reinterpret_cast<const char*>(blob.data()), blob.size()));
        if (parsed.parentPath.empty())
        {
            out.reason = parsed.valid() ? "prefab declares no MeshObject and no parent: " + current
                                        : "prefab header unreadable (" + parsed.error + "): " + current;
            return out;
        }
        current = parsed.parentPath;
    }
    out.reason = "prefab inheritance deeper than " + std::to_string(depthLimit) + " from " + etPath;
    return out;
}

// ---------------------------------------------------------------------------
// `.emat` -> a `.paa` the engine can bind
// ---------------------------------------------------------------------------

//! Defined with the rest of the path helpers below; used here first.
std::filesystem::path OnDiskPathFor(const std::string& outputRoot, const std::string& virtualPath);

struct TextureOptions
{
    bool enabled = false;
    std::string prefix = "reforger\\everon\\textures";
    //! Longest edge written. Reforger authors 2048 and 4096 albedos; the whole point
    //! of the cap is that this writes one file per material and the corpus's top 50
    //! models name around sixty of them. 4096 also runs into the writer's truncated
    //! mip chain (measured on DXT3), so staying well under it is not only a budget.
    int maxSize = 1024;
    //! Bake `NMOMap` into a DXT5nm `.paa`. Needs `rvmats` to reach the renderer: a
    //! normal has no face-texture slot to arrive through, only a material stage.
    bool normals = false;
    //! Write a real `.rvmat` per material and name THAT in the MLOD material field.
    //! Superseded by `emats` below, which carries everything this does and the albedo
    //! too; kept for the A/B and for a reader that wants an RVMAT-only tree.
    bool rvmats = false;
    //! Deploy the material itself: the `.emat`, inheritance folded in and its texture
    //! keys rewritten to the `.paa` files this converter wrote, under the texture
    //! prefix at the source's own relative path (`Assets/X/Y.emat` ->
    //! `<prefix>\assets\x\y.emat`), and name THAT in the MLOD material field. The
    //! engine already reads an `.emat` as a material (LoadTranslatedMaterial); what it
    //! could never do was OPEN one, because every section named the path inside the
    //! `.pak`. Default ON with `--textures`: a written `.emat` the engine cannot parse
    //! costs exactly what the unopenable one cost, and one it can parse is the whole
    //! difference between `family=` and `family=MatPBRTreeCrown`. `--no-material-emat`
    //! is the A/B.
    bool emats = true;
    //! Deploy `MatPBRMulti`'s MASK and its `BCR_2..4` / `NMO_2..4` layer tiles too, so
    //! the runtime blends the layers itself instead of receiving one collapsed tile.
    //!
    //! The runtime blend has been complete for a while (`gpu_driven.wgsl`: one mask
    //! sample, RGB weighting three tiles at their own UV transforms, remainder to the
    //! base, renormalised on over-paint, same weights reused for the normals) and it
    //! never ran, because 0 of the 1,793 deployed materials named a `MaskMap` or a
    //! `BCR_N` -- this converter had already collapsed them. Against 4,044 and 3,905
    //! in Reforger's own 10,312. `MatPBRMulti` is 51% of the deployed corpus.
    //!
    //! `--no-material-layers` restores the collapse, which is the A/B and the fallback
    //! for a material whose mask or tiles do not decode.
    bool layers = true;
    //! Longest edge for a layer MASK, and the format is uncompressed on purpose.
    //!
    //! A mask's R, G and B are three INDEPENDENT weights. DXT1 encodes a 4x4 block as
    //! two endpoint colours and interpolates between them, so it forces the three
    //! channels onto one line in colour space -- which is exactly the correlation a mask
    //! does not have. Baked as DXT1 the church's mask came back as flat blocks of pure
    //! green, red and blue and the blend drew broad diagonal bands across the roof and
    //! walls. Uncompressed ARGB8888 keeps the channels apart.
    //!
    //! 512 because it is a low-frequency weight map, not detail: the detail comes from
    //! the tiles it selects between. Uncompressed at 1024 would cost ~5.6 MB per object.
    int maxMaskSize = 512;
};

//! Which two channels of a decoded normal texture carry X and Y.
//!
//! An `.emat` NMO is not in this engine's convention and the two live conventions
//! disagree about WHERE, not just how:
//!
//!   BC5/"RG"      X in R, Y in G, B and A carry the other payload (NMO's O)
//!   DXT5nm/"AG"   X in A, Y in G, R and B unused -- what Real Virtuality authors,
//!                 and what `gpu_driven.wgsl:646` samples: `vec2(sample.a, sample.g)`
//!
//! Binding one as the other yields a normal map that is wrong per-texel but never
//! errors -- lighting simply goes strange -- which is exactly the failure this
//! programme keeps measuring its way out of. So detect rather than assume: a tangent
//! space normal satisfies x^2+y^2 <= 1 for every texel, and a channel pair that is not
//! a normal pair violates it constantly. The one thing the inequality cannot tell
//! apart is X-then-Y from Y-then-X, since it is symmetric; that is settled by the
//! container's own convention (BC5 is X,Y in R,G; DXT5nm is X in A) and reported per
//! model so a wrong call is visible rather than silent.
enum class NormalPacking
{
    None,
    RG, //!< X in R, Y in G
    AG, //!< X in A, Y in G
};

inline const char* ToString(NormalPacking packing)
{
    switch (packing)
    {
        case NormalPacking::RG:
            return "RG";
        case NormalPacking::AG:
            return "AG";
        default:
            return "none";
    }
}

//! Fraction of sampled texels for which the pair satisfies x^2+y^2 <= 1, and whether
//! both channels vary at all. A constant channel passes the inequality by accident
//! (R=0 gives x=-1, and x^2+y^2 <= 1 then forces y=0) so "varies" is not optional.
struct PackingScore
{
    double obeys = 0.0;
    bool varies = false;
};

inline PackingScore ScorePacking(const std::vector<uint8_t>& rgba, int xChannel, int yChannel)
{
    const size_t texels = rgba.size() / 4;
    if (texels == 0)
        return {};
    // Every texel up to a cap: a normal map's violating region can be a small patch,
    // and a stride that skips it reports a clean 1.0.
    const size_t stride = texels > 262144 ? texels / 262144 : 1;
    size_t tested = 0;
    size_t obeying = 0;
    uint8_t xMin = 255, xMax = 0, yMin = 255, yMax = 0;
    for (size_t t = 0; t < texels; t += stride)
    {
        const uint8_t xb = rgba[t * 4 + static_cast<size_t>(xChannel)];
        const uint8_t yb = rgba[t * 4 + static_cast<size_t>(yChannel)];
        // Both channels exactly zero is not a normal, it is the absence of one -- the
        // cut-away region of a leaf card, where nothing was ever authored. Decoded it
        // reads (-1,-1), which violates the test by construction, so counting those
        // texels rejects every alpha-cutout vegetation texture in the corpus for having
        // a background. They are excluded from the denominator, not from the file.
        if (xb == 0 && yb == 0)
            continue;
        xMin = std::min(xMin, xb);
        xMax = std::max(xMax, xb);
        yMin = std::min(yMin, yb);
        yMax = std::max(yMax, yb);
        const double x = xb / 127.5 - 1.0;
        const double y = yb / 127.5 - 1.0;
        // 1.02 rather than 1.0: the source is block-compressed, so a texel authored
        // exactly on the unit circle decodes a quantisation step outside it.
        if (x * x + y * y <= 1.02)
            ++obeying;
        ++tested;
    }
    PackingScore score;
    if (tested == 0)
        return score;
    score.obeys = static_cast<double>(obeying) / static_cast<double>(tested);
    score.varies = (xMax - xMin) > 8 && (yMax - yMin) > 8;
    return score;
}

//! The packing a decoded NMO uses, or None when no candidate pair behaves like one.
inline NormalPacking DetectNormalPacking(const std::vector<uint8_t>& rgba)
{
    const PackingScore rg = ScorePacking(rgba, 0, 1);
    const PackingScore ag = ScorePacking(rgba, 3, 1);
    const NormalPacking best = (ag.varies && ag.obeys > rg.obeys) ? NormalPacking::AG : NormalPacking::RG;
    const PackingScore& score = best == NormalPacking::AG ? ag : rg;
    if (!score.varies || score.obeys < 0.90)
        return NormalPacking::None;
    return best;
}

//! Decode a `.edds` out of the paks into straight RGBA8.
bool LoadEdds(const PakSet& paks, const std::string& path, Poseidon::Image& out)
{
    std::vector<uint8_t> bytes;
    if (!paks.Read(path, bytes) || bytes.empty())
        return false;
    Poseidon::EddsImage edds = Poseidon::ReadEddsBuffer(bytes.data(), bytes.size());
    if (!edds.valid() || edds.mipmaps.empty())
        return false;
    Poseidon::Image image(edds.width, edds.height, edds.format, std::move(edds.mipmaps[0].data));
    out = image.ToRGBA();
    return out.valid() && out.data().size() >= static_cast<size_t>(out.width()) * static_cast<size_t>(out.height()) * 4;
}

//! One 2x2 box step. Exact on the power-of-two textures Reforger authors, and the
//! reason no resampler is linked in here: every one of them halves cleanly.
void HalveRGBA(std::vector<uint8_t>& pixels, int& width, int& height)
{
    const int nw = width / 2;
    const int nh = height / 2;
    std::vector<uint8_t> out(static_cast<size_t>(nw) * static_cast<size_t>(nh) * 4);
    for (int y = 0; y < nh; ++y)
    {
        for (int x = 0; x < nw; ++x)
        {
            const size_t a = (static_cast<size_t>(2 * y) * width + 2 * x) * 4;
            const size_t b = a + 4;
            const size_t c = a + static_cast<size_t>(width) * 4;
            const size_t d = c + 4;
            for (int ch = 0; ch < 4; ++ch)
            {
                const unsigned sum = pixels[a + ch] + pixels[b + ch] + pixels[c + ch] + pixels[d + ch];
                out[(static_cast<size_t>(y) * nw + x) * 4 + ch] = static_cast<uint8_t>((sum + 2) / 4);
            }
        }
    }
    pixels = std::move(out);
    width = nw;
    height = nh;
}

struct TextureStats
{
    size_t materialsSeen = 0;  //!< distinct `.emat` paths asked about
    size_t materialsBound = 0; //!< of those, the ones that produced a `.paa`
    size_t filesWritten = 0;   //!< distinct `.paa` files on disk
    size_t filesWithAlpha = 0; //!< of those, the ones carrying a cutout mask
    size_t baseLayerOnly = 0;  //!< MatPBRMulti materials approximated by a shared tile
    size_t layerTinted = 0;    //!< of those, the ones whose tile was rescaled to Color_N
    size_t layerSetsWritten = 0;    //!< MatPBRMulti materials deployed WHOLE: mask + layer tiles
    size_t layerTilesWritten = 0;   //!< tiles and layer normals written for those (mask excluded)
    size_t layerNormalsWritten = 0; //!< of those, the per-layer NMO_N maps
    size_t layerSetsColourOnly = 0; //!< masked materials whose upper layers name no tile at all
    size_t layerSetsTileFailed = 0; //!< masked materials where a layer tile refused to bake
    size_t layerSetsMaskFailed = 0; //!< ... or where the mask itself refused to bake
    //!< of those, the ones whose layer weights came from the MaskMap rather than
    //!< from assuming every present layer covers the object equally
    size_t layerMaskWeighted = 0;
    size_t bytesWritten = 0;
    size_t materialsWithNormal = 0;         //!< materials that resolved a normal map
    size_t normalsWritten = 0;              //!< distinct normal `.paa` files on disk
    size_t rvmatsWritten = 0;               //!< distinct `.rvmat` files on disk
    size_t ematsWritten = 0;                //!< distinct deployed `.emat` files on disk
    size_t ematsCutout = 0;                 //!< of those, the ones that say they are alpha-tested
    //!< of those, the ones deployed with NO baked image, carrying only their authored
    //!< flat `Color`. Counted apart on purpose: this path is correct for Enfusion's
    //!< baked-LOD materials, which genuinely have no map, but it would also quietly
    //!< absorb a material whose albedo bake REGRESSED -- that object would render in a
    //!< plausible flat colour instead of an obvious white. If this number jumps, the
    //!< question is whether the corpus gained coarse LODs or the baker lost textures.
    size_t ematsColourOnly = 0;
    size_t materialsHidden = 0;             //!< unresolvable decal/water-erase materials whose faces are dropped
    std::map<std::string, size_t> packings; //!< detected NMO packing -> distinct textures
    //! Whether the BCR's alpha channel varies. Recorded, NOT bound: Enfusion is
    //! understood to pack roughness there, and an SMDI-shaped specular map could be
    //! synthesised from it -- but this programme does not bind a channel it has not
    //! measured, and nothing here has measured that one. The count is the denominator
    //! whoever measures it will need.
    size_t bcrAlphaVaries = 0;
    size_t bcrAlphaFlat = 0;
    std::map<std::string, size_t> failures; //!< reason -> distinct materials
};

//! Turns the `.emat` a `.xob` part names into a `.paa` at a virtual path, once per
//! material, and reports why when it cannot.
//!
//! Two caches, and they are not the same cache: several `.emat` files share one
//! `BCRMap` (every Picea LOD's crown is the same atlas), so the material cache stops
//! the parse and the image cache stops the decode-and-encode. Keying the image cache
//! on the BCR ALONE would be wrong -- two materials may share an albedo and differ in
//! their opacity mask -- so the key is the pair.
//! What one `.emat` became: the face texture, and the material file that carries
//! everything a face texture has no slot for.
struct BakedMaterial
{
    std::string colourPaa; //!< the MLOD TEXTURE field
    std::string normalPaa; //!< DXT5nm, reachable only through the rvmat below
    std::string rvmatPath; //!< a Super `.rvmat` naming the normal, "" when none was written
    std::string ematPath;  //!< the deployed `.emat`, "" when none was written
    //! (source key -> deployed `.paa`) for every texture beyond the albedo and the
    //! object's own normal: `MaskMap` and the `BCR_N` / `NMO_N` layer tiles. Written
    //! back into the `.emat` under the SOURCE's own key names, so the deployed file
    //! reads exactly like the file it came from and the engine's translation needs no
    //! converter-specific spelling.
    std::vector<std::pair<std::string, std::string>> extraTextures;
    //! What the MLOD MATERIAL field should say. The `.emat` when there is one -- it
    //! names the albedo AND the normal and keeps every non-texture key -- else the
    //! `.rvmat`, else "" so the caller keeps the source path, which at least says
    //! which material was meant.
    const std::string& MaterialField() const { return !ematPath.empty() ? ematPath : rvmatPath; }
    //! Do not write this material's faces at all. Set for a material that bound no
    //! albedo AND whose family says its faces must not draw opaque: `MatPBRDecal` is an
    //! alpha-blended overlay shell duplicating the surface underneath (the BTR-70's
    //! `Decals_01` is 6,713 triangles wrapping the whole hull), and a `*WaterErase*`
    //! material is Enfusion's invisible water-suppression surface. Left in, such a
    //! section draws UNTEXTURED and OPAQUE over the real surface -- measured on the
    //! BTR-70, whose olive body read near-black because its own decal shell was drawn
    //! on top of it in fallback grey.
    bool hideFaces = false;
};

class TextureBaker
{
  public:
    TextureBaker(const PakSet& paks, const std::string& outputRoot, const TextureOptions& options)
        : _paks(paks), _outputRoot(outputRoot), _options(options)
    {
    }

    const TextureStats& Stats() const { return _stats; }

    //! What a section naming `ematPath` should write into its face. Both fields may be
    //! empty: a material this engine can bind nothing of is not an error, it is a
    //! section that keeps drawing the way it did before.
    const BakedMaterial& For(const std::string& ematPath)
    {
        const std::string key = LowerCopy(ematPath);
        const auto found = _byMaterial.find(key);
        if (found != _byMaterial.end())
            return found->second;
        _stats.materialsSeen++;
        BakedMaterial result = Bake(ematPath);
        if (!result.colourPaa.empty())
            _stats.materialsBound++;
        return _byMaterial.emplace(key, std::move(result)).first->second;
    }

  private:
    void Unbound(const char* reason) { _stats.failures[reason]++; }

    BakedMaterial Bake(const std::string& ematPath)
    {
        std::vector<uint8_t> blob;
        if (!_paks.Read(ematPath, blob))
        {
            Unbound(".emat not in any .pak");
            return {};
        }
        Poseidon::Asset::Material::EmatMaterial emat = Poseidon::Asset::Material::ParseEmat(
            std::string_view(reinterpret_cast<const char*>(blob.data()), blob.size()));
        if (!emat.valid())
        {
            Unbound(".emat does not parse");
            return {};
        }
        // 18.3% of Reforger's materials get their maps only from a parent, and a child
        // read without one keeps its own overrides and silently loses the albedo.
        Poseidon::Asset::Material::ResolveEmatInheritance(emat,
                                                          [&](const std::string& path, std::string& text)
                                                          {
                                                              std::vector<uint8_t> parent;
                                                              if (!_paks.Read(path, parent))
                                                                  return false;
                                                              text.assign(parent.begin(), parent.end());
                                                              return true;
                                                          });

        // `MatPBRMulti` is the one common class with no albedo of its own, and what it
        // has instead is easy to mistake for one. It names `BCR_1..4`, but those are
        // SHARED LIBRARY TILES -- `Assets/_SharedData/Metal/ST_MetalPaint_Coated_02_1m_BCR`,
        // one square metre of generic painted metal that hundreds of unrelated objects
        // also name -- blended through a `MaskMap` and, crucially, TINTED by a per-layer
        // `Color_N`. The jerrycan is not olive because its texture is olive; it is olive
        // because `Color_1` is 0.103 0.105 0.041 over a grey tile.
        //
        // Two of the corpus's top 50 models are that class -- GraniteCliff_01 (9,445
        // placements) and the Picea stump (8,275) -- so refusing them costs more than
        // approximating them, and the tile alone is a poor approximation: it draws every
        // such object in the shared tile's colour instead of its own.
        //
        // So the tile is RESCALED to have `Color_N` as its mean, keeping its detail and
        // taking its colour from the material. That is a choice between two readings of
        // `Color_N` and it is the safe one: read as an absolute base colour it is exact,
        // and read as a multiplier it is off by a constant, whereas multiplying directly
        // would render a 0.3-mean tile at 0.03 -- near black -- if the first reading is
        // the true one. Counted separately either way.
        std::string bcr = emat.TextureOf("BCRMap");
        float tint[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        bool tinted = false;
        //! The mask and the layer tiles, when the full set could be deployed. Empty
        //! means the collapsed approximation below stands.
        std::vector<std::pair<std::string, std::string>> multi;
        if (bcr.empty())
        {
            // WHICH layer, and in what proportion. Taking the first non-empty
            // `BCR_N` and its `Color_N` is what this did, and on any object whose
            // first-listed layer is not its dominant one it renders the whole thing
            // in a minority colour. WaterTower_01 is the case that exposed it: its
            // layers are 2 (`Color_2` 0.031 0.133 0.191, a near-black blue), 3 and 4
            // (0.381 and 0.402 grey). First-wins painted a concrete-and-steel tower
            // in the near-black blue, and it read as an untextured object.
            //
            // The proportions are in the `MaskMap`, and its channels are a partition
            // of unity over the layers: measured on WaterTower_02's global mask the
            // means are R 0.127, G 0.299, B 0.238, summing to 0.664 with the
            // remainder belonging to the base layer. So R, G and B weight layers 2,
            // 3 and 4, and layer 1 takes what is left.
            //
            // Weighting the tower's own colours that way gives roughly 0.32 0.34
            // 0.35 -- light grey with a faint blue cast, which is what the object is.
            float weight[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f}; // 1-based
            bool haveMask = false;
            const std::string maskPath = emat.TextureOf("MaskMap");
            if (!maskPath.empty())
            {
                Poseidon::Image mask;
                if (LoadEdds(_paks, maskPath, mask))
                {
                    const std::vector<uint8_t>& texels = mask.data();
                    const size_t count = texels.size() / 4;
                    if (count != 0)
                    {
                        double sum[3] = {0.0, 0.0, 0.0};
                        for (size_t t = 0; t < count; ++t)
                            for (int c = 0; c < 3; ++c)
                                sum[c] += texels[t * 4 + static_cast<size_t>(c)];
                        double covered = 0.0;
                        for (int c = 0; c < 3; ++c)
                        {
                            const double mean = sum[c] / (255.0 * static_cast<double>(count));
                            weight[c + 2] = static_cast<float>(mean);
                            covered += mean;
                        }
                        weight[1] = static_cast<float>(covered < 1.0 ? 1.0 - covered : 0.0);
                        haveMask = true;
                        _stats.layerMaskWeighted++;
                    }
                    else
                    {
                        Unbound("MaskMap decodes to no texels");
                    }
                }
                else
                {
                    Unbound("MaskMap .edds does not decode");
                }
            }

            // A layer counts only if it exists AND is not switched off. Without a
            // usable mask every present layer weighs the same: still a guess, but an
            // unbiased one, where first-wins is biased by file order.
            // WHICH TILE and WHAT COLOUR are two different questions, and answering
            // both from the mask is wrong. The mask weights OVERLAY layers -- mud,
            // dirt, rust, weathering -- over a BASE layer; it is not a partition
            // among equals. Picking the heaviest-weighted layer's tile therefore
            // swaps the surface's material for its weathering: it turned the
            // church's ceramic roof tiles into painted metal plates, its brick
            // walls into clean plaster, and its castle stone into rusted metal.
            //
            // So the TILE stays the base layer, the lowest present index, which is
            // what supplies the surface's actual grain. Only the COLOUR is
            // mask-weighted across the layers, which is the part the mask really
            // does describe.
            float total = 0.0f;
            float blended[3] = {0.0f, 0.0f, 0.0f};
            std::string baseTile;
            for (int layer = 1; layer <= 4; ++layer)
            {
                const std::string tile = emat.TextureOf("BCR_" + std::to_string(layer));
                if (tile.empty())
                    continue;
                float enabled = 1.0f;
                emat.FloatOf("Enabled_" + std::to_string(layer), enabled);
                if (enabled == 0.0f)
                    continue;
                if (baseTile.empty())
                    baseTile = tile;
                float colour[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                if (!emat.Vec4Of("Color_" + std::to_string(layer), colour))
                    continue;
                const float w = haveMask ? weight[layer] : 1.0f;
                for (int c = 0; c < 3; ++c)
                    blended[c] += colour[c] * w;
                total += w;
            }

            if (!baseTile.empty() && total > 1e-4f)
            {
                bcr = baseTile;
                for (int c = 0; c < 3; ++c)
                    tint[c] = blended[c] / total;
                tinted = true;
                _stats.baseLayerOnly++;
                _stats.layerTinted++;
            }
            // The real thing, when the mask and the layers can both be deployed: keep
            // every enabled layer above the base as its own tile and send the mask with
            // them, so the runtime blends per fragment instead of receiving one tile in
            // an averaged colour. The base then carries its OWN `Color_N` rather than the
            // mask-weighted average -- the average exists only to stand in for a blend
            // that is not happening, and applying it as well as the blend would tint the
            // base twice.
            //
            // Deliberately all-or-nothing: a mask with no layers weights nothing, and
            // layers with no mask have no weights. Either way the collapse above stands,
            // which is why it is computed first and only overwritten on success.
            else
            {
                // No layer carried a colour. Fall back to the old behaviour rather
                // than refusing: a tile in the wrong colour still beats nothing.
                for (int layer = 1; layer <= 4 && bcr.empty(); ++layer)
                {
                    bcr = emat.TextureOf("BCR_" + std::to_string(layer));
                    if (!bcr.empty())
                        _stats.baseLayerOnly++;
                }
            }
            if (_options.layers && haveMask && !bcr.empty())
                multi = BuildLayerSet(emat, maskPath, bcr, tint, tinted);
        }
        if (bcr.empty())
        {
            Unbound("material names no BCRMap and no BCR_N layer");
            // No albedo will ever bind. For most families the section keeps drawing the
            // way it did before -- untextured, but real geometry. Two families must NOT
            // do that, because their faces are overlays that only exist to draw ON TOP
            // of the surface beneath them (see BakedMaterial::hideFaces): drawn opaque
            // in fallback grey they replace the surface instead of decorating it.
            BakedMaterial unresolved;
            const bool decalFamily = Poseidon::Asset::Material::EmatMaterial::NameMatches(emat.className, "MatPBRDecal");
            const bool waterErase = LowerCopy(ematPath).find("watererase") != std::string::npos ||
                                    LowerCopy(emat.className).find("watererase") != std::string::npos;
            if (decalFamily || waterErase)
            {
                unresolved.hideFaces = true;
                _stats.materialsHidden++;
                std::cerr << "  hiding faces of " << ematPath << " (family " << emat.className
                          << ", no albedo resolved: " << (decalFamily ? "overlay decal shell" : "water-erase surface")
                          << ")" << std::endl;
            }
            // MAT-054: "no image" is not "nothing bindable". Reforger's baked-LOD
            // materials -- `*_MLOD`, `*_lod3`, `*_Color_palette` -- are `MatPBRBasic`
            // carrying a single flat `Color` and no map at all, which is how Enfusion
            // draws a coarse LOD. EmatMaterialAdapter has synthesised a procedural
            // `#(argb,8,8,3)color(...)` base colour from `Color` since DZ-002, so such a
            // material IS bindable; not deploying it left the model naming a pak path
            // nothing can open, and the section fell to the 1.0 white at bindless slot 0.
            // Every one of the twelve authored colours MAT-054 measured is between 0.016
            // and 0.32, so white was the largest error available.
            //
            // Not for the two families above: a decal shell or a water-erase surface with
            // a flat colour is still an overlay that must not be drawn as a surface, and
            // hideFaces already says so.
            float flatColour[4] = {};
            if (_options.emats && !unresolved.hideFaces && emat.Vec4Of("Color", flatColour))
            {
                unresolved.ematPath = WriteEmat(ematPath, emat, unresolved, false, std::string());
                if (!unresolved.ematPath.empty())
                    _stats.ematsColourOnly++;
            }
            return unresolved;
        }
        const std::string opacity = emat.TextureOf("OpacityMap");

        BakedMaterial baked;
        std::string imageKey = LowerCopy(bcr) + "|" + LowerCopy(opacity);
        if (tinted)
        {
            // Two materials sharing a tile but tinting it differently are two files.
            char suffix[64];
            std::snprintf(suffix, sizeof(suffix), "|%.4f,%.4f,%.4f", tint[0], tint[1], tint[2]);
            imageKey += suffix;
        }
        const auto cached = _byImage.find(imageKey);
        if (cached != _byImage.end())
            baked.colourPaa = cached->second;
        else
            baked.colourPaa = _byImage.emplace(imageKey, Write(bcr, opacity, tinted ? tint : nullptr)).first->second;

        // The normal, and note which keys are NOT in this list. `NMO_1..4` are the same
        // shared one-metre tiles as `BCR_1..4`, in their own tiled UV frame -- bound as
        // the object's normal map they would light every surface wrongly, which is worse
        // than the nothing they replace. Only maps in the object's own UV layout qualify:
        // `NMOMap` (2,402 files), MatPBRMulti's `GlobalNMOMap`, and `NTCMap` (334), which
        // is what the vegetation families call theirs.
        std::string nmo;
        std::string nmoKey;
        for (const char* key : {"NMOMap", "GlobalNMOMap", "NTCMap"})
        {
            nmo = emat.TextureOf(key);
            if (!nmo.empty())
            {
                nmoKey = key;
                break;
            }
        }
        if (_options.normals && nmo.empty())
        {
            // Counted, because it is the ceiling on how much of a world can ever get a
            // normal map. A `MatPBRMulti` that names only `NMO_1..4` lands here on
            // purpose: those are tiling library maps in their own UV frame, and the
            // reason for refusing them is at the top of this function.
            Unbound("material names no NMOMap, GlobalNMOMap or NTCMap");
        }
        if (_options.normals && !nmo.empty())
        {
            const std::string normalKey = LowerCopy(nmo);
            const auto seen = _byNormal.find(normalKey);
            baked.normalPaa =
                seen != _byNormal.end() ? seen->second : _byNormal.emplace(normalKey, WriteNormal(nmo)).first->second;
            if (!baked.normalPaa.empty())
                _stats.materialsWithNormal++;
        }

        // The material file, and the reason it has to exist at all: a face texture is
        // one image. Every other map a surface has -- the normal above, the specular a
        // later pass may add -- reaches the renderer only through a MATERIAL, and the
        // material this section names today is an `.emat` sitting inside a `.pak` that
        // is not deployed with the game. So write one here, in the format the engine
        // already reads, naming the files this converter just wrote.
        if (_options.rvmats && !baked.normalPaa.empty())
            baked.rvmatPath = WriteRvmat(ematPath, baked);
        // The `.emat` itself, deployed.
        //
        // This used to be gated on "an albedo was baked", on the reasoning that a material
        // with nothing bindable would be an empty file the engine opens and reports as
        // binding nothing. That reasoning was wrong for one large class of material, and
        // MAT-054 is the measurement: 40 distinct `.emat` files drew UNTEXTURED on Everon,
        // and twelve of them are Reforger's BAKED-LOD materials (`*_MLOD`, `*_lod3`,
        // `*_Color_palette`) -- `MatPBRBasic` carrying a single flat `Color` and no map at
        // all. That is not a broken asset, it is how Enfusion draws a coarse LOD.
        //
        // And it IS bindable: EmatMaterialAdapter has synthesised a procedural
        // `#(argb,8,8,3)color(...)` base colour from `Color` since DZ-002, for exactly the
        // no-albedo case (that is how DayZ's pond water gets its colour). The gate meant the
        // engine never got the chance -- the model kept the raw pak path in its MATERIAL
        // field, nothing opened, and the section fell to the 1.0 white at bindless slot 0.
        // Every one of the twelve authored colours is between 0.016 and 0.32, so white was
        // the largest error available.
        //
        // So: deploy when there is a baked albedo OR an authored `Color` to stand in for
        // one. The two cases are counted separately, because a colour-only deploy is right
        // for a coarse LOD and would be a disguise for a bake regression.
        // Reached only when an albedo WAS baked -- the colour-only case returns from the
        // `bcr.empty()` branch above, which is where its deploy now happens. Putting the
        // widened gate here instead was my first attempt and it was a no-op: a 400-model
        // Everon export reported 21 materials with "no BCRMap and no BCR_N layer" and
        // zero colour-only deploys, because control never reaches this line for them.
        // MAT-054 named the right branch; I took the "equivalently" alternative it offered
        // and the two are only equivalent if the code gets there.
        baked.extraTextures = std::move(multi);
        if (_options.emats)
            baked.ematPath =
                WriteEmat(ematPath, emat, baked, _imageHasAlpha.count(LowerCopy(baked.colourPaa)) != 0, nmoKey);
        return baked;
    }

    //! `assets/vegetation/_polyplanes/polyplane_picea_abies_bcr.paa` under the prefix;
    //! `_ca` appended when an opacity mask was folded in, which is Real Virtuality's
    //! own notation for "colour plus alpha" and keeps the two variants of a shared
    //! albedo apart.
    std::string VirtualNameFor(const std::string& sourcePath, const char* suffix, const char* extension) const
    {
        std::string stem = LowerCopy(sourcePath);
        const size_t dot = stem.rfind('.');
        if (dot != std::string::npos)
            stem = stem.substr(0, dot);
        for (char& c : stem)
            if (c == '/')
                c = '\\';
        if (suffix && *suffix)
            stem += suffix;
        std::string head = LowerCopy(_options.prefix);
        for (char& c : head)
            if (c == '/')
                c = '\\';
        while (!head.empty() && head.back() == '\\')
            head.pop_back();
        const std::string base = head.empty() ? stem : head + "\\" + stem;
        std::string candidate = base + extension;
        // Two distinct (albedo, opacity) pairs can still land on one name when the
        // same albedo is masked two different ways. Rare, but a collision here means
        // one material silently wears another's cutout.
        for (int n = 2; _taken.count(candidate) != 0; ++n)
            candidate = base + "_" + std::to_string(n) + extension;
        return candidate;
    }

    //! Decode an NMO, repack it into the convention the renderer samples, and write it.
    //!
    //! `gpu_driven.wgsl:646` reads `vec2(sample.a, sample.g)`, so X must land in ALPHA
    //! and Y in GREEN whatever the source did -- and DXT5 is the format that survives
    //! that, because its alpha block is stored and interpolated separately from the
    //! colour block. A DXT1 normal map has no alpha at all and would arrive with X
    //! constant 1.
    std::string WriteNormal(const std::string& nmoPath)
    {
        Poseidon::Image source;
        if (!LoadEdds(_paks, nmoPath, source))
        {
            Unbound("NMOMap .edds does not decode");
            return {};
        }
        int width = source.width();
        int height = source.height();
        std::vector<uint8_t> pixels = source.data();
        pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

        const NormalPacking packing = DetectNormalPacking(pixels);
        _stats.packings[ToString(packing)]++;
        if (packing == NormalPacking::None)
        {
            // Not a refusal to be quiet about: whatever this texture is, binding it as
            // a normal map would light every surface using it wrongly, and a wrong
            // normal map is harder to notice than a missing one. Name it and show the
            // numbers, once per distinct texture -- a refusal with no evidence is the
            // kind that gets "fixed" by lowering the threshold.
            const PackingScore rg = ScorePacking(pixels, 0, 1);
            const PackingScore ag = ScorePacking(pixels, 3, 1);
            std::cerr << "  no normal packing: " << nmoPath << "  RG obeys " << std::fixed << std::setprecision(3)
                      << rg.obeys << (rg.varies ? "" : " (flat)") << "  AG obeys " << ag.obeys
                      << (ag.varies ? "" : " (flat)") << "\n";
            Unbound("NMOMap has no channel pair that behaves like a normal");
            return {};
        }
        const int xChannel = packing == NormalPacking::AG ? 3 : 0;

        // Repack in place, back to front is unnecessary -- X and Y are read out of the
        // source before either is overwritten.
        for (size_t t = 0; t * 4 + 3 < pixels.size(); ++t)
        {
            const uint8_t x = pixels[t * 4 + static_cast<size_t>(xChannel)];
            const uint8_t y = pixels[t * 4 + 1];
            pixels[t * 4 + 0] = 0;   // measured dead on every RV NOHQ; MaterialChannels.hpp
            pixels[t * 4 + 1] = y;   // Y
            pixels[t * 4 + 2] = 255; // Z is reconstructed, never sampled
            pixels[t * 4 + 3] = x;   // X
        }

        // Halving a normal map averages X and Y, which shortens the vector rather than
        // rotating it -- the same softening a mip chain applies, and the reason a
        // normal map may be box-filtered at all.
        while ((width > _options.maxSize || height > _options.maxSize) && width % 2 == 0 && height % 2 == 0 &&
               width > 4 && height > 4)
            HalveRGBA(pixels, width, height);

        const std::string virtualPath = VirtualNameFor(NormalStem(nmoPath), "_nohq", ".paa");
        const std::filesystem::path target = OnDiskPathFor(_outputRoot, virtualPath);
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (!Poseidon::PAAEncoder::WritePAA(target.string(),
                                            Poseidon::Image::FromRGBA(width, height, std::move(pixels)),
                                            Poseidon::PixelFormat::DXT5))
        {
            Unbound("WritePAA refused the normal map");
            return {};
        }
        Poseidon::PAAInfo info;
        const Poseidon::DecodedImage back = Poseidon::DecodePAAFile(target.string());
        if (!ReadPAAInfo(target.string(), info) || !back.valid() || back.width != width || back.height != height)
        {
            std::filesystem::remove(target, ec);
            Unbound("normal .paa does not read back");
            return {};
        }
        _taken.insert(virtualPath);
        _stats.normalsWritten++;
        _stats.bytesWritten += static_cast<size_t>(std::filesystem::file_size(target, ec));
        return virtualPath;
    }

    //! `..._nmo.edds` -> `..._nmo`, so the written file is `..._nohq.paa` and not
    //! `..._nmo_nohq.paa`. The suffix carries meaning here: `_nohq` is what the engine's
    //! own sibling-enhancement lookup and every RV convention expect a normal to be
    //! called, and a name that claims two conventions at once belongs to neither.
    static std::string NormalStem(const std::string& nmoPath)
    {
        std::string stem = nmoPath;
        const size_t dot = stem.rfind('.');
        if (dot != std::string::npos)
            stem = stem.substr(0, dot);
        for (const char* tail : {"_nmo", "_ntc", "_nm"})
        {
            const size_t length = std::strlen(tail);
            if (stem.size() > length && LowerCopy(stem.substr(stem.size() - length)) == tail)
                return stem.substr(0, stem.size() - length);
        }
        return stem;
    }

    //! A text RVMAT naming what this converter wrote. The text itself is
    //! `MakeSuperRvmatText`, which lives beside the reader that has to parse it back --
    //! see the note there for why the format is not spelled out at this end.
    std::string WriteRvmat(const std::string& ematPath, const BakedMaterial& baked)
    {
        const std::string virtualPath = VirtualNameFor(ematPath, "", ".rvmat");
        const std::filesystem::path target = OnDiskPathFor(_outputRoot, virtualPath);
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
        std::ofstream file(target, std::ios::trunc);
        if (!file)
        {
            Unbound("cannot open the .rvmat for writing");
            return {};
        }
        file << Poseidon::Asset::Material::MakeSuperRvmatText(ematPath, baked.normalPaa);
        file.close();
        if (!file)
        {
            Unbound("cannot write the .rvmat");
            return {};
        }
        _taken.insert(virtualPath);
        _stats.rvmatsWritten++;
        _stats.bytesWritten += static_cast<size_t>(std::filesystem::file_size(target, ec));
        return virtualPath;
    }

    //! The material, deployed: `MakeDeployedEmat` decides what goes in it and
    //! `WriteEmatText` spells it -- both live beside the reader that has to open it
    //! again (EmatMaterialAdapter.hpp / EmatSource.hpp), so the converter cannot drift
    //! from the engine. The path is `VirtualNameFor(ematPath, "", ".emat")`, i.e. the
    //! source's own relative path under the texture prefix, lower-cased -- and that
    //! spelling is a CONTRACT: `EmatDeployRoots()` in the engine rebuilds it from the
    //! raw path so the 1,132 models already on disk, whose sections still name
    //! `Assets/...emat`, open this file without a re-export.
    std::string WriteEmat(const std::string& ematPath, const Poseidon::Asset::Material::EmatMaterial& resolved,
                          const BakedMaterial& baked, bool opacityFolded, const std::string& normalKey)
    {
        const std::string virtualPath = VirtualNameFor(ematPath, "", ".emat");
        const std::filesystem::path target = OnDiskPathFor(_outputRoot, virtualPath);
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
        const Poseidon::Asset::Material::EmatMaterial deployed = Poseidon::Asset::Material::MakeDeployedEmat(
            resolved, baked.colourPaa, opacityFolded, normalKey, baked.normalPaa, baked.extraTextures);
        std::ofstream file(target, std::ios::trunc | std::ios::binary);
        if (!file)
        {
            Unbound("cannot open the .emat for writing");
            return {};
        }
        file << Poseidon::Asset::Material::WriteEmatText(deployed);
        file.close();
        if (!file)
        {
            Unbound("cannot write the .emat");
            return {};
        }
        _taken.insert(virtualPath);
        _stats.ematsWritten++;
        if (Poseidon::Asset::Material::EmatUsesLeafCards(deployed))
            _stats.ematsCutout++;
        _stats.bytesWritten += static_cast<size_t>(std::filesystem::file_size(target, ec));
        return virtualPath;
    }

    //! `MatPBRMulti`, deployed whole: the mask plus every enabled layer above the base,
    //! each tile carrying its OWN `Color_N`.
    //!
    //! Returns (source key -> deployed `.paa`) pairs, and rewrites the base tile's colour
    //! on success. Empty on any failure, which leaves the caller's collapsed
    //! approximation exactly as it was -- there is no half-blended state, because a mask
    //! whose layers are missing weights nothing and layers with no mask have no weights.
    //!
    //! Layer numbering, which is the part that is easy to get wrong: the mask's R, G and
    //! B weight layers 2, 3 and 4 and layer 1 takes the remainder (measured on
    //! WaterTower_02: channel means 0.127 / 0.299 / 0.238, summing to 0.664). So layer 1
    //! is the BASE and only 2..4 go out as layer tiles -- there are exactly three engine
    //! layer slots and that is not a coincidence. A material whose lowest enabled layer
    //! is not 1 has that one as its base, and its own tile is skipped below rather than
    //! being sent twice.
    //!
    //! The mask goes through `Write` untinted and with no opacity map: its channels are
    //! data, not colour, and `Write` leaves them alone in that configuration (the alpha
    //! it forces to 255 is unread).
    std::vector<std::pair<std::string, std::string>> BuildLayerSet(
        const Poseidon::Asset::Material::EmatMaterial& emat, const std::string& maskPath, const std::string& baseTile,
        float baseTint[4], bool& baseTinted)
    {
        int baseLayer = 0;
        std::vector<std::pair<std::string, std::string>> out;
        for (int layer = 1; layer <= 4; ++layer)
        {
            const std::string key = "BCR_" + std::to_string(layer);
            const std::string tile = emat.TextureOf(key);
            if (tile.empty())
                continue;
            float enabled = 1.0f;
            emat.FloatOf("Enabled_" + std::to_string(layer), enabled);
            if (enabled == 0.0f)
                continue;
            float colour[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            const bool hasColour = emat.Vec4Of("Color_" + std::to_string(layer), colour);
            if (baseLayer == 0 && tile == baseTile)
            {
                // The base, identified by BEING the tile the caller already chose rather
                // than by its index: the caller skips a layer with no `Color_N` while this
                // loop does not, so "the first enabled layer" is not always the same layer
                // in both. Matching on the tile makes them agree by construction.
                //
                // All that changes for it is the tint: its own `Color_N` instead of the
                // mask-weighted average, because the average exists only to stand in for a
                // blend that is now actually happening.
                baseLayer = layer;
                if (hasColour)
                {
                    for (int c = 0; c < 3; ++c)
                        baseTint[c] = colour[c];
                    baseTinted = true;
                }
                continue;
            }
            const std::string paa = BakeTile(tile, hasColour ? colour : nullptr);
            if (paa.empty())
            {
                // One tile that will not bake takes the whole set down, so say which:
                // a silent fall back to the collapse is indistinguishable from a
                // material that never had layers.
                std::cerr << "  layer tile did not bake: " << tile << " (" << key << ")" << std::endl;
                _stats.layerSetsTileFailed++;
                return {};
            }
            out.emplace_back(key, paa);
            // The layer's own normal, at the layer's own transform. Refused as an OBJECT
            // normal map (they are one-metre tiles in a tiled frame) and correct as a
            // LAYER normal, which is sampled at exactly that frame.
            const std::string nmoKey = "NMO_" + std::to_string(layer);
            const std::string nmo = emat.TextureOf(nmoKey);
            if (_options.normals && !nmo.empty())
            {
                const auto seen = _byNormal.find(LowerCopy(nmo));
                const std::string normalPaa =
                    seen != _byNormal.end() ? seen->second : _byNormal.emplace(LowerCopy(nmo), WriteNormal(nmo)).first->second;
                if (!normalPaa.empty())
                {
                    out.emplace_back(nmoKey, normalPaa);
                    _stats.layerNormalsWritten++;
                }
            }
        }
        if (out.empty())
        {
            // Named, because "the blend did not happen" has two very different causes and
            // only one of them is a converter bug. A material can carry a mask, four
            // Color_N and four Enabled_N and still name no tile above the base -- its
            // upper layers are flat colour overlays (mud, dirt) rather than surfaces --
            // and for those the collapse is not an approximation, it is the whole
            // material. Counting them keeps that population out of the defect column.
            _stats.layerSetsColourOnly++;
            return {}; // nothing above the base: a mask with nothing to weight
        }
        const std::string maskPaa = BakeTile(maskPath, nullptr, /*isMask=*/true);
        if (maskPaa.empty())
        {
            // The tiles baked and the mask did not, which leaves weights with nothing to
            // weight -- so the whole set goes back to the collapse rather than blending
            // by an implied uniform mask, which is not what the material says.
            std::cerr << "  layer MASK did not bake: " << maskPath << std::endl;
            _stats.layerSetsMaskFailed++;
            return {};
        }
        out.emplace_back("MaskMap", maskPaa);
        _stats.layerSetsWritten++;
        // The caller counted this as a collapse before it knew better; it is not one.
        if (_stats.baseLayerOnly)
            _stats.baseLayerOnly--;
        _stats.layerTilesWritten += out.size() - 1;
        return out;
    }

    //! One tile through the shared albedo path and the shared cache, so a library tile
    //! used by fifty objects at the same tint is written once.
    std::string BakeTile(const std::string& path, const float* tint, bool isMask = false)
    {
        std::string key = LowerCopy(path) + (isMask ? "|mask" : "|");
        if (tint)
        {
            char suffix[64];
            std::snprintf(suffix, sizeof(suffix), "|%.4f,%.4f,%.4f", tint[0], tint[1], tint[2]);
            key += suffix;
        }
        const auto cached = _byImage.find(key);
        if (cached != _byImage.end())
            return cached->second;
        return _byImage.emplace(key, Write(path, "", tint, isMask)).first->second;
    }

    std::string Write(const std::string& bcrPath, const std::string& opacityPath, const float* tint,
                      bool isMask = false)
    {
        Poseidon::Image albedo;
        if (!LoadEdds(_paks, bcrPath, albedo))
        {
            Unbound("BCRMap .edds does not decode");
            return {};
        }
        int width = albedo.width();
        int height = albedo.height();
        std::vector<uint8_t> pixels = albedo.data();
        pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

        // Rescale the shared tile to the layer's own colour -- see the note at the call
        // site. Per channel, so a grey tile can become olive; a channel whose mean is
        // zero is left alone rather than multiplied by infinity.
        //
        // `Color_N` is LINEAR and the tile's texels are sRGB-ENCODED, so the target has
        // to be encoded before the two are compared. Matching them raw makes every
        // tinted material about a stop and a half too dark: WaterTower_01's blend of
        // 0.19 linear was written as a 0.19 sRGB mean, i.e. 0.03 linear, and the tower
        // rendered as a flat black sphere. That the values are linear is what the
        // corpus says -- Color_3 0.381 and Color_4 0.402 are its steel and concrete,
        // which are light grey (sRGB ~0.65) and not the near-black those figures would
        // be if read as sRGB.
        if (tint)
        {
            const auto LinearToSrgb = [](double v)
            { return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055; };
            const size_t texels = pixels.size() / 4;
            for (int c = 0; c < 3; ++c)
            {
                double sum = 0.0;
                for (size_t t = 0; t < texels; ++t)
                    sum += pixels[t * 4 + static_cast<size_t>(c)];
                const double mean = sum / (255.0 * static_cast<double>(texels));
                if (!(mean > 1e-3))
                    continue;
                const double target = LinearToSrgb(std::clamp<double>(tint[c], 0.0, 1.0));
                const double gain = target / mean;
                for (size_t t = 0; t < texels; ++t)
                    pixels[t * 4 + static_cast<size_t>(c)] = static_cast<uint8_t>(
                        std::clamp(pixels[t * 4 + static_cast<size_t>(c)] * gain + 0.5, 0.0, 255.0));
            }
        }

        // Does the BCR's alpha carry anything? Counted BEFORE it is overwritten below,
        // because that is the only moment it exists. Enfusion is understood to pack
        // roughness there -- if so, this is where a specular map would come from, and
        // this count is the denominator that decides whether it is worth deriving.
        {
            uint8_t low = 255, high = 0;
            for (size_t at = 3; at < pixels.size(); at += 4)
            {
                low = std::min(low, pixels[at]);
                high = std::max(high, pixels[at]);
            }
            ((high - low) > 8 ? _stats.bcrAlphaVaries : _stats.bcrAlphaFlat)++;
        }

        // The opacity map is a separate single-channel image and may be authored at a
        // different resolution than the albedo. BC4 decodes into R with G and B left
        // at zero and BC7 greyscale into all three, so the maximum reads both without
        // having to know which the file was.
        bool hasAlpha = false;
        if (!opacityPath.empty())
        {
            Poseidon::Image mask;
            if (!LoadEdds(_paks, opacityPath, mask))
            {
                Unbound("OpacityMap .edds does not decode");
            }
            else
            {
                const std::vector<uint8_t>& mp = mask.data();
                for (int y = 0; y < height; ++y)
                {
                    const int sy = mask.height() == height ? y : y * mask.height() / height;
                    for (int x = 0; x < width; ++x)
                    {
                        const int sx = mask.width() == width ? x : x * mask.width() / width;
                        const size_t at = (static_cast<size_t>(sy) * mask.width() + sx) * 4;
                        const uint8_t value = std::max({mp[at], mp[at + 1], mp[at + 2]});
                        pixels[(static_cast<size_t>(y) * width + x) * 4 + 3] = value;
                    }
                }
                hasAlpha = true;
            }
        }
        if (!hasAlpha)
        {
            for (size_t at = 3; at < pixels.size(); at += 4)
                pixels[at] = 255;
        }

        const int sizeCap = isMask ? _options.maxMaskSize : _options.maxSize;
        while ((width > sizeCap || height > sizeCap) && width % 2 == 0 && height % 2 == 0 && width > 4 && height > 4)
            HalveRGBA(pixels, width, height);

        // Binary, and only after the downscale: `ClassifyAlpha` calls anything with
        // more than 2% partial-alpha texels a Blend, and `ClassifyGpuSection` drops a
        // Blend section out of the retained pass altogether. A hard 0/255 top mip is
        // what makes the same texture answer Cutout, which is what alpha-tests the
        // leaf cards at the 0.5 reference the retained path forces on them.
        if (hasAlpha)
        {
            for (size_t at = 3; at < pixels.size(); at += 4)
                pixels[at] = pixels[at] >= 128 ? 255 : 0;
        }

        const std::string virtualPath = VirtualNameFor(bcrPath, hasAlpha ? "_ca" : "", ".paa");
        const std::filesystem::path target = OnDiskPathFor(_outputRoot, virtualPath);
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
        // Uncompressed for a mask -- see maxMaskSize for why a block-compressed mask is
        // not a slightly worse mask but a different image.
        const Poseidon::PixelFormat format =
            isMask ? Poseidon::PixelFormat::ARGB8888 : (hasAlpha ? Poseidon::PixelFormat::DXT5 : Poseidon::PixelFormat::DXT1);
        if (!Poseidon::PAAEncoder::WritePAA(target.string(),
                                            Poseidon::Image::FromRGBA(width, height, std::move(pixels)), format))
        {
            Unbound("WritePAA refused it");
            return {};
        }

        // WritePAA accepts formats the PAA READER cannot read, and a texture that
        // fails to load is silent all the way to a white draw. Read it back.
        Poseidon::PAAInfo info;
        const Poseidon::DecodedImage back = Poseidon::DecodePAAFile(target.string());
        if (!ReadPAAInfo(target.string(), info) || !back.valid() || back.width != width || back.height != height)
        {
            std::filesystem::remove(target, ec);
            Unbound(".paa does not read back");
            return {};
        }

        _taken.insert(virtualPath);
        _stats.filesWritten++;
        _stats.filesWithAlpha += hasAlpha ? 1 : 0;
        if (hasAlpha)
            _imageHasAlpha.insert(LowerCopy(virtualPath));
        _stats.bytesWritten += static_cast<size_t>(std::filesystem::file_size(target, ec));
        return virtualPath;
    }

    const PakSet& _paks;
    std::string _outputRoot;
    TextureOptions _options;
    std::unordered_map<std::string, BakedMaterial> _byMaterial;
    std::unordered_map<std::string, std::string> _byImage;
    //! Keyed on the NMO alone, unlike `_byImage`: a normal map is folded with nothing,
    //! so two materials naming one NMO really are the same file.
    std::unordered_map<std::string, std::string> _byNormal;
    //! Colour `.paa` virtual paths (lower-cased) whose alpha carries a folded
    //! OpacityMap -- what the deployed `.emat` needs to know to say `AlphaTest 1`.
    std::unordered_set<std::string> _imageHasAlpha;
    std::unordered_set<std::string> _taken;
    TextureStats _stats;
};

// ---------------------------------------------------------------------------
// `.xob` -> MLOD P3DM
// ---------------------------------------------------------------------------

struct ConvertOptions
{
    float resolution = 1.0f;  //!< runtime detail value at which the first coarser LOD becomes active
    bool secondUv = false;    //!< emit both #UVSet# blocks when a part declares two sets
    bool flipWinding = false; //!< see the winding note at the top of this file
    //! Fact 6: negate the written normal AND swap corners 0/1, together. Half of this
    //! is worse than none of it -- see the note at the top of the file.
    bool fixOrientation = false;
    //! Fact 7: write `autocenter=0` so the engine stops moving the model's origin to
    //! its bounding-box centre. See the note at the top of the file.
    bool fixOrigin = false;
    //! Fact 8: write a `map` named property from the source path -- `rock`, `tree`,
    //! `bush`, `house`, `wall`, else `hide` -- so the engine's one map-type table
    //! (ResolveMapTypeProperty, ShapeLOD.cpp) classifies a converted model the way it
    //! classifies an authored one, instead of falling back to a name heuristic. The
    //! table drives wind sway and canopy shading (Object.cpp GCurrentFoliageKind,
    //! EngineWgpu WGR_INSTANCE_CANOPY_*), so a rock and a tree MUST NOT share a class.
    //! Default ON: `--no-map-property` restores the property-less file, for the A/B.
    bool mapProperty = true;
    //! Fact 9: write a Geometry LOD built from the `.xob`'s COLL shapes, with
    //! `ComponentXX` selections and a `#Mass#` tagg. Default ON: `--no-geometry`
    //! restores the visual-only file, for the A/B.
    bool geometry = true;
    //! Fact 9, for inspection: use every COLL shape whatever its layer, so a fire
    //! geometry mesh and a foliage volume become walls too. Off by default.
    bool geometryAllLayers = false;
    TextureOptions textures;
};

//! Fact 9's mass table, keyed by fact 8's class. The numbers are the brief's -- a
//! house 5000, wall 2000, rock 1000, tree 500 -- with bush and the unclassified rest
//! chosen to sit well above Object::IsPassable's 10 kg. Mass changes nothing about
//! WHERE a static object collides, only whether the character response runs at all,
//! so these are not tuning values.
float GeometryMassForClass(const std::string& mapClass)
{
    if (mapClass == "house")
        return 5000.0f;
    if (mapClass == "wall")
        return 2000.0f;
    if (mapClass == "rock")
        return 1000.0f;
    if (mapClass == "tree")
        return 500.0f;
    if (mapClass == "bush")
        return 200.0f;
    return 1000.0f;
}

//! Fact 8's table: what a converted model's `map` named property says, from where the
//! `.xob` lives in the pak. Spelled in the words `ResolveMapTypeTable` accepts.
//!
//! Measured over the 1,132 models `ev_DALL.wrp` places (`%TEMP%\rf\manifestALL.txt`,
//! 2026-08-16): 30 under `Assets/Rocks/`, 66 `Assets/Vegetation/Tree/t_*`, 18
//! `Assets/Vegetation/Bush/b_*`, 11 other vegetation (plants `p_*`, vegetables, and the
//! `t_stump_debris_01` / `t_trunk_debris_01` tree parts), 30 `Structures/*/houses`, ~250
//! `Structures/Walls/*`, the rest props, signs, infrastructure and decals. The rules:
//!
//!   Assets/Rocks/**                                        rock
//!   Assets/Vegetation/Tree/**/t_*   (not stump/fallen/debris/trunk)   tree
//!   Assets/Vegetation/Bush/**       (and any b_* under Vegetation)    bush
//!   Assets/Structures/**/Houses/**  and Structures/Houses/**          house
//!   Assets/Structures/Walls/**                             wall
//!   everything else                                        hide
//!
//! `hide` is what an ABSENT property already resolves to (ShapeLOD.cpp: the unknown
//! case is MapHide), so nothing outside the five classes changes; it is written anyway
//! so a reader of the file sees a decision rather than an omission. A stump, a fallen
//! trunk or a piece of debris is `hide` and not `tree` for the reason the engine's own
//! fallback gives: it has no crown to bend, and swaying one reads as the ground moving.
//! Nothing here is `forest`, `small tree` or `building`: the first two would need a
//! measurement of canopy size this converter does not make, and `building` would put
//! a map symbol on every kerb and pole.
std::string MapPropertyForXob(const std::string& sourcePath)
{
    std::string path = LowerCopy(sourcePath);
    for (char& c : path)
        if (c == '\\')
            c = '/';
    const size_t slash = path.rfind('/');
    const std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    const auto has = [&](const char* needle) { return path.find(needle) != std::string::npos; };
    const auto baseHas = [&](const char* needle) { return base.find(needle) != std::string::npos; };

    if (has("/rocks/"))
        return "rock";
    if (has("/vegetation/"))
    {
        const bool debris = baseHas("stump") || baseHas("fallen") || baseHas("debris") || baseHas("trunk");
        if (base.rfind("b_", 0) == 0 || has("/vegetation/bush/"))
            return debris ? "hide" : "bush";
        if (base.rfind("t_", 0) == 0 && has("/vegetation/tree/"))
            return debris ? "hide" : "tree";
        return "hide";
    }
    if (has("/structures/"))
    {
        if (has("/houses/"))
            return "house";
        if (has("/structures/walls/"))
            return "wall";
    }
    return "hide";
}

//! `XOB_FIX_ORIENTATION=1` turns fact 6's correction on without a command line change,
//! so a batch script that already exists can be A/B'd from the environment.
inline bool OrientationFixFromEnv()
{
    const char* value = std::getenv("XOB_FIX_ORIENTATION");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

//! `XOB_FIX_ORIGIN=1`, likewise, for fact 7.
inline bool OriginFixFromEnv()
{
    const char* value = std::getenv("XOB_FIX_ORIGIN");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

struct ConvertStats
{
    size_t lodIndex = 0;
    size_t lodCount = 0;
    size_t lodsWritten = 0;
    size_t lodsDropped = 0;
    float threshold = 0.0f;
    size_t parts = 0;
    size_t points = 0;
    size_t triangles = 0;
    size_t droppedUnreferenced = 0; //!< source vertices no triangle indexes
    size_t materials = 0;
    size_t partsTextured = 0;     //!< parts whose material resolved to a written `.paa`
    size_t partsWithMaterial = 0; //!< parts naming a DEPLOYED material file rather than the pak path
    size_t partsHidden = 0;       //!< parts dropped whole: unresolvable decal shells / water-erase surfaces
    size_t crossAlongNormal = 0;  //!< triangles whose edge cross agrees with the stored normals
    //! Fact 6's absolute check, over LOD0 only: does the WRITTEN normal point away from
    //! the LOD's point-cloud centroid? Meaningful per model only for a closed convex
    //! solid (a boulder); over a corpus the aggregate is still the number that moves
    //! when the sign is wrong, because no corpus is majority-concave.
    size_t normalOutward = 0;
    size_t normalInward = 0;
    float bboxMin[3] = {0.0f, 0.0f, 0.0f};
    float bboxMax[3] = {0.0f, 0.0f, 0.0f};
    std::string mapProperty; //!< fact 8: the `map` value written on every LOD, "" when none
    //! Fact 9: the geometry LOD. `geometryWritten` false with `collisionShapes > 0`
    //! is a model whose shapes are all fire/view/foliage volumes -- correct, and
    //! worth seeing on the model line as `geo 0 comps`.
    bool collisionPresent = false; //!< the `.xob` has a COLL chunk
    bool collisionCloses = true;   //!< its record walk closed
    std::string collisionError;    //!< why it did not
    size_t collisionShapes = 0;    //!< records in COLL
    size_t collisionBlocking = 0;  //!< on a character-blocking layer
    bool geometryWritten = false;
    GeometryLodStats geometry;
    float geometryMass = 0.0f;

    float Extent(int axis) const { return bboxMax[axis] - bboxMin[axis]; }
};

//! Selects the finest visual LOD: the MAXIMUM threshold. See fact 1 at the top --
//! taking the minimum is the mistake this exists to make impossible.
size_t FinestLodIndex(const XobHeader& header)
{
    size_t best = 0;
    for (size_t i = 1; i < header.lods.size(); ++i)
        if (header.lods[i].threshold > header.lods[best].threshold)
            best = i;
    return best;
}

//! XOB thresholds and this engine's resolutions do not have proven compatible
//! units. Use the native content convention instead: measured Arma 3 tree and rock
//! ladders are 1/2/3/4[/5], independent of their very different bounds. `base`
//! remains an explicit calibration multiplier rather than hiding a guessed unit
//! conversion in the exporter.
float VisualLodResolution(size_t rank, float base)
{
    return base * static_cast<float>(rank + 1);
}

//! `sourcePath` is the `.xob`'s own path in the pak; it decides fact 8's `map` property
//! and nothing else. Empty disables the property (a caller with no path has no class).
bool ConvertXob(const std::vector<uint8_t>& bytes, const ConvertOptions& options, TextureBaker* baker,
                const std::string& sourcePath, Fmt::MLOD::WriteModel& out, ConvertStats& stats, std::string& error)
{
    const XobHeader header = ReadXobHeader(bytes.data(), bytes.size());
    if (!header.valid())
    {
        error = "HEAD: " + header.error;
        return false;
    }
    if (header.lods.empty())
    {
        error = "no LODs";
        return false;
    }

    if (!(options.resolution > 0.0f) || !std::isfinite(options.resolution))
    {
        error = "LOD resolution anchor must be finite and positive";
        return false;
    }

    stats.lodCount = header.lods.size();
    stats.lodIndex = FinestLodIndex(header);
    stats.threshold = header.lods[stats.lodIndex].threshold;
    stats.mapProperty = options.mapProperty && !sourcePath.empty() ? MapPropertyForXob(sourcePath) : std::string();
    std::unordered_set<std::string> materialPaths;
    float boxMin[3] = {1e30f, 1e30f, 1e30f};
    float boxMax[3] = {-1e30f, -1e30f, -1e30f};

    // The corpus is already finest-first, but order by threshold here so a valid
    // file with descriptors in another order still reaches MLOD's ascending
    // resolution contract. Equal thresholds are impossible in the measured corpus.
    std::vector<size_t> lodOrder(header.lods.size());
    for (size_t i = 0; i < lodOrder.size(); ++i)
        lodOrder[i] = i;
    std::stable_sort(lodOrder.begin(), lodOrder.end(),
                     [&](size_t a, size_t b) { return header.lods[a].threshold > header.lods[b].threshold; });

    out.lods.clear();
    for (size_t rank = 0; rank < lodOrder.size(); ++rank)
    {
        const size_t lodIndex = lodOrder[rank];
        XobLod lod;
        std::string lodError;
        if (!ReadXobLod(bytes.data(), bytes.size(), header, lodIndex, lod, lodError))
        {
            if (rank == 0)
            {
                error = "LOD " + std::to_string(lodIndex) + ": " + lodError;
                return false;
            }
            stats.lodsDropped++;
            continue;
        }

        Fmt::MLOD::WriteLod writeLod;
        // Fact 7. On every LOD, not just LOD0: `Model.cpp:67` reads lodLevels[0] but
        // `LODShape::PropertyValue` consults the GEOMETRY LOD first, and a converted
        // model has no geometry LOD today but may grow one.
        if (options.fixOrigin)
            writeLod.properties.push_back({"autocenter", "0"});
        // Fact 8, on every LOD for the same reason as autocenter: `PropertyValue` reads
        // the geometry LOD first and LOD 0 second, and which LOD is which is the
        // engine's decision, not this file's.
        if (!stats.mapProperty.empty())
            writeLod.properties.push_back({"map", stats.mapProperty});
        writeLod.resolution = VisualLodResolution(rank, options.resolution);
        if (!(writeLod.resolution < 900.0f) || !std::isfinite(writeLod.resolution))
        {
            error = "LOD " + std::to_string(lodIndex) + " maps outside the visual resolution range";
            return false;
        }
        bool anySecondSet = false;

        for (const XobPart& part : lod.parts)
        {
            if (part.uvSets.empty() || part.uvSets[0].size() != part.positions.size())
            {
                error = "LOD " + std::to_string(lodIndex) + " part has no usable UV set 0";
                return false;
            }

            // The material first, because it can say the whole part must not draw
            // (BakedMaterial::hideFaces) -- and then its points must not be written
            // either, or they would sit in the file referenced by nothing.
            std::string materialSource;
            if (part.materialIndex < header.materials.size())
                materialSource = header.materials[part.materialIndex].path;
            const BakedMaterial* bakedPart = nullptr;
            if (baker && !materialSource.empty())
            {
                bakedPart = &baker->For(materialSource);
                if (bakedPart->hideFaces)
                {
                    stats.partsHidden++;
                    continue;
                }
            }

            // Only referenced source vertices become points. Every point must be
            // referenced for the round-trip check below to be an identity.
            std::vector<int32_t> remap(part.positions.size(), -1);
            for (uint16_t index : part.indices)
                if (index < remap.size())
                    remap[index] = 0;

            for (size_t v = 0; v < part.positions.size(); ++v)
            {
                if (remap[v] != 0)
                {
                    stats.droppedUnreferenced++;
                    continue;
                }
                remap[v] = static_cast<int32_t>(writeLod.points.size());

                Fmt::MLOD::WritePoint point;
                point.position = {part.positions[v].x, part.positions[v].y, part.positions[v].z};
                point.flags = 0;
                writeLod.points.push_back(point);
                // Fact 6: `.xob` normals are outward, this engine's are inward. The
                // reader stays faithful to the file; the convention change belongs here,
                // in the converter, and is paired with the winding swap below.
                const float sign = options.fixOrientation ? -1.0f : 1.0f;
                writeLod.normals.push_back(
                    {sign * part.normals[v].x, sign * part.normals[v].y, sign * part.normals[v].z});

                for (int axis = 0; axis < 3; ++axis)
                {
                    const float value = axis == 0   ? part.positions[v].x
                                        : axis == 1 ? part.positions[v].y
                                                    : part.positions[v].z;
                    boxMin[axis] = std::min(boxMin[axis], value);
                    boxMax[axis] = std::max(boxMax[axis], value);
                }
            }

            std::string texturePath;
            std::string materialPath = materialSource;
            if (bakedPart != nullptr)
            {
                texturePath = bakedPart->colourPaa;
                // Only when a file was actually written. Naming a material file that
                // does not exist is strictly worse than naming the source `.emat`:
                // both fail to open, but the second at least says which material it
                // meant.
                if (!bakedPart->MaterialField().empty())
                {
                    materialPath = bakedPart->MaterialField();
                    stats.partsWithMaterial++;
                }
            }
            if (!texturePath.empty())
                stats.partsTextured++;
            if (!materialPath.empty())
                materialPaths.insert(materialPath);

            const bool hasSecond = part.uvSets.size() > 1 && part.uvSets[1].size() == part.positions.size();
            anySecondSet = anySecondSet || hasSecond;

            for (size_t t = 0; t + 2 < part.indices.size(); t += 3)
            {
                uint16_t corner[3] = {part.indices[t], part.indices[t + 1], part.indices[t + 2]};
                // Two independent reasons to swap, so XOR: asking for both is asking for
                // the original order back, and silently applying one twice would be the
                // kind of cancellation this file exists to make impossible.
                if (options.flipWinding != options.fixOrientation)
                    std::swap(corner[0], corner[1]);

                {
                    const XobVec3& p0 = part.positions[corner[0]];
                    const XobVec3& p1 = part.positions[corner[1]];
                    const XobVec3& p2 = part.positions[corner[2]];
                    const float e1[3] = {p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
                    const float e2[3] = {p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
                    const float cross[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                            e1[0] * e2[1] - e1[1] * e2[0]};
                    float mean[3] = {0.0f, 0.0f, 0.0f};
                    for (int c = 0; c < 3; ++c)
                    {
                        mean[0] += part.normals[corner[c]].x;
                        mean[1] += part.normals[corner[c]].y;
                        mean[2] += part.normals[corner[c]].z;
                    }
                    if (cross[0] * mean[0] + cross[1] * mean[1] + cross[2] * mean[2] > 0.0f)
                        stats.crossAlongNormal++;
                }

                Fmt::MLOD::WriteFace face;
                face.vertexCount = 3;
                face.flags = 0;
                face.material = materialPath;
                face.texture = texturePath;
                for (int c = 0; c < 3; ++c)
                {
                    const int32_t point = remap[corner[c]];
                    face.vertices[c].point = point;
                    face.vertices[c].normal = point;
                    face.vertices[c].u = part.uvSets[0][corner[c]].u;
                    face.vertices[c].v = part.uvSets[0][corner[c]].v;
                    if (hasSecond)
                    {
                        face.vertices[c].u1 = part.uvSets[1][corner[c]].u;
                        face.vertices[c].v1 = part.uvSets[1][corner[c]].v;
                    }
                }
                writeLod.faces.push_back(std::move(face));
            }
        }

        if (writeLod.points.empty() || writeLod.faces.empty())
        {
            error = "LOD " + std::to_string(lodIndex) + " has no geometry";
            return false;
        }

        // Fact 6's absolute orientation check. LOD0 only: it is the level that is drawn
        // near the camera, it is the only one whose geometry is dense enough for the
        // centroid to be a fair reference, and running it on every rung would say the
        // same thing five times. Faces whose centroid coincides with the model's are
        // skipped rather than counted as either sign.
        if (rank == 0 && writeLod.points.size() >= 4)
        {
            float cx = 0.0f, cy = 0.0f, cz = 0.0f;
            for (const Fmt::MLOD::WritePoint& point : writeLod.points)
            {
                cx += point.position.x;
                cy += point.position.y;
                cz += point.position.z;
            }
            const float invN = 1.0f / static_cast<float>(writeLod.points.size());
            cx *= invN;
            cy *= invN;
            cz *= invN;
            for (const Fmt::MLOD::WriteFace& face : writeLod.faces)
            {
                float nx = 0.0f, ny = 0.0f, nz = 0.0f;
                float fx = 0.0f, fy = 0.0f, fz = 0.0f;
                for (int c = 0; c < face.vertexCount; ++c)
                {
                    // -1 is the sanctioned "no normal" and would index out of range.
                    if (face.vertices[c].normal < 0)
                        continue;
                    const auto& normal = writeLod.normals[static_cast<size_t>(face.vertices[c].normal)];
                    nx += normal.x;
                    ny += normal.y;
                    nz += normal.z;
                    const auto& position = writeLod.points[static_cast<size_t>(face.vertices[c].point)].position;
                    fx += position.x;
                    fy += position.y;
                    fz += position.z;
                }
                const float invC = 1.0f / static_cast<float>(face.vertexCount);
                fx = fx * invC - cx;
                fy = fy * invC - cy;
                fz = fz * invC - cz;
                const float dot = nx * fx + ny * fy + nz * fz;
                if (dot > 0.0f)
                    stats.normalOutward++;
                else if (dot < 0.0f)
                    stats.normalInward++;
            }
        }

        writeLod.secondUVChannel = options.secondUv && anySecondSet;
        stats.parts += lod.parts.size();
        stats.points += writeLod.points.size();
        stats.triangles += writeLod.faces.size();
        stats.lodsWritten++;
        out.lods.push_back(std::move(writeLod));
    }

    stats.materials = materialPaths.size();
    for (int axis = 0; axis < 3; ++axis)
    {
        stats.bboxMin[axis] = boxMin[axis];
        stats.bboxMax[axis] = boxMax[axis];
    }

    // Fact 9: the Geometry LOD, from the COLL chunk. After the visual ladder, so
    // MLOD's ascending-resolution contract holds (1e13 is above any visual value),
    // and carrying the same properties as every visual LOD because
    // LODShape::PropertyValue consults the geometry LOD FIRST -- a geometry LOD
    // without `autocenter=0` would undo fact 7 on the model that has it.
    if (options.geometry && !out.lods.empty())
    {
        const XobCollision collision = ReadXobCollision(bytes.data(), bytes.size(), header);
        stats.collisionPresent = collision.present;
        stats.collisionCloses = collision.closes || !collision.present;
        stats.collisionError = collision.error;
        stats.collisionShapes = collision.shapes.size();
        for (const XobCollisionShape& shape : collision.shapes)
            if (XobLayerBlocksCharacters(shape.layer))
                ++stats.collisionBlocking;
        if (collision.present && !collision.shapes.empty())
        {
            GeometryLodOptions geometryOptions;
            geometryOptions.totalMass = GeometryMassForClass(stats.mapProperty);
            geometryOptions.allLayers = options.geometryAllLayers;
            // A boulder is solid, lumpy and sometimes modelled without an underside;
            // a building's open pieces are sheets around rooms and its concavities
            // are the rooms. Only the class knows which, so rocks take their mesh
            // pieces whole at a looser tolerance (a stone's bumps, up to half its
            // radius, become the intersection of its face half-spaces -- measured on
            // GraniteBeachCluster_01/02: 9 and 11 stones, 5-50% of radius), chunk
            // what is still concave, and everything else keeps the tight tolerance
            // that sends a house shell to slabs.
            const bool rock = stats.mapProperty == "rock";
            geometryOptions.solidMesh = rock;
            if (rock)
            {
                geometryOptions.nearConvexTolerance = 0.5f;
                geometryOptions.nearConvexMaxTolerance = 2.0f;
            }
            Fmt::MLOD::WriteLod geometryLod;
            if (BuildGeometryLod(collision, geometryOptions, geometryLod, stats.geometry))
            {
                geometryLod.properties = out.lods.front().properties;
                stats.geometryWritten = true;
                stats.geometryMass = geometryOptions.totalMass;
                out.lods.push_back(std::move(geometryLod));
            }
        }
    }

    return !out.lods.empty();
}

// ---------------------------------------------------------------------------
// Round-trip through the engine's own reader
// ---------------------------------------------------------------------------

// This is the check that makes a written model trustworthy, and it is an exact
// identity rather than a tolerance: with one point per source vertex and every point
// referenced, MLODLoader's per-point vertex merge produces exactly one vertex per
// point, and its sortVerticesByPointIndex then puts vertex k at point k (all points
// carry the same flags, so the sort is by point index alone). Anything that breaks
// that -- a stride, an index base, a UV taken from the wrong stream -- shows up as a
// mismatch count, not as a "loaded fine".
struct RoundTrip
{
    bool ok = false;
    std::string reason;
    size_t vertices = 0;
    size_t triangles = 0;
    size_t resolutionMismatches = 0;
    size_t positionMismatches = 0;
    size_t uvMismatches = 0;
    size_t topologyMismatches = 0;
};

RoundTrip VerifyRoundTrip(const std::string& filePath, const Fmt::MLOD::WriteModel& written)
{
    RoundTrip result;
    Poseidon::Model::Model loaded;
    try
    {
        loaded = Fmt::MLODLoader::load(filePath);
    }
    catch (const std::exception& e)
    {
        result.reason = std::string("MLODLoader refused it: ") + e.what();
        return result;
    }

    if (loaded.lodLevels.size() != written.lods.size())
    {
        result.reason =
            "LOD count " + std::to_string(loaded.lodLevels.size()) + " != " + std::to_string(written.lods.size());
        return result;
    }
    for (size_t lodIndex = 0; lodIndex < written.lods.size(); ++lodIndex)
    {
        const auto& source = written.lods[lodIndex];
        const auto& loadedLod = loaded.lodLevels[lodIndex];
        const auto& mesh = loadedLod.mesh;
        result.vertices += mesh.vertices.size();
        result.triangles += mesh.triangles.size();

        if (loadedLod.resolution != source.resolution)
            result.resolutionMismatches++;
        if (mesh.vertices.size() != source.points.size())
        {
            result.reason = "LOD " + std::to_string(lodIndex) + " vertex count " +
                            std::to_string(mesh.vertices.size()) + " != point count " +
                            std::to_string(source.points.size());
            return result;
        }
        if (mesh.triangles.size() + mesh.quads.size() != source.faces.size())
        {
            result.reason = "LOD " + std::to_string(lodIndex) + " face count " + std::to_string(mesh.triangles.size()) +
                            " (+" + std::to_string(mesh.quads.size()) +
                            " quads) != " + std::to_string(source.faces.size());
            return result;
        }

        for (size_t i = 0; i < source.points.size(); ++i)
        {
            const auto& want = source.points[i].position;
            const auto& got = mesh.vertices[i].position;
            if (got.x != want.x || got.y != want.y || got.z != want.z)
                result.positionMismatches++;
        }

        // The loader splits faces into a triangle vector and a quad vector; the
        // source position of each is originalIndex. The visual LODs are all
        // triangles; the geometry LOD (fact 9) mixes quads in, so index by
        // originalIndex rather than assuming the two orders coincide.
        std::vector<const Poseidon::Model::Triangle*> triangleAt(source.faces.size(), nullptr);
        std::vector<const Poseidon::Model::Quad*> quadAt(source.faces.size(), nullptr);
        for (const auto& triangle : mesh.triangles)
            if (triangle.originalIndex < triangleAt.size())
                triangleAt[triangle.originalIndex] = &triangle;
        for (const auto& quad : mesh.quads)
            if (quad.originalIndex < quadAt.size())
                quadAt[quad.originalIndex] = &quad;

        for (size_t f = 0; f < source.faces.size(); ++f)
        {
            const auto& face = source.faces[f];
            // MLODLoader.hpp:384 swaps corners 0/1 (and 2/3 on a quad) on load; undo
            // it to compare.
            uint32_t got[4] = {0, 0, 0, 0};
            if (face.vertexCount == 3 && triangleAt[f])
            {
                got[0] = triangleAt[f]->indices[1];
                got[1] = triangleAt[f]->indices[0];
                got[2] = triangleAt[f]->indices[2];
            }
            else if (face.vertexCount == 4 && quadAt[f])
            {
                got[0] = quadAt[f]->indices[1];
                got[1] = quadAt[f]->indices[0];
                got[2] = quadAt[f]->indices[3];
                got[3] = quadAt[f]->indices[2];
            }
            else
            {
                result.topologyMismatches++;
                continue;
            }
            for (int c = 0; c < face.vertexCount; ++c)
            {
                if (got[c] != static_cast<uint32_t>(face.vertices[c].point))
                {
                    result.topologyMismatches++;
                    break;
                }
            }
            for (int c = 0; c < face.vertexCount; ++c)
            {
                const uint32_t vertex = got[c];
                if (vertex >= mesh.vertices.size())
                    continue;
                if (mesh.vertices[vertex].uv.u != face.vertices[c].u ||
                    mesh.vertices[vertex].uv.v != face.vertices[c].v)
                {
                    result.uvMismatches++;
                    break;
                }
            }
        }
    }

    result.ok = result.resolutionMismatches == 0 && result.positionMismatches == 0 && result.uvMismatches == 0 &&
                result.topologyMismatches == 0;
    if (!result.ok)
        result.reason = std::to_string(result.resolutionMismatches) + " resolutions, " +
                        std::to_string(result.positionMismatches) + " positions, " +
                        std::to_string(result.uvMismatches) + " UVs, " + std::to_string(result.topologyMismatches) +
                        " triangles differ";
    return result;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

//! `Assets/Vegetation/.../t_picea_abies_0.xob` ->
//! `reforger\everon\models\assets\vegetation\...\t_picea_abies_0.p3d`.
//!
//! Lowercased and backslash-separated because that is what the engine's virtual file
//! system compares on (ShapeAdapter.cpp:300-305 lowercases before every lookup).
std::string VirtualPathFor(const std::string& prefix, const std::string& sourcePath)
{
    std::string out = LowerCopy(sourcePath);
    const size_t dot = out.rfind('.');
    if (dot != std::string::npos)
        out = out.substr(0, dot);
    out += ".p3d";
    for (char& c : out)
        if (c == '/')
            c = '\\';
    if (prefix.empty())
        return out;
    std::string head = LowerCopy(prefix);
    for (char& c : head)
        if (c == '/')
            c = '\\';
    if (head.back() != '\\')
        head += '\\';
    return head + out;
}

std::filesystem::path OnDiskPathFor(const std::string& outputRoot, const std::string& virtualPath)
{
    std::string relative = virtualPath;
    for (char& c : relative)
        if (c == '\\')
            c = '/';
    return std::filesystem::path(outputRoot) / relative;
}

// ---------------------------------------------------------------------------
// One model, end to end
// ---------------------------------------------------------------------------

struct ModelResult
{
    std::string virtualPath;
    ConvertStats stats;
    RoundTrip roundTrip;
    bool written = false;
    std::string error;
};

ModelResult ConvertOne(const PakSet& paks, const std::string& sourcePath, const std::string& outputRoot,
                       const std::string& prefix, const ConvertOptions& options, TextureBaker* baker)
{
    ModelResult result;
    const std::string* stored = paks.StoredPath(sourcePath);
    result.virtualPath = VirtualPathFor(prefix, stored ? *stored : sourcePath);

    std::vector<uint8_t> bytes;
    if (!paks.Read(sourcePath, bytes))
    {
        result.error = "not in any .pak";
        return result;
    }

    Fmt::MLOD::WriteModel model;
    // The stored (pak) spelling of the path is what fact 8 classifies on; the query
    // string may be a lower-cased or slash-flipped alias of it.
    if (!ConvertXob(bytes, options, baker, stored ? *stored : sourcePath, model, result.stats, result.error))
        return result;

    const std::filesystem::path target = OnDiskPathFor(outputRoot, result.virtualPath);
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    try
    {
        Fmt::MLODWriter::write(model, target.string());
    }
    catch (const std::exception& e)
    {
        result.error = std::string("MLOD writer refused it: ") + e.what();
        return result;
    }
    result.written = true;
    result.roundTrip = VerifyRoundTrip(target.string(), model);
    return result;
}

void PrintModelLine(const ModelResult& result)
{
    std::cout << "  " << std::left << std::setw(94) << result.virtualPath << " ";
    if (!result.error.empty())
    {
        std::cout << "FAILED: " << result.error << "\n";
        return;
    }
    const ConvertStats& s = result.stats;
    std::cout << std::right << std::setw(7) << s.triangles << " tri " << std::setw(7) << s.points << " vtx  LODs "
              << s.lodsWritten << "/" << s.lodCount;
    if (s.lodsDropped)
        std::cout << " (" << s.lodsDropped << " refused)";
    std::cout << " finest " << s.lodIndex << " thr " << std::fixed << std::setprecision(3) << s.threshold << "  bbox "
              << std::setprecision(2) << s.Extent(0) << " x " << s.Extent(1) << " x " << s.Extent(2) << " m  tex "
              << s.partsTextured << "/" << s.parts << "  ";
    // Facts 6 and 7, per model, because both are per-model properties and both are
    // invisible in every other number on this line. On a closed convex solid (a
    // boulder) `nrm` is 1.00 or 0.00 and nothing in between; `originY` is the metres
    // the model sinks if it is autocentred.
    if (s.normalOutward + s.normalInward)
        std::cout << "nrm " << std::setprecision(2)
                  << (static_cast<double>(s.normalOutward) / static_cast<double>(s.normalOutward + s.normalInward))
                  << " out  ";
    std::cout << "originY " << std::setprecision(2) << ((s.bboxMin[1] + s.bboxMax[1]) * 0.5f) << "  ";
    // Fact 8: the class the engine will read back. `map -` means no property was written.
    std::cout << "map " << (s.mapProperty.empty() ? "-" : s.mapProperty) << "  ";
    // Fact 9: the geometry LOD. `geo -` is --no-geometry or a source with no COLL
    // chunk; `geo 0 comps` is a COLL whose every shape is a fire/view/foliage
    // volume, i.e. walk-through by design.
    if (!s.collisionPresent)
        std::cout << "geo -  ";
    else if (!s.geometryWritten)
        std::cout << "geo 0 comps (" << s.collisionShapes << " shapes, " << s.collisionBlocking << " blocking)  ";
    else
        std::cout << "geo " << s.geometry.Components() << " comps (" << s.geometry.componentsPrimitive << " prim, "
                  << s.geometry.componentsHull << " hull, " << s.geometry.componentsPiece << " piece, "
                  << s.geometry.componentsPrism << " slab" << (s.geometry.componentsDropped ? ", " : "")
                  << (s.geometry.componentsDropped ? std::to_string(s.geometry.componentsDropped) + " dropped" : "")
                  << ") " << s.geometry.faces << " faces " << std::setprecision(0) << s.geometryMass << " kg  ";
    if (!s.collisionCloses)
        std::cout << "COLL: " << s.collisionError << "  ";
    std::cout << (result.roundTrip.ok ? "round-trips" : "ROUND-TRIP: " + result.roundTrip.reason) << "\n";
}

//! Fact 9's corpus line: how many written models the engine can collide with. The
//! COLL closure count is the one that says the reader is right; the geometry count
//! is the one that says the world will be solid.
void PrintGeometryReport(const std::vector<ModelResult>& results)
{
    size_t written = 0, withColl = 0, closes = 0, withGeometry = 0, blockingNone = 0, components = 0, dropped = 0;
    for (const ModelResult& result : results)
    {
        if (!result.written)
            continue;
        ++written;
        const ConvertStats& s = result.stats;
        if (!s.collisionPresent)
            continue;
        ++withColl;
        if (s.collisionCloses)
            ++closes;
        if (s.geometryWritten)
        {
            ++withGeometry;
            components += s.geometry.Components();
            dropped += s.geometry.componentsDropped;
        }
        else if (s.collisionBlocking == 0)
            ++blockingNone;
    }
    if (written == 0)
        return;
    std::cout << "Geometry:   " << withGeometry << " / " << written << " written models carry a geometry LOD ("
              << components << " convex components, " << dropped << " dropped over the cap); " << withColl
              << " sources have a COLL chunk, " << closes << " of them close, " << blockingNone
              << " have no character-blocking shape (walk-through in Reforger too).\n";
}

//! The one number that says whether the models will draw textured: a part with no
//! `.paa` in its face slot is a section that reaches the renderer with texture_id 0
//! and logs "section draws UNTEXTURED".
void PrintTextureReport(const TextureStats& stats, size_t partsTextured, size_t partsTotal)
{
    std::cout << "\nTextures:   " << stats.materialsBound << " / " << stats.materialsSeen
              << " materials resolved an albedo; " << stats.filesWritten << " .paa written (" << stats.filesWithAlpha
              << " with a cutout mask), " << std::fixed << std::setprecision(1)
              << static_cast<double>(stats.bytesWritten) / 1e6 << " MB.\n";
    std::cout << "            " << partsTextured << " / " << partsTotal
              << " written parts name a texture; the rest will draw UNTEXTURED.\n";
    if (stats.layerSetsWritten)
        std::cout << "            " << stats.layerSetsWritten
                  << " MatPBRMulti materials deployed with their MASK and layer tiles (" << stats.layerTilesWritten
                  << " tiles, " << stats.layerNormalsWritten
                  << " layer normals) -- the runtime blends these per fragment.\n";
    if (stats.layerSetsColourOnly)
        std::cout << "            " << stats.layerSetsColourOnly
                  << " masked MatPBRMulti materials name NO tile above the base -- their upper layers are flat\n"
                     "            colour overlays, so one tile IS the material and the collapse below is exact.\n";
    if (stats.baseLayerOnly)
        std::cout << "            " << stats.baseLayerOnly
                  << " MatPBRMulti materials bound one shared library tile instead of a blend, " << stats.layerTinted
                  << " of them rescaled to the layers' blended Color_N;\n            " << stats.layerMaskWeighted
                  << " took their layer weights from the MaskMap (the rest weigh every present layer equally).\n";
    if (stats.normalsWritten || stats.rvmatsWritten)
    {
        std::cout << "Normals:    " << stats.materialsWithNormal << " / " << stats.materialsSeen
                  << " materials resolved a normal map; " << stats.normalsWritten << " DXT5nm .paa and "
                  << stats.rvmatsWritten << " .rvmat written.\n";
        if (!stats.packings.empty())
        {
            std::cout << "            source packing:";
            for (const auto& [name, count] : stats.packings)
                std::cout << "  " << name << " " << count;
            std::cout << "   (RG = X in red, AG = X in alpha; `none` was refused)\n";
        }
    }
    if (stats.ematsWritten)
        std::cout << "Materials:  " << stats.ematsWritten << " .emat deployed under the texture prefix ("
                  << stats.ematsCutout << " alpha-tested); sections name them instead of the pak path.\n";
    if (stats.ematsColourOnly)
        std::cout << "            " << stats.ematsColourOnly
                  << " of those carry a flat Color and NO baked image -- Enfusion's coarse-LOD\n"
                     "            materials, which the engine renders through a procedural colour texture.\n"
                     "            Before MAT-054 these were not deployed at all and drew WHITE. A sharp\n"
                     "            rise here with no new coarse LODs means the albedo baker regressed.\n";
    if (stats.materialsHidden)
        std::cout << "            " << stats.materialsHidden
                  << " unresolvable overlay materials (decal shells / water-erase) had their faces DROPPED --\n"
                     "            drawn untextured they sit opaque on top of the surface they decorate.\n";
    if (stats.bcrAlphaVaries || stats.bcrAlphaFlat)
        std::cout << "            BCR alpha varies in " << stats.bcrAlphaVaries << " of "
                  << (stats.bcrAlphaVaries + stats.bcrAlphaFlat)
                  << " albedos. It is ROUGHNESS, not opacity (measured 99.8-100% partial on opaque and "
                     "cutout sources alike); opacity is the OpacityMap, folded into the _ca alpha.\n";
    if (!stats.failures.empty())
    {
        std::cout << "Materials that bound nothing:\n";
        for (const auto& [reason, count] : stats.failures)
            std::cout << "  " << std::left << std::setw(44) << reason << count << "\n";
    }
}

// ---------------------------------------------------------------------------
// Subcommand: inspect
// ---------------------------------------------------------------------------

int Inspect(const std::string& root, const std::string& filter, int limit)
{
    PakSet paks;
    if (!paks.Open(root))
    {
        std::cerr << "Error: " << paks.Error() << "\n";
        return 1;
    }
    const std::string needle = LowerCopy(filter);

    int shown = 0;
    for (const PakArchive& archive : paks.Archives())
    {
        if (!archive.IsOpen())
            continue;
        for (const PakEntry& entry : archive.Entries())
        {
            const std::string lower = LowerCopy(entry.path);
            if (!EndsWith(lower, ".xob"))
                continue;
            if (!needle.empty() && lower.find(needle) == std::string::npos)
                continue;
            if (shown >= limit)
                return 0;
            std::vector<uint8_t> bytes;
            if (!archive.Read(entry, bytes))
                continue;
            const XobHeader header = ReadXobHeader(bytes.data(), bytes.size());
            std::cout << entry.path << "\n";
            if (!header.valid())
            {
                std::cout << "  HEAD failed: " << header.error << "\n";
                shown++;
                continue;
            }
            std::cout << "  head bbox " << std::fixed << std::setprecision(3) << header.bboxMin.x << " "
                      << header.bboxMin.y << " " << header.bboxMin.z << "  ..  " << header.bboxMax.x << " "
                      << header.bboxMax.y << " " << header.bboxMax.z << "   (" << (header.bboxMax.x - header.bboxMin.x)
                      << " x " << (header.bboxMax.y - header.bboxMin.y) << " x "
                      << (header.bboxMax.z - header.bboxMin.z) << " m)\n";
            for (size_t i = 0; i < header.lods.size(); ++i)
            {
                size_t tris = 0, verts = 0;
                for (const XobPartDesc& part : header.lods[i].parts)
                {
                    tris += part.triangleCount;
                    verts += part.vertexCount;
                }
                std::cout << "  lod" << i << " thr " << std::setprecision(4) << header.lods[i].threshold << "  parts "
                          << header.lods[i].partCount << "  tris " << tris << "  verts " << verts
                          << (i == FinestLodIndex(header) ? "   <- taken" : "") << "\n";
            }
            for (const XobMaterial& material : header.materials)
                std::cout << "  material " << material.path << "\n";
            // Fact 9: the COLL shapes, one line each, with the character-blocking
            // verdict the geometry LOD will act on.
            const XobCollision collision = ReadXobCollision(bytes.data(), bytes.size(), header);
            if (!collision.present)
                std::cout << "  collision: none (no COLL chunk)\n";
            else
            {
                std::cout << "  collision: " << collision.shapes.size() << " shapes"
                          << (collision.closes ? "" : "  NOT CLOSED: " + collision.error) << "\n";
                for (const XobCollisionShape& shape : collision.shapes)
                {
                    std::cout << "    " << ToString(shape.kind) << " '" << shape.name << "' layer '" << shape.layer
                              << "' " << (XobLayerBlocksCharacters(shape.layer) ? "BLOCKS" : "passable");
                    if (shape.kind == XobCollisionKind::Box)
                        std::cout << "  half " << std::setprecision(3) << shape.halfExtents.x << " x "
                                  << shape.halfExtents.y << " x " << shape.halfExtents.z;
                    else if (shape.kind == XobCollisionKind::Sphere)
                        std::cout << "  r " << std::setprecision(3) << shape.radius;
                    else if (shape.kind == XobCollisionKind::Capsule || shape.kind == XobCollisionKind::Cylinder)
                        std::cout << "  r " << std::setprecision(3) << shape.radius << " halfH " << shape.halfHeight;
                    else
                        std::cout << "  verts " << shape.vertices.size() << " polys " << shape.polygons.size();
                    std::cout << "  at " << std::setprecision(2) << shape.position.x << "," << shape.position.y << ","
                              << shape.position.z << "\n";
                }
            }
            shown++;
        }
    }
    if (shown == 0)
        std::cerr << "No .xob matched '" << filter << "'.\n";
    return shown == 0 ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Subcommand: convert
// ---------------------------------------------------------------------------

int Convert(const std::string& root, const std::string& filter, const std::string& outputRoot,
            const std::string& prefix, int limit, const ConvertOptions& options)
{
    PakSet paks;
    if (!paks.Open(root))
    {
        std::cerr << "Error: " << paks.Error() << "\n";
        return 1;
    }
    const std::string needle = LowerCopy(filter);

    std::vector<std::string> targets;
    for (const PakArchive& archive : paks.Archives())
    {
        if (!archive.IsOpen())
            continue;
        for (const PakEntry& entry : archive.Entries())
        {
            const std::string lower = LowerCopy(entry.path);
            if (EndsWith(lower, ".xob") && (needle.empty() || lower.find(needle) != std::string::npos))
                targets.push_back(entry.path);
        }
    }
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    if (static_cast<int>(targets.size()) > limit)
        targets.resize(static_cast<size_t>(limit));

    TextureBaker baker(paks, outputRoot, options.textures);
    size_t taken = 0;
    size_t partsTextured = 0, partsTotal = 0;
    std::vector<ModelResult> results;
    results.reserve(targets.size());
    for (const std::string& path : targets)
    {
        const ModelResult result =
            ConvertOne(paks, path, outputRoot, prefix, options, options.textures.enabled ? &baker : nullptr);
        PrintModelLine(result);
        partsTextured += result.stats.partsTextured;
        partsTotal += result.stats.parts;
        if (result.written && result.roundTrip.ok)
            taken++;
        results.push_back(result);
    }
    std::cout << "\n" << taken << " / " << targets.size() << " models converted and round-tripped.\n";
    if (options.textures.enabled)
        PrintTextureReport(baker.Stats(), partsTextured, partsTotal);
    if (options.geometry)
        PrintGeometryReport(results);
    return taken == targets.size() ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Subcommand: export
// ---------------------------------------------------------------------------

//! How near zero a stored Y has to be before a flagless placement is read as an
//! offset above the terrain rather than an absolute elevation.
//!
//! The guard is not decoration. `GenericEntity` is flagless 99.9% of the time and
//! its Y IS absolute -- 81.5% of its 25,860 Everon placements already sit within
//! 0.5 m of the terrain, median Y 35.98. Treating those as offsets would raise
//! every one of them by its own ground height. The flagless placements that are
//! genuinely relative have a median stored Y of exactly 0.00, so a 2 m window
//! separates the two populations without touching the middle of either.
constexpr float kTerrainRelativeYLimit = 2.0f;

//! True when the placement's `coords` Y is an offset above the terrain surface.
//!
//! Enfusion resolves these at load; an importer that takes the number literally
//! buries the object. On Everon the rule moves 293,397 of 1,228,065 placements
//! (23.9%) and takes the share sitting within 0.5 m of the terrain from 72.8% to
//! 92.6%. See EbinPlacement::hasFlags for the measurement behind the flag itself.
//!
//! It is a rule about the data, not a proof: 2.4% of the world is still more than
//! 2 m off terrain afterwards, and the ~62,000 flagless placements with |Y| >= 2
//! are left absolute because nothing measured says otherwise.
inline bool IsTerrainRelative(const EbinPlacement& placement)
{
    return !placement.hasFlags && std::fabs(placement.position[1]) < kTerrainRelativeYLimit;
}

struct ExportOptions
{
    std::string root;
    std::string world = "worlds/Eden/Eden.ent";
    std::string outputRoot;
    std::string manifest;
    std::string prefix = "reforger\\everon\\models";
    std::string resmapCache;
    size_t maxModels = 50;
    size_t maxPlacements = 20000;
    ConvertOptions convert;
};

int Export(const ExportOptions& options)
{
    PakSet paks;
    if (!paks.Open(options.root))
    {
        std::cerr << "Error: " << paks.Error() << "\n";
        return 1;
    }
    std::cout << "Archives:   " << paks.ArchiveCount() << " .pak, " << paks.EntryCount() << " entries\n";

    // --- the world -------------------------------------------------------
    std::vector<uint8_t> worldBytes;
    std::string worldPath = options.world;
    if (!paks.Read(worldPath, worldBytes))
    {
        // Fall back to a substring search so `-w Eden` works as well as a full path.
        const std::string needle = LowerCopy(options.world);
        worldPath.clear();
        for (const PakArchive& archive : paks.Archives())
        {
            if (!archive.IsOpen())
                continue;
            for (const PakEntry& entry : archive.Entries())
            {
                const std::string lower = LowerCopy(entry.path);
                if (EndsWith(lower, ".ent") && lower.find(needle) != std::string::npos)
                {
                    if (worldPath.empty() || entry.path.size() < worldPath.size())
                        worldPath = entry.path;
                }
            }
        }
        if (worldPath.empty() || !paks.Read(worldPath, worldBytes))
        {
            std::cerr << "Error: no .ent world matching '" << options.world << "'\n";
            return 1;
        }
    }
    const EbinWorld world = ReadEbin(worldBytes.data(), worldBytes.size());
    if (!world.valid())
    {
        std::cerr << "Error: " << worldPath << " does not close: " << world.error << "\n";
        return 1;
    }
    std::cout << "World:      " << worldPath << "  " << world.placements.size() << " placements, " << world.consumed
              << " / " << worldBytes.size() << " bytes accounted for\n";

    // --- GUID -> resource path -------------------------------------------
    ResourceMap resources;
    bool fromCache = false;
    if (!options.resmapCache.empty() && LoadResourceMapCache(options.resmapCache, resources))
    {
        fromCache = true;
    }
    else
    {
        std::cout << "Scanning text resources for {GUID}path references...\n";
        resources = BuildResourceMap(paks);
        // The addon's own index, which is complete where the scan is opportunistic.
        LoadResourceDatabases(options.root, resources);
        if (!options.resmapCache.empty())
            SaveResourceMapCache(options.resmapCache, resources);
    }
    std::cout << "Resources:  " << resources.byReference.size() << " GUIDs from references + "
              << resources.byOwnId.size() << " from .et ID lines + " << resources.byDatabase.size()
              << " from resourceDatabase.rdb = " << resources.Size() << (fromCache ? "  (cache)" : "") << "\n";
    // Silence here would look exactly like success: the scanned tiers alone still
    // resolve 99% of placements, and the 1% they miss is whole buildings.
    if (!fromCache && resources.databasesRead == 0)
        std::cout << "            NO resourceDatabase.rdb found under " << options.root
                  << " -- prefabs that no text file happens to name will not resolve\n";
    if (resources.databasesRefused != 0)
        std::cout << "            " << resources.databasesRefused
                  << " database(s) REFUSED for not closing; their GUIDs are absent\n";

    // --- which prefabs matter --------------------------------------------
    std::unordered_map<std::string, size_t> byPrefab;
    size_t withoutPrefab = 0, withoutPosition = 0;
    for (const EbinPlacement& placement : world.placements)
    {
        if (!placement.hasPosition)
        {
            withoutPosition++;
            continue;
        }
        if (placement.prefabGuid.empty())
        {
            withoutPrefab++;
            continue;
        }
        byPrefab[placement.prefabGuid]++;
    }
    std::vector<std::pair<std::string, size_t>> ranked(byPrefab.begin(), byPrefab.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b)
              { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    std::cout << "Prefabs:    " << ranked.size() << " distinct GUIDs; " << withoutPosition
              << " placements have no transform, " << withoutPrefab << " no prefab\n\n";

    // --- resolve and convert the top N -----------------------------------
    std::cout << "Converting the " << options.maxModels << " most-placed prefabs that resolve to a model:\n";
    std::unordered_map<std::string, std::string> modelOfPrefab;  //!< prefab GUID -> written virtual path
    std::unordered_map<std::string, std::string> convertedByXob; //!< .xob path -> virtual path
    std::map<std::string, size_t> failures;                      //!< reason -> count
    std::vector<ModelResult> results;
    size_t attempted = 0, roundTripped = 0;
    size_t coveredPlacements = 0;
    TextureBaker baker(paks, options.outputRoot, options.convert.textures);

    for (const auto& [guid, count] : ranked)
    {
        if (convertedByXob.size() >= options.maxModels)
            break;
        const std::string* etPath = resources.Find(guid);
        if (!etPath)
        {
            failures["prefab GUID in no resource map"]++;
            continue;
        }
        const PrefabResolution resolved = ResolvePrefabModel(paks, *etPath);
        if (resolved.xobPath.empty())
        {
            failures[resolved.reason.substr(0, resolved.reason.find(':'))]++;
            continue;
        }
        const auto already = convertedByXob.find(LowerCopy(resolved.xobPath));
        if (already != convertedByXob.end())
        {
            modelOfPrefab[guid] = already->second;
            coveredPlacements += count;
            continue;
        }
        if (!paks.Has(resolved.xobPath))
        {
            failures[".xob not in any .pak"]++;
            continue;
        }

        attempted++;
        ModelResult result = ConvertOne(paks, resolved.xobPath, options.outputRoot, options.prefix, options.convert,
                                        options.convert.textures.enabled ? &baker : nullptr);
        std::cout << "  " << std::right << std::setw(6) << count << "x ";
        PrintModelLine(result);
        if (!result.written || !result.roundTrip.ok)
        {
            failures[result.error.empty() ? "round-trip mismatch" : "conversion failed"]++;
            continue;
        }
        roundTripped++;
        convertedByXob[LowerCopy(resolved.xobPath)] = result.virtualPath;
        modelOfPrefab[guid] = result.virtualPath;
        coveredPlacements += count;
        results.push_back(std::move(result));
    }

    std::cout << "\n"
              << roundTripped << " / " << attempted
              << " attempted models converted AND round-tripped through MLODLoader.\n";
    if (!failures.empty())
    {
        std::cout << "Prefabs skipped while filling the quota:\n";
        for (const auto& [reason, count] : failures)
            std::cout << "  " << std::left << std::setw(44) << reason << count << "\n";
    }

    // --- bounding boxes, in metres ---------------------------------------
    //
    // Reported for every model because a unit-scale error is the failure a parse
    // check cannot see. Reforger `.xob` positions are plain float32 metres
    // (XobModel.hpp: "Positions are plain float32 and are NOT quantised").
    std::cout << "\nBounding boxes (metres, Y up):\n";
    for (const ModelResult& result : results)
    {
        std::cout << "  " << std::left << std::setw(94) << result.virtualPath << std::right << std::fixed
                  << std::setprecision(2) << std::setw(8) << result.stats.Extent(0) << " x " << std::setw(8)
                  << result.stats.Extent(1) << " x " << std::setw(8) << result.stats.Extent(2) << "   Y "
                  << std::setw(8) << result.stats.bboxMin[1] << " .. " << std::setw(8) << result.stats.bboxMax[1]
                  << "\n";
    }

    size_t crossAlong = 0, trianglesTotal = 0;
    for (const ModelResult& result : results)
    {
        crossAlong += result.stats.crossAlongNormal;
        trianglesTotal += result.stats.triangles;
    }
    if (options.convert.textures.enabled)
    {
        size_t partsTextured = 0, partsTotal = 0;
        for (const ModelResult& result : results)
        {
            partsTextured += result.stats.partsTextured;
            partsTotal += result.stats.parts;
        }
        PrintTextureReport(baker.Stats(), partsTextured, partsTotal);
    }

    if (trianglesTotal)
    {
        std::cout << "\nWinding: " << crossAlong << " / " << trianglesTotal
                  << " written triangles have their edge cross product along the stored normal ("
                  << std::setprecision(4) << (static_cast<double>(crossAlong) / static_cast<double>(trianglesTotal))
                  << ").\n";
    }

    // The absolute companion to the relative line above. A corpus that reads mostly
    // OUTWARD is inverted for this engine (fact 6); mostly INWARD is what native
    // content gives. The two lines together cannot both be satisfied by an inversion
    // of both halves, which is the failure the winding line alone could not see.
    size_t outward = 0, inward = 0;
    for (const ModelResult& result : results)
    {
        outward += result.stats.normalOutward;
        inward += result.stats.normalInward;
    }
    if (outward + inward)
    {
        std::cout << "Orientation: LOD0 faces whose written normal points AWAY from the model centroid: " << outward
                  << " outward / " << inward << " inward (" << std::setprecision(4)
                  << (static_cast<double>(outward) / static_cast<double>(outward + inward))
                  << " outward). This engine wants INWARD -- see fact 6 and --fix-orientation.\n";
    }
    if (options.convert.geometry)
        PrintGeometryReport(results);

    // --- the manifest -----------------------------------------------------
    std::error_code ec;
    const auto manifestParent = std::filesystem::path(options.manifest).parent_path();
    if (!manifestParent.empty())
        std::filesystem::create_directories(manifestParent, ec);
    std::ofstream manifest(options.manifest, std::ios::trunc);
    if (!manifest)
    {
        std::cerr << "Error: cannot write " << options.manifest << "\n";
        return 1;
    }

    // Counted first, then sampled at a stride. Enfusion groups a world's records by
    // prefab ($grp), so file order is prefab order and not a walk of the island:
    // taking the first 20,000 eligible placements yields 11,256 of one boulder and
    // 8,696 of one raspberry bush and nothing else. A stride spreads the subset over
    // every converted model and over the whole map.
    size_t eligible = 0;
    for (const EbinPlacement& placement : world.placements)
        if (placement.hasPosition && modelOfPrefab.count(placement.prefabGuid) != 0)
            eligible++;
    const size_t stride = (options.maxPlacements == 0 || eligible <= options.maxPlacements)
                              ? 1
                              : (eligible + options.maxPlacements - 1) / options.maxPlacements;

    size_t emitted = 0, unresolved = 0, noTransform = 0, seen = 0, terrainRelative = 0;
    std::map<std::string, size_t> perModel;
    for (const EbinPlacement& placement : world.placements)
    {
        if (!placement.hasPosition)
        {
            noTransform++;
            continue;
        }
        const auto model = modelOfPrefab.find(placement.prefabGuid);
        if (model == modelOfPrefab.end())
        {
            unresolved++;
            continue;
        }
        if (seen++ % stride != 0)
            continue;
        // position[1] is up and angles[1] is yaw, both stated by EbinWorld.hpp and
        // both left in source order here: renaming an axis in the manifest is how a
        // consumer ends up guessing which convention it is in.
        //
        // The seventh field says whether that Y is absolute (0) or an offset above
        // the terrain (1). It is written as a column rather than resolved here
        // because this command reads models and has no heightfield; `pak export`
        // does, and resolves it. A six-field manifest still reads, as absolute.
        const bool relative = IsTerrainRelative(placement);
        char line[512];
        std::snprintf(line, sizeof(line), "%s\t%.4f\t%.4f\t%.4f\t%.3f\t%.5f\t%d", model->second.c_str(),
                      placement.position[0], placement.position[1], placement.position[2], placement.angles[1],
                      placement.scale, relative ? 1 : 0);
        manifest << line << '\n';
        perModel[model->second]++;
        emitted++;
        if (relative)
            terrainRelative++;
    }
    manifest.close();

    std::cout << "\nManifest:   " << options.manifest << "\n";
    std::cout << "            " << coveredPlacements << " of "
              << (world.placements.size() - withoutPosition - withoutPrefab) << " world placements are of one of the "
              << convertedByXob.size() << " converted models (" << std::setprecision(1)
              << 100.0 * static_cast<double>(coveredPlacements) /
                     static_cast<double>(world.placements.size() - withoutPosition - withoutPrefab)
              << "%).\n";
    std::cout << "            " << modelOfPrefab.size() << " prefab GUIDs map onto those " << convertedByXob.size()
              << " models -- several prefabs share one mesh and differ only in their material overrides.\n";
    std::cout << "            " << emitted << " written, sampled 1 in " << stride << " of " << eligible
              << " eligible (cap " << options.maxPlacements << ").\n";
    std::cout << "            " << unresolved << " skipped: prefab resolves to no converted model; " << noTransform
              << " skipped: no transform.\n";
    std::cout << "            " << terrainRelative << " of " << emitted << " carry no Flags and |Y| < "
              << std::setprecision(1) << kTerrainRelativeYLimit
              << " m, so their Y is an offset above the terrain that `pak export` resolves.\n";
    if (emitted != 0 && terrainRelative == 0)
        std::cout << "            (none of them -- every placement is absolute. On Everon this is 23.9%, so a zero"
                     " here on a Reforger world means the Flags property is not reaching the reader.)\n";
    std::cout << "Per model:\n";
    for (const auto& [path, count] : perModel)
        std::cout << "  " << std::left << std::setw(94) << path << " " << count << "\n";

    return roundTripped == attempted && emitted > 0 ? 0 : 1;
}

} // namespace

void XobCommand::Setup(CLI::App& app)
{
    auto* xob = app.add_subcommand("xob", "Arma Reforger .xob models -> MLOD .p3d, and world placement manifests");
    xob->require_subcommand(1);
    {
        static std::string root;
        static std::string filter;
        static int limit = 20;
        auto* cmd = xob->add_subcommand("inspect", "Report a .xob's LOD ladder, bounding box and materials");
        cmd->add_option("input", root, "A .pak file, or a directory to search")->required();
        cmd->add_option("-f,--filter", filter, "Only paths containing this substring (case-insensitive)");
        cmd->add_option("-n,--limit", limit, "Stop after this many models (default 20)");
        cmd->callback([]() { std::exit(Inspect(root, filter, limit)); });
    }
    {
        static std::string root;
        static std::string filter;
        static std::string output;
        static std::string prefix = "reforger\\everon\\models";
        static int limit = 50;
        static ConvertOptions options;
        auto* cmd = xob->add_subcommand("convert", "Convert matching .xob files to MLOD .p3d and round-trip each one");
        cmd->add_option("input", root, "A .pak file, or a directory to search")->required();
        cmd->add_option("-f,--filter", filter, "Only paths containing this substring (case-insensitive)");
        cmd->add_option("-o,--output", output, "Output root; files land at <root>/<virtual path>")->required();
        cmd->add_option("--virtual-prefix", prefix, "Virtual path prefix (default reforger\\everon\\models)");
        cmd->add_option("-n,--limit", limit, "Stop after this many models (default 50)");
        cmd->add_option("--resolution", options.resolution,
                        "Visual LOD resolution step (default 1.0 writes 1/2/3/...)");
        cmd->add_flag("--second-uv", options.secondUv, "Emit both #UVSet# blocks when a part declares two UV sets");
        cmd->add_flag("--flip-winding", options.flipWinding, "Reverse triangle winding (see the winding note)");
        cmd->add_flag("--fix-orientation", options.fixOrientation,
                      "Fact 6: negate normals AND reverse winding together (env XOB_FIX_ORIENTATION=1)");
        cmd->add_flag("--fix-origin", options.fixOrigin,
                      "Fact 7: write autocenter=0 so the origin is not moved to the bbox centre "
                      "(env XOB_FIX_ORIGIN=1)");
        cmd->add_flag("--textures", options.textures.enabled,
                      "Bake each material's BCRMap (+OpacityMap) to .paa and name it in the face texture slot");
        cmd->add_option("--texture-prefix", options.textures.prefix,
                        "Virtual path prefix for baked textures (default reforger\\everon\\textures)");
        cmd->add_option("--max-texture", options.textures.maxSize, "Longest baked texture edge (default 1024)");
        cmd->add_flag("--normals", options.textures.normals,
                      "Also bake NMOMap to a DXT5nm .paa; the deployed .emat (default) or --material-rvmat names it.");
        cmd->add_flag("--material-rvmat", options.textures.rvmats,
                      "Write a Super .rvmat per material naming the baked normal, and name IT in the material slot");
        cmd->add_flag("--material-layers,!--no-material-layers", options.textures.layers,
                      "Deploy MatPBRMulti's mask and BCR_2..4 / NMO_2..4 layer tiles so the runtime blends "
                      "them per fragment (default ON; --no-material-layers collapses to one tile, the A/B)");
        cmd->add_flag("--material-emat,!--no-material-emat", options.textures.emats,
                      "Deploy each material as a resolved .emat under the texture prefix and name IT in the "
                      "material slot (default ON; --no-material-emat is the A/B)");
        cmd->add_flag("--geometry,!--no-geometry", options.geometry,
                      "Write a Geometry LOD from the .xob COLL shapes (ComponentXX + #Mass#; fact 9, default on)");
        cmd->add_flag("--geometry-all-layers", options.geometryAllLayers,
                      "Geometry LOD from EVERY COLL shape, fire/view/foliage layers included (inspection only)");
        cmd->add_flag("--map-property,!--no-map-property", options.mapProperty,
                      "Fact 8: write a `map` named property (rock/tree/bush/house/wall/hide) from the source path "
                      "(default ON; --no-map-property is the A/B)");
        cmd->callback(
            []()
            {
                options.fixOrientation = options.fixOrientation || OrientationFixFromEnv();
                options.fixOrigin = options.fixOrigin || OriginFixFromEnv();
                std::exit(Convert(root, filter, output, prefix, limit, options));
            });
    }
    {
        static ExportOptions options;
        auto* cmd =
            xob->add_subcommand("export", "Convert the most-placed prefabs of a world and write a placement manifest");
        cmd->add_option("input", options.root, "A .pak file, or a directory to search")->required();
        cmd->add_option("-w,--world", options.world, "World .ent path or substring (default worlds/Eden/Eden.ent)");
        cmd->add_option("-o,--output", options.outputRoot, "Output root for the .p3d tree")->required();
        cmd->add_option("-m,--manifest", options.manifest, "Placement manifest path")->required();
        cmd->add_option("--virtual-prefix", options.prefix, "Virtual path prefix (default reforger\\everon\\models)");
        cmd->add_option("--max-models", options.maxModels, "How many distinct models to convert (default 50)");
        cmd->add_option("--max-placements", options.maxPlacements, "Cap on manifest lines (default 20000)");
        cmd->add_option("--resmap-cache", options.resmapCache,
                        "Read/write the GUID->path map here so a rerun skips the corpus scan");
        cmd->add_option("--resolution", options.convert.resolution,
                        "Visual LOD resolution step (default 1.0 writes 1/2/3/...)");
        cmd->add_flag("--second-uv", options.convert.secondUv, "Emit both #UVSet# blocks");
        cmd->add_flag("--flip-winding", options.convert.flipWinding, "Reverse triangle winding");
        cmd->add_flag("--fix-orientation", options.convert.fixOrientation,
                      "Fact 6: negate normals AND reverse winding together (env XOB_FIX_ORIENTATION=1)");
        cmd->add_flag("--fix-origin", options.convert.fixOrigin,
                      "Fact 7: write autocenter=0 so the origin is not moved to the bbox centre "
                      "(env XOB_FIX_ORIGIN=1)");
        cmd->add_flag("--textures", options.convert.textures.enabled,
                      "Bake each material's BCRMap (+OpacityMap) to .paa and name it in the face texture slot");
        cmd->add_option("--texture-prefix", options.convert.textures.prefix,
                        "Virtual path prefix for baked textures (default reforger\\everon\\textures)");
        cmd->add_option("--max-texture", options.convert.textures.maxSize, "Longest baked texture edge (default 1024)");
        cmd->add_flag("--normals", options.convert.textures.normals,
                      "Also bake NMOMap to a DXT5nm .paa; the deployed .emat (default) or --material-rvmat names it.");
        cmd->add_flag("--material-rvmat", options.convert.textures.rvmats,
                      "Write a Super .rvmat per material naming the baked normal, and name IT in the material slot");
        cmd->add_flag("--material-layers,!--no-material-layers", options.convert.textures.layers,
                      "Deploy MatPBRMulti's mask and BCR_2..4 / NMO_2..4 layer tiles so the runtime blends "
                      "them per fragment (default ON; --no-material-layers collapses to one tile, the A/B)");
        cmd->add_flag("--material-emat,!--no-material-emat", options.convert.textures.emats,
                      "Deploy each material as a resolved .emat under the texture prefix and name IT in the "
                      "material slot (default ON; --no-material-emat is the A/B)");
        cmd->add_flag("--geometry,!--no-geometry", options.convert.geometry,
                      "Write a Geometry LOD from the .xob COLL shapes (ComponentXX + #Mass#; fact 9, default on)");
        cmd->add_flag("--geometry-all-layers", options.convert.geometryAllLayers,
                      "Geometry LOD from EVERY COLL shape, fire/view/foliage layers included (inspection only)");
        cmd->add_flag("--map-property,!--no-map-property", options.convert.mapProperty,
                      "Fact 8: write a `map` named property (rock/tree/bush/house/wall/hide) from the source path "
                      "(default ON; --no-map-property is the A/B)");
        cmd->callback(
            []()
            {
                options.convert.fixOrientation = options.convert.fixOrientation || OrientationFixFromEnv();
                options.convert.fixOrigin = options.convert.fixOrigin || OriginFixFromEnv();
                std::exit(Export(options));
            });
    }
}

} // namespace PoseidonTools
