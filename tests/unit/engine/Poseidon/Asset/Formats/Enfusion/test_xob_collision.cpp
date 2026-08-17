// test_xob_collision.cpp -- COL-001 for Reforger conversions.
//
// "The player walks through every Reforger building." A converted `.xob` carried
// visual LODs only: no Geometry LOD, no ComponentXX selections, no #Mass#, so the
// engine had nothing to collide with and Object::IsPassable() was true besides.
//
// The `.xob` COLL chunk carries the authored physics shapes (XobCollision.hpp), and
// XobCollisionGeometry.hpp turns the character-blocking ones into the Geometry LOD
// the engine wants. These tests build the bytes by hand from the record layout --
// nothing Reforger-owned is committed -- and drive the result through the writer,
// the reader and ShapeAdapter, so the assertion at the end is the engine's own:
// LODShape::FindGeometryLevel() >= 0, GetGeomComponents().Size() >= 1, Mass() >= 10,
// and ConvexComponent::IsInside true in a wall and false in the doorway.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <Poseidon/Asset/Formats/Enfusion/XobCollision.hpp>
#include <Poseidon/Asset/Formats/Enfusion/XobCollisionGeometry.hpp>
#include <Poseidon/Asset/Formats/Enfusion/XobModel.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODLoader.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODWriter.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/World/Model/LodPurpose.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using Catch::Approx;
using namespace Poseidon::Asset::Formats::Enfusion;
namespace MLODW = Poseidon::Asset::Formats::MLOD;
using Poseidon::Asset::Formats::MLODLoader;
using Poseidon::Asset::Formats::MLODWriter;

// Shape.hpp pulls in Math3D.hpp, which #defines Vector3; the engine's vector is
// spelt through the macro on purpose, and the format's structs are never named by
// their Vector3 member type below (brace-init only).
using EngineVector3 = ::Vector3;
using ConvexComponent = Poseidon::ConvexComponent;
using ConvexComponents = Poseidon::ConvexComponents;

