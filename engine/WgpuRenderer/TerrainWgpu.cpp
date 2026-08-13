#include "TerrainWgpu.hpp"

#include "CdlodDriver.hpp"
#include "EngineWgpu.hpp"
#include "TextureWgpu.hpp"

#include <Poseidon/Core/Global.hpp> // Glob.time (coast wet-band animation clock)
#include <Poseidon/Core/TaskPool.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <stb_image.h>
#include <stb_image_resize2.h>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace Poseidon
{
// Grid-mesh resolution per axis; must match GRID_N in terrain/mod.rs.
constexpr int TerrainGridN = 32;

// ---------------------------------------------------------------------------
// The geography word uploaded to the grass system (one R32Uint texel per land
// cell). Mirrored by GEO_* in grass/grass.wgsl -- change both together.
//
//   bits  0-14  GeographyInfo::packed, verbatim from Landscape (AITypes.hpp):
//               0-1 waterDepth, 2 full, 3 forestInner, 4 forestOuter, 5 road,
//               6 track, 7 slow, 8-9 howManyObjects, 10-11 howManyHardObjects,
//               12-14 gradient.
//   bit  15     GrassAuthoredCell -- this cell's clutter answer came from the
//               map's OWN per-texel LCA surface mask, so the two fields below
//               are meaningful. Clear on every OFP world and on any cell whose
//               material carries no authored mask; there the legacy per-layer
//               texture classification set bit 31 and these fields are 0.
//   bits 16-22  GrassCoverage, 0..127. The cell's authored clutter density,
//               scaled so the densest clutter character on this world is 127.
//               This is what makes Takistan's grass (character total 0.94) and
//               its desert (0.12) different fields rather than the same lawn.
//   bits 23-27  GrassSurfaceId, 0..30, with 31 = none/unclassified. A dense id
//               for the surface that dominates the cell. NOTHING consumes it
//               yet: it is what per-surface clutter VARIETY selects on, and it
//               costs nothing to carry now that the mask is being decoded.
//   bits 28-30  reserved, always 0.
//   bit  31     GrassTextureCell -- this cell can grow grass at all.
// ---------------------------------------------------------------------------
constexpr uint32_t GrassTextureCell = 0x80000000u;
constexpr uint32_t GrassAuthoredCell = 0x00008000u;
constexpr uint32_t GrassCoverageShift = 16u;
constexpr uint32_t GrassCoverageMask = 0x007f0000u;
constexpr uint32_t GrassSurfaceShift = 23u;
constexpr uint32_t GrassSurfaceMask = 0x0f800000u;
constexpr uint32_t GrassSurfaceNone = 31u;
// Every bit this file writes on top of GeographyInfo, so a re-bake starts clean.
constexpr uint32_t GrassBakedMask = GrassTextureCell | GrassAuthoredCell | GrassCoverageMask | GrassSurfaceMask;

static bool ContainsNoCase(const char* text, const char* needle)
{
    if (text == nullptr || needle == nullptr || *needle == '\0')
    {
        return false;
    }
    const size_t needleLen = std::strlen(needle);
    for (const char* start = text; *start; ++start)
    {
        size_t i = 0;
        for (; i < needleLen && start[i]; ++i)
        {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(start[i])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[i])));
            if (a != b)
            {
                break;
            }
        }
        if (i == needleLen)
        {
            return true;
        }
    }
    return false;
}

// CWR/OFP terrain assets are mostly named in English or Czech.  Keep this
// strict: an unclassified texture must not grow procedural grass over desert,
// dirt, concrete, or rock.
//
// This is the LAST resort. Where the map's own CfgSurfaces is readable it is the
// authority (TerrainSurfaceCatalog); this only answers for data that names no
// surface class at all, and for Arma archives that ship no config we can parse.
static bool IsGrassSurfaceName(const char* name)
{
    return ContainsNoCase(name, "grass") || ContainsNoCase(name, "trava") || ContainsNoCase(name, "louka") ||
           ContainsNoCase(name, "meadow") || ContainsNoCase(name, "pasture");
}

static bool IsGrassTexture(const Texture* texture)
{
    if (texture == nullptr)
    {
        return false;
    }
    return IsGrassSurfaceName(texture->Name());
}

static bool EqualsNoCase(const char* a, const char* b)
{
    if (a == nullptr || b == nullptr)
    {
        return false;
    }
    for (; *a != '\0' && *b != '\0'; ++a, ++b)
    {
        if (tolower(static_cast<unsigned char>(*a)) != tolower(static_cast<unsigned char>(*b)))
        {
            return false;
        }
    }
    return *a == *b;
}

// Does this terrain texture resolve to a surface the GAME ITSELF calls grass?
//
// Operation Flashpoint's own CfgSurfaces answers this exactly, and names three
// grass classes and no others: Grass (`tn??????`), GrassAbel (`tt??????`) and
// GrassHigh (`tv??????`) -- measured over the retail CONFIG.BIN's 23 surface
// classes. Everon's terrain is pre-blended four-quadrant tiles of two-character
// codes, so `GetTextureQuadrants` gives a per-quadrant answer; a tile counts as
// grass-capable if any quadrant is one of the three.
//
// This exists because the name heuristic above matches NOTHING on Everon
// (`eden\tn`, `eden\pl`), which used to trip an "enable every layer" fallback and
// made sand, rock, forest floor and shore all grass-capable. `outResolved` reports
// whether the surface catalogue said anything at all about this texture, so that
// fallback can be kept for genuinely unclassifiable data instead of for data we
// simply were not reading.
static bool IsGrassSurfaceClass(const SurfaceInfo* const* quadrants, bool& outResolved)
{
    outResolved = false;
    if (quadrants == nullptr)
    {
        return false;
    }
    bool grass = false;
    for (int q = 0; q < 4; ++q)
    {
        const SurfaceInfo* surface = quadrants[q];
        if (surface == nullptr || surface->_class.GetLength() == 0)
        {
            continue;
        }
        outResolved = true;
        const char* cls = surface->_class.Data();
        if (EqualsNoCase(cls, "Grass") || EqualsNoCase(cls, "GrassAbel") || EqualsNoCase(cls, "GrassHigh"))
        {
            grass = true;
        }
    }
    return grass;
}

static float EnvFloat(const char* name, float fallback)
{
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0')
    {
        return fallback;
    }
    return std::strtof(v, nullptr);
}

TerrainWgpu::TerrainWgpu(EngineWgpu& engine, WgrRenderer* renderer) : _engine(engine), _renderer(renderer)
{
    _baseMult = EnvFloat("WGR_TERRAIN_LOD_BASE", 4.0f);
    _lodRatio = EnvFloat("WGR_TERRAIN_LOD_RATIO", 2.0f);
    _morphRegion = std::clamp(EnvFloat("WGR_TERRAIN_MORPH", 0.50f), 0.05f, 1.0f);
    // >1 extends terrain past the map edges (clamped edge heights) to complement the
    // infinite ocean; 1 = map only. Defaults to match the water extent (WGR_WATER_EXTENT)
    // so seabed underlies the whole transparent ocean with no seam at the old map edge.
    _extentFactor = std::max(1.0f, EnvFloat("WGR_TERRAIN_EXTENT", 3.0f));
}

void TerrainWgpu::BuildQuadtree(const Landscape& land)
{
    const int range = land.GetTerrainRange();
    const float grid = land.GetTerrainGrid();

    // Optionally over-size + centre the root so land continues past the map edges. At
    // extent 1 this is exactly the map-only root at origin 0 (byte-identical selection).
    _extended = _extentFactor > 1.0f;
    const int coverage = _extended ? std::max(range, static_cast<int>(std::lround(_extentFactor * range))) : range;
    const int rootTexels = CdlodRootTexels(coverage, TerrainGridN);
    const int originTexel = _extended ? CdlodCenteredOrigin(rootTexels, range, TerrainGridN) : 0;

    // Each leaf's world-height extent is scanned from the heightmap (same per-texel
    // min/max the CDLOD selection frustum-tests against). Off-map texels clamp to the
    // nearest edge height — the vertex shader samples the heightmap the same clamped
    // way, so the extended border is a flat continuation of the boundary terrain.
    auto leafBounds = [&](int ox, int oz, int span, float& mn, float& mx)
    {
        for (int z = oz; z <= oz + span; z++)
        {
            for (int x = ox; x <= ox + span; x++)
            {
                const float h = land.GetHeight(std::clamp(z, 0, range - 1), std::clamp(x, 0, range - 1));
                mn = std::min(mn, h);
                mx = std::max(mx, h);
            }
        }
    };
    BuildCdlodTree(rootTexels, originTexel, originTexel, grid, TerrainGridN, leafBounds, _tree, _rootIndex, _numLevels,
                   _leafSize);
    if (_rootIndex < 0)
    {
        return;
    }
    ComputeCdlodRanges(_leafSize * _baseMult, _lodRatio, _numLevels, _ranges);
    _treeMin = originTexel * grid;
    _treeMax = (originTexel + rootTexels) * grid;
}

