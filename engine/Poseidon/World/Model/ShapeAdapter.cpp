#include <mutex>
#include <memory>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <unordered_map>
#include <unordered_set>
#include <Poseidon/World/Model/ShapeAdapter.hpp>
#include <Poseidon/Dev/Diag/ScopedTimer.hpp>
#include <Poseidon/World/Model/Model.hpp>

#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Material.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/IO/Streams/ArchiveSourceBinding.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/Graphics/Rendering/Draw/SpecLods.hpp>
#include <vector>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdint.h>
#include <string.h>
#include <string>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Containers/BoolArray.hpp>
#include <Poseidon/Foundation/Containers/StreamArray.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Foundation/platform.hpp>

using namespace Poseidon;
namespace Poseidon
{
extern Object* NewObject(RString typeName, RString shapeName);
}
extern Object* NewProxyObject(RString shapeName);

namespace Poseidon
{
namespace Model
{
namespace ShapeAdapter
{


// A tangent basis is defined by geometry and UV0, not by the packed ST values
// currently captured from the narrow ODOL-73 reader.  Generate it here, after
// source vertices have been split at normal/UV seams, so a normal map receives
// a stable per-vertex frame.  Degenerate UV triangles leave their vertices at
// zero and the WGPU shader uses its derivative fallback for those cases.
static std::vector<std::array<float, 3>> buildTangents(const Mesh& mesh,
                                                        std::vector<std::array<float, 3>>& outBinormals)
{
    const size_t count = mesh.vertices.size();
    std::vector<std::array<float, 3>> tangents(count, {0.f, 0.f, 0.f});
    outBinormals.assign(count, {0.f, 0.f, 0.f});

    const auto addTriangle = [&](uint32_t ia, uint32_t ib, uint32_t ic)
    {
        if (ia >= count || ib >= count || ic >= count)
            return;

        const Vertex& a = mesh.vertices[ia];
        const Vertex& b = mesh.vertices[ib];
        const Vertex& c = mesh.vertices[ic];
        const float e1x = b.position.x - a.position.x;
        const float e1y = b.position.y - a.position.y;
        const float e1z = b.position.z - a.position.z;
        const float e2x = c.position.x - a.position.x;
        const float e2y = c.position.y - a.position.y;
        const float e2z = c.position.z - a.position.z;
        const float du1 = b.uv.u - a.uv.u;
        const float dv1 = b.uv.v - a.uv.v;
        const float du2 = c.uv.u - a.uv.u;
        const float dv2 = c.uv.v - a.uv.v;
        const float determinant = du1 * dv2 - du2 * dv1;
        if (std::fabs(determinant) <= 1e-8f)
            return;

        const float reciprocal = 1.f / determinant;
        const std::array<float, 3> tangent = {
            (e1x * dv2 - e2x * dv1) * reciprocal,
            (e1y * dv2 - e2y * dv1) * reciprocal,
            (e1z * dv2 - e2z * dv1) * reciprocal,
        };
        const std::array<float, 3> binormal = {
            (e2x * du1 - e1x * du2) * reciprocal,
            (e2y * du1 - e1y * du2) * reciprocal,
            (e2z * du1 - e1z * du2) * reciprocal,
        };
        for (uint32_t index : {ia, ib, ic})
        {
            for (int axis = 0; axis < 3; ++axis)
            {
                tangents[index][axis] += tangent[axis];
                outBinormals[index][axis] += binormal[axis];
            }
        }
    };

    for (const Triangle& triangle : mesh.triangles)
        addTriangle(triangle.indices[0], triangle.indices[1], triangle.indices[2]);
    for (const Quad& quad : mesh.quads)
    {
        addTriangle(quad.indices[0], quad.indices[1], quad.indices[2]);
        addTriangle(quad.indices[0], quad.indices[2], quad.indices[3]);
    }

    return tangents;
}


// MAT-053 seat registry (see the header). Guarded: convertToLODShape runs on loader
// threads.
namespace
{
std::mutex GBridgedSeatMutex;
std::unordered_map<const void*, ::Vector3> GBridgedSeats;
} // namespace

bool GetBridgedPilotSeat(const LODShapeWithShadow* shape, float out[3])
{
    std::lock_guard<std::mutex> lock(GBridgedSeatMutex);
    const auto found = GBridgedSeats.find(static_cast<const void*>(shape));
    if (found == GBridgedSeats.end())
    {
        return false;
    }
    out[0] = found->second.X();
    out[1] = found->second.Y();
    out[2] = found->second.Z();
    return true;
}

ProxyModelName normalizeProxyModelName(const std::string& selectionName)
{
    constexpr const char* proxyPrefix = "proxy:";
    constexpr size_t proxyPrefixLen = 6;

    std::string modelName = selectionName;
    if (modelName.rfind(proxyPrefix, 0) == 0)
    {
        modelName.erase(0, proxyPrefixLen);
    }

    int id = -1;
    size_t dot = modelName.find('.');
    if (dot != std::string::npos)
    {
        std::string idText = modelName.substr(dot + 1);
        modelName.erase(dot);
        id = atoi(idText.c_str());
    }

    return {modelName, id};
}

static int convertFaceFlagsToSpecial(FaceFlags flags)
{
    int spec = 0;
    uint32_t raw = static_cast<uint32_t>(flags);

    if (raw & static_cast<uint32_t>(FaceFlags::IsShadow))
    {
        spec |= 0x40;
    }
    if (raw & static_cast<uint32_t>(FaceFlags::NoShadow))
    {
        spec |= 0x20;
    }

    if (raw & static_cast<uint32_t>(FaceFlags::ZBiasMask))
    {
        int bias = (raw & static_cast<uint32_t>(FaceFlags::ZBiasMask)) / static_cast<uint32_t>(FaceFlags::ZBiasStep);
        spec |= bias * ZBiasStep;
    }

    // Texture clamping (values match between FaceFlags and old special flags)
    if (raw & 0x2000)
        spec |= 0x2000; // NoClamp
    if (raw & 0x4000)
        spec |= 0x4000; // ClampU
    if (raw & 0x8000)
        spec |= 0x8000; // ClampV

    // Texture merging
    if (raw & static_cast<uint32_t>(FaceFlags::DisableTexMerge))
    {
        spec |= 0x1000000;
    }

    return spec;
}

// COL-001 -- one line per model, at load, saying whether the runtime can collide
// with it. Enabled by WGR_COLLISION_STATS=1.
//
//   collision: model=<path> src=<fmt>/<rev> geometry lod=<n> geomFaces=<f>
//              componentSels=<s> withFaces=<t> components=<k> planesOK=<a>/<k>
//              mass=<m> passable=<0|1> fire=<n> view=<n> landContact=<n>
//              roadway=<n> paths=<n> autocenter=<0|1> bcenter=<x>,<y>,<z>
//
// `components` is what Object::Intersect iterates; zero means no hit is ever
// produced. `componentSels` is how many ComponentXX named selections the
// geometry LOD carries and `withFaces` how many of them arrived with the four
// or more faces InitConvexComponents needs -- a selection short of that is a
// face-index unit bug in a loader, not a modelling one. `passable` is
// Object::IsPassable() -- mass under 10 kg -- and a soldier's collision response
// skips every object for which it is true, which is how a house with a
// perfectly good geometry LOD was walked through. `planesOK` tests each
// component's half-spaces against the centroid of its own vertices (always
// inside a convex hull); a component that fails has its face planes facing the
// wrong way, i.e. the source winding is not the engine's. `autocenter` and
// `bcenter` are the shape's origin bookkeeping, for the "partly in the ground"
// question: a non-zero bcenter is a model whose vertices were moved to their
// bounding-box centre at load.
static void LogCollisionStats(LODShapeWithShadow* shape, const Poseidon::Model::Model& model)
{
    static const bool enabled = []
    {
        const char* v = std::getenv("WGR_COLLISION_STATS");
        return v && *v && std::strcmp(v, "0") != 0;
    }();
    if (!enabled || !shape)
        return;

    const int geomLevel = shape->FindGeometryLevel();
    int components = 0;
    int planesOk = 0;
    int geomFaces = 0;
    int componentSels = 0;
    int componentSelsWithFaces = 0;
    if (geomLevel >= 0)
    {
        if (Shape* geom = shape->GeometryLevel())
        {
            geomFaces = geom->NFaces();
            for (int s = 0; s < geom->NNamedSel(); ++s)
            {
                const ::NamedSelection& sel = geom->NamedSel(s);
                if (strnicmp(sel.Name(), "component", 9) != 0)
                    continue;
                ++componentSels;
                if (sel.Faces().Size() >= 4)
                    ++componentSelsWithFaces;
            }
        }
        const ConvexComponents& cc = shape->GetGeomComponents();
        components = cc.Size();
        for (int c = 0; c < components; ++c)
        {
            const ConvexComponent& component = *cc[c];
            Shape* geom = component.GetShape();
            const int n = component.Size();
            if (!geom || n <= 0 || component.NPlanes() < 4)
                continue;
            ::Vector3 centroid(VZero);
            for (int i = 0; i < n; ++i)
                centroid += geom->Pos(component[i]);
            centroid = centroid * (1.0f / static_cast<float>(n));
            const float tolerance = -1e-3f * std::max(component.GetRadius(), 1e-3f);
            bool inside = true;
            for (int p = 0; p < component.NPlanes() && inside; ++p)
                inside = component.GetPlane(p).Distance(centroid) >= tolerance;
            if (inside)
                ++planesOk;
        }
    }
    const ::Vector3 bcenter = shape->BoundingCenter();
    LOG_INFO(Graphics,
             "collision: model={} src={}/{} geometry lod={} geomFaces={} componentSels={} withFaces={} components={} "
             "planesOK={}/{} mass={:.1f} passable={} fire={} view={} landContact={} roadway={} paths={} "
             "autocenter={} bcenter={:.2f},{:.2f},{:.2f}",
             model.sourcePath, model.sourceFormat, model.sourceVersion, geomLevel, geomFaces, componentSels,
             componentSelsWithFaces, components, planesOk, components, shape->Mass(), shape->Mass() < 10.0f ? 1 : 0,
             shape->FindFireGeometryLevel(), shape->FindViewGeometryLevel(), shape->FindLandContactLevel(),
             shape->FindRoadwayLevel(), shape->FindPaths(), shape->IsAutoCenter() ? 1 : 0, bcenter.X(), bcenter.Y(),
             bcenter.Z());
}

// COL-001 (opt-in, WGR_SYNTH_GEOMETRY_BBOX=1) -- give a model that has NO
// geometry LOD at all a one-component box built from its bounding box, so it
// blocks movement instead of being walked through.
//
// This exists for the Reforger conversions: `PoseidonTools xob convert` writes
// visual LODs only, so those models have nothing the collision code can use and
// no amount of reading fixes that. A box is a crude stand-in -- a house becomes
// a solid block you cannot enter, and a tree's canopy becomes a wall -- which is
// why it is off by default and why the real fix is a geometry LOD with convex
// components in the converter. Returns true when a LOD was added; the caller
// (a friend of LODShape) then gives it a mass, because a massless box is
// Object::IsPassable() and would change nothing.
static bool SynthesizeBoxGeometryIfRequested(LODShapeWithShadow* shape, const char* modelName)
{
    static const bool enabled = []
    {
        const char* v = std::getenv("WGR_SYNTH_GEOMETRY_BBOX");
        return v && *v && std::strcmp(v, "0") != 0;
    }();
    if (!enabled || !shape)
        return false;
    if (shape->FindGeometryLevel() >= 0)
        return false;
    if (shape->NLevels() < 1 || shape->NLevels() >= MAX_LOD_LEVELS)
        return false;
    Shape* visual = shape->Level(0);
    if (!visual || visual->NVertex() < 3)
        return false;

    const ::Vector3 mn = visual->Min();
    const ::Vector3 mx = visual->Max();
    if (!(mx.X() - mn.X() > 0.05f) || !(mx.Y() - mn.Y() > 0.05f) || !(mx.Z() - mn.Z() > 0.05f))
        return false;

    // Eight corners, six quads. The winding is checked against the engine's own
    // convention below rather than assumed: the box is built once, its planes
    // computed, and if the box centre is not on the inside of every plane the
    // faces are rebuilt reversed.
    const ::Vector3 corners[8] = {
        ::Vector3(mn.X(), mn.Y(), mn.Z()), ::Vector3(mx.X(), mn.Y(), mn.Z()),
        ::Vector3(mx.X(), mn.Y(), mx.Z()), ::Vector3(mn.X(), mn.Y(), mx.Z()),
        ::Vector3(mn.X(), mx.Y(), mn.Z()), ::Vector3(mx.X(), mx.Y(), mn.Z()),
        ::Vector3(mx.X(), mx.Y(), mx.Z()), ::Vector3(mn.X(), mx.Y(), mx.Z()),
    };
    static const int quads[6][4] = {
        {0, 1, 2, 3}, // bottom
        {4, 7, 6, 5}, // top
        {0, 4, 5, 1}, // -z
        {1, 5, 6, 2}, // +x
        {2, 6, 7, 3}, // +z
        {3, 7, 4, 0}, // -x
    };
    const ::Vector3 center = (mn + mx) * 0.5f;

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const bool reversed = attempt == 1;
        Ref<Shape> geom = new Shape();
        geom->Init(8);
        for (int v = 0; v < 8; ++v)
        {
            geom->SetPos(v) = corners[v];
            geom->SetNorm(v) = (corners[v] - center).Normalized();
            geom->SetU(v, 0.0f);
            geom->SetV(v, 0.0f);
            geom->SetClip(v, ClipAll);
        }
        std::vector<SelInfo> selInfos;
        for (int v = 0; v < 8; ++v)
            selInfos.push_back(SelInfo(static_cast<VertexIndex>(v), 255));
        std::vector<VertexIndex> faceIndices;
        for (int f = 0; f < 6; ++f)
        {
            Poly poly;
            poly.Init();
            poly.SetN(4);
            for (int i = 0; i < 4; ++i)
                poly.Set(i, static_cast<VertexIndex>(quads[f][reversed ? 3 - i : i]));
            poly.SetTexture(nullptr);
            poly.SetSpecial(0);
            geom->AddFace(poly);
            faceIndices.push_back(static_cast<VertexIndex>(f));
        }
        geom->AddNamedSel(::NamedSelection("Component01", selInfos.data(), static_cast<int>(selInfos.size()),
                                           faceIndices.data(), static_cast<int>(faceIndices.size())));
        geom->Compact();
        geom->CalculateMinMax();
        geom->StoreOriginalMinMax();
        geom->InitPlanes();
        geom->RecalculateNormals(true);

        bool inside = true;
        for (int f = 0; f < 6 && inside; ++f)
            inside = geom->GetPlane(f).Distance(center) >= 0.0f;
        if (!inside && !reversed)
            continue; // rebuild with the other winding

        // GEOMETRY_SPEC: 1e13, the same sentinel every generation stores.
        shape->AddShape(geom, 1e13f); // AddShape ends in ScanShapes, so _geometry is set here
        LOG_INFO(Graphics, "ShapeAdapter: {} had no geometry LOD; synthesised a bounding-box component ({}x{}x{} m, {})",
                 modelName ? modelName : "<unnamed>", mx.X() - mn.X(), mx.Y() - mn.Y(), mx.Z() - mn.Z(),
                 reversed ? "reversed winding" : "source winding");
        return true;
    }
    return false;
}

// The friend-scope half of the synthesis: give the box a mass, because
// Object::IsPassable() is `mass < 10` and a massless box changes nothing. Called
// from convertToLODShape (a friend of Shape and LODShape); the arrays it fills
// are protected. 100 t is "static, immovable" for every consumer that reads
// mass -- soldiers stop at it, vehicles treat it as a wall.
static void SynthesizeBoxGeometryMass(LODShapeWithShadow* shape, Shape* geom, AutoArray<float>& massArray,
                                      float& mass, float& invMass, AutoArray<VertexIndex>& pointToVertex,
                                      AutoArray<VertexIndex>& vertexToPoint)
{
    if (!shape || !geom)
        return;
    const int n = geom->NVertex();
    pointToVertex.Resize(n);
    vertexToPoint.Resize(n);
    for (int v = 0; v < n; ++v)
    {
        pointToVertex[v] = static_cast<VertexIndex>(v);
        vertexToPoint[v] = static_cast<VertexIndex>(v);
    }
    constexpr float kTotal = 100000.0f;
    massArray.Resize(n);
    for (int v = 0; v < n; ++v)
        massArray[v] = n > 0 ? kTotal / static_cast<float>(n) : 0.0f;
    mass = kTotal;
    invMass = 1.0f / kTotal;
    shape->CalculateMass();
}

// vertex.flags always holds pre-encoded ClipFlags regardless of source format:
//   ODOL: SaveOptimized() pre-computes ClipFlags (materials 200-203, fog, land, decal);
//         ODOLLoader stores them directly in vertex.flags.
//   MLOD: MLODLoader converts raw POINT_* flags (data3d.h) to ClipFlags, encodes
//         materials into ClipUserMask, then stores in vertex.flags.
// Both paths are identical here — always cast vertex.flags to ClipFlags directly.
void BuildAdapterBankTables(const Poseidon::Model::Model& model, AdapterBankTables& tables)
{
    static const bool trace = [] {
        const char* value = std::getenv("POSEIDON_BANK_TABLE_TRACE");
        return value && std::strcmp(value, "1") == 0;
    }();
    double loadMs = 0, headerMs = 0, materialMs = 0;
    size_t textureCalls = 0, materialCalls = 0;
    const bool isODOL = (model.sourceFormat == "ODOL");
    const int lodCount = static_cast<int>(model.lodLevels.size());
    tables.textures.resize(lodCount);
    tables.surfMats.resize(lodCount);
    tables.mlodSurfMats.clear();
    tables.mlodSurfMats.resize(lodCount);
    for (int i = 0; i < lodCount; ++i)
    {
        const Mesh& mesh = model.lodLevels[i].mesh;
        auto& texRow = tables.textures[i];
        texRow.resize(mesh.materials.size());
        for (size_t matIdx = 0; matIdx < mesh.materials.size(); ++matIdx)
        {
            const Material& mat = mesh.materials[matIdx];
            if (mat.texturePath.empty())
                continue;
            std::string texPath = mat.texturePath;
            for (char& c : texPath)
            {
                if (c >= 'A' && c <= 'Z')
                    c = c - 'A' + 'a';
            }
            auto started = trace ? Dev::Perf::Now() : Dev::Perf::Point{};
            {
                // Only the already-existing owner primary-texture Init/read is
                // prioritised. Cached unwrapped headers remain Unknown; no new
                // LoadHeaders or fresh-name capture is added for ODOL.
                ArchiveSourceBinding::ModelReadScope sourcePurpose;
                texRow[matIdx] = GlobLoadTexture(texPath.c_str());
                if (trace) { loadMs += Dev::Perf::ElapsedMs(started); started = Dev::Perf::Now(); }
                // The MLOD specials scans read IsAlpha/IsTransparent/IsAnimated, which
                // need the headers; load them here so the conversion never has to.
                if (!isODOL && texRow[matIdx])
                    texRow[matIdx]->LoadHeaders();
            }
            if (trace) { headerMs += Dev::Perf::ElapsedMs(started); ++textureCalls; }
        }
        auto& matRow = tables.surfMats[i];
        if (mesh.sections.empty())
        {
            auto& mlodRow = tables.mlodSurfMats[i];
            for (size_t m = 0; m < mesh.materials.size(); ++m)
            {
                const std::string& path = mesh.materials[m].materialPath;
                if (path.empty() || !texRow[m] || mlodRow.count(path))
                    continue;
                const auto started = trace ? Dev::Perf::Now() : Dev::Perf::Point{};
                mlodRow.emplace(path, GTexMaterialBank.New(path.c_str()));
                if (trace) { materialMs += Dev::Perf::ElapsedMs(started); ++materialCalls; }
            }
        }
        matRow.resize(mesh.sections.size());
        for (size_t s = 0; s < mesh.sections.size(); ++s)
        {
            const auto started = trace ? Dev::Perf::Now() : Dev::Perf::Point{};
            const auto& sec = mesh.sections[s];
            Texture* tex = nullptr;
            if (sec.materialIndex != UINT32_MAX && sec.materialIndex < texRow.size())
                tex = texRow[sec.materialIndex];
            if (sec.materialIndex != UINT32_MAX && sec.materialIndex < mesh.materials.size() &&
                !mesh.materials[sec.materialIndex].materialPath.empty())
                matRow[s] = GTexMaterialBank.New(mesh.materials[sec.materialIndex].materialPath.c_str());
            else
                matRow[s] = GTexMaterialBank.TextureToMaterial(tex);
            if (trace) { materialMs += Dev::Perf::ElapsedMs(started); ++materialCalls; }
        }
    }
    if (trace && loadMs + headerMs + materialMs >= 1.0)
        LOG_INFO(Graphics, "Adapter bank split: format={} lods={} textures={} sections={} load={:.3f} headers={:.3f} materials={:.3f} ms",
                 model.sourceFormat, lodCount, textureCalls, materialCalls, loadMs, headerMs, materialMs);
}

// Proxy objects for a converted shape. MAIN THREAD ONLY: the probe reads the
// config (Pars) and the file table (QIFStreamB), and NewProxyObject/NewObject
// recurse into ShapeBank::New. Split out of convertToLODShape so the geometry
// conversion can run on a preparer worker; callers that pass bank tables to the
// conversion MUST call this afterwards, before OptimizeOneShape
// (Shape::SerializeBin creates proxies before optimize, and that order stands).
void CreateAdapterProxies(LODShapeWithShadow* shape, const Poseidon::Model::Model& model)
{
    if (model.sourceFormat != "ODOL")
        return;
    shape->InvalidateQueryPolicy();
    const int lodCount = static_cast<int>(model.lodLevels.size());
    // MAT-053: candidate pilot/driver seats from SKIPPED crew proxies, one per
    // lod they appear on; resolved to a single seat after the loop, when the
    // shape's resolutions exist and FindSpecLevel can name the interior LOD.
    std::vector<std::pair<int, ::Vector3>> bridgedSeatCandidates;
    // Proxies must be created before OptimizeShapes (Shape::SerializeBin order)
    {
        for (int li = 0; li < lodCount; li++)
        {
            Shape* lod = shape->_lods[li];
            if (!lod)
                continue;
            if (li >= static_cast<int>(model.lodLevels.size()))
                continue;
            const auto& lodLevel = model.lodLevels[li];
            for (size_t pi = 0; pi < lodLevel.mesh.proxies.size(); pi++)
            {
                const auto& proxy = lodLevel.mesh.proxies[pi];
                ProxyModelName proxyName = normalizeProxyModelName(proxy.name);
                RString modelName(proxyName.modelName.c_str());

                // AST-016B: probe before constructing. On OFP data every
                // proxy shape ships, but the rev-40+ ODOL conversion now
                // forwards proxy records whose models this install may not
                // carry (Arma 1's crew placeholders under ca\temp\proxies\
                // are not files anywhere -- the retail game resolves them
                // another way). Letting Shapes.New discover the absence
                // raises WarningMessage, and one warning is all it takes:
                // InitVehicles fails the whole mission on the error latch
                // (GetMaxError() >= EMError). An absent proxy model must
                // degrade to an absent proxy, not a refused mission.
                //
                // Owner report 2026-08-26 ("even in original OFP assets I
                // spawn far behind the cockpit"): the first version of this
                // probe checked ONLY the file, but a classic crew proxy is
                // a bare name ("uh60pilot") whose model is supplied by its
                // CfgNonAIVehicles class ("Proxyuh60pilot") -- there is no
                // file of that name and never was. The probe silently
                // dropped 102 stock cockpit-crew proxies per mission, the
                // driver ManProxy was never Present(), GetProxyCamera
                // returned false, and every interior camera fell back to
                // the MODEL ORIGIN -- metres behind the seat on aircraft.
                // So: a proxy whose config class exists is NEVER skipped;
                // the file probe is the fallback for config-less paths
                // (the Arma-era \ca\temp\... placeholders this probe was
                // written for, which have neither class nor file).
                const bool proxyHasConfigClass =
                    (Pars >> "CfgNonAIVehicles").FindEntry(RString("Proxy") + modelName) != nullptr;
                if (!proxyHasConfigClass && !QIFStreamB::FileExist(GetShapeName(modelName)))
                {
                    // MAT-053: before dropping a PILOT/DRIVER seat, remember where it
                    // was (first one wins -- proxy sequence 1 is the pilot). The name
                    // test mirrors CfgCrew's own vocabulary; "view_*" anchors are not
                    // seats.
                    {
                        const char* base = modelName;
                        for (const char* q = modelName; *q; q++)
                            if (*q == '\\' || *q == '/')
                                base = q + 1;
                        std::string leaf(base);
                        for (char& c : leaf)
                            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                        // "out" excluded: a tank ships both `driver` and
                        // `driverout` (the turn-out seat), and `find("driver")`
                        // matches both -- storing the out seat here put a
                        // buttoned-up driver's camera at the hatch head position.
                        if (leaf.rfind("view", 0) != 0 &&
                            (leaf.find("pilot") != std::string::npos ||
                             leaf.find("driver") != std::string::npos) &&
                            leaf.find("codriver") == std::string::npos &&
                            leaf.find("out") == std::string::npos)
                        {
                            const auto& pm = proxy.transform.m;
                            bridgedSeatCandidates.push_back(
                                {li, ::Vector3(-pm[0][3], pm[1][3], -pm[2][3])});
                        }
                    }
                    LOG_INFO(Graphics, "Proxy model absent, skipped: {} (from {})",
                             (const char*)modelName, proxy.name);
                    continue;
                }

                Ref<ProxyObject> po = new ProxyObject;
                if (GReplaceProxies)
                {
                    RString proxyType = RString("Proxy") + modelName;
                    RString proxyShape = GetShapeName(modelName);
                    po->obj = NewObject(proxyType, proxyShape);
                }
                else
                {
                    po->obj = NewProxyObject(modelName);
                }
                if (!po->obj)
                    continue;

                po->name = modelName;
                po->id = proxy.id >= 0 ? proxy.id : proxyName.id;
                po->selection = static_cast<int>(proxy.selectionIndex);

                const auto& m = proxy.transform.m;
                Matrix4 trans;
                trans.SetDirectionAside(::Vector3(m[0][0], m[0][1], m[0][2]));
                trans.SetDirectionUp(::Vector3(m[1][0], m[1][1], m[1][2]));
                trans.SetDirection(::Vector3(m[2][0], m[2][1], m[2][2]));
                trans.SetPosition(::Vector3(m[0][3], m[1][3], m[2][3]));

                LODShapeWithShadow* pshape = po->obj->GetShape();
                if (pshape)
                    trans.SetPosition(trans.FastTransform(pshape->BoundingCenter()));
                po->obj->SetTransform(trans);
                po->invTransform = trans.InverseScaled();
                po->obj->SetDestructType(DestructNo);
                lod->_proxy.Add(po);
            }
        }
    }

    // MAT-053 resolution: pick the seat from the VIEW-PILOT (else VIEW-GUNNER) LOD
    // when one carries it -- on every bridged model measured, the graphical-LOD
    // records disagree with the view-LOD record by a 180-degree turn about Y (A1
    // UH-60 lod0 seat -0.65,-1.51,-4.57 vs view-lod +0.65,-1.51,+4.57), and the
    // view-LOD one is the one that put the camera in the cockpit.
    if (!bridgedSeatCandidates.empty())
    {
        // Preference order among the interior LODs. VIEW_CARGO is in the list
        // because an Arma 3 helicopter need not have a VIEW_PILOT LOD at all --
        // the MH-9 ships 1200 (cargo) for the whole cabin and no 1100, so
        // without this the seat came from whichever graphical LOD happened to
        // be first.
        const int vp = shape->FindSpecLevel(VIEW_PILOT);
        const int vg = shape->FindSpecLevel(VIEW_GUNNER);
        const int vc = shape->FindSpecLevel(VIEW_CARGO);
        const ::Vector3* chosen = &bridgedSeatCandidates.front().second;
        for (const auto& cand : bridgedSeatCandidates)
        {
            if (cand.first == vp)
            {
                chosen = &cand.second;
                break;
            }
            if (cand.first == vg || cand.first == vc)
            {
                chosen = &cand.second;
            }
        }
        std::lock_guard<std::mutex> lock(GBridgedSeatMutex);
        GBridgedSeats[static_cast<const void*>(shape)] = *chosen;
    }
}

// The ODOL adapter TAIL: proxy creation plus everything that must run AFTER it in
// Shape::SerializeBin order (the MAT-044 proxy-marker patching, the selections
// retrofit, OptimizeShapes/ScanShapes, mass, convex components, view density,
// reverse). MAIN THREAD ONLY - CreateAdapterProxies probes the config and file
// table and recurses into ShapeBank::New, and OptimizeShapes must see the proxies
// (REN-TEMP-002 taught the hard way what binding proxies AFTER the optimisation
// does to skinning). A worker-side conversion (finishTail=false) stops before this
// point; the main-thread install runs it before OptimizeOneShape.
void FinishOdolAdapterTail(LODShapeWithShadow* shape, const Poseidon::Model::Model& model, bool reversed)
{
    if (model.sourceFormat != "ODOL")
        return; // the MLOD tail has no proxies and stays inside the conversion
    shape->InvalidateQueryPolicy();
    const int lodCount = static_cast<int>(model.lodLevels.size());
    (void)lodCount;
    // Proxies first: OptimizeShapes below reorders/compacts vertices and
    // selections, and a proxy created afterwards binds to the wrong skinning
    // data (2026-08-30, soldiers' rifles floating beside them).
    CreateAdapterProxies(shape, model);

    // Oxygen creates sections before marking proxy faces hidden, so section hints
    // lack IsHiddenProxy. Patch any section containing a proxy face selection.
    for (int li = 0; li < lodCount; li++)
    {
        Shape* lod = shape->_lods[li];
        if (!lod)
            continue;
        const auto& irSelections = model.lodLevels[li].mesh.selections;
        for (int si = 0; si < lod->NNamedSel(); si++)
        {
            const ::NamedSelection& sel = lod->NamedSel(si);
            if (strncmp(sel.Name(), "proxy:", 6) != 0)
                continue;

            // MAT-044: mark by the selection's OWN section list when the face list is
            // empty.
            //
            // The face route below matches `selectedFaces` against a rebuilt byte-offset
            // table (ODOLLoader), which only works while the table's stride matches the
            // file's. On the Arma 2 family it evidently does not, so `triangleIndices`
            // comes through empty and this patch used to give up -- leaving the section
            // with hints 0, so neither Shape::Draw nor RegisterGpuModel's
            // IsHidden|IsHiddenProxy skip could fire, and one untextured triangle per
            // proxy was drawn as ordinary geometry. Measured on Takistan: 40 of 513
            // models, 1,898 markers on visual LODs.
            //
            // ODOL named selections carry an explicit `sections` array (the IR's
            // `sectionIndices`) alongside the face list -- the pre-computed mapping the
            // format already provides. Using it needs no offset arithmetic at all, so it
            // is immune to the stride question rather than depending on its answer.
            bool marked = false;
            if (sel.Faces().Size() < 1 && si < static_cast<int>(irSelections.size()) &&
                irSelections[si].name == sel.Name())
            {
                for (uint32_t secIdx : irSelections[si].sectionIndices)
                {
                    if (secIdx >= static_cast<uint32_t>(lod->NSections()))
                        continue;
                    ShapeSection& sec = lod->GetSection(static_cast<int>(secIdx));
                    sec.properties.OrSpecial(IsHiddenProxy);
                    sec.properties.SetTexture(nullptr);
                    marked = true;
                }
            }
            if (marked)
                continue;

            if (sel.Faces().Size() < 1)
            {
                // MAT-044: this early-out is why Arma 2 houses bake their proxy-marker
                // section as untextured geometry. The section keeps hints 0, so neither
                // Shape::Draw nor RegisterGpuModel's IsHidden|IsHiddenProxy skip fires,
                // and one triangle per proxy is drawn with no texture and no RVMAT.
                // Measured: section 0's triangle count equals the proxy count exactly on
                // house_k_1 (14), house_k_3 (29), house_k_5 (17) and house_l_3 (12).
                // Whether the face list is empty because the A2 reader drops it or because
                // the source selects only vertices is the open question -- say which.
                if (const char* trace = std::getenv("WGR_MATERIAL_DEBUG_RVMAT");
                    trace && std::strcmp(trace, "__trace__") == 0)
                {
                    LOG_INFO(Graphics,
                             "ShapeAdapter proxy patch SKIPPED: model={} lod={} selection={} points={} "
                             "faces={} irSections={} sections={}",
                             model.sourcePath, li, sel.Name(), sel.Size(), sel.Faces().Size(),
                             si < static_cast<int>(irSelections.size())
                                 ? static_cast<int>(irSelections[si].sectionIndices.size())
                                 : -1,
                             lod->NSections());
                }
                continue;
            }
            int faceIdx = sel.Faces()[0];
            Offset faceOfs = lod->Faces().Find(faceIdx);
            for (int s = 0; s < lod->NSections(); s++)
            {
                ShapeSection& sec = lod->GetSection(s);
                if (faceOfs >= sec.beg && faceOfs < sec.end)
                {
                    sec.properties.OrSpecial(IsHiddenProxy);
                    sec.properties.SetTexture(nullptr);
                    break;
                }
            }
        }
    }

    // LODShape::SerializeBin IsLoading block
    shape->OptimizeShapes();
    // COL-001: OptimizeShapes ends in ScanShapes only when GApp exists, and
    // that scan is what turns the resolutions into _geometry / _landContact /
    // _roadway once the LOD list has been compacted. Run it explicitly so the
    // shape is the same object in a tool or a test as it is in the game.
    shape->ScanShapes();
    shape->CheckForcedProperties();
    // A file whose mass total is zero while its per-point table is not was
    // binarised without one; derive it the way the MLOD loader would.
    if (!(shape->_mass > 0.0f) && shape->_massArray.Size() > 0)
    {
        Shape* geom = shape->GeometryLevel();
        if (geom && geom->_pointToVertex.Size() <= shape->_massArray.Size())
            shape->CalculateMass();
    }
    if (SynthesizeBoxGeometryIfRequested(shape, model.sourcePath.c_str()))
    {
        Shape* geom = shape->GeometryLevel();
        if (geom)
            SynthesizeBoxGeometryMass(shape, geom, shape->_massArray, shape->_mass, shape->_invMass,
                                      geom->_pointToVertex, geom->_vertexToPoint);
    }
    shape->InitConvexComponents();
    shape->_propertyClass = shape->PropertyValue("class");
    shape->_propertyDammage = shape->PropertyValue("dammage");
    LogCollisionStats(shape, model);

    // shapeFile.cpp:588-598
    {
        float alpha = shape->_color.A8() * (1.0f / 255.0f);
        float transparency = 1.0f - alpha * 1.5f;
        if (transparency >= 0.99f)
            shape->_viewDensity = 0;
        if (transparency > 0.01f)
            shape->_viewDensity = std::log(transparency) * 4.0f;
        else
            shape->_viewDensity = -10;
    }

    if (reversed)
    {
        shape->Reverse();
        shape->SetRemarks(shape->Remarks() | REM_REVERSED);
    }
}

static bool ControlledMlodEligible(const Poseidon::Model::Model& model);

LODShapeWithShadow* ConvertWithContext(const Poseidon::Model::Model& model, bool reversed,
                                      const AdapterBankTables* tables, bool finishTail, bool controlledOffline)
{
    if (controlledOffline && (reversed || !finishTail || !tables ||
        !tables->textures.empty() || !tables->surfMats.empty() || !tables->mlodSurfMats.empty() ||
        !ControlledMlodEligible(model))) return nullptr;
    // Sub-bucket timers for the Phase-6 question "what inside adapt is main-thread-bound".
    // Accumulated across LODs, logged once per model when the total is worth reading.
    const auto adaptT0 = ::Poseidon::Dev::Perf::Now();
    double tanMs = 0.0, texMs = 0.0, matMs = 0.0, vfillMs = 0.0, faceMs = 0.0, areaMs = 0.0, tailMs = 0.0;
    auto shape = new LODShapeWithShadow();
    std::unique_ptr<LODShapeWithShadow> controlledGuard(controlledOffline ? shape : nullptr);

    // Must match the lowered name used in ShapeBank::New
    shape->_name = model.sourcePath.c_str();

    int lodCount = static_cast<int>(model.lodLevels.size());
    shape->_nLods = static_cast<signed char>(lodCount);

    for (int i = 0; i < lodCount; ++i)
    {
        const LODLevel& lodLevel = model.lodLevels[i];
        const Mesh& mesh = lodLevel.mesh;
        const int sourceVertexCount = static_cast<int>(mesh.vertices.size());
        // The runtime face stream stores vertex references as VertexIndex, and a
        // blind cast of anything past that type's range turns an index negative
        // before RecalculateAreas dereferences it. Keep the representable prefix
        // and reject only faces that cannot be expressed, which is preferable to
        // crashing an entire streamed world on first sight of one model.
        //
        // VertexIndex was `short`, so this ceiling was 32768 and imported
        // later-generation buildings sat well above it -- see Core/Types.hpp for
        // the measurement and what it looked like on screen. At 32 bits no model
        // in any supported corpus comes close, and the clamp is a guard rather
        // than a working limit.
        //
        // Computed in 64-bit on purpose: `numeric_limits<VertexIndex>::max() + 1`
        // is fine for a 16-bit type and OVERFLOWS for a 32-bit one, which would
        // make the count negative and reject every face in the world.
        constexpr int64_t maxRuntimeVertices = static_cast<int64_t>(std::numeric_limits<VertexIndex>::max()) + 1;
        const int vertexCount =
            static_cast<int>(std::min<int64_t>(static_cast<int64_t>(sourceVertexCount), maxRuntimeVertices));
        int triangleCount = static_cast<int>(mesh.triangles.size());
        int quadCount = static_cast<int>(mesh.quads.size());
        bool isODOL = (model.sourceFormat == "ODOL");
        std::vector<std::array<float, 3>> generatedBinormals;
        const auto tanT0 = ::Poseidon::Dev::Perf::Now();
        const std::vector<std::array<float, 3>> generatedTangents = buildTangents(mesh, generatedBinormals);
        tanMs += ::Poseidon::Dev::Perf::ElapsedMs(tanT0);
        bool hasGeneratedTangentFrame = false;
        for (size_t vertexIndex = 0; vertexIndex < generatedTangents.size(); ++vertexIndex)
        {
            const auto& tangent = generatedTangents[vertexIndex];
            const auto& binormal = generatedBinormals[vertexIndex];
            const float tangentLength2 = tangent[0] * tangent[0] + tangent[1] * tangent[1] + tangent[2] * tangent[2];
            const float binormalLength2 = binormal[0] * binormal[0] + binormal[1] * binormal[1] + binormal[2] * binormal[2];
            if (tangentLength2 > 1e-10f && binormalLength2 > 1e-10f)
            {
                hasGeneratedTangentFrame = true;
                break;
            }
        }

        shape->_resolutions[i] = lodLevel.resolution;
        shape->_lods[i] = new Shape();
        shape->_lods[i]->Init(vertexCount);

        if (isODOL && !mesh.edges.mlodIndices.empty())
        {
            int nPoints = static_cast<int>(mesh.edges.mlodIndices.size());
            int nVerts = static_cast<int>(mesh.edges.vertexIndices.size());
            shape->_lods[i]->_pointToVertex.Resize(nPoints);
            for (int v = 0; v < nPoints; ++v)
            {
                const uint32_t index = mesh.edges.mlodIndices[v];
                shape->_lods[i]->_pointToVertex[v] =
                    index < static_cast<uint32_t>(maxRuntimeVertices) ? static_cast<VertexIndex>(index) : -1;
            }
            shape->_lods[i]->_vertexToPoint.Resize(nVerts);
            for (int v = 0; v < nVerts; ++v)
            {
                const uint32_t index = mesh.edges.vertexIndices[v];
                shape->_lods[i]->_vertexToPoint[v] =
                    index < static_cast<uint32_t>(maxRuntimeVertices) ? static_cast<VertexIndex>(index) : -1;
            }
        }
        else
        {
            shape->_lods[i]->_pointToVertex.Resize(vertexCount);
            shape->_lods[i]->_vertexToPoint.Resize(vertexCount);
            for (int v = 0; v < vertexCount; ++v)
            {
                shape->_lods[i]->_pointToVertex[v] = static_cast<VertexIndex>(v);
                shape->_lods[i]->_vertexToPoint[v] = static_cast<VertexIndex>(v);
            }
        }

        // The second UV set, when the source carried one. Multi's mask / macro / ambient-shadow
        // stages address `tex1`; without this stream they land on the tiling UV and paint
        // rectangles across the wall (owner report, Takistan walls_l3.rvmat, 2026-08-16).
        bool anyUv1 = false;
        for (int v = 0; v < vertexCount && !anyUv1; ++v)
            anyUv1 = mesh.vertices[v].uv1.u != 0.0f || mesh.vertices[v].uv1.v != 0.0f;
        if (anyUv1)
            shape->_lods[i]->ResizeUV1(vertexCount);
        const auto vfillT0 = ::Poseidon::Dev::Perf::Now();
        for (int v = 0; v < vertexCount; ++v)
        {
            const Vertex& vertex = mesh.vertices[v];
            if (anyUv1)
                shape->_lods[i]->SetUV1(v, vertex.uv1.u, vertex.uv1.v);
            shape->_lods[i]->SetPos(v) = ::Vector3(vertex.position.x, vertex.position.y, vertex.position.z);
            shape->_lods[i]->SetNorm(v) = ::Vector3(vertex.normal.x, vertex.normal.y, vertex.normal.z);
            shape->_lods[i]->SetTangent(v) = ::Vector3(generatedTangents[v][0], generatedTangents[v][1], generatedTangents[v][2]);
            shape->_lods[i]->SetBinormal(v) = ::Vector3(generatedBinormals[v][0], generatedBinormals[v][1], generatedBinormals[v][2]);
            shape->_lods[i]->SetU(v, vertex.uv.u);
            shape->_lods[i]->SetV(v, vertex.uv.v);

            ClipFlags c = static_cast<ClipFlags>(vertex.flags);
            // MLOD: clear ClipBack for sky/disable fog vertices (matches Shape::Load lines 729-733)
            if (!isODOL)
            {
                ClipFlags fog = c & ClipFogMask;
                if (fog == ClipFogSky || fog == ClipFogDisable)
                    c = static_cast<ClipFlags>(c & ~ClipBack);
            }
            shape->_lods[i]->SetClip(v, c);
        }
        vfillMs += ::Poseidon::Dev::Perf::ElapsedMs(vfillT0);
        shape->_lods[i]->SetHasTangentFrame(hasGeneratedTangentFrame);

        if (isODOL && !mesh.frames.empty())
        {
            Shape* lod = shape->_lods[i];
            for (const auto& frame : mesh.frames)
            {
                if (frame.positions.empty())
                    continue;

                AnimationPhase phase;
                phase.Resize(vertexCount);

                const bool directVertexSpace = frame.positions.size() == static_cast<size_t>(vertexCount);
                for (int v = 0; v < vertexCount; ++v)
                {
                    int sourceIndex = v;
                    if (!directVertexSpace)
                    {
                        sourceIndex = (v < lod->_vertexToPoint.Size()) ? static_cast<int>(lod->_vertexToPoint[v]) : -1;
                    }

                    if (sourceIndex >= 0 && sourceIndex < static_cast<int>(frame.positions.size()))
                    {
                        const auto& pos = frame.positions[sourceIndex];
                        phase[v] = ::Vector3(pos.x, pos.y, pos.z);
                    }
                    else
                    {
                        phase[v] = lod->Pos(v);
                    }
                }

                phase.SetTime(frame.time);
                lod->AddPhase(phase);
            }
        }

        int nMaterials = static_cast<int>(mesh.materials.size());
        shape->_lods[i]->_textures.Realloc(nMaterials);
        shape->_lods[i]->_textures.Resize(nMaterials);
        shape->_lods[i]->_areaOTex.Resize(nMaterials);
        std::vector<Texture*> materialIndexToTexture(mesh.materials.size(), nullptr);

        const auto texT0 = ::Poseidon::Dev::Perf::Now();
        if (tables)
        {
            const auto& texRow = static_cast<size_t>(i) < tables->textures.size()
                                     ? tables->textures[i]
                                     : std::vector<Ref<Texture>>{};
            for (size_t matIdx = 0; matIdx < mesh.materials.size(); ++matIdx)
            {
                Texture* texture = matIdx < texRow.size() ? texRow[matIdx].GetRef() : nullptr;
                shape->_lods[i]->_textures[static_cast<int>(matIdx)] = texture;
                materialIndexToTexture[matIdx] = texture;
            }
        }
        else
        {
            for (size_t matIdx = 0; matIdx < mesh.materials.size(); ++matIdx)
            {
                const Material& mat = mesh.materials[matIdx];

                if (mat.texturePath.empty())
                {
                    shape->_lods[i]->_textures[static_cast<int>(matIdx)] = nullptr;
                }
                else
                {
                    std::string texPath = mat.texturePath;
                    for (char& c : texPath)
                    {
                        if (c >= 'A' && c <= 'Z')
                            c = c - 'A' + 'a';
                    }
                    Ref<Texture> texture = GlobLoadTexture(texPath.c_str());
                    shape->_lods[i]->_textures[static_cast<int>(matIdx)] = texture;
                    materialIndexToTexture[matIdx] = texture;
                }
            }
        }
        texMs += ::Poseidon::Dev::Perf::ElapsedMs(texT0);

        const auto faceT0 = ::Poseidon::Dev::Perf::Now();
        struct FaceRef
        {
            bool isQuad;
            uint32_t index;
            uint32_t originalIndex;
        };

        std::vector<FaceRef> faceRefs;
        faceRefs.reserve(triangleCount + quadCount);

        uint32_t rejectedFaces = 0;
        for (int f = 0; f < triangleCount; ++f)
        {
            const Triangle& triangle = mesh.triangles[f];
            if (triangle.indices[0] >= static_cast<uint32_t>(vertexCount) ||
                triangle.indices[1] >= static_cast<uint32_t>(vertexCount) ||
                triangle.indices[2] >= static_cast<uint32_t>(vertexCount))
            {
                ++rejectedFaces;
                continue;
            }
            faceRefs.push_back({false, static_cast<uint32_t>(f), triangle.originalIndex});
        }
        for (int f = 0; f < quadCount; ++f)
        {
            const Quad& quad = mesh.quads[f];
            if (quad.indices[0] >= static_cast<uint32_t>(vertexCount) ||
                quad.indices[1] >= static_cast<uint32_t>(vertexCount) ||
                quad.indices[2] >= static_cast<uint32_t>(vertexCount) ||
                quad.indices[3] >= static_cast<uint32_t>(vertexCount))
            {
                ++rejectedFaces;
                continue;
            }
            faceRefs.push_back({true, static_cast<uint32_t>(f), quad.originalIndex});
        }
        if (rejectedFaces != 0)
            LOG_WARN(Graphics,
                     "ShapeAdapter: {} LOD {} rejected {} faces outside the signed-16-bit runtime vertex range "
                     "(sourceVertices={} runtimeVertices={})",
                     model.sourcePath, i, rejectedFaces, sourceVertexCount, vertexCount);

        // Sort by originalIndex to restore file order. Loaders split triangles and quads
        // into separate arrays; originalIndex tracks position in the original face stream.
        std::sort(faceRefs.begin(), faceRefs.end(),
                  [](const FaceRef& a, const FaceRef& b) { return a.originalIndex < b.originalIndex; });

        // COL-001: source face index (originalIndex, the position in the file's
        // mixed triangle/quad stream) -> final face index in the runtime stream.
        // Both loaders now number selection faces in that unit, so one table
        // serves both; it also stays right when a face was rejected above, where
        // "position == index" silently would not.
        uint32_t maxOriginal = 0;
        for (const FaceRef& ref : faceRefs)
            maxOriginal = std::max(maxOriginal, ref.originalIndex);
        std::vector<uint32_t> originalToFace(faceRefs.empty() ? 0 : static_cast<size_t>(maxOriginal) + 1, 0xFFFFFFFF);
        for (size_t finalFaceIdx = 0; finalFaceIdx < faceRefs.size(); ++finalFaceIdx)
            originalToFace[faceRefs[finalFaceIdx].originalIndex] = static_cast<uint32_t>(finalFaceIdx);

        for (const auto& faceRef : faceRefs)
        {
            Poly poly;
            poly.Init();

            uint32_t materialIndex = 0;
            FaceFlags faceFlags = FaceFlags::None;

            if (faceRef.isQuad)
            {
                const Quad& quad = mesh.quads[faceRef.index];
                materialIndex = quad.materialIndex;
                faceFlags = quad.flags;
                poly.SetN(4);
                poly.Set(0, static_cast<VertexIndex>(quad.indices[0]));
                poly.Set(1, static_cast<VertexIndex>(quad.indices[1]));
                poly.Set(2, static_cast<VertexIndex>(quad.indices[2]));
                poly.Set(3, static_cast<VertexIndex>(quad.indices[3]));

                if (quad.materialIndex < materialIndexToTexture.size())
                {
                    poly.SetTexture(materialIndexToTexture[quad.materialIndex]);
                }
                else
                {
                    poly.SetTexture(nullptr);
                }

                poly.SetSpecial(isODOL ? static_cast<int>(quad.flags) : convertFaceFlagsToSpecial(quad.flags));
            }
            else
            {
                const Triangle& tri = mesh.triangles[faceRef.index];
                materialIndex = tri.materialIndex;
                faceFlags = tri.flags;
                poly.SetN(3);
                poly.Set(0, static_cast<VertexIndex>(tri.indices[0]));
                poly.Set(1, static_cast<VertexIndex>(tri.indices[1]));
                poly.Set(2, static_cast<VertexIndex>(tri.indices[2]));

                if (tri.materialIndex < materialIndexToTexture.size())
                {
                    poly.SetTexture(materialIndexToTexture[tri.materialIndex]);
                }
                else
                {
                    poly.SetTexture(nullptr);
                }

                poly.SetSpecial(isODOL ? static_cast<int>(tri.flags) : convertFaceFlagsToSpecial(tri.flags));
            }

            // MLOD: derive face specials from texture and vertex flags (Shape::Load lines 756-797)
            if (!isODOL)
            {
                Texture* tex = poly.GetTexture();
                if (tex)
                {
                    if (!tables)
                        tex->LoadHeaders(); // with tables the headers are pre-loaded
                    int spec = poly.Special();
                    if (tex->IsAlpha())
                        spec |= IsAlpha | IsAlphaOrdered;
                    if (tex->IsTransparent())
                        spec |= IsTransparent;
                    if (tex->IsAnimated())
                        spec |= ::IsAnimated;
                    const char* ext = strrchr(tex->Name(), '.');
                    if (ext && !stricmp(ext, ".paa") && !strstr(tex->Name(), "\\000"))
                        spec |= IsAlphaOrdered;
                    poly.SetSpecial(spec);
                }
                int nVerts = poly.N();
                for (int vi = 0; vi < nVerts; vi++)
                {
                    int vertIdx = poly.GetVertex(vi);
                    if (vertIdx >= 0 && vertIdx < vertexCount)
                    {
                        ClipFlags land = static_cast<ClipFlags>(mesh.vertices[vertIdx].flags) & ClipLandMask;
                        if (land == ClipLandOn)
                        {
                            poly.OrSpecial(OnSurface);
                            break;
                        }
                    }
                }
            }

            shape->_lods[i]->AddFace(poly);
        }

        for (const auto& selection : mesh.selections)
        {
            std::vector<SelInfo> selInfos;
            selInfos.reserve(selection.vertexIndices.size());
            bool hasWeights = !selection.vertexWeights.empty();
            for (size_t vi = 0; vi < selection.vertexIndices.size(); ++vi)
            {
                uint32_t vertIdx = selection.vertexIndices[vi];
                if (vertIdx >= static_cast<uint32_t>(vertexCount))
                    continue;
                uint8_t weight =
                    (hasWeights && vi < selection.vertexWeights.size()) ? selection.vertexWeights[vi] : 255;
                selInfos.push_back(SelInfo(static_cast<VertexIndex>(vertIdx), weight));
            }

            // Selection faces are source face indices on both paths (COL-001):
            // positions in the file's mixed tri/quad stream, remapped through
            // originalToFace to the runtime face stream.
            std::vector<VertexIndex> faceIndices;
            faceIndices.reserve(selection.triangleIndices.size());
            for (uint32_t sourceIdx : selection.triangleIndices)
            {
                if (sourceIdx < originalToFace.size() && originalToFace[sourceIdx] != 0xFFFFFFFF)
                    faceIndices.push_back(static_cast<VertexIndex>(originalToFace[sourceIdx]));
            }

            ::NamedSelection namedSel(
                selection.name.c_str(), selInfos.empty() ? nullptr : selInfos.data(), static_cast<int>(selInfos.size()),
                faceIndices.empty() ? nullptr : faceIndices.data(), static_cast<int>(faceIndices.size()));

            shape->_lods[i]->AddNamedSel(namedSel);
        }

        for (const auto& prop : lodLevel.mesh.properties)
        {
            shape->_lods[i]->_prop.Add(::NamedProperty(prop.name.c_str(), prop.value.c_str()));
        }

        shape->_lods[i]->Compact();

        if (!isODOL)
        {
            shape->_lods[i]->CalculateHints();
            // AutoClamp must run before FindSections (Shape::Load order: line 846 then 4788)
            shape->_lods[i]->AutoClamp();
            shape->_lods[i]->RecalculateNormals(true);
            shape->_lods[i]->CalculateMinMax();
            shape->_lods[i]->StoreOriginalMinMax();
        }

        // ODOL section offsets are x86 byte offsets (sizeof(PolyProperties)==8).
        // On x64 (sizeof==16) we derive face counts from those offsets then recompute
        // native offsets by walking the actual face stream.
        shape->_lods[i]->Faces().SetSections(nullptr, 0);
        if (!mesh.sections.empty())
        {
            int nSections = static_cast<int>(mesh.sections.size());
            int totalFaces = static_cast<int>(faceRefs.size());

            constexpr int kFilePolyPropsSize = 8; // sizeof(PolyProperties) on x86
            constexpr int kFileVertexIndexSize = 2; // ODOL stores indices as 16-bit

            std::vector<int> secEndFile(nSections);
            for (int s = 0; s < nSections; ++s)
                secEndFile[s] = static_cast<int>(mesh.sections[s].startTriangle + mesh.sections[s].triangleCount);

            std::vector<int> facesPerSection(nSections, 0);
            // Re-deriving face counts from byte offsets requires knowing the
            // stride of the stream those offsets came from, and this walk knows
            // only one: the v7 layout, 8 bytes of PolyProperties plus a 2-byte
            // index per vertex, so 16 bytes for a triangle. Revision 73's face
            // stream is 4 + 4 per vertex -- also 16 for a triangle -- so Arma 3
            // has been correct here by coincidence, and drifts on quads (20 vs
            // 18). The Arma 2 family stores 2 + 2, so a triangle is 8 bytes and
            // every section boundary lands at twice its true face, which drew
            // half of Takistan's tree crowns with the trunk's bark texture.
            //
            // Where the loader already worked the ranges out face by face, take
            // them. That removes the stride assumption instead of adding a
            // per-revision branch to it. `faceRangeKnown` exists for exactly
            // this and was never read.
            int exactTotal = 0;
            const bool exactRanges =
                std::all_of(mesh.sections.begin(), mesh.sections.end(),
                            [](const Poseidon::Model::Section& s) { return s.faceRangeKnown; });
            if (exactRanges)
                for (const auto& sec : mesh.sections)
                    exactTotal += static_cast<int>(sec.faceCount);
            // Only when the exact ranges account for the whole face stream. A
            // partial cover would silently drop the remainder, which the byte
            // walk never does -- it assigns every face to some section.
            if (exactRanges && exactTotal == totalFaces)
            {
                for (int s = 0; s < nSections; ++s)
                    facesPerSection[s] = static_cast<int>(mesh.sections[s].faceCount);
            }
            else
            {
                int fileBytePos = 0;
                int faceStreamPos = 0;
                int curSec = 0;
                for (int f = 0; f < totalFaces; ++f)
                {
                    const int sectionProgress = isODOL ? fileBytePos : faceStreamPos;
                    while (curSec < nSections - 1 && sectionProgress >= secEndFile[curSec])
                        curSec++;
                    facesPerSection[curSec]++;
                    int nVerts = faceRefs[f].isQuad ? 4 : 3;
                    // 2, not sizeof(VertexIndex): this walks the FILE's byte layout,
                    // where a vertex index is two bytes whatever the runtime type is.
                    fileBytePos += kFilePolyPropsSize + kFileVertexIndexSize * (1 + nVerts);
                    ++faceStreamPos;
                }
            }

            std::vector<ShapeSection> shapeSections(nSections);
            Offset nativePos(0);
            auto& faceStream = shape->_lods[i]->Faces();
            for (int s = 0; s < nSections; ++s)
            {
                const auto& sec = mesh.sections[s];
                shapeSections[s].beg = nativePos;
                shapeSections[s].material = sec.specialMaterial;
                Texture* tex = nullptr;
                if (sec.materialIndex != UINT32_MAX && sec.materialIndex < materialIndexToTexture.size())
                    tex = materialIndexToTexture[sec.materialIndex];
                shapeSections[s].properties.SetTexture(tex);
                shapeSections[s].properties.SetSpecial(static_cast<int>(sec.hints));
                // An Arma P3D section may name an RVMAT independently of its
                // base texture. Prefer that authored material over the legacy
                // CfgTextureToMaterial lookup, which cannot recover the RVMAT
                // once the common model IR has preserved it separately.
                const auto matT0 = ::Poseidon::Dev::Perf::Now();
                if (tables)
                {
                    if (static_cast<size_t>(i) < tables->surfMats.size() &&
                        static_cast<size_t>(s) < tables->surfMats[i].size())
                        shapeSections[s].surfMat = tables->surfMats[i][s];
                }
                else if (sec.materialIndex != UINT32_MAX && sec.materialIndex < mesh.materials.size() &&
                         !mesh.materials[sec.materialIndex].materialPath.empty())
                    shapeSections[s].surfMat = GTexMaterialBank.New(mesh.materials[sec.materialIndex].materialPath.c_str());
                else
                    shapeSections[s].surfMat = GTexMaterialBank.TextureToMaterial(tex);
                matMs += ::Poseidon::Dev::Perf::ElapsedMs(matT0);
                for (int f = 0; f < facesPerSection[s]; ++f)
                    faceStream.Next(nativePos);
                shapeSections[s].end = nativePos;
            }
            faceStream.SetSections(shapeSections.data(), nSections);
            faceMs += ::Poseidon::Dev::Perf::ElapsedMs(faceT0) - 0.0;
            // Pair the explicit WGPU material trace with its model/LOD origin.
            // Shape itself carries no source path at draw time, so this is the
            // only reliable way to distinguish a visible LOD from its similarly
            // numbered shadow LOD without changing runtime data structures.
            if (const char* trace = std::getenv("WGR_MATERIAL_DEBUG_RVMAT"); trace && std::strcmp(trace, "__trace__") == 0)
            {
                for (int s = 0; s < nSections; ++s)
                {
                    const auto& sec = mesh.sections[s];
                    const char* materialPath = sec.materialIndex != UINT32_MAX && sec.materialIndex < mesh.materials.size()
                                                   ? mesh.materials[sec.materialIndex].materialPath.c_str()
                                                   : "<unassigned>";
                    LOG_INFO(Graphics, "ShapeAdapter material trace: model={} lod={} shape={} section={} material={}",
                             model.sourcePath, i, static_cast<const void*>(shape->_lods[i]), s,
                             materialPath && *materialPath ? materialPath : "<none>");
                }
            }
        }
        else
        {
            shape->_lods[i]->FindSections();
            // RFG-040: an MLOD carries no section table, so the branch above never runs
            // and FindSections -- which groups faces by texture/special only -- leaves
            // every ShapeSection::surfMat null. That is the whole reason a natively
            // loaded Reforger `.xob` (converted through MLOD in memory) reports
            // mat '(none)' and can never reach a normal map: the renderer resolves
            // NMOMap out of the section's surface material.
            //
            // Recover it from the material the loader DID preserve. Sections are keyed
            // by texture here, so map texture -> authored material path and only use a
            // texture whose materials agree on one path; an ambiguous texture (two
            // RVMATs sharing one base texture) is left exactly as before rather than
            // guessed at. Materials with no path -- every classic OFP MLOD, whose faces
            // name a texture and nothing else -- produce an empty map and no change.
            std::unordered_map<const Texture*, std::string> textureToMaterialPath;
            std::unordered_set<const Texture*> ambiguousTextures;
            for (size_t matIdx = 0; matIdx < mesh.materials.size(); ++matIdx)
            {
                const std::string& matPath = mesh.materials[matIdx].materialPath;
                if (matPath.empty())
                    continue;
                const Texture* tex = matIdx < materialIndexToTexture.size() ? materialIndexToTexture[matIdx] : nullptr;
                if (!tex)
                    continue;
                auto [it, inserted] = textureToMaterialPath.emplace(tex, matPath);
                if (!inserted && it->second != matPath)
                    ambiguousTextures.insert(tex);
            }
            if (!textureToMaterialPath.empty())
            {
                Shape* lod = shape->_lods[i];
                for (int s = 0; s < lod->NSections(); ++s)
                {
                    ShapeSection& section = lod->GetSection(s);
                    if (!section.surfMat.IsNull())
                        continue;
                    const Texture* tex = section.properties.GetTexture();
                    if (!tex || ambiguousTextures.count(tex) != 0)
                        continue;
                    const auto found = textureToMaterialPath.find(tex);
                    if (found == textureToMaterialPath.end())
                        continue;
                    if (tables)
                    {
                        // Worker conversion must never fall back to the global bank:
                        // concurrent Insert corrupts its hash table and can hang shutdown.
                        if (static_cast<size_t>(i) < tables->mlodSurfMats.size())
                        {
                            const auto& resolved = tables->mlodSurfMats[i];
                            const auto material = resolved.find(found->second);
                            if (material != resolved.end())
                                section.surfMat = material->second;
                        }
                    }
                    else
                        section.surfMat = GTexMaterialBank.New(found->second.c_str());
                }
            }
        }

        if (isODOL)
        {
            // ODOL: use pre-computed section data from binary (FaceSelection::SerializeBin)
            for (size_t si = 0; si < mesh.selections.size() && si < static_cast<size_t>(shape->_lods[i]->NNamedSel());
                 si++)
            {
                const auto& modelSel = mesh.selections[si];
                ::NamedSelection& sel = shape->_lods[i]->NamedSel(static_cast<int>(si));
                sel.SetNeedsSections(modelSel.needsSections);
                if (!modelSel.sectionIndices.empty())
                {
                    std::vector<int> sections(modelSel.sectionIndices.begin(), modelSel.sectionIndices.end());
                    sel.SetSections(sections.data(), static_cast<int>(sections.size()));
                }
            }
        }
        else
        {
            for (int si = 0; si < shape->_lods[i]->NNamedSel(); si++)
            {
                ::NamedSelection& sel = shape->_lods[i]->NamedSel(si);
                if (sel.Faces().Size() > 0)
                {
                    sel.FaceOffsets(shape->_lods[i]);
                    sel.RescanSections(shape->_lods[i]);
                }
            }
        }

        if (isODOL)
        {
            shape->_lods[i]->_special = static_cast<int>(lodLevel.mesh.special);
            shape->_lods[i]->_colorTop = PackedColor(mesh.iconColor);
            shape->_lods[i]->_color = PackedColor(mesh.selectedColor);
            shape->_lods[i]->SetHints(static_cast<ClipFlags>(lodLevel.mesh.orHints),
                                      static_cast<ClipFlags>(lodLevel.mesh.andHints));
        }
        else
        {
            shape->_lods[i]->CalculateColor();
            // Compute _special from section textures and face flags (Shape::ScanShapes lines 763-826)
            {
                Shape* lod = shape->_lods[i];
                int orSpec = 0, andSpec = -1;
                for (int s = 0; s < lod->NSections(); s++)
                {
                    int spec = lod->GetSection(s).properties.Special();
                    Texture* tex = lod->GetSection(s).properties.GetTexture();
                    if (tex)
                    {
                        if (!tables)
                            tex->LoadHeaders(); // with tables the headers are pre-loaded
                        if (tex->IsAlpha())
                            spec |= IsAlpha | IsAlphaOrdered;
                        if (tex->IsTransparent())
                            spec |= IsTransparent;
                        if (tex->IsAnimated())
                            spec |= ::IsAnimated;
                        const char* ext = strrchr(tex->Name(), '.');
                        if (ext && !strcmpi(ext, ".paa"))
                            if (!strstr(tex->Name(), "\\000"))
                                spec |= IsAlphaOrdered;
                    }
                    orSpec |= spec;
                    andSpec &= spec;
                }
                for (int v = 0; v < lod->NVertex(); v++)
                {
                    if ((lod->Clip(v) & ClipLandMask) == ClipLandOn)
                    {
                        orSpec |= OnSurface;
                        break;
                    }
                }
                lod->_special = orSpec & (IsAlpha | IsTransparent | ::IsAnimated | OnSurface);
                lod->_special |= andSpec & (NoShadow | ZBiasMask);
            }
        }
        shape->_lods[i]->_faceNormalsValid = true;
        const auto areaT0 = ::Poseidon::Dev::Perf::Now();
        shape->_lods[i]->RecalculateAreas();
        shape->_lods[i]->StoreOriginalMinMax();
        areaMs += ::Poseidon::Dev::Perf::ElapsedMs(areaT0);
    }
    const auto tailT0 = ::Poseidon::Dev::Perf::Now();

    if (controlledOffline)
    {
        // No GApp/ENGINE_CONFIG quality pruning or live Pars/name-derived class,
        // map/geometry policy. The strict wrapper has no helpers or proxies to
        // finalize. autocenter=0 was checked, not silently authored here.
        shape->ScanShapes();
        shape->_autoCenter = false;
        shape->CalculateMinMax(true);
        shape->CalculateHints();
    }
    else if (model.sourceFormat == "ODOL")
    {
        for (int i = 0; i < lodCount; ++i)
        {
            if (shape->_lods[i])
            {
                const Mesh& m = model.lodLevels[i].mesh;
                shape->_lods[i]->_minMax[0] = ::Vector3(m.boundingBox.min.x, m.boundingBox.min.y, m.boundingBox.min.z);
                shape->_lods[i]->_minMax[1] = ::Vector3(m.boundingBox.max.x, m.boundingBox.max.y, m.boundingBox.max.z);
                shape->_lods[i]->_bCenter = ::Vector3(m.bCenter.x, m.bCenter.y, m.bCenter.z);
                shape->_lods[i]->_bRadius = m.bRadius;
                shape->_lods[i]->_minMaxDirty = false;
                shape->_lods[i]->StoreOriginalMinMax();
            }
        }

        // LODShape::SerializeBin fields
        shape->_special = static_cast<int>(model.special);
        shape->_boundingSphere = model.boundingSphere.radius;
        shape->_geometrySphere = model.geometrySphere.radius;
        shape->_minMax[0] = ::Vector3(model.boundingBox.min.x, model.boundingBox.min.y, model.boundingBox.min.z);
        shape->_minMax[1] = ::Vector3(model.boundingBox.max.x, model.boundingBox.max.y, model.boundingBox.max.z);
        shape->_boundingCenter = ::Vector3(model.boundingCenter.x, model.boundingCenter.y, model.boundingCenter.z);
        shape->_geometryCenter = ::Vector3(model.geometryCenter.x, model.geometryCenter.y, model.geometryCenter.z);
        shape->_aimingCenter = ::Vector3(model.aimingCenter.x, model.aimingCenter.y, model.aimingCenter.z);
        shape->_centerOfMass = ::Vector3(model.centerOfMass.x, model.centerOfMass.y, model.centerOfMass.z);
        shape->_color = PackedColor(model.color);
        shape->_colorTop = PackedColor(model.colorTop);
        shape->_viewDensity = model.viewDensity;
        shape->_autoCenter = (model.autoCenterEnabled != 0);
        shape->_lockAutoCenter = (model.lockAutoCenter != 0);
        shape->_canOcclude = (model.canOcclude != 0);
        shape->_canBeOccluded = (model.canBeOccluded != 0);
        shape->_allowAnimation = (model.allowAnimation != 0);
        shape->SetRemarks(model.remarksFlags);
        shape->_andHints = static_cast<int>(model.andHints);
        shape->_orHints = static_cast<int>(model.orHints);
        // WLD/VEG-SWAY -- resolve the map type from the model's OWN `map` named property, the
        // same string LODShape::Load resolves, through the one shared table
        // (ResolveMapTypeProperty, Shape.hpp). `model.mapType` is NOT usable here: it is written
        // only by the ODOL-7 prologue reader (P3DStructures.hpp readModel); convertOdolStatic --
        // the converter behind EVERY modern revision, 40 / 48 / 49 / 50 / 52 / 54 / 73 -- never
        // assigns it, so it arrived as its default 0, and MapType's zeroth enumerator is MapTree.
        // Every Arma 1, Arma 2, Arma 3, DayZ and Reforger model was therefore loading as a TREE.
        // That is the gate for wind sway and for canopy leaf normals, which is why building walls
        // bent in the wind: the sway amplitude scales with a vertex's model-space height, so a
        // wall's base stays planted while its top swings, exactly like grass.
        //
        // The assignment itself sits further down, immediately after the LOD indices, because
        // LODShape::PropertyValue consults the GEOMETRY LOD before LOD 0 and `_geometry` is not
        // set until then. Nothing between here and there reads _mapType.
        //
        // WGR_ODOL_MAPTYPE=0 restores the previous behaviour (the unset byte, i.e. MapTree for
        // everything) so the two can be captured back to back from one binary.
        memcpy(&shape->_invInertia, model.invInertia, sizeof(model.invInertia));

        // LODShape::SerializeBin lines 541-558
        if (!model.massArray.empty())
        {
            shape->_massArray.Resize(static_cast<int>(model.massArray.size()));
            for (int m = 0; m < static_cast<int>(model.massArray.size()); m++)
                shape->_massArray[m] = model.massArray[m];
        }
        shape->_mass = model.mass;
        shape->_invMass = model.invMass;
        shape->_armor = model.armor;
        shape->_invArmor = model.invArmor;
        if (model.armor > 1e-10f)
            shape->_logArmor = std::log(model.armor);
        else
            shape->_logArmor = 25.0f;

        // shapeFile.cpp:560-571
        shape->_memory = model.memoryIdx;
        shape->_geometry = model.geometryIdx;
        shape->_geometryFire = model.geometryFireIdx;
        shape->_geometryView = model.geometryViewIdx;
        shape->_geometryViewPilot = model.geometryViewPilotIdx;
        shape->_geometryViewGunner = model.geometryViewGunnerIdx;
        shape->_geometryViewCommander = model.geometryViewCommanderIdx;
        shape->_geometryViewCargo = model.geometryViewCargoIdx;

        {
            static const bool legacy = []
            {
                const char* v = std::getenv("WGR_ODOL_MAPTYPE");
                return v && strcmp(v, "0") == 0;
            }();
            if (legacy)
            {
                shape->_mapType = static_cast<MapType>(model.mapType);
            }
            else
            {
                shape->_mapType = ResolveMapTypeProperty(shape->PropertyValue("map"),
                                                         model.sourcePath.c_str());
            }
        }
        shape->_landContact = model.landContactIdx;
        shape->_roadway = model.roadwayIdx;
        shape->_paths = model.pathsIdx;
        shape->_hitpoints = model.hitpointsIdx;

        // The tail (proxies + optimize + scan + mass + convex) runs here on the
        // normal inline path; a worker-side conversion (finishTail=false) leaves it
        // for the main-thread install (FinishOdolAdapterTail), preserving the
        // proxies-before-OptimizeShapes order either way.
        if (finishTail)
            FinishOdolAdapterTail(shape, model, reversed);
    }
    else
    {
        // The `autocenter` property, read the way LODShape::Load reads it -- BEFORE
        // the first CalculateMinMax, which is where CalculateBoundingSphere moves
        // every vertex to the bounding-box centre when _autoCenter is set. This
        // branch never cleared it, so `autocenter=0` was honoured by the IR-level
        // recentring in Model.cpp and then undone here: the vertices were shifted
        // anyway and the offset landed in _boundingCenter, which the
        // authored-elevation placement path (Landscape::ObjectCreate with
        // preserveAuthoredElevation) never reads. That is one half of "buildings
        // partly in the ground" on Reforger conversions; the xob converter's
        // --fix-origin writes the property, and this is what makes it stick.
        // ScanShapes first so PropertyValue can consult the geometry LOD, exactly
        // as Load's AddShape has already done by the time it asks.
        shape->ScanShapes();
        {
            const RStringB& autoCenter = shape->PropertyValue("autocenter");
            if (autoCenter.GetLength() > 0 && atoi(autoCenter) == 0)
                shape->_autoCenter = false;
        }
        shape->CalculateMinMax(true);
        shape->OptimizeShapes();
        // COL-001: see the ODOL branch. Without GApp OptimizeShapes never scans,
        // and without the scan the geometry LOD is not the geometry LOD.
        shape->ScanShapes();
        // MLOD twin of the ODOL branch's map-type resolution: honours the `map`
        // property the xob converter writes, and the vegetation-path fallback for
        // models that carry none. Left unassigned, _mapType kept whatever DoClear
        // left it (now MapHide; before that fresh memory, i.e. MapTree), and every
        // converted Reforger rock swayed in the wind. After ScanShapes so
        // PropertyValue's geometry-LOD lookup sees a valid _geometry.
        shape->_mapType = ResolveMapTypeProperty(shape->PropertyValue("map"), model.sourcePath.c_str());

        // Shape.cpp:4961-4971
        for (int i = 0; i < lodCount; i++)
            if (shape->_resolutions[i] < 900 && shape->_lods[i])
                shape->_special |= shape->_lods[i]->Special();

        shape->CheckForcedProperties();
        // COL-001: the geometry LOD's `#Mass#` tagg, carried per vertex by the
        // MLOD loader. LODShape::Load does exactly this for SP3X through the
        // legacy path; the canonical adapter never did, so every P3DM model
        // (Arma samples, Reforger conversions) weighed nothing and was passable.
        {
            const int geomLevel = shape->FindGeometryLevel();
            if (geomLevel >= 0 && geomLevel < shape->NLevels() && shape->_lods[geomLevel])
            {
                Shape* geom = shape->_lods[geomLevel];
                // OptimizeShapes may have compacted the LOD list; find the source
                // LOD by resolution rather than by index.
                const LODLevel* source = nullptr;
                for (const LODLevel& candidate : model.lodLevels)
                    if (Poseidon::Model::ClassifyLodResolution(candidate.resolution) ==
                        Poseidon::Model::LodPurpose::Geometry)
                    {
                        source = &candidate;
                        break;
                    }
                if (source && !source->mesh.vertexMass.empty())
                {
                    const int points = geom->_pointToVertex.Size();
                    shape->_massArray.Resize(points);
                    for (int p = 0; p < points; ++p)
                    {
                        const int vertex = geom->_pointToVertex[p];
                        shape->_massArray[p] =
                            vertex >= 0 && static_cast<size_t>(vertex) < source->mesh.vertexMass.size()
                                ? source->mesh.vertexMass[static_cast<size_t>(vertex)]
                                : 0.0f;
                    }
                    shape->CalculateMass();
                }
            }
        }
        if (SynthesizeBoxGeometryIfRequested(shape, model.sourcePath.c_str()))
        {
            Shape* geom = shape->GeometryLevel();
            if (geom)
                SynthesizeBoxGeometryMass(shape, geom, shape->_massArray, shape->_mass, shape->_invMass,
                                          geom->_pointToVertex, geom->_vertexToPoint);
        }
        shape->ScanProxies();
        // ScanProxies marks the MLOD proxy faces hidden after FindSections has
        // already cached each section's render flags. Hide a section only when
        // every face in it belongs to a proxy selection; otherwise hiding the
        // section would also erase ordinary geometry. HorseTest's two isolated
        // untextured proxy triangles otherwise remain visible as white sails.
        for (int li = 0; li < shape->NLevels(); ++li)
        {
            Shape* lod = shape->Level(li);
            if (!lod) continue;
            std::unordered_set<int> proxyFaces;
            for (int si = 0; si < lod->NNamedSel(); ++si)
            {
                const ::NamedSelection& sel = lod->NamedSel(si);
                if (strncmp(sel.Name(), "proxy:", 6) != 0) continue;
                for (int fi = 0; fi < sel.Faces().Size(); ++fi)
                    proxyFaces.insert(static_cast<int>(lod->Faces().Find(sel.Faces()[fi])));
            }
            if (proxyFaces.empty()) continue;
            for (int si = 0; si < lod->NSections(); ++si)
            {
                ShapeSection& sec = lod->GetSection(si);
                if (sec.beg >= sec.end) continue;
                bool allProxy = true;
                for (Offset face = sec.beg; face < sec.end; lod->NextFace(face))
                    if (proxyFaces.count(static_cast<int>(face)) == 0)
                    {
                        allProxy = false;
                        break;
                    }
                if (allProxy)
                {
                    sec.properties.OrSpecial(IsHiddenProxy);
                    sec.properties.SetTexture(nullptr);
                }
            }
        }
        // The canonical MLOD path must honour the same reversed contract as the
        // ODOL tail. Do this after proxies exist but before convex geometry is
        // derived, so both the visible model and its simulation face the same way.
        if (reversed)
        {
            shape->Reverse();
            shape->SetRemarks(shape->Remarks() | REM_REVERSED);
        }
        shape->InitConvexComponents();
        shape->ScanProperties();
        shape->CalculateHints();
        shape->_propertyClass = shape->PropertyValue("class");
        shape->_propertyDammage = shape->PropertyValue("dammage");
        LogCollisionStats(shape, model);
    }

    // One row per model that cost real time; `other` is geometry/face conversion — the
    // worker-safe part. tex/mat/proxy are the main-thread-affine banks.
    tailMs = ::Poseidon::Dev::Perf::ElapsedMs(tailT0);
    const double adaptTotal = ::Poseidon::Dev::Perf::ElapsedMs(adaptT0);
    if (adaptTotal >= 5.0)
    {
        LOG_INFO(Graphics,
                 "ShapeAdapter split: {} total={:.1f} [tan={:.1f} vfill={:.1f} face={:.1f} tex={:.1f} mat={:.1f} area={:.1f} tail={:.1f} other={:.1f}]",
                 model.sourcePath.c_str(), adaptTotal, tanMs, vfillMs, faceMs, texMs, matMs, areaMs, tailMs,
                 adaptTotal - tanMs - vfillMs - faceMs - texMs - matMs - areaMs - tailMs);
    }
    if (controlledOffline) controlledGuard.release();
    return shape;
}

LODShapeWithShadow* convertToLODShape(const Poseidon::Model::Model& model, bool reversed,
                                      const AdapterBankTables* tables, bool finishTail)
{
    return ConvertWithContext(model,reversed,tables,finishTail,false);
}

static bool ControlledMlodEligible(const Poseidon::Model::Model& model)
{
    if (!Foundation::IsMainThread() || model.sourceFormat != "MLOD" || model.sourceVersion != 11 ||
        model.lodLevels.size() != 2 || model.sourcePath.size() > 127 || model.mapType ||
        model.canOcclude || model.canBeOccluded || model.allowAnimation || !model.metadata.empty() ||
        !model.massArray.empty() || model.mass != 0 || model.special != 0 ||
        model.memoryIdx >= 0 || model.geometryIdx >= 0 || model.geometryFireIdx >= 0 ||
        model.geometryViewIdx >= 0 || model.landContactIdx >= 0 || model.roadwayIdx >= 0 ||
        model.pathsIdx >= 0 || model.hitpointsIdx >= 0 || model.geometryViewPilotIdx >= 0 ||
        model.geometryViewGunnerIdx >= 0 || model.geometryViewCommanderIdx >= 0 || model.geometryViewCargoIdx >= 0 ||
        model.memoryLODIndex >= 0 || model.geometryLODIndex >= 0 || model.fireGeometryLODIndex >= 0 ||
        model.viewGeometryLODIndex >= 0 || model.viewPilotLODIndex >= 0 || model.viewGunnerLODIndex >= 0 ||
        model.viewCommanderLODIndex >= 0 || model.viewCargoLODIndex >= 0 || model.landContactLODIndex >= 0 ||
        model.roadwayLODIndex >= 0 || model.pathsLODIndex >= 0 || model.hitpointsLODIndex >= 0)
        return false;
    for (unsigned char c : model.sourcePath) if (!c || c > 127) return false;
    float previous = -1;
    for (const auto& level : model.lodLevels)
    {
        const Mesh& mesh = level.mesh;
        if (level.sourceEncoding != "P3DM" || !std::isfinite(level.resolution) ||
            level.resolution < 0 || level.resolution >= 900 || level.resolution == previous ||
            level.purpose != Poseidon::Model::LodPurpose::Visual || !level.uvChannels.empty() || level.uvSetCount ||
            mesh.vertices.empty() || mesh.vertices.size() > 4096 || mesh.triangles.size() > 8192 ||
            mesh.quads.size() > 8192 || mesh.triangles.size()*3+mesh.quads.size()*4 > 4096 ||
            mesh.triangles.size()+mesh.quads.size() == 0 ||
            !mesh.selections.empty() || !mesh.proxies.empty() || !mesh.frames.empty() ||
            !mesh.vertexMass.empty() || !mesh.edges.mlodIndices.empty() || !mesh.edges.vertexIndices.empty() ||
            mesh.materials.size() != 1 || mesh.sections.size() != 1 || mesh.properties.size() != 1 ||
            mesh.properties[0].name != "autocenter" || mesh.properties[0].value != "0" ||
            static_cast<uint32_t>(mesh.special) != 0)
            return false;
        previous = level.resolution;
        const auto& material = mesh.materials[0];
        if (material.name != "#default#" || !material.texturePath.empty() || !material.materialPath.empty() ||
            !material.embeddedStages.empty() || static_cast<uint32_t>(material.flags) != 0 ||
            material.metallic != 0 || material.roughness != 1 || material.emissive != 0)
            return false;
        const auto& section = mesh.sections[0];
        if (section.materialIndex != 0 || section.startTriangle != 0 ||
            section.triangleCount != mesh.triangles.size()+mesh.quads.size() ||
            static_cast<uint32_t>(section.hints) != 0 || section.specialMaterial != 0 || section.faceRangeKnown ||
            section.firstFace || section.faceCount)
            return false;
        for (const auto& vertex : mesh.vertices)
        {
            if (static_cast<uint32_t>(vertex.flags) != static_cast<uint32_t>(ClipAll) || vertex.hasTangentFrame || vertex.uv1.u != 0 || vertex.uv1.v != 0)
                return false;
            const float normalLength2 = vertex.normal.x*vertex.normal.x+vertex.normal.y*vertex.normal.y+vertex.normal.z*vertex.normal.z;
            if (!std::isfinite(normalLength2) || normalLength2 < 1e-12f || normalLength2 > 1e8f) return false;
            for (float value : {vertex.position.x,vertex.position.y,vertex.position.z,vertex.normal.x,
                vertex.normal.y,vertex.normal.z,vertex.uv.u,vertex.uv.v,vertex.uv1.u,vertex.uv1.v})
                if (!std::isfinite(value) || std::abs(value) > 10000) return false;
        }
        std::array<bool,4096> seenSourceFaces{};
        const auto validFace = [&mesh,&seenSourceFaces](const auto& face) {
            const size_t count = mesh.triangles.size()+mesh.quads.size();
            if (face.originalIndex >= count || seenSourceFaces[face.originalIndex]) return false;
            seenSourceFaces[face.originalIndex] = true;
            if (face.materialIndex || static_cast<uint32_t>(face.flags)) return false;
            for (uint32_t index : face.indices) if (index >= mesh.vertices.size()) return false;
            return true;
        };
        for (const auto& face : mesh.triangles) if (!validFace(face)) return false;
        for (const auto& face : mesh.quads) if (!validFace(face)) return false;
    }
    return true;
}

LODShapeWithShadow* ConvertControlledMlod(const Poseidon::Model::Model& model)
{
    const AdapterBankTables emptyBanks;
    return ConvertWithContext(model,false,&emptyBanks,true,true);
}

} // namespace ShapeAdapter
} // namespace Model
} // namespace Poseidon