namespace
{

// ------------------------------------------------------------- byte builders

void AppendBe32(std::vector<uint8_t>& out, uint32_t value)
{
    for (int i = 3; i >= 0; --i)
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
}
void AppendLe32(std::vector<uint8_t>& out, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
}
void AppendLe16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}
void AppendF32(std::vector<uint8_t>& out, float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    AppendLe32(out, bits);
}
void AppendVec3(std::vector<uint8_t>& out, float x, float y, float z)
{
    AppendF32(out, x);
    AppendF32(out, y);
    AppendF32(out, z);
}
void AppendTag(std::vector<uint8_t>& out, const char* tag)
{
    out.insert(out.end(), tag, tag + 4);
}
void AppendChunk(std::vector<uint8_t>& out, const char* tag, const std::vector<uint8_t>& payload)
{
    AppendTag(out, tag);
    AppendBe32(out, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
}

// The HEAD string table every record's ordinals point into.
const std::vector<std::string> kStrings = {
    "Building",                                             // 0
    "Foliage",                                              // 1
    "FireView",                                             // 2
    "UBX_post",                                             // 3
    "{DEADBEEFCAFEF00D}Common/Materials/Game/wood.gamemat", // 4
    "UCS_trunk",                                            // 5
    "UCL_leaves",                                           // 6
    "UTM_walls",                                            // 7
    "UCX_stone",                                            // 8
    "USP_ball",                                             // 9
    "Tree",                                                 // 10
    "UTM_fire",                                             // 11
    "UTM_grouped",                                          // 12
    "UTM_rock",                                             // 13
};
enum Ordinal : uint16_t
{
    kBuilding = 0,
    kFoliage = 1,
    kFireView = 2,
    kUbxPost = 3,
    kWood = 4,
    kUcsTrunk = 5,
    kUclLeaves = 6,
    kUtmWalls = 7,
    kUcxStone = 8,
    kUspBall = 9,
    kTree = 10,
    kUtmFire = 11,
    kUtmGrouped = 12,
    kUtmRock = 13,
};

//! A HEAD with no materials, bones, LODs or points: ReadXobHeader stops right after
//! the string table on such a file and reports it closed, which is the format's own
//! allowance (two corpus files do exactly this).
std::vector<uint8_t> MinimalHead()
{
    std::vector<uint8_t> head;
    AppendLe32(head, 0);                     // version
    AppendVec3(head, -10.0f, -1.0f, -10.0f); // bboxMin
    AppendVec3(head, 10.0f, 12.0f, 10.0f);   // bboxMax
    AppendVec3(head, 0.0f, 0.0f, 0.0f);      // sphere centre
    AppendF32(head, 15.0f);                  // sphere radius
    AppendLe16(head, 0);                     // materials
    AppendLe16(head, 0);                     // bones
    head.push_back(0);                       // LODs
    head.push_back(0);                       // points
    AppendLe16(head, 0);                     // pad
    AppendLe32(head, 0);                     // quads
    std::vector<uint8_t> table;
    for (const std::string& s : kStrings)
    {
        table.insert(table.end(), s.begin(), s.end());
        table.push_back(0);
    }
    AppendLe32(head, static_cast<uint32_t>(table.size()));
    head.insert(head.end(), table.begin(), table.end());
    return head;
}

const float kIdentity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

//! The 64-byte record head shared by every COLL type.
void AppendRecordHead(std::vector<uint8_t>& out, uint8_t type, uint16_t layer, uint16_t name, uint16_t material,
                      const float rotation[9], float x, float y, float z)
{
    out.push_back(type);
    out.push_back(0xFF);
    AppendLe16(out, layer);
    for (int i = 0; i < 9; ++i)
        AppendF32(out, rotation[i]);
    AppendVec3(out, x, y, z);
    AppendF32(out, 0.0f); // the unexplained zero after position
    AppendLe16(out, name);
    AppendLe16(out, material);
    AppendLe32(out, 0);
}

void AppendBox(std::vector<uint8_t>& out, uint16_t layer, uint16_t name, const float rotation[9], float x, float y,
               float z, float hx, float hy, float hz)
{
    AppendRecordHead(out, 3, layer, name, kWood, rotation, x, y, z);
    AppendVec3(out, hx, hy, hz);
}
void AppendRound(std::vector<uint8_t>& out, uint8_t type, uint16_t layer, uint16_t name, float x, float y, float z,
                 float radius, float halfHeight)
{
    AppendRecordHead(out, type, layer, name, kWood, kIdentity, x, y, z);
    AppendF32(out, radius);
    if (type != 1)
        AppendF32(out, halfHeight);
}
struct Tri
{
    uint16_t a, b, c;
};
void AppendMesh(std::vector<uint8_t>& out, uint16_t layer, uint16_t name, const std::vector<float>& xyz,
                const std::vector<Tri>& tris, bool grouped = false)
{
    AppendRecordHead(out, grouped ? 6 : 5, layer, name, kWood, kIdentity, 0.0f, 0.0f, 0.0f);
    AppendLe16(out, static_cast<uint16_t>(xyz.size() / 3));
    AppendLe16(out, static_cast<uint16_t>(tris.size()));
    if (grouped)
    {
        AppendLe32(out, 1);
        AppendLe16(out, kWood);
        AppendLe16(out, static_cast<uint16_t>(tris.size() - 1));
    }
    for (float f : xyz)
        AppendF32(out, f);
    for (const Tri& t : tris)
    {
        AppendLe16(out, t.a);
        AppendLe16(out, t.b);
        AppendLe16(out, t.c);
    }
}
//! A tetrahedron as the format's hull: 4 verts, 6 edges (v0, v1, f0, f1), and
//! 4 faces that are runs of EDGE indices -- e0 e1 e2 is the base (0,1,2), e0 e4 e3
//! is (0,1,3), and so on. The reader chains each run back into a vertex ring.
void AppendTetraHull(std::vector<uint8_t>& out, uint16_t layer, uint16_t name, float x, float y, float z)
{
    AppendRecordHead(out, 4, layer, name, kWood, kIdentity, x, y, z);
    AppendLe16(out, 4);  // verts
    AppendLe16(out, 4);  // faces
    AppendLe16(out, 6);  // edges
    AppendLe16(out, 12); // edge indices, two per edge
    AppendVec3(out, 0.0f, 0.0f, 0.0f);
    AppendVec3(out, 1.0f, 0.0f, 0.0f);
    AppendVec3(out, 0.0f, 0.0f, 1.0f);
    AppendVec3(out, 0.0f, 1.0f, 0.0f);
    const uint16_t edges[6][4] = {{0, 1, 0, 1}, {1, 2, 0, 2}, {2, 0, 0, 3}, {0, 3, 1, 3}, {1, 3, 1, 2}, {2, 3, 2, 3}};
    for (const auto& e : edges)
        for (uint16_t v : e)
            AppendLe16(out, v);
    const uint16_t edgeRuns[12] = {0, 1, 2, 0, 4, 3, 1, 5, 4, 2, 3, 5};
    for (uint16_t i : edgeRuns)
        AppendLe16(out, i);
    for (uint16_t f = 0; f < 4; ++f)
    {
        AppendLe16(out, static_cast<uint16_t>(3 * f));
        AppendLe16(out, 3);
    }
}

std::vector<uint8_t> XobWith(const std::vector<uint8_t>& coll, bool withColl = true)
{
    std::vector<uint8_t> chunks;
    AppendChunk(chunks, "HEAD", MinimalHead());
    if (withColl)
        AppendChunk(chunks, "COLL", coll);
    std::vector<uint8_t> out;
    AppendTag(out, "FORM");
    AppendBe32(out, static_cast<uint32_t>(4 + chunks.size()));
    AppendTag(out, "XOB9");
    out.insert(out.end(), chunks.begin(), chunks.end());
    return out;
}

XobCollision Decode(const std::vector<uint8_t>& xob)
{
    const XobHeader header = ReadXobHeader(xob.data(), xob.size());
    REQUIRE(header.valid());
    REQUIRE(header.strings.size() == kStrings.size());
    return ReadXobCollision(xob.data(), xob.size(), header);
}

// ------------------------------------------------------------ the fixtures

//! A sheet-walled room, 6 x 3 x 6 m, open top and bottom, with a 1 m doorway in
//! the +z wall. Single-sided quads that share their corner vertices, so the four
//! walls are ONE connected, OPEN mesh piece -- the case where "one hull" seals the
//! door and where the near-convex whole-piece rule must not fire.
void AppendRoom(std::vector<uint8_t>& out, uint16_t layer, uint16_t name)
{
    // 0..3 bottom corners, 4..7 top corners; 8..11 the door jambs (bottom, top).
    const std::vector<float> xyz = {
        -3,    0, -3, /*0*/ 3,    0, -3, /*1*/ 3,     0, 3, /*2*/ -3,    0, 3, /*3*/
        -3,    3, -3, /*4*/ 3,    3, -3, /*5*/ 3,     3, 3, /*6*/ -3,    3, 3, /*7*/
        -0.5f, 0, 3,  /*8*/ 0.5f, 0, 3,  /*9*/ -0.5f, 3, 3, /*10*/ 0.5f, 3, 3, /*11*/
    };
    const std::vector<Tri> tris = {
        {0, 1, 5},  {0, 5, 4},  // -z wall
        {1, 2, 6},  {1, 6, 5},  // +x wall
        {3, 0, 4},  {3, 4, 7},  // -x wall
        {3, 8, 10}, {3, 10, 7}, // +z wall, left of the door
        {9, 2, 6},  {9, 6, 11}, // +z wall, right of the door
    };
    AppendMesh(out, layer, name, xyz, tris);
}

//! A square pyramid with no base: apex up, four triangles, open underneath -- a
//! boulder the way the corpus models them.
void AppendPyramid(std::vector<uint8_t>& out, uint16_t layer, uint16_t name, float x, float z)
{
    const std::vector<float> xyz = {
        x - 1, 0, z - 1, x + 1, 0, z - 1, x + 1, 0, z + 1, x - 1, 0, z + 1, x, 1.5f, z,
    };
    const std::vector<Tri> tris = {{0, 1, 4}, {1, 2, 4}, {2, 3, 4}, {3, 0, 4}};
    AppendMesh(out, layer, name, xyz, tris);
}

//! A visual LOD to sit in front of the geometry LOD, since the writer's contract
//! (and the engine's) is that LOD 0 is a visual one.
MLODW::WriteLod VisualTriangle()
{
    MLODW::WriteLod lod;
    lod.resolution = 1.0f;
    lod.points.push_back({{0.0f, 0.0f, 0.0f}, 0});
    lod.points.push_back({{1.0f, 0.0f, 0.0f}, 0});
    lod.points.push_back({{0.0f, 1.0f, 0.0f}, 0});
    for (int i = 0; i < 3; ++i)
        lod.normals.push_back({0.0f, 0.0f, 1.0f});
    MLODW::WriteFace face;
    face.vertexCount = 3;
    for (int i = 0; i < 3; ++i)
    {
        face.vertices[i].point = i;
        face.vertices[i].normal = i;
    }
    lod.faces.push_back(face);
    return lod;
}

//! Writer -> reader -> ShapeAdapter, with `autocenter=0` on every LOD so shape space
//! is model space and the points below can be reasoned about directly.
std::unique_ptr<LODShapeWithShadow> ShapeFrom(const MLODW::WriteLod& geometry)
{
    MLODW::WriteModel model;
    model.lods.push_back(VisualTriangle());
    model.lods.push_back(geometry);
    for (auto& lod : model.lods)
        lod.properties.push_back({"autocenter", "0"});
    const std::vector<char> bytes = MLODWriter::writeToBuffer(model);
    const auto loaded = MLODLoader::loadFromBuffer(bytes.data(), static_cast<int>(bytes.size()), "xob-geometry");
    REQUIRE(loaded.lodLevels.size() == 2);
    REQUIRE(loaded.lodLevels[1].purpose == Poseidon::Model::LodPurpose::Geometry);
    std::unique_ptr<LODShapeWithShadow> shape(Poseidon::Model::ShapeAdapter::convertToLODShape(loaded, false));
    REQUIRE(shape != nullptr);
    return shape;
}

bool InsideAny(const ConvexComponents& components, const EngineVector3& point)
{
    for (int c = 0; c < components.Size(); ++c)
        if (components[c]->IsInside(point))
            return true;
    return false;
}

//! LogCollisionStats' `planesOK`: the centroid of a component's own vertices is
//! inside every one of its planes. A component that fails has its faces wound the
//! wrong way for the engine.
int PlanesOk(const ConvexComponents& components)
{
    int ok = 0;
    for (int c = 0; c < components.Size(); ++c)
    {
        const ConvexComponent& component = *components[c];
        Shape* geom = component.GetShape();
        const int n = component.Size();
        if (!geom || n <= 0 || component.NPlanes() < 4)
            continue;
        EngineVector3 centroid(VZero);
        for (int i = 0; i < n; ++i)
            centroid += geom->Pos(component[i]);
        centroid = centroid * (1.0f / static_cast<float>(n));
        bool inside = true;
        for (int p = 0; p < component.NPlanes() && inside; ++p)
            inside = component.GetPlane(p).Distance(centroid) >= -1e-3f;
        if (inside)
            ++ok;
    }
    return ok;
}

} // namespace