bool TerrainWgpu::UploadIfNeeded(const Landscape& land)
{
    const int range = land.GetTerrainRange();
    const char* name = land.GetName();
    if (_uploaded == &land && _uploadedRange == range && _uploadedName == name)
    {
        return false;
    }

    std::vector<float> heights(static_cast<size_t>(range) * range);
    for (int z = 0; z < range; z++)
    {
        for (int x = 0; x < range; x++)
        {
            heights[static_cast<size_t>(z) * range + x] = land.GetHeight(z, x);
        }
    }

    // Fill the static params (the coast wet-band fields are refreshed per frame in DrawTerrain).
    _params.world_origin = {0.0f, 0.0f};
    _params.land_grid = land.GetLandGrid();
    _params.terrain_grid = land.GetTerrainGrid();
    _params.hm_width = static_cast<uint32_t>(range);
    _params.hm_height = static_cast<uint32_t>(range);
    _params.land_range = static_cast<uint32_t>(land.GetLandRange());
    _params.data_scale = 1.0f;
    wgr_terrain_set_heightmap(_renderer, heights.data(), &_params);

    UploadGroundTextures(land);
    UploadIndexMap(land);
    UploadGeography(land);
    UploadJitterMap(land);
    UploadDetailNoise();

    BuildQuadtree(land);

    _uploaded = &land;
    _uploadedRange = range;
    _uploadedName = name;
    // Confirmed from the installed CWA world data: Eden is Everon. Its raw
    // GeographyInfo flags are not reliable grass exclusions, unlike the other
    // stock worlds, so the shader must rely on exact height/slope plus the
    // explicit surface controls for this map.
    _grassNeedsCompatibilityOverride = ContainsNoCase(name, "eden.wrp");
    if (_grassNeedsCompatibilityOverride)
    {
        LOG_WARN(Graphics, "Wgpu grass: enabling Everon/Eden legacy geography compatibility override");
    }
    return true;
}

void TerrainWgpu::UploadGroundTextures(const Landscape& land)
{
    // A terrain material and a ground layer are different things, and conflating
    // them costs a world its terrain. Materials live in a storage buffer with no
    // cap; ground layers live in a binding_array of at most
    // WGR_TERRAIN_MAX_GROUND_LAYERS views. Takistan declares 3,293 materials over
    // a far smaller set of distinct images, so binding one layer per material
    // overran the array and left every material past the cap sampling the last
    // layer that fitted. Deduplicating by texture makes the layer array a
    // function of the artwork rather than of the material count.
    const int materialCount = std::max(1, land.GetNTextures());
    std::vector<uint64_t> handles;
    std::vector<Texture*> boundTextures;
    // Layer 0 is the renderer's white fallback. Reserving it means every material
    // whose cell texture is missing or would not upload costs no layer at all.
    handles.push_back(0);
    boundTextures.push_back(nullptr);
    size_t dropped = 0;
    const auto bindLayer = [&](Texture* raw) -> uint32_t
    {
        if (raw == nullptr)
        {
            return 0;
        }
        const auto found = std::find(boundTextures.begin(), boundTextures.end(), raw);
        if (found != boundTextures.end())
        {
            return static_cast<uint32_t>(found - boundTextures.begin());
        }
        if (handles.size() >= WGR_TERRAIN_MAX_GROUND_LAYERS)
        {
            ++dropped;
            return 0;
        }
        const uint64_t handle = static_cast<TextureWgpu*>(raw)->EnsureUploaded();
        if (handle == 0)
        {
            return 0;
        }
        boundTextures.push_back(raw);
        handles.push_back(handle);
        return static_cast<uint32_t>(handles.size() - 1);
    };

    std::vector<uint32_t> cellLayer(static_cast<size_t>(materialCount), 0);
    for (int i = 0; i < materialCount; i++)
    {
        cellLayer[static_cast<size_t>(i)] = bindLayer(land.GetTexture(i));
    }

    // Each index-map value is a terrain-material index, not merely a colour
    // texture.  Legacy worlds have one descriptor per colour texture; OPRW25
    // descriptors additionally select an LCA mask and base/overlay surfaces.
    std::vector<WgrTerrainMaterial> materials(static_cast<size_t>(materialCount));
    _a3TerrainTextures.clear();
    size_t authored = 0;
    size_t complete = 0;
    // `resolved` says whether the authored image actually reached a layer. It has
    // to be reported rather than inferred from the returned index: an authored
    // surface is often the very image the cell already uses -- Stratis's
    // gdt_strbeach_co is both -- so a correctly deduplicated binding shares the
    // cell's layer and is indistinguishable from a fallback by index alone.
    const auto bindTexture = [&](const RStringB& name, uint32_t fallback, bool& resolved) -> uint32_t
    {
        resolved = false;
        if (name.GetLength() == 0)
        {
            return fallback;
        }
        Ref<Texture> texture = GlobLoadTexture(name);
        Texture* raw = texture;
        if (raw == nullptr)
        {
            return fallback;
        }
        // Retain every Ref we resolve, whether or not it took a new layer.
        // Landscape owns the cell textures, but nothing else owns these, and
        // dedup compares raw pointers: releasing one would let a later load
        // reuse its address and match the wrong layer.
        _a3TerrainTextures.push_back(texture);
        const uint32_t layer = bindLayer(raw);
        if (layer == 0)
        {
            return fallback;
        }
        resolved = true;
        return layer;
    };
    for (int i = 0; i < materialCount; ++i)
    {
        WgrTerrainMaterial& material = materials[static_cast<size_t>(i)];
        material.legacy = cellLayer[static_cast<size_t>(i)];
        material.satellite = material.legacy;
        const A3TerrainMaterial* source = land.GetA3TerrainMaterial(i);
        if (source == nullptr || source->surfaceCount == 0 || source->mask.GetLength() == 0)
        {
            continue;
        }
        ++authored;
        // Preserve the legacy cell texture solely as a safe fallback. A
        // TerrainSNX Stage0 is its own low-frequency satellite image and has
        // its own TexGen; sampling the cell's close-detail texture through
        // Stage0's coordinates makes an authored material look corrupted.
        const uint32_t fallback = material.legacy;
        bool satelliteResolved = false;
        bool maskResolved = false;
        material.satellite = bindTexture(source->satellite, fallback, satelliteResolved);
        material.mask = bindTexture(source->mask, material.satellite, maskResolved);
        // Normals fall back to 0, not to a colour texture: the shader tests for 0
        // and keeps the geometric normal, whereas a colour map decoded as a normal
        // would tilt every surface by whatever its albedo happened to be.
        bool normalResolved = false;
        material.tile_normal = bindTexture(source->tileNormal, 0, normalResolved);
        const auto setUvSource = [&](uint32_t slot, const A3TerrainUv& uv)
        {
            if (uv.worldPos)
                material.uv_source_mask |= 1u << slot;
        };
        setUvSource(0, source->satelliteUv);
        setUvSource(1, source->maskUv);
        std::copy(source->satelliteUv.u.begin(), source->satelliteUv.u.end(), material.satellite_uv.u);
        std::copy(source->satelliteUv.v.begin(), source->satelliteUv.v.end(), material.satellite_uv.v);
        std::copy(source->maskUv.u.begin(), source->maskUv.u.end(), material.mask_uv.u);
        std::copy(source->maskUv.v.begin(), source->maskUv.v.end(), material.mask_uv.v);
        material.surface_count = source->surfaceCount;
        bool anySurface = false;
        for (uint32_t slot = 0; slot < material.surface_count; ++slot)
        {
            // A hole stays a hole. Falling back to another surface here would
            // give the mask's selector a texture where the source says there is
            // none, which is how an unpainted slot ends up covering ground.
            bool surfaceResolved = false;
            material.surfaces[slot] = bindTexture(source->surfaces[slot], 0, surfaceResolved);
            material.surface_normals[slot] = bindTexture(source->surfaceNormals[slot], 0, normalResolved);
            anySurface = anySurface || surfaceResolved;
            setUvSource(2 + slot, source->surfaceUvs[slot]);
            std::copy(source->surfaceUvs[slot].u.begin(), source->surfaceUvs[slot].u.end(),
                      material.surface_uvs[slot].u);
            std::copy(source->surfaceUvs[slot].v.begin(), source->surfaceUvs[slot].v.end(),
                      material.surface_uvs[slot].v);
        }
        // The mask and at least one surface are what make the composition
        // authored; a Stage0 satellite is optional and most families do not
        // carry one. This counts materials whose authored images all reached a
        // layer, not materials whose layer indices happen to differ.
        (void)satelliteResolved;
        if (maskResolved && anySurface)
        {
            ++complete;
        }
    }
    wgr_terrain_set_ground_layers(_renderer, handles.data(), static_cast<uint32_t>(handles.size()));
    wgr_terrain_set_materials(_renderer, materials.data(), static_cast<uint32_t>(materials.size()));
    LOG_INFO(Graphics,
             "Wgpu terrain materials: {} of {} authored LCA blends bound; {} complete, {} of {} ground layers used, "
             "{} texture bindings dropped at {}-layer limit",
             authored, materialCount, complete, handles.size(), WGR_TERRAIN_MAX_GROUND_LAYERS, dropped,
             WGR_TERRAIN_MAX_GROUND_LAYERS);
}

void TerrainWgpu::UploadIndexMap(const Landscape& land)
{
    const int n = land.GetLandRange();
    if (n <= 0)
    {
        return;
    }
    // A cell names a terrain material, not a ground layer. The material array is a
    // storage buffer sized to the world's material count; the ground-layer cap
    // applies to the layer each material points at, which UploadGroundTextures
    // keeps in range. Clamping here to the layer cap silently collapsed every
    // material past it onto one texture -- on Takistan, 2,269 of 3,293.
    // CELL_LAYER_MASK reserves bit 15, so the material index must still fit 15 bits.
    const int maxMaterial = std::min(std::max(1, land.GetNTextures()), 0x8000) - 1;
    std::vector<uint16_t> indices(static_cast<size_t>(n) * n);
    for (int z = 0; z < n; z++)
    {
        for (int x = 0; x < n; x++)
        {
            // Same (col=x, row=z) orientation as the heightmap upload;
            // GetTexture(z, x) == GetTex(x, z) is the land cell's layer index.
            // Bit 15 marks non-simple (transition) textures, which map exactly
            // once onto their cell instead of tiling — the GL33 path's
            // ClampU|ClampV (see Landscape::ClampFlags).
            const int layer = std::clamp(land.GetTexture(z, x), 0, maxMaterial);
            uint16_t entry = static_cast<uint16_t>(layer);
            if (!land.TextureIsSimple(layer))
            {
                entry |= 0x8000;
            }
            indices[static_cast<size_t>(z) * n + x] = entry;
        }
    }
    wgr_terrain_set_index_map(_renderer, static_cast<uint32_t>(n), static_cast<uint32_t>(n), indices.data());
}