// ------------------------------------------------------------------ decoding

TEST_CASE("XOB COLL: every record type decodes and the walk closes", "[asset][enfusion][xob][collision][COL-001]")
{
    std::vector<uint8_t> coll;
    // A box rotated a quarter turn about Y-up's Z: local X -> +Y, local Y -> -X.
    const float quarter[9] = {0, 1, 0, -1, 0, 0, 0, 0, 1};
    AppendBox(coll, kBuilding, kUbxPost, quarter, 10.0f, 0.0f, 0.0f, 0.5f, 2.0f, 0.25f);
    AppendRound(coll, 2, kTree, kUcsTrunk, 0.0f, 1.0f, 0.0f, 0.2f, 0.8f);
    AppendRound(coll, 7, kFoliage, kUclLeaves, 0.0f, 3.0f, 0.0f, 1.5f, 0.5f);
    AppendRound(coll, 1, kBuilding, kUspBall, 4.0f, 0.3f, 0.0f, 0.3f, 0.0f);
    AppendTetraHull(coll, kBuilding, kUcxStone, -4.0f, 0.0f, 0.0f);
    AppendMesh(coll, kBuilding, kUtmWalls, {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0}, {{0, 1, 2}, {0, 2, 3}});
    AppendMesh(coll, kFireView, kUtmGrouped, {0, 0, 0, 1, 0, 0, 1, 1, 0}, {{0, 1, 2}}, true);

    const XobCollision collision = Decode(XobWith(coll));
    REQUIRE(collision.present);
    INFO(collision.error);
    REQUIRE(collision.closes);
    REQUIRE(collision.valid());
    REQUIRE(collision.shapes.size() == 7);

    const XobCollisionShape& box = collision.shapes[0];
    REQUIRE(box.kind == XobCollisionKind::Box);
    REQUIRE(box.layer == "Building");
    REQUIRE(box.name == "UBX_post");
    REQUIRE(box.material == "Common/Materials/Game/wood.gamemat"); // GUID stripped
    REQUIRE(box.halfExtents.x == Approx(0.5f));
    REQUIRE(box.halfExtents.y == Approx(2.0f));
    REQUIRE(box.halfExtents.z == Approx(0.25f));
    // Row-vector placement: local (0.5, 2, 0.25) -> 0.5*row0 + 2*row1 + 0.25*row2 + pos.
    const XobVec3 corner = box.ToModel({0.5f, 2.0f, 0.25f});
    REQUIRE(corner.x == Approx(8.0f));
    REQUIRE(corner.y == Approx(0.5f));
    REQUIRE(corner.z == Approx(0.25f));

    REQUIRE(collision.shapes[1].kind == XobCollisionKind::Capsule);
    REQUIRE(collision.shapes[1].layer == "Tree");
    REQUIRE(collision.shapes[1].radius == Approx(0.2f));
    REQUIRE(collision.shapes[1].halfHeight == Approx(0.8f));
    REQUIRE(collision.shapes[2].kind == XobCollisionKind::Cylinder);
    REQUIRE(collision.shapes[2].layer == "Foliage");
    REQUIRE(collision.shapes[3].kind == XobCollisionKind::Sphere);
    REQUIRE(collision.shapes[3].radius == Approx(0.3f));

    const XobCollisionShape& hull = collision.shapes[4];
    REQUIRE(hull.kind == XobCollisionKind::ConvexHull);
    REQUIRE(hull.vertices.size() == 4);
    REQUIRE(hull.polygons.size() == 4);
    REQUIRE(hull.indices.size() == 12);
    REQUIRE(hull.edges.size() == 24);
    REQUIRE(hull.polygons[3].first == 9);
    REQUIRE(hull.polygons[3].second == 3);
    // Face 3 is edges e2 (2,0), e3 (0,3), e5 (2,3): chained from e2 that is the
    // ring 2, 0, 3.
    REQUIRE(hull.indices[9] == 2);
    REQUIRE(hull.indices[10] == 0);
    REQUIRE(hull.indices[11] == 3);
    // Every ring is a triangle of distinct vertices.
    for (const auto& polygon : hull.polygons)
    {
        REQUIRE(polygon.second == 3);
        const uint32_t a = hull.indices[polygon.first], b = hull.indices[polygon.first + 1],
                       c = hull.indices[polygon.first + 2];
        REQUIRE(a != b);
        REQUIRE(b != c);
        REQUIRE(a != c);
    }
    REQUIRE(hull.ToModel(hull.vertices[3]).y == Approx(1.0f));
    REQUIRE(hull.ToModel(hull.vertices[3]).x == Approx(-4.0f));

    const XobCollisionShape& mesh = collision.shapes[5];
    REQUIRE(mesh.kind == XobCollisionKind::TriMesh);
    REQUIRE(mesh.IsMesh());
    REQUIRE(mesh.vertices.size() == 4);
    REQUIRE(mesh.polygons.size() == 2);
    REQUIRE(mesh.polygons[1].first == 3);
    REQUIRE(mesh.polygons[1].second == 3);
    REQUIRE(mesh.materialGroups.empty());

    const XobCollisionShape& grouped = collision.shapes[6];
    REQUIRE(grouped.kind == XobCollisionKind::TriMeshGrouped);
    REQUIRE(grouped.layer == "FireView");
    REQUIRE(grouped.materialGroups.size() == 1);
    REQUIRE(grouped.materialGroups[0].second == 0);
    REQUIRE(grouped.polygons.size() == 1);
}

TEST_CASE("XOB COLL: absent, truncated and unknown are three different answers",
          "[asset][enfusion][xob][collision][COL-001]")
{
    SECTION("no COLL chunk")
    {
        const XobCollision collision = Decode(XobWith({}, false));
        REQUIRE_FALSE(collision.present);
        REQUIRE(collision.valid());
        REQUIRE(collision.shapes.empty());
        REQUIRE(collision.error.empty());
    }
    SECTION("a record cut short keeps the complete ones and names the failure")
    {
        std::vector<uint8_t> coll;
        AppendBox(coll, kBuilding, kUbxPost, kIdentity, 0, 0, 0, 1, 1, 1);
        std::vector<uint8_t> capsule;
        AppendRound(capsule, 2, kTree, kUcsTrunk, 0, 1, 0, 0.2f, 0.8f);
        coll.insert(coll.end(), capsule.begin(), capsule.begin() + 40);
        const XobCollision collision = Decode(XobWith(coll));
        REQUIRE(collision.present);
        REQUIRE_FALSE(collision.closes);
        REQUIRE_FALSE(collision.valid());
        REQUIRE(collision.shapes.size() == 1);
        REQUIRE(collision.error.find("runs past") != std::string::npos);
    }
    SECTION("an unknown type stops the walk where it is")
    {
        std::vector<uint8_t> coll;
        AppendBox(coll, kBuilding, kUbxPost, kIdentity, 0, 0, 0, 1, 1, 1);
        AppendRecordHead(coll, 9, kBuilding, kUbxPost, kWood, kIdentity, 0, 0, 0);
        AppendF32(coll, 1.0f);
        const XobCollision collision = Decode(XobWith(coll));
        REQUIRE_FALSE(collision.closes);
        REQUIRE(collision.shapes.size() == 1);
        REQUIRE(collision.error.find("unknown record type 9") != std::string::npos);
    }
}