static bool StartsWithNoCase(const char* text, const char* prefix)
{
    if (text == nullptr || prefix == nullptr || *prefix == '\0')
    {
        return false;
    }
    for (; *prefix != '\0'; ++text, ++prefix)
    {
        if (*text == '\0')
        {
            return false;
        }
        char a = static_cast<char>(std::tolower(static_cast<unsigned char>(*text)));
        char b = static_cast<char>(std::tolower(static_cast<unsigned char>(*prefix)));
        // Archive prefixes and texture paths disagree about the separator often
        // enough that treating them as different characters loses whole banks.
        a = (a == '/') ? '\\' : a;
        b = (b == '/') ? '\\' : b;
        if (a != b)
        {
            return false;
        }
    }
    return true;
}

// Read one file out of the virtual filesystem (PBO or loose). Empty on failure;
// every caller here treats "absent" as a fallback, never as an error.
static std::vector<uint8_t> ReadVfsFile(const char* path)
{
    std::vector<uint8_t> bytes;
    QIFStreamB file;
    file.AutoOpen(path);
    if (file.fail() || file.rest() <= 0)
    {
        return bytes;
    }
    bytes.resize(static_cast<size_t>(file.rest()));
    file.read(bytes.data(), static_cast<int>(bytes.size()));
    return bytes;
}

void TerrainWgpu::BuildSurfaceCatalog(const Landscape& land)
{
    _surfaceCatalog = TerrainSurfaceCatalog{};

    // Which archives can possibly define THIS world's surfaces? The ones that own
    // its terrain images, and every mounted archive they nest inside. Offering
    // every mounted archive instead would parse 487 configs on an Arma 3 install.
    //
    // Measured against the retail installs, this is exactly the right net, and it
    // is the IMAGE PATHS that carry it -- follow the references, do not scan:
    //
    //   Stratis   surfaces  a3\map_stratis\data\layers\p_*.rvmat ->
    //                       a3\map_data\gdt_strdrygrass_co.paa
    //             mask      a3\map_stratis\data\layers\m_000_000_lca.paa
    //             CfgSurfaces + CfgSurfaceCharacters are in a3\map_data\config.bin
    //             the `class clutter` bodies are in a3\map_stratis\config.bin,
    //             which nothing but the MASK path reaches -- no surface texture
    //             lives under a3\map_stratis\.
    //   Takistan  surfaces  ca\takistan\data\tk_trava_co.paa, and that one
    //             archive's config.bin holds all three classes.
    //
    // Bank prefixes are stored with a trailing separator and nest, so a single
    // reference collects every ancestor that is actually mounted: `a3\`,
    // `a3\map_stratis\`, `a3\map_stratis\data\`, `a3\map_stratis\data\layers\`.
    // That is at most a handful of configs -- about 150 KB of raP on Stratis --
    // rather than the whole addon set.
    std::vector<std::string> owners;
    const auto note = [&](const RStringB& reference)
    {
        if (reference.GetLength() == 0)
        {
            return;
        }
        for (int b = 0; b < GFileBanks.Size(); ++b)
        {
            const RString prefix = GFileBanks[b].GetPrefix();
            if (prefix.GetLength() == 0 || !StartsWithNoCase(reference.Data(), prefix.Data()))
            {
                continue;
            }
            std::string owner(prefix.Data());
            if (std::find(owners.begin(), owners.end(), owner) == owners.end())
            {
                owners.push_back(std::move(owner));
            }
        }
    };
    const int materialCount = std::max(0, land.GetNTextures());
    for (int i = 0; i < materialCount; ++i)
    {
        const A3TerrainMaterial* source = land.GetA3TerrainMaterial(i);
        if (source == nullptr)
        {
            continue;
        }
        for (const RStringB& surface : source->surfaces)
        {
            note(surface);
        }
        // The mask, satellite and tile normal are the world's OWN images, so
        // they are what reaches the world's own addon -- which on Arma 3 is the
        // only archive carrying the `class clutter` bodies. Noting only the
        // surfaces found CfgSurfaces and missed every clutter model.
        note(source->mask);
        note(source->satellite);
        note(source->tileNormal);
    }
    // And the world by name, for a world mounted under its own addon rather than
    // loaded from a loose path.
    note(RStringB(land.GetName() ? land.GetName() : ""));

    // Escape hatch: point at extra configs without a rebuild when a world turns
    // out to declare its surfaces somewhere this rule does not reach.
    if (const char* extra = std::getenv("WGR_GRASS_SURFACE_CONFIGS"))
    {
        std::string list(extra);
        size_t start = 0;
        while (start <= list.size())
        {
            const size_t end = list.find(';', start);
            std::string one = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!one.empty())
            {
                const std::vector<uint8_t> bytes = ReadVfsFile(one.c_str());
                if (!bytes.empty() && _surfaceCatalog.AddConfig(bytes))
                {
                    LOG_INFO(Graphics, "Wgpu grass surfaces: added config '{}' (WGR_GRASS_SURFACE_CONFIGS)", one);
                }
                else
                {
                    LOG_WARN(Graphics, "Wgpu grass surfaces: could not read config '{}'", one);
                }
            }
            if (end == std::string::npos)
            {
                break;
            }
            start = end + 1;
        }
    }

    // A world referencing an unbounded number of addons must not turn world load
    // into a config parse. Nothing observed comes close to this.
    constexpr size_t MaxSurfaceConfigs = 16;
    int offered = 0, accepted = 0;
    std::string parsedNames;
    for (const std::string& owner : owners)
    {
        if (static_cast<size_t>(offered) >= MaxSurfaceConfigs)
        {
            LOG_WARN(Graphics, "Wgpu grass surfaces: stopping at {} configs; {} archives were candidates",
                     MaxSurfaceConfigs, owners.size());
            break;
        }
        // `QFBank::GetPrefix()` already ends with the separator -- BankList::Load
        // mounts with `prefix + NATIVE_DIR_STR`, and `AutoBank` strips exactly
        // that length before looking the remainder up inside the archive. Adding
        // another one produced `a3\map_data\\config.bin`, whose remainder was
        // `\config.bin`, which is not an entry in any PBO. That is why every
        // Arma world reported "0 offered a config" and ran on the name heuristic.
        std::string path = owner;
        if (!path.empty() && path.back() != '\\' && path.back() != '/')
        {
            path += '\\';
        }
        path += "config.bin";
        const std::vector<uint8_t> bytes = ReadVfsFile(path.c_str());
        if (bytes.empty())
        {
            continue;
        }
        ++offered;
        if (_surfaceCatalog.AddConfig(bytes))
        {
            ++accepted;
            if (!parsedNames.empty())
            {
                parsedNames += ", ";
            }
            parsedNames += path;
        }
    }
    LOG_INFO(Graphics,
             "Wgpu grass surfaces: {} archive(s) own or contain this world's terrain images, {} offered a config, "
             "{} parsed -> {} surfaces, {} characters, {} clutter classes [{}]",
             owners.size(), offered, accepted, _surfaceCatalog.SurfaceCount(), _surfaceCatalog.CharacterCount(),
             _surfaceCatalog.ClutterCount(), parsedNames.empty() ? "none" : parsedNames);
}

namespace
{
// One authored surface slot's contribution, resolved once per material.
struct SlotClutter
{
    float coverage = 0.0f;    // authored, unnormalised (see Coverage())
    uint8_t surfaceId = 0xff; // dense id, 0xff = not clutter-bearing
    bool present = false;     // false for a hole: the slot has no surface
};

// The WLD-019 decode, on the CPU. The mask is an INDEXED selector, not a weight
// field: RGB is strictly one-hot and alpha takes three authored levels. The three
// levels are read as a partition of unity so a filtered texel between two of them
// crossfades instead of decoding as a level nobody painted.
inline void DecodeMaskWeights(const uint8_t* texel, float out[A3TerrainMaterial::MaxSurfaces])
{
    const float r = texel[0] * (1.0f / 255.0f);
    const float g = texel[1] * (1.0f / 255.0f);
    const float b = texel[2] * (1.0f / 255.0f);
    const float a = texel[3] * (1.0f / 255.0f);
    const float group = std::clamp((a - 0.5f) * 2.0f, 0.0f, 1.0f);   // alpha 1.0 -> RGB slots
    const float levelFour = 1.0f - std::abs(a - 0.5f) * 2.0f;        // alpha 0.5 -> slot 4
    const float levelFive = std::clamp(1.0f - a * 2.0f, 0.0f, 1.0f); // alpha 0.0 -> slot 5
    const float painted = std::min(r + g + b, 1.0f);
    out[0] = group * (1.0f - painted);
    out[1] = group * r;
    out[2] = group * g;
    out[3] = group * b;
    out[4] = levelFour;
    out[5] = levelFive;
}

// terrain.wgsl's `terrain_uv`, on the CPU: the transform is a basis, so u is the
// first component of aside/up/dir and v the second (WLD-019 bug 4).
inline void MaskUv(const A3TerrainUv& uv, float wx, float wy, float wz, float tileU, float tileV, float& outU,
                   float& outV)
{
    const float sx = uv.worldPos ? wx : tileU;
    const float sy = uv.worldPos ? wy : tileV;
    const float sz = uv.worldPos ? wz : 0.0f;
    outU = sx * uv.u[0] + sy * uv.u[1] + sz * uv.u[2] + uv.u[3];
    outV = sx * uv.v[0] + sy * uv.v[1] + sz * uv.v[2] + uv.v[3];
}

inline int WrapTexel(float coord, int dim)
{
    // The mask is sampled through a repeating sampler on the GPU.
    float f = coord - std::floor(coord);
    int t = static_cast<int>(f * static_cast<float>(dim));
    return std::clamp(t, 0, dim - 1);
}
} // namespace