TEST_CASE("XOB COLL: layer presets say what a character collides with", "[asset][enfusion][xob][collision][COL-001]")
{
    for (const char* blocking :
         {"Building",     "BuildingFireView", "BuildingView", "BuildingNoNavmesh", "Prop",
          "PropFireView", "PropView",         "Tree",         "TreePart",          "TreeFireView",
          "Rock",         "RockFireView",     "Door",         "DoorFireView",      "Ladder",
          "Debris",       "Terrain",          "ItemFireView", "VehicleComplex",    "WeaponFire",
          "building"})
    {
        INFO(blocking);
        REQUIRE(XobLayerBlocksCharacters(blocking));
    }
    for (const char* passable : {"FireView", "FireGeo", "Foliage", "GlassFire", "Glass", "ViewGeo", "CharNoCollide",
                                 "CharSpecialCollisionNoCollide", "PropNoCollide", "MineTrigger", "Interaction",
                                 "Projectile", "Cover", "Wheel", "Bush", ""})
    {
        INFO(passable);
        REQUIRE_FALSE(XobLayerBlocksCharacters(passable));
    }
}

// --------------------------------------------------------- the geometry LOD

TEST_CASE("XOB geometry LOD: a sheet-walled room stays enterable, a post is solid, foliage and fire volumes are "
          "ignored",
          "[asset][enfusion][xob][collision][COL-001][ShapeAdapter]")
{
    std::vector<uint8_t> coll;
    AppendBox(coll, kBuilding, kUbxPost, kIdentity, 10.0f, 1.0f, 0.0f, 0.2f, 1.0f, 0.2f);
    AppendRoom(coll, kBuilding, kUtmWalls);
    AppendRound(coll, 7, kFoliage, kUclLeaves, 0.0f, 10.0f, 0.0f, 3.0f, 1.0f);
    // A fire-geometry box big enough to swallow the room. If the layer filter
    // failed, the room centre below would be inside a component.
    AppendBox(coll, kFireView, kUtmFire, kIdentity, 0.0f, 1.5f, 0.0f, 8.0f, 2.0f, 8.0f);
    const XobCollision collision = Decode(XobWith(coll));
    REQUIRE(collision.closes);
    REQUIRE(collision.shapes.size() == 4);

    GeometryLodOptions options;
    options.totalMass = 5000.0f;
    MLODW::WriteLod geometry;
    GeometryLodStats stats;
    REQUIRE(BuildGeometryLod(collision, options, geometry, stats));

    REQUIRE(stats.shapesTotal == 4);
    REQUIRE(stats.shapesBlocking == 2);
    REQUIRE(stats.shapesUsed == 2);
    REQUIRE(stats.componentsPrimitive == 1);
    // Five wall panels: three whole walls and the two either side of the door.
    // Each is a planar patch of two triangles merged into one convex quad and
    // extruded into a slab; the near-convex whole-piece rule must NOT have fired
    // on this open room, or there would be one component and no doorway.
    REQUIRE(stats.componentsPiece == 0);
    REQUIRE(stats.componentsPrism == 5);
    REQUIRE(stats.componentsHull == 0);
    REQUIRE(stats.componentsDropped == 0);
    REQUIRE(stats.Components() == 6);
    REQUIRE(geometry.resolution == 1.0e13f);
    REQUIRE(geometry.selections.size() == 6);
    REQUIRE(geometry.selections[0].name == "Component01");
    REQUIRE(geometry.selections[5].name == "Component06");
    REQUIRE(geometry.mass.size() == geometry.points.size());
    float total = 0.0f;
    for (float m : geometry.mass)
        total += m;
    REQUIRE(total == Approx(5000.0f).margin(0.5f));
    for (const auto& selection : geometry.selections)
        REQUIRE(selection.faces.size() >= 4);

    // Through the writer, the reader and the adapter: the engine's own verdicts.
    std::unique_ptr<LODShapeWithShadow> shape = ShapeFrom(geometry);
    REQUIRE(shape->FindGeometryLevel() >= 0);
    REQUIRE(shape->Mass() == Approx(5000.0f).margin(1.0f));
    REQUIRE(shape->Mass() >= 10.0f);
    REQUIRE(shape->BoundingCenter().SquareSize() == Approx(0.0f).margin(1e-8f));
    const ConvexComponents& components = shape->GetGeomComponents();
    REQUIRE(components.Size() == 6);
    REQUIRE(PlanesOk(components) == 6);

    // The post is solid.
    REQUIRE(InsideAny(components, EngineVector3(10.0f, 1.0f, 0.0f)));
    // The walls are solid...
    REQUIRE(InsideAny(components, EngineVector3(0.0f, 1.5f, -3.0f)));
    REQUIRE(InsideAny(components, EngineVector3(3.0f, 1.5f, 0.0f)));
    REQUIRE(InsideAny(components, EngineVector3(-2.0f, 1.5f, 3.0f)));
    REQUIRE(InsideAny(components, EngineVector3(2.0f, 1.5f, 3.0f)));
    // ...and the room and the doorway are not.
    REQUIRE_FALSE(InsideAny(components, EngineVector3(0.0f, 1.5f, 0.0f)));
    REQUIRE_FALSE(InsideAny(components, EngineVector3(0.0f, 1.5f, 3.0f)));
    REQUIRE_FALSE(InsideAny(components, EngineVector3(0.0f, 1.0f, 2.0f)));
    // The foliage cylinder and the fire box left nothing behind.
    REQUIRE_FALSE(InsideAny(components, EngineVector3(0.0f, 10.0f, 0.0f)));
    REQUIRE_FALSE(InsideAny(components, EngineVector3(5.0f, 1.5f, 5.0f)));
}

TEST_CASE("XOB geometry LOD: an open boulder is one component for a rock and slabs for anything else",
          "[asset][enfusion][xob][collision][COL-001][ShapeAdapter]")
{
    std::vector<uint8_t> coll;
    AppendPyramid(coll, kBuilding, kUtmRock, 0.0f, 0.0f);
    const XobCollision collision = Decode(XobWith(coll));
    REQUIRE(collision.closes);

    SECTION("rock: the piece is used whole, capped underneath, and its centre is inside")
    {
        GeometryLodOptions options;
        options.totalMass = 1000.0f;
        options.solidMesh = true;
        MLODW::WriteLod geometry;
        GeometryLodStats stats;
        REQUIRE(BuildGeometryLod(collision, options, geometry, stats));
        REQUIRE(stats.componentsPiece == 1);
        REQUIRE(stats.componentsPrism == 0);
        // Four sides plus the floor cap.
        REQUIRE(geometry.faces.size() == 5);

        std::unique_ptr<LODShapeWithShadow> shape = ShapeFrom(geometry);
        const ConvexComponents& components = shape->GetGeomComponents();
        REQUIRE(components.Size() == 1);
        REQUIRE(PlanesOk(components) == 1);
        REQUIRE(components[0]->IsInside(EngineVector3(0.0f, 0.5f, 0.0f)));
        REQUIRE_FALSE(components[0]->IsInside(EngineVector3(0.9f, 1.2f, 0.9f)));
        REQUIRE_FALSE(components[0]->IsInside(EngineVector3(0.0f, -0.5f, 0.0f)));
        REQUIRE(shape->Mass() == Approx(1000.0f).margin(1.0f));
    }
    SECTION("building: an open piece is never taken whole")
    {
        GeometryLodOptions options;
        MLODW::WriteLod geometry;
        GeometryLodStats stats;
        REQUIRE(BuildGeometryLod(collision, options, geometry, stats));
        REQUIRE(stats.componentsPiece == 0);
        REQUIRE(stats.componentsPrism == 4);
        std::unique_ptr<LODShapeWithShadow> shape = ShapeFrom(geometry);
        const ConvexComponents& components = shape->GetGeomComponents();
        REQUIRE(components.Size() == 4);
        REQUIRE(PlanesOk(components) == 4);
        // The slabs sit on the faces; the hollow underneath is free.
        REQUIRE_FALSE(InsideAny(components, EngineVector3(0.0f, 0.3f, 0.0f)));
    }
}