// Taps per land-cell axis. 4x4 is the smallest grid that resolves a coverage
// FRACTION rather than a yes/no from one texel, which is what turns a mask edge
// into a thinning field instead of a new hard boundary one step finer than the
// old one. On Stratis a 4 m cell spans 4 mask texels, so this is a full cover.
constexpr int GrassMaskTapsPerAxis = 4;

bool TerrainWgpu::BakeAuthoredGrassMask(const Landscape& land, std::vector<uint32_t>& geography, bool refreshSelection)
{
    const int n = land.GetLandRange();
    const int materialCount = std::max(0, land.GetNTextures());
    if (n <= 0 || materialCount <= 0)
    {
        return false;
    }
    const size_t cells = static_cast<size_t>(n) * static_cast<size_t>(n);
    const std::string world = land.GetName() ? land.GetName() : "";

    // Applies the baked answer, filtered by the dev panel's per-material
    // selection. Separated from the bake itself so a live toggle costs a pass
    // over the cells rather than a decode of every mask on the map.
    const auto apply = [&]()
    {
        for (int z = 0; z < n; ++z)
        {
            for (int x = 0; x < n; ++x)
            {
                const size_t cell = static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(x);
                const uint32_t baked = _grassAuthoredCells[cell];
                if ((baked & GrassAuthoredCell) == 0)
                {
                    continue; // no authored mask here: the legacy answer stands
                }
                const int material = land.GetTexture(z, x);
                const bool selected = material >= 0 && material < static_cast<int>(_grassSurfaceEnabled.size()) &&
                                      _grassSurfaceEnabled[static_cast<size_t>(material)];
                uint32_t word = (geography[cell] & ~GrassBakedMask) | baked;
                if (!selected)
                {
                    word &= ~GrassTextureCell;
                }
                geography[cell] = word;
            }
        }
    };

    // ---- Resolve every material's slots to a clutter coverage ---------------
    //
    // The catalogue is the authority where it answers. Where it does not -- an
    // OFP world, or an Arma archive whose config we could not read -- the slot's
    // own texture NAME is the fallback, which is still a per-texel answer and so
    // still strictly better than the per-material name match it replaces.
    std::vector<std::array<SlotClutter, A3TerrainMaterial::MaxSurfaces>> slots(static_cast<size_t>(materialCount));
    std::unordered_map<std::string, uint8_t> surfaceIds;
    std::unordered_map<std::string, float> coverageCache;
    float maxCoverage = 0.0f;
    int authoredMaterials = 0;
    int byCatalogue = 0, byName = 0;
    // Every distinct surface, with the character and coverage it resolved to.
    // This is the line to read when the question is "is the map's own config
    // reaching us": a world running on the name fallback reports every surface
    // at exactly 1.00 with no character, and one reading its config reports the
    // authored spread. `models` is how many of the character's clutter classes
    // found a `model=`, which is zero when the world's own addon is not mounted
    // even though CfgSurfaceCharacters was.
    std::string surfaceReport;
    for (int m = 0; m < materialCount; ++m)
    {
        const A3TerrainMaterial* source = land.GetA3TerrainMaterial(m);
        if (source == nullptr || source->surfaceCount == 0 || source->mask.GetLength() == 0)
        {
            continue;
        }
        ++authoredMaterials;
        for (unsigned char s = 0; s < source->surfaceCount && s < A3TerrainMaterial::MaxSurfaces; ++s)
        {
            const RStringB& surface = source->surfaces[s];
            if (surface.GetLength() == 0)
            {
                continue; // a hole keeps its slot but grows nothing
            }
            SlotClutter& slot = slots[static_cast<size_t>(m)][s];
            slot.present = true;
            std::string key(surface.Data());
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            const auto cached = coverageCache.find(key);
            if (cached != coverageCache.end())
            {
                slot.coverage = cached->second;
            }
            else
            {
                float coverage = 0.0f;
                size_t models = 0, clutterCount = 0;
                const RStringB character = _surfaceCatalog.Character(surface.Data());
                if (character.GetLength() != 0)
                {
                    coverage = _surfaceCatalog.Coverage(character.Data());
                    for (const TerrainSurfaceCatalog::Clutter& entry : _surfaceCatalog.ClutterFor(character.Data()))
                    {
                        ++clutterCount;
                        models += entry.model.GetLength() != 0 ? 1 : 0;
                    }
                    if (coverage > 0.0f)
                    {
                        ++byCatalogue;
                    }
                }
                const bool authoredAnswer = coverage > 0.0f;
                if (coverage <= 0.0f && !_surfaceCatalog.Declares(surface.Data()) && IsGrassSurfaceName(surface.Data()))
                {
                    // No authored probability to scale by; treat it as a full
                    // field so a config-less install still gets its meadows.
                    // Only when the catalogue said NOTHING about the surface: one
                    // the map itself declares as `character = "Empty"` grows
                    // nothing, and guessing from its name would put the grass
                    // straight back where the map says there is none.
                    coverage = 1.0f;
                    ++byName;
                }
                if (coverage > 0.0f)
                {
                    char line[192];
                    snprintf(line, sizeof(line), "%s%s -> %s %.2f (%zu/%zu models)", surfaceReport.empty() ? "" : ", ",
                             key.c_str(), authoredAnswer ? character.Data() : "<by name>", coverage, models,
                             clutterCount);
                    surfaceReport += line;
                }
                coverageCache.emplace(key, coverage);
                slot.coverage = coverage;
            }
            if (slot.coverage > 0.0f)
            {
                maxCoverage = std::max(maxCoverage, slot.coverage);
                const auto existing = surfaceIds.find(key);
                if (existing != surfaceIds.end())
                {
                    slot.surfaceId = existing->second;
                }
                else if (surfaceIds.size() < GrassSurfaceNone)
                {
                    const uint8_t id = static_cast<uint8_t>(surfaceIds.size());
                    surfaceIds.emplace(key, id);
                    slot.surfaceId = id;
                }
            }
        }
    }
    if (authoredMaterials == 0)
    {
        return false; // every OFP world takes this exit before touching anything
    }
    if (maxCoverage <= 0.0f)
    {
        LOG_WARN(Graphics,
                 "Wgpu grass: {} authored materials but no surface on this world grows clutter; keeping the legacy "
                 "per-layer classification",
                 authoredMaterials);
        return false;
    }

    // Re-derive the dev panel's per-material selection from the authored
    // surfaces. Before this it came from matching the material's slot-0 texture
    // NAME, which is the defect being fixed: 271 of Stratis's 479 materials read
    // as `gdt_strbeach` and grew nothing at all, whatever their mask painted.
    if (refreshSelection && static_cast<int>(_grassSurfaceEnabled.size()) == materialCount)
    {
        int selectable = 0;
        for (int m = 0; m < materialCount; ++m)
        {
            const A3TerrainMaterial* source = land.GetA3TerrainMaterial(m);
            if (source == nullptr || source->surfaceCount == 0 || source->mask.GetLength() == 0)
            {
                continue; // no authored mask: the legacy name/class answer stands
            }
            bool hasClutter = false;
            for (const SlotClutter& slot : slots[static_cast<size_t>(m)])
            {
                hasClutter = hasClutter || slot.coverage > 0.0f;
            }
            _grassSurfaceEnabled[static_cast<size_t>(m)] = hasClutter;
            selectable += hasClutter ? 1 : 0;
        }
        LOG_INFO(Graphics, "Wgpu grass surfaces: {} of {} authored materials carry a clutter-bearing surface",
                 selectable, materialCount);
    }

    // A live dev-panel toggle re-enters here with the same world; re-applying the
    // cached bake costs a pass over the cells instead of decoding every mask.
    if (_grassAuthoredWorld == world && _grassAuthoredCells.size() == cells)
    {
        apply();
        return true;
    }

    // ---- Group cells by the mask they resolve through -----------------------
    //
    // A mask serves every variant of its satellite tile -- Stratis has 479
    // materials over 64 masks, Takistan 3,293 over 220 -- so decoding per
    // material would decode each image seven to fifteen times over.
    std::unordered_map<std::string, int> maskIds;
    std::vector<std::string> maskPaths;
    std::vector<int> materialMask(static_cast<size_t>(materialCount), -1);
    for (int m = 0; m < materialCount; ++m)
    {
        const A3TerrainMaterial* source = land.GetA3TerrainMaterial(m);
        if (source == nullptr || source->surfaceCount == 0 || source->mask.GetLength() == 0)
        {
            continue;
        }
        std::string key(source->mask.Data());
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        const auto found = maskIds.find(key);
        if (found != maskIds.end())
        {
            materialMask[static_cast<size_t>(m)] = found->second;
            continue;
        }
        const int id = static_cast<int>(maskPaths.size());
        maskIds.emplace(key, id);
        maskPaths.emplace_back(source->mask.Data());
        materialMask[static_cast<size_t>(m)] = id;
    }
    if (maskPaths.empty())
    {
        return false;
    }

    const size_t cellCount = cells;
    std::vector<uint32_t> cellsByMask(cellCount);
    std::vector<uint32_t> maskOffset(maskPaths.size() + 2, 0); // +1 for the "no mask" bucket
    {
        // Counting sort: one pass to count, one to place. 16 MB of indices on a
        // 2048-cell world, against 268 MB if every decoded mask were kept live.
        for (int z = 0; z < n; ++z)
        {
            for (int x = 0; x < n; ++x)
            {
                const int material = land.GetTexture(z, x);
                const int mask =
                    (material >= 0 && material < materialCount) ? materialMask[static_cast<size_t>(material)] : -1;
                ++maskOffset[static_cast<size_t>(mask + 1) + 1];
            }
        }
        for (size_t i = 1; i < maskOffset.size(); ++i)
        {
            maskOffset[i] += maskOffset[i - 1];
        }
        std::vector<uint32_t> cursor(maskOffset.begin(), maskOffset.end() - 1);
        for (int z = 0; z < n; ++z)
        {
            for (int x = 0; x < n; ++x)
            {
                const int material = land.GetTexture(z, x);
                const int mask =
                    (material >= 0 && material < materialCount) ? materialMask[static_cast<size_t>(material)] : -1;
                cellsByMask[cursor[static_cast<size_t>(mask + 1)]++] =
                    static_cast<uint32_t>(z) * static_cast<uint32_t>(n) + static_cast<uint32_t>(x);
            }
        }
    }

    // ---- Decode each mask once and answer its own cells ---------------------
    const float landGrid = land.GetLandGrid();
    const float originX = _params.world_origin.x;
    const float originZ = _params.world_origin.y;
    const float invMax = 1.0f / maxCoverage;
    std::atomic<uint32_t> decoded{0};
    std::atomic<uint32_t> missing{0};
    std::atomic<uint64_t> maskBytes{0};

    const auto processMask = [&](size_t maskIndex, const std::vector<uint8_t>& bytes)
    {
        const uint32_t begin = maskOffset[maskIndex + 1];
        const uint32_t end = maskOffset[maskIndex + 2];
        if (begin >= end)
        {
            return; // a mask nobody's cells use
        }
        const DecodedImage image = DecodePAABuffer(bytes.data(), bytes.size(), /*isPaa=*/true);
        if (!image.valid())
        {
            missing.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        decoded.fetch_add(1, std::memory_order_relaxed);
        const int mw = image.width;
        const int mh = image.height;
        const uint8_t* pixels = image.rgba.data();
        constexpr float tapStep = 1.0f / static_cast<float>(GrassMaskTapsPerAxis);
        for (uint32_t c = begin; c < end; ++c)
        {
            const uint32_t cell = cellsByMask[c];
            const int x = static_cast<int>(cell % static_cast<uint32_t>(n));
            const int z = static_cast<int>(cell / static_cast<uint32_t>(n));
            const int material = land.GetTexture(z, x);
            if (material < 0 || material >= materialCount)
            {
                continue;
            }
            const A3TerrainMaterial* source = land.GetA3TerrainMaterial(material);
            if (source == nullptr)
            {
                continue;
            }
            const auto& slotTable = slots[static_cast<size_t>(material)];
            const float height = land.GetHeight(z, x);
            float coverageSum = 0.0f;
            int histogram[A3TerrainMaterial::MaxSurfaces] = {0, 0, 0, 0, 0, 0};
            for (int tz = 0; tz < GrassMaskTapsPerAxis; ++tz)
            {
                for (int tx = 0; tx < GrassMaskTapsPerAxis; ++tx)
                {
                    const float fx = static_cast<float>(x) + (static_cast<float>(tx) + 0.5f) * tapStep;
                    const float fz = static_cast<float>(z) + (static_cast<float>(tz) + 0.5f) * tapStep;
                    const float wx = originX + fx * landGrid;
                    const float wz = originZ + fz * landGrid;
                    float u = 0.0f, v = 0.0f;
                    MaskUv(source->maskUv, wx, height, wz, fx, fz, u, v);
                    const uint8_t* texel = pixels + (static_cast<size_t>(WrapTexel(v, mh)) * static_cast<size_t>(mw) +
                                                     static_cast<size_t>(WrapTexel(u, mw))) *
                                                        4u;
                    float weights[A3TerrainMaterial::MaxSurfaces];
                    DecodeMaskWeights(texel, weights);
                    // Argmax over the slots this material actually filled. A hole
                    // must not win: the source says there is no surface there, and
                    // handing its weight to a neighbour is exactly the renumbering
                    // bug WLD-019 records.
                    int best = -1;
                    float bestWeight = 0.0f;
                    for (unsigned char s = 0; s < source->surfaceCount && s < A3TerrainMaterial::MaxSurfaces; ++s)
                    {
                        if (!slotTable[s].present)
                        {
                            continue;
                        }
                        if (best < 0 || weights[s] > bestWeight)
                        {
                            best = s;
                            bestWeight = weights[s];
                        }
                    }
                    if (best < 0)
                    {
                        continue;
                    }
                    coverageSum += slotTable[static_cast<size_t>(best)].coverage;
                    ++histogram[best];
                }
            }
            constexpr float taps = static_cast<float>(GrassMaskTapsPerAxis * GrassMaskTapsPerAxis);
            const float normalised = std::clamp((coverageSum / taps) * invMax, 0.0f, 1.0f);
            const uint32_t quantised = static_cast<uint32_t>(std::lround(normalised * 127.0f));
            // The dominant CLUTTER-bearing surface, not the dominant surface. A
            // cell that is mostly beach with a corner of dry grass grows dry
            // grass, so that is the variety its id has to name; the beach's id
            // would say nothing about the blades actually standing there.
            int dominant = -1;
            for (int s = 0; s < static_cast<int>(A3TerrainMaterial::MaxSurfaces); ++s)
            {
                if (histogram[s] <= 0 || slotTable[static_cast<size_t>(s)].surfaceId == 0xff)
                {
                    continue;
                }
                if (dominant < 0 || histogram[s] > histogram[dominant])
                {
                    dominant = s;
                }
            }
            const uint32_t surfaceId =
                dominant >= 0 ? slotTable[static_cast<size_t>(dominant)].surfaceId : GrassSurfaceNone;
            uint32_t word = GrassAuthoredCell;
            word |= (quantised << GrassCoverageShift) & GrassCoverageMask;
            word |= (surfaceId << GrassSurfaceShift) & GrassSurfaceMask;
            if (quantised > 0)
            {
                word |= GrassTextureCell;
            }
            _grassAuthoredCells[cell] = word;
        }
    };

    // I/O stays on the calling thread (QFBank is not MT-safe -- MT_SAFE is 0 in
    // QBStream.hpp); only the decode and the gather are parallel. Batching keeps
    // the compressed masks resident for one batch instead of all at once.
    const auto started = std::chrono::steady_clock::now();
    _grassAuthoredCells.assign(cellCount, 0u);
    TaskPool* pool = GetGlobalTaskPool();
    const size_t batchSize = std::max<size_t>(1, (pool ? pool->ThreadCount() : 1) * 2u);
    std::vector<std::vector<uint8_t>> batch;
    batch.reserve(batchSize);
    for (size_t first = 0; first < maskPaths.size(); first += batchSize)
    {
        const size_t last = std::min(first + batchSize, maskPaths.size());
        batch.clear();
        for (size_t i = first; i < last; ++i)
        {
            batch.push_back(ReadVfsFile(maskPaths[i].c_str()));
            maskBytes.fetch_add(batch.back().size(), std::memory_order_relaxed);
            if (batch.back().empty())
            {
                missing.fetch_add(1, std::memory_order_relaxed);
            }
        }
        const auto run = [&](uint32_t begin, uint32_t end)
        {
            for (uint32_t i = begin; i < end; ++i)
            {
                if (!batch[i].empty())
                {
                    processMask(first + i, batch[i]);
                }
            }
        };
        if (pool != nullptr)
        {
            pool->ParallelFor(static_cast<uint32_t>(last - first), run);
        }
        else
        {
            run(0, static_cast<uint32_t>(last - first));
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

    // Cells whose material carries no authored mask keep whatever the legacy
    // per-layer pass decided; they are reported so a partly-authored world is
    // visible as a number rather than as a look nobody can attribute.
    const uint32_t legacyCells = maskOffset[1];
    LOG_INFO(Graphics,
             "Wgpu grass mask bake: {} masks decoded ({} unreadable, {:.1f} MB read) for {} authored materials; "
             "{} surfaces grow clutter ({} from the map's config, {} by name), peak coverage {:.2f}; "
             "{} cells left on the legacy path; {:.0f} ms on {} thread(s)",
             decoded.load(), missing.load(), maskBytes.load() / (1024.0 * 1024.0), authoredMaterials, surfaceIds.size(),
             byCatalogue, byName, maxCoverage, legacyCells, ms, pool ? pool->ThreadCount() : 1u);
    LOG_INFO(Graphics, "Wgpu grass surface coverage: [{}]", surfaceReport.empty() ? "none" : surfaceReport);
    if (decoded.load() == 0)
    {
        // Nothing decoded: leave the legacy classification alone rather than
        // blanking a world's grass because its archives were not mounted.
        _grassAuthoredCells.clear();
        _grassAuthoredWorld.clear();
        return false;
    }
    _grassAuthoredWorld = world;
    apply();
    return true;
}

void TerrainWgpu::UploadGeography(const Landscape& land)
{
    const int n = land.GetLandRange();
    if (n <= 0)
    {
        return;
    }
    const int layerCount = std::max(0, land.GetNTextures());
    bool layersChanged = static_cast<int>(_grassSurfaceNames.size()) != layerCount;
    if (!layersChanged)
    {
        for (int i = 0; i < layerCount; ++i)
        {
            const char* name = land.GetTexture(i) ? land.GetTexture(i)->Name() : "<null>";
            if (_grassSurfaceNames[static_cast<size_t>(i)] != name)
            {
                layersChanged = true;
                break;
            }
        }
    }
    if (layersChanged)
    {
        _grassSurfaceNames.clear();
        _grassSurfaceEnabled.clear();
        _grassSurfaceNames.reserve(static_cast<size_t>(layerCount));
        _grassSurfaceEnabled.reserve(static_cast<size_t>(layerCount));
        // Two independent signals, because the generations classify differently.
        // The authored CfgSurfaces class is the authority wherever it resolves --
        // that is what covers OFP, whose texture names carry no semantics at all.
        // The name heuristic covers the Arma generations, whose surfaces resolve to
        // `Default` here (their own CfgSurfaces lives in a config.bin ParamFile
        // cannot parse) but whose texture names ARE semantic: Takistan's
        // `tk_trava_co` is matched by "trava".
        //
        // On an Arma-generation world both signals are only a SEED. They classify
        // a whole terrain material by the name of its slot-0 surface, which is
        // the cell's material for a whole land cell -- 32 m on Stratis. The
        // authored mask bake below replaces that answer per geography cell, and
        // re-derives this selection from the surfaces that actually grow clutter.
        int classGrass = 0, nameGrass = 0, resolvedLayers = 0;
        for (int i = 0; i < layerCount; ++i)
        {
            const Texture* texture = land.GetTexture(i);
            _grassSurfaceNames.emplace_back(texture ? texture->Name() : "<null>");
            bool resolved = false;
            const bool byClass = IsGrassSurfaceClass(land.GetTextureQuadrants(i), resolved);
            const bool byName = IsGrassTexture(texture);
            classGrass += byClass;
            nameGrass += byName;
            resolvedLayers += resolved;
            _grassSurfaceEnabled.push_back(byClass || byName);
        }

        // Fall back to every terrain layer ONLY when neither signal said anything
        // and the surface catalogue did not recognise a single layer -- i.e. data
        // we genuinely cannot classify. It used to fire on Everon, where the name
        // heuristic matches nothing, and made sand, rock, forest floor and shore
        // all grass-capable; the class signal now answers that world from its own
        // config instead. Water, roads, forests, cliffs and shore are still
        // rejected by the authoritative geography/slope checks in the grass
        // shader, and the Grass tab can still narrow the selection.
        const bool noneSelected = std::none_of(_grassSurfaceEnabled.begin(), _grassSurfaceEnabled.end(),
                                               [](bool enabled) { return enabled; });
        if (noneSelected && resolvedLayers == 0 && layerCount > 0)
        {
            std::fill(_grassSurfaceEnabled.begin(), _grassSurfaceEnabled.end(), true);
            LOG_INFO(Graphics,
                     "Wgpu grass: no surface class and no named grass on this world; enabling all {} terrain layers",
                     layerCount);
        }
        LOG_INFO(Graphics,
                 "Wgpu grass surfaces: {} of {} layers grass-capable ({} by CfgSurfaces class, {} by name, {} layers "
                 "the surface catalogue recognised)",
                 std::count(_grassSurfaceEnabled.begin(), _grassSurfaceEnabled.end(), true), layerCount, classGrass,
                 nameGrass, resolvedLayers);
    }

    std::vector<bool> grassLayer(static_cast<size_t>(layerCount), false);
    std::string grassLayerNames;
    std::string allLayerNames;
    int grassLayerCount = 0;
    for (int i = 0; i < land.GetNTextures(); ++i)
    {
        if (!allLayerNames.empty())
        {
            allLayerNames += ", ";
        }
        allLayerNames += std::to_string(i);
        allLayerNames += ":";
        allLayerNames += land.GetTexture(i) ? land.GetTexture(i)->Name() : "<null>";
        grassLayer[static_cast<size_t>(i)] = _grassSurfaceEnabled[static_cast<size_t>(i)];
        if (grassLayer[static_cast<size_t>(i)])
        {
            if (!grassLayerNames.empty())
            {
                grassLayerNames += ", ";
            }
            // Null-guarded, unlike the original: a layer slot can carry no
            // texture at all, and the "no named grass" fallback above enables
            // every slot including those. On a world whose terrain materials do
            // not resolve, that made this line dereference null for layer 0 and
            // took the renderer down inside DrawGround. The same call is already
            // guarded where the names are first collected.
            grassLayerNames += land.GetTexture(i) ? land.GetTexture(i)->Name() : "<null>";
            ++grassLayerCount;
        }
    }

    std::vector<uint32_t> geography(static_cast<size_t>(n) * n);
    for (int z = 0; z < n; ++z)
    {
        for (int x = 0; x < n; ++x)
        {
            uint32_t cell = land.GetGeography(x, z).packed;
            const int layer = land.GetTexture(z, x);
            if (layer >= 0 && layer < static_cast<int>(grassLayer.size()) && grassLayer[static_cast<size_t>(layer)])
            {
                cell |= GrassTextureCell;
            }
            geography[static_cast<size_t>(z) * n + x] = cell;
        }
    }

    // The map's own answer, where the map has one. OFP worlds have no authored
    // terrain materials at all, so `BakeAuthoredGrassMask` returns false before
    // touching a byte and everything below runs on the array just built.
    //
    // The catalogue is skipped outright on those worlds rather than merely going
    // unused. An OFP `config.bin` is raP version 2 with a different layout from
    // the version-0 container `ArmaRapReader` reads, so offering one is at best a
    // wasted read and at worst a misparse; and this keeps the OFP path free of
    // any Arma config handling by construction instead of by luck.
    bool anyAuthored = false;
    for (int i = 0; i < layerCount && !anyAuthored; ++i)
    {
        const A3TerrainMaterial* source = land.GetA3TerrainMaterial(i);
        anyAuthored = source != nullptr && source->surfaceCount != 0 && source->mask.GetLength() != 0;
    }
    const std::string world = land.GetName() ? land.GetName() : "";
    if (anyAuthored && _surfaceCatalogWorld != world)
    {
        BuildSurfaceCatalog(land);
        _surfaceCatalogWorld = world;
    }
    const bool authored = BakeAuthoredGrassMask(land, geography, layersChanged);

    // Match grass.wgsl: water, forests, roads/tracks and hard buildings are
    // excluded, but the legacy "full" flag is valid ordinary ground on Everon
    // and must remain grass-capable.
    size_t grassCellCount = 0;
    size_t grassRenderableCellCount = 0;
    for (uint32_t cell : geography)
    {
        if ((cell & GrassTextureCell) == 0)
        {
            continue;
        }
        ++grassCellCount;
        if ((cell & 0x00000c7bu) == 0)
        {
            ++grassRenderableCellCount;
        }
    }
    // Some legacy Everon WRP revisions mark a broad part of ordinary ground
    // as forest. If that leaves no possible grass anywhere, treat only that
    // blanket forest flag as invalid. Water, roads and tracks stay protected.
    // Real forest/builder exclusions remain intact whenever the source data
    // provides even one valid grass cell.
    if (grassCellCount > 0 && grassRenderableCellCount == 0)
    {
        for (uint32_t& cell : geography)
            cell &= ~0x00000018u; // forestInner | forestOuter
        for (uint32_t cell : geography)
        {
            if ((cell & GrassTextureCell) != 0 && (cell & 0x00000c7bu) == 0)
                ++grassRenderableCellCount;
        }
        LOG_WARN(Graphics,
                 "Wgpu grass: legacy geography had no usable grass cells; relaxed blanket forest flags -> {} cells",
                 grassRenderableCellCount);
    }
    // A handful of old WRP files also store hard-object density across entire
    // ground regions. Only as a final zero-candidate fallback do we relax it;
    // this is preferable to silently rendering no grass at all on the map.
    if (grassCellCount > 0 && grassRenderableCellCount == 0)
    {
        for (uint32_t& cell : geography)
            cell &= ~0x00000c00u; // howManyHardObjects
        for (uint32_t cell : geography)
        {
            if ((cell & GrassTextureCell) != 0 && (cell & 0x00000c7bu) == 0)
                ++grassRenderableCellCount;
        }
        LOG_WARN(
            Graphics,
            "Wgpu grass: legacy geography still had no usable cells; relaxed blanket hard-object flags -> {} cells",
            grassRenderableCellCount);
    }
    // What the shader will ACTUALLY place. grass.wgsl splits the mask: hard
    // exclusions (water/road/track/hard objects) always apply, forest only when
    // the compatibility override is off. `grassRenderableCellCount` above uses
    // the combined mask, so it under-reports whenever the override is active.
    size_t hardOnlyCells = 0;
    for (uint32_t cell : geography)
    {
        if ((cell & GrassTextureCell) != 0 && (cell & 0x00000c63u) == 0)
            ++hardOnlyCells;
    }
    LOG_INFO(Graphics,
             "Wgpu grass texture mask ({}): {} layer(s), {} / {} surface cells, {} renderable after geography, "
             "{} with forest relaxed (road/water/building always excluded) [{}]",
             authored ? "authored LCA surfaces" : "legacy per-layer", grassLayerCount, grassCellCount, geography.size(),
             grassRenderableCellCount, hardOnlyCells, grassLayerNames.empty() ? "none matched" : grassLayerNames);
    LOG_INFO(Graphics, "Wgpu terrain texture layers: [{}]", allLayerNames);
    wgr_grass_set_geography(_renderer, static_cast<uint32_t>(n), static_cast<uint32_t>(n), geography.data());

    // The grass atlases are WORLD-scoped, not process-scoped.
    //
    // They used to be guarded by a plain `uploaded` bool on the single
    // EngineWgpu::_terrain, so the first world to load owned the atlas for the
    // rest of the session. That is harmless while the only content is our own
    // loose photo cards, and becomes a real defect the moment the atlas is built
    // from the MAP's clutter: load Takistan, then Everon, and OFP terrain grows
    // Arma clutter with no way to notice. Closing it now, before that content
    // exists, is the cheap ordering.
    //
    // The key is what the atlas content is derived FROM. An OFP world derives
    // nothing from the world, so every OFP world shares one key and rebuilds
    // never fire -- Everon keeps exactly the cards it has today at exactly
    // today's cost. An Arma world is keyed by name, so any Arma <-> OFP or
    // Arma <-> Arma transition rebuilds.
    const std::string atlasKey = anyAuthored ? ("arma:" + world) : std::string("loose");
    UploadGrassTuft(atlasKey);
    UploadGrassBladeAtlas(atlasKey);
}

// GRS-E — upload an authored photographed grass clump for the mid LOD's crossed
// cards. Entirely OPTIONAL: its absence must change nothing except how the mid
// ring is drawn.
//
// If no texture is found, `have_tuft` stays false on the renderer side and the
// mid ring keeps its procedural crossed ribbons -- the long-standing look. That
// is the only fallback, deliberately. The game also ships a 2001 grass PAA
// (data/trava1_pmp2.pac), but its opaque texels average (0.322, 0.375, 0.334):
// blue level with red, i.e. grey-teal rather than green. Falling back to that
// would hand anyone without this asset a WORSE picture than no photo cards at
// all, so it is reachable only by pointing WGR_GRASS_TUFT at a converted copy.
namespace
{
// One discovered photographed grass family: a colour plate and the opacity plate
// that carries its cut-out.
struct PhotoGrassSource
{
    std::string name;
    std::string colour;
    std::string opacity;
};

// The shader indexes a fixed nine-layer array (primary + eight families), so
// this is the number of families the atlas can carry, not a list of which ones.
constexpr uint32_t MaxPhotoGrassFamilies = 8;

bool HasSuffixToken(const std::string& stem, std::string_view token)
{
    return stem.find(token) != std::string::npos;
}

// Scan assets/grass/more_grass for colour/opacity plate pairs.
//
// The convention is only that the two files sit in the same directory and that
// their names differ by a role token -- the shipped packs use albedo/opacity
// (Poliigon-style) and diff/alpha (Poly Haven-style), and both are matched by
// substituting the token. Anything following either convention works with no
// code change, which is the point: the families are whatever is installed.
std::vector<PhotoGrassSource> DiscoverPhotoGrassFamilies(uint32_t limit)
{
    namespace fs = std::filesystem;
    static constexpr std::string_view kColourTokens[] = {"albedo", "diff", "color", "colour", "basecolor"};
    static constexpr std::string_view kOpacityTokens[] = {"opacity", "alpha", "mask"};

    std::vector<PhotoGrassSource> found;
    std::error_code ec;
    const fs::path root{"assets/grass/more_grass"};
    if (!fs::is_directory(root, ec))
    {
        return found;
    }
    for (fs::recursive_directory_iterator it{root, fs::directory_options::skip_permission_denied, ec}, end;
         it != end && found.size() < limit; it.increment(ec))
    {
        if (ec)
        {
            break;
        }
        if (!it->is_regular_file(ec))
        {
            continue;
        }
        const fs::path& colour = it->path();
        std::string extension = colour.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (extension != ".jpg" && extension != ".jpeg" && extension != ".png" && extension != ".tga")
        {
            continue;
        }
        std::string stem = colour.stem().string();
        std::string lower = stem;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        // Never treat an opacity plate as a colour plate: "..._alpha_4k" also
        // contains no colour token, but "..._diff_alpha" style names could.
        bool isOpacity = false;
        for (std::string_view token : kOpacityTokens)
        {
            isOpacity = isOpacity || HasSuffixToken(lower, token);
        }
        if (isOpacity)
        {
            continue;
        }
        std::string_view matched;
        for (std::string_view token : kColourTokens)
        {
            if (HasSuffixToken(lower, token))
            {
                matched = token;
                break;
            }
        }
        if (matched.empty())
        {
            continue;
        }
        // Find the sibling opacity plate by swapping the role token, trying each
        // opacity spelling and each supported extension.
        const size_t at = lower.find(matched);
        PhotoGrassSource source;
        for (std::string_view replacement : kOpacityTokens)
        {
            std::string candidateStem = stem;
            candidateStem.replace(at, matched.size(), replacement);
            for (const char* candidateExt : {".png", ".jpg", ".jpeg", ".tga"})
            {
                fs::path candidate = colour.parent_path() / (candidateStem + candidateExt);
                if (fs::is_regular_file(candidate, ec))
                {
                    source.opacity = candidate.string();
                    break;
                }
            }
            if (!source.opacity.empty())
            {
                break;
            }
        }
        if (source.opacity.empty())
        {
            continue;
        }
        source.colour = colour.string();
        // Name the family after its own directory when that is descriptive (the
        // packs each ship in one), otherwise after the plate itself.
        std::string folder = colour.parent_path().filename().string();
        source.name = (folder == "textures" || folder.empty()) ? stem : folder;
        found.push_back(std::move(source));
    }
    // Directory iteration order is filesystem-defined; sorting keeps atlas layer
    // indices stable between runs, so a weight slider means the same family
    // every time and a saved setting is not silently reassigned.
    std::sort(found.begin(), found.end(),
              [](const PhotoGrassSource& a, const PhotoGrassSource& b) { return a.colour < b.colour; });
    return found;
}
} // namespace

const char* TerrainWgpu::GrassPhotoFamilyName(int index) const
{
    if (index < 0 || index >= static_cast<int>(_grassPhotoFamilies.size()))
    {
        return "";
    }
    return _grassPhotoFamilies[static_cast<size_t>(index)].c_str();
}

void TerrainWgpu::UploadGrassTuft(const std::string& worldKey)
{
    if (!_renderer || _grassTuftWorld == worldKey)
    {
        return;
    }
    // Read straight off disk rather than the VFS: the file sits beside the
    // binaries, not in a PBO. stb_image is already vendored for the JPEG loader.
    // The upload is a fixed nine-layer local texture array: primary A3 clump
    // plus every available user-local grass family.
    const char* overridePath = std::getenv("WGR_GRASS_TUFT");
    const char* primary[] = {overridePath, "assets/grass/meadow-grass-clump-alpha-1024.png"};
    constexpr int TargetSize = 1024;
    std::vector<uint8_t> layers;
    const auto appendLayer = [&](const char* colourPath, const char* opacityPath) -> bool
    {
        int w = 0, h = 0, channels = 0;
        uint8_t* pixels = stbi_load(colourPath, &w, &h, &channels, 4);
        if (pixels == nullptr)
        {
            return false;
        }
        std::vector<uint8_t> resized(static_cast<size_t>(TargetSize) * TargetSize * 4u);
        stbir_resize_uint8_linear(pixels, w, h, w * 4, resized.data(), TargetSize, TargetSize, TargetSize * 4,
                                  STBIR_RGBA);
        stbi_image_free(pixels);
        if (opacityPath != nullptr)
        {
            int aw = 0, ah = 0, ac = 0;
            uint8_t* opacity = stbi_load(opacityPath, &aw, &ah, &ac, 4);
            if (opacity == nullptr)
                return false;
            std::vector<uint8_t> alpha(static_cast<size_t>(TargetSize) * TargetSize * 4u);
            stbir_resize_uint8_linear(opacity, aw, ah, aw * 4, alpha.data(), TargetSize, TargetSize, TargetSize * 4,
                                      STBIR_RGBA);
            stbi_image_free(opacity);
            for (size_t i = 0; i < resized.size() / 4u; ++i)
                resized[i * 4u + 3u] = alpha[i * 4u];
        }
        layers.insert(layers.end(), resized.begin(), resized.end());
        return true;
    };
    for (const char* authored : primary)
    {
        if (authored != nullptr && *authored != '\0' && appendLayer(authored, nullptr))
            break;
    }
    if (!layers.empty())
    {
        _grassPhotoFamilies.clear();
        _grassPhotoFamilies.emplace_back("meadow-grass-clump (primary)");
        // Local families are DISCOVERED, not listed: anything dropped into
        // assets/grass/more_grass with a colour plate and a matching opacity
        // plate becomes a family. Hardcoding the eight that happened to be on one
        // machine meant the feature only worked on that machine.
        const size_t primaryBytes = static_cast<size_t>(TargetSize) * TargetSize * 4u;
        // Copy the primary layer out FIRST. Inserting a range that aliases the
        // same vector is undefined behaviour the moment the insert reallocates --
        // and it reallocates on every one of these appends. That fired precisely
        // when the optional `more_grass` sources are absent, i.e. for anyone who
        // has not installed the extra grass assets: the fallback path crashed.
        const std::vector<uint8_t> primaryLayer(layers.begin(), layers.begin() + primaryBytes);
        for (const PhotoGrassSource& variety : DiscoverPhotoGrassFamilies(MaxPhotoGrassFamilies))
        {
            if (appendLayer(variety.colour.c_str(), variety.opacity.c_str()))
            {
                _grassPhotoFamilies.push_back(variety.name);
            }
            else
            {
                LOG_WARN(Graphics, "Wgpu grass: photo family '{}' failed to decode; skipped", variety.name);
            }
        }
        // The shader's layer indices are fixed, so unfilled slots repeat the
        // primary clump rather than sampling uninitialised memory. They are not
        // named, so the Grass tab only ever offers families that really exist.
        const size_t named = _grassPhotoFamilies.size();
        while (layers.size() < (MaxPhotoGrassFamilies + 1u) * primaryBytes)
        {
            layers.insert(layers.end(), primaryLayer.begin(), primaryLayer.end());
        }
        wgr_grass_set_tufts(_renderer, TargetSize, TargetSize, MaxPhotoGrassFamilies + 1u, layers.data());
        _grassTuftWorld = worldKey;
        LOG_INFO(Graphics, "Wgpu grass: {} photographed clump families discovered ({} atlas layers)", named,
                 MaxPhotoGrassFamilies + 1u);
        return;
    }
    // Expected for anyone without the optional asset -- INFO, not a warning.
    LOG_INFO(Graphics, "Wgpu grass: no mid-LOD clump texture; mid ring uses procedural ribbons");
}

// GRS-F — replace only the near ribbon's procedural surface detail. The near
// grass keeps its existing per-blade geometry, density and wind; these are
// opaque texture layers, not alpha-cutout cards.
void TerrainWgpu::UploadGrassBladeAtlas(const std::string& worldKey)
{
    if (!_renderer || _grassBladeAtlasWorld == worldKey)
    {
        return;
    }
    constexpr std::array<const char*, 8> BladePaths = {
        "assets/grass/meadow-grass-blade-0.png", "assets/grass/meadow-grass-blade-1.png",
        "assets/grass/meadow-grass-blade-2.png", "assets/grass/meadow-grass-blade-3.png",
        "assets/grass/meadow-grass-blade-4.png", "assets/grass/meadow-grass-blade-5.png",
        "assets/grass/meadow-grass-blade-6.png", "assets/grass/meadow-grass-blade-7.png",
    };

    int width = 0;
    int height = 0;
    std::vector<uint8_t> layers;
    for (const char* path : BladePaths)
    {
        int w = 0;
        int h = 0;
        int channels = 0;
        uint8_t* pixels = stbi_load(path, &w, &h, &channels, 4); // force opaque RGBA upload
        if (pixels == nullptr || (width != 0 && (w != width || h != height)))
        {
            stbi_image_free(pixels);
            LOG_INFO(Graphics,
                     "Wgpu grass: no complete near-blade photo atlas; near ring uses procedural surface detail");
            return;
        }
        if (width == 0)
        {
            width = w;
            height = h;
            layers.reserve(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u * BladePaths.size());
        }
        layers.insert(layers.end(), pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
        stbi_image_free(pixels);
    }

    wgr_grass_set_blade_atlas(_renderer, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                              static_cast<uint32_t>(BladePaths.size()), layers.data());
    _grassBladeAtlasWorld = worldKey;
    LOG_INFO(Graphics, "Wgpu grass: near-LOD photo blade atlas uploaded ({} layers, {}x{})", BladePaths.size(), width,
             height);
}

int TerrainWgpu::GrassSurfaceCount() const
{
    return static_cast<int>(_grassSurfaceNames.size());
}

const char* TerrainWgpu::GrassSurfaceName(int index) const
{
    return index >= 0 && index < GrassSurfaceCount() ? _grassSurfaceNames[static_cast<size_t>(index)].c_str() : "";
}

bool TerrainWgpu::GrassSurfaceEnabled(int index) const
{
    return index >= 0 && index < static_cast<int>(_grassSurfaceEnabled.size()) &&
           _grassSurfaceEnabled[static_cast<size_t>(index)];
}

void TerrainWgpu::SetGrassSurfaceEnabled(int index, bool enabled)
{
    if (index < 0 || index >= static_cast<int>(_grassSurfaceEnabled.size()) ||
        _grassSurfaceEnabled[static_cast<size_t>(index)] == enabled)
    {
        return;
    }
    _grassSurfaceEnabled[static_cast<size_t>(index)] = enabled;
    if (_uploaded != nullptr)
    {
        UploadGeography(*_uploaded);
    }
}

void TerrainWgpu::UploadJitterMap(const Landscape& land)
{
    const int n = land.GetLandRange();
    if (n <= 0)
    {
        return;
    }
    // Per-grid-point random UV offsets (Landscape::_random). GetRandomColor
    // yields at most +-0.7 UV, so the snorm quantisation loses nothing next to
    // the source's own 0.1 granularity.
    std::vector<int8_t> offsets(2 * static_cast<size_t>(n) * n);
    for (int z = 0; z < n; z++)
    {
        for (int x = 0; x < n; x++)
        {
            float u = 0.0f;
            float v = 0.0f;
            land.GetRandomColor(x, z, u, v);
            const size_t at = 2 * (static_cast<size_t>(z) * n + x);
            offsets[at + 0] = static_cast<int8_t>(std::lround(u * 127.0f));
            offsets[at + 1] = static_cast<int8_t>(std::lround(v * 127.0f));
        }
    }
    wgr_terrain_set_jitter_map(_renderer, static_cast<uint32_t>(n), static_cast<uint32_t>(n), offsets.data());
}

void TerrainWgpu::UploadDetailNoise()
{
    // Global config-driven texture; load once for the renderer's lifetime,
    // through the shared texture bank like any other texture.
    if (_detailNoiseTried)
    {
        return;
    }
    _detailNoiseTried = true;

    const ParamEntry& names = Remaster >> "CfgDetailTextures";
    RStringB detailName = names >> "detail";
    _detailNoise = GlobLoadTexture(detailName);
    Texture* tex = _detailNoise;
    if (tex == nullptr)
    {
        return;
    }
    const uint64_t handle = static_cast<TextureWgpu*>(tex)->EnsureUploaded();
    if (handle != 0)
    {
        wgr_terrain_set_detail_layer(_renderer, handle);
    }
}

void TerrainWgpu::DrawTerrain(Scene& scene, int xBeg, int zBeg, int xEnd, int zEnd)
{
    if (_renderer == nullptr || GLandscape == nullptr)
    {
        return;
    }
    const Landscape& land = *GLandscape;
    UploadIfNeeded(land);
    if (_rootIndex < 0)
    {
        return;
    }

    // Refresh the coast wet-band params every frame: the animated sea level + clock (+ the
    // live-tuned coast look) drive the damp intertidal band, keyed on the SAME sea level and
    // swash the water uses so the two register at one moving waterline.
    const Engine::WaterSettings& coast = _engine.WaterLook();
    _params.sea_level = land.GetSeaLevel();
    _params.time = Glob.time.toFloat();
    _params.swash_speed = coast.swashSpeed;
    _params.swash_amp = coast.swashAmp;
    _params.wet_height = coast.wetHeight;
    _params.wet_darken = coast.wetDarken;
    wgr_terrain_set_params(_renderer, &_params);

    Camera* camera = scene.GetCamera();
    if (camera == nullptr)
    {
        return;
    }
    // The engine's visible rect (xBeg..zEnd) is the camera frustum projected to the
    // ground out to the far plane — already off-map-extending, not map-clamped. Map-only
    // clips it to the map (draw distance); extended clips it to the built tree so the
    // off-map border draws too. Either way frustum + CDLOD ranges + fog bound the result.
    const float landGrid = land.GetLandGrid();
    const int landRange = land.GetLandRange();
    float rx0, rz0, rx1, rz1;
    if (_extended)
    {
        rx0 = std::max(xBeg * landGrid, _treeMin);
        rz0 = std::max(zBeg * landGrid, _treeMin);
        rx1 = std::min(xEnd * landGrid, _treeMax);
        rz1 = std::min(zEnd * landGrid, _treeMax);
    }
    else
    {
        rx0 = std::max(xBeg, 0) * landGrid;
        rz0 = std::max(zBeg, 0) * landGrid;
        rx1 = std::min(xEnd, landRange) * landGrid;
        rz1 = std::min(zEnd, landRange) * landGrid;
    }

    auto emit = [&](const CdlodSelection& s)
    {
        WgrTerrainNode node{};
        node.origin = {s.originX, s.originZ};
        node.size = s.size;
        node.lod = static_cast<uint32_t>(s.level);
        node.morph_start = s.morphStart;
        node.morph_end = s.morphEnd;
        _selected.push_back(node);
    };

    _selected.clear();
    // Off-map terrain draws in full, seabed included — NOT just above-water land. The
    // water is transparent, so the seabed shows through it; pruning below-sea off-map
    // would leave a visible seam at the map edge where in-map water sits over seabed
    // but off-map water sits over the empty sky background. (Once the water look plan
    // makes deep water opaque, an occlusion prune of fully-submerged off-map terrain
    // can come back as a pure optimization — keep the terrain extent >= the water
    // extent so seabed always underlies the ocean.)
    SelectVisibleCdlod(
        _tree, _rootIndex, _numLevels, _ranges, _morphRegion, *camera, rx0, rz0, rx1, rz1,
        [](const CdlodNode&) { return true; }, emit);

    _engine.SubmitTerrain(_selected);
    // The procedural path must remain available on maps that use the modern GPU
    // terrain path (which can skip the legacy alpha overlay callback entirely).
    // SubmitGrass is frame-deduplicated by EngineWgpu; SetGrassParams below is an
    // additional legacy signal, not the only activation path.
    _engine.SubmitGrass();
}

} // namespace Poseidon