TEST_CASE("XOB geometry LOD: a concave solid is split into cells and its notch stays open",
          "[asset][enfusion][xob][collision][COL-001][ShapeAdapter]")
{
    // An L-shaped closed solid, 1 m tall: footprint [0,2]x[0,1] plus [0,1]x[1,2].
    // Consistently wound, right-hand normals inward as written. One hull would
    // fill the notch at (1.5, *, 1.5); the solid-mesh kd-split must not.
    const std::vector<float> xyz = {
        0, 0, 0, 2, 0, 0, 2, 0, 1, 1, 0, 1, 1, 0, 2, 0, 0, 2, // bottom ring 0..5
        0, 1, 0, 2, 1, 0, 2, 1, 1, 1, 1, 1, 1, 1, 2, 0, 1, 2, // top ring 6..11
    };
    std::vector<Tri> tris = {
        {0, 2, 1}, {0, 3, 2}, {0, 4, 3},  {0, 5, 4},   // bottom
        {6, 7, 8}, {6, 8, 9}, {6, 9, 10}, {6, 10, 11}, // top
    };
    for (uint16_t i = 0; i < 6; ++i)
    {
        const uint16_t j = static_cast<uint16_t>((i + 1) % 6);
        tris.push_back({i, j, static_cast<uint16_t>(j + 6)});
        tris.push_back({i, static_cast<uint16_t>(j + 6), static_cast<uint16_t>(i + 6)});
    }
    // The same solid with every triangle reversed (right-hand normals outward):
    // the orientation comes from the mesh's own winding, whichever way it goes,
    // so both files must give the same answers. A builder that oriented faces
    // toward a centroid instead would fill the notch on both.
    std::vector<Tri> reversed;
    for (const Tri& t : tris)
        reversed.push_back({t.a, t.c, t.b});

    const bool inwardWinding = GENERATE(true, false);
    INFO((inwardWinding ? "right-hand normals inward" : "right-hand normals outward"));
    std::vector<uint8_t> coll;
    AppendMesh(coll, kBuilding, kUtmRock, xyz, inwardWinding ? tris : reversed);
    const XobCollision collision = Decode(XobWith(coll));
    REQUIRE(collision.closes);

    GeometryLodOptions options;
    options.totalMass = 1000.0f;
    options.solidMesh = true;
    options.nearConvexTolerance = 0.5f; // the converter's rock settings
    options.nearConvexMaxTolerance = 2.0f;
    MLODW::WriteLod geometry;
    GeometryLodStats stats;
    REQUIRE(BuildGeometryLod(collision, options, geometry, stats));
    REQUIRE(stats.meshPiecesConcave == 1);
    REQUIRE(stats.meshChunks >= 2);
    REQUIRE(stats.componentsPiece >= 2);
    REQUIRE(stats.componentsPrism == 0);
    REQUIRE(stats.componentsPiece == stats.Components());

    std::unique_ptr<LODShapeWithShadow> shape = ShapeFrom(geometry);
    const ConvexComponents& components = shape->GetGeomComponents();
    REQUIRE(components.Size() == static_cast<int>(stats.Components()));
    // Both arms and the corner are solid...
    REQUIRE(InsideAny(components, EngineVector3(1.5f, 0.5f, 0.5f)));
    REQUIRE(InsideAny(components, EngineVector3(0.5f, 0.5f, 1.5f)));
    REQUIRE(InsideAny(components, EngineVector3(0.5f, 0.5f, 0.5f)));
    REQUIRE(InsideAny(components, EngineVector3(1.9f, 0.9f, 0.9f)));
    REQUIRE(InsideAny(components, EngineVector3(0.1f, 0.1f, 1.9f)));
    // ...the notch, the air above and the space beyond are not.
    REQUIRE_FALSE(InsideAny(components, EngineVector3(1.5f, 0.5f, 1.5f)));
    REQUIRE_FALSE(InsideAny(components, EngineVector3(0.5f, 1.5f, 0.5f)));
    REQUIRE_FALSE(InsideAny(components, EngineVector3(3.0f, 0.5f, 0.5f)));
    REQUIRE(shape->Mass() == Approx(1000.0f).margin(1.0f));
}

TEST_CASE("XOB geometry LOD: a closed convex block is one component from its own faces",
          "[asset][enfusion][xob][collision][COL-001][ShapeAdapter]")
{
    // A 2 x 1 x 1 m block as a closed triangle mesh -- the corpus's wall segments
    // and posts. Twelve triangles, every edge shared by two. Wound inward as
    // written; the reversed copy must come out identical (signed volume decides).
    const std::vector<float> xyz = {
        0, 0, 0, 2, 0, 0, 2, 0, 1, 0, 0, 1, 0, 1, 0, 2, 1, 0, 2, 1, 1, 0, 1, 1,
    };
    std::vector<Tri> tris = {
        {0, 2, 1}, {0, 3, 2}, // bottom
        {4, 5, 6}, {4, 6, 7}, // top
        {0, 1, 5}, {0, 5, 4}, // -z
        {1, 2, 6}, {1, 6, 5}, // +x
        {2, 3, 7}, {2, 7, 6}, // +z
        {3, 0, 4}, {3, 4, 7}, // -x
    };
    const bool inwardWinding = GENERATE(true, false);
    INFO((inwardWinding ? "right-hand normals inward" : "right-hand normals outward"));
    if (!inwardWinding)
        for (Tri& t : tris)
            std::swap(t.b, t.c);
    std::vector<uint8_t> coll;
    AppendMesh(coll, kBuilding, kUtmWalls, xyz, tris);
    const XobCollision collision = Decode(XobWith(coll));
    REQUIRE(collision.closes);

    GeometryLodOptions options;
    options.totalMass = 2000.0f;
    MLODW::WriteLod geometry;
    GeometryLodStats stats;
    REQUIRE(BuildGeometryLod(collision, options, geometry, stats));
    REQUIRE(stats.componentsPiece == 1);
    REQUIRE(stats.Components() == 1);
    REQUIRE(geometry.faces.size() == 12);

    std::unique_ptr<LODShapeWithShadow> shape = ShapeFrom(geometry);
    const ConvexComponents& components = shape->GetGeomComponents();
    REQUIRE(components.Size() == 1);
    REQUIRE(PlanesOk(components) == 1);
    REQUIRE(components[0]->IsInside(EngineVector3(1.0f, 0.5f, 0.5f)));
    REQUIRE_FALSE(InsideAny(components, EngineVector3(3.0f, 0.5f, 0.5f)));
    REQUIRE_FALSE(InsideAny(components, EngineVector3(1.0f, 1.5f, 0.5f)));
}

TEST_CASE("XOB geometry LOD: nothing blocking means no LOD, not an empty one",
          "[asset][enfusion][xob][collision][COL-001]")
{
    std::vector<uint8_t> coll;
    AppendRound(coll, 7, kFoliage, kUclLeaves, 0.0f, 0.5f, 0.0f, 1.0f, 0.5f);
    AppendMesh(coll, kFireView, kUtmFire, {0, 0, 0, 1, 0, 0, 1, 1, 0}, {{0, 1, 2}});
    const XobCollision collision = Decode(XobWith(coll));
    REQUIRE(collision.closes);
    GeometryLodOptions options;
    MLODW::WriteLod geometry;
    GeometryLodStats stats;
    REQUIRE_FALSE(BuildGeometryLod(collision, options, geometry, stats));
    REQUIRE(geometry.points.empty());
    REQUIRE(geometry.selections.empty());
    REQUIRE(stats.shapesTotal == 2);
    REQUIRE(stats.shapesBlocking == 0);
    // With every layer admitted the foliage cylinder becomes a component; the
    // one-triangle mesh is a single open patch and yields a slab too.
    options.allLayers = true;
    REQUIRE(BuildGeometryLod(collision, options, geometry, stats));
    REQUIRE(stats.shapesBlocking == 2);
    REQUIRE(stats.componentsPrimitive == 1);
    REQUIRE(stats.componentsPrism == 1);
}
