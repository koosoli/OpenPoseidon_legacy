#pragma once

#include <Poseidon/Asset/Formats/Enfusion/XobModel.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Arma Reforger's `.xob` COLL chunk -- the authored physics shapes (COL-001).
//
// Decoded here for the first time; XobModel.hpp reads the render geometry and left
// COLL as "not decoded". Established against the local install on 2026-08-17: the
// chunk is a flat sequence of records, no count and no index, each
//
//   u8      type          1 sphere, 2 capsule, 3 box, 4 convex hull, 5 triangle
//                         mesh, 6 triangle mesh with material groups, 7 cylinder
//   u8      0xFF          on every record measured
//   u16     layerOrdinal  HEAD string-table ordinal of the layer PRESET name
//                         ("Building", "FireView", "Foliage", ...)
//   f32[9]  rotation      3x3, ROW-vector convention: model = local * M + position.
//                         The rows are the local basis vectors. Measured, not
//                         assumed: on 3,077 rotated boxes and hulls in data003/4/5,
//                         placing the shape with this convention keeps it inside
//                         the model's HEAD bounding box 1,182 times and the
//                         column-vector reading does 1 time (1,894 ties, symmetric
//                         shapes both ways).
//   f32[3]  position      model space, metres
//   f32     0             purpose not established, zero on every record
//   u16     nameOrdinal   the shape's own name (UBX_*, UCS_*, UCX_*, UTM_*, UCL_*)
//   u16     materialOrd   the surface `.gamemat`
//   u32     0             purpose not established
//   then, by type:
//     1  f32 radius
//     2  f32 radius, f32 halfHeight   capsule along local Y; the hemispheres ADD
//                                      radius beyond +-halfHeight (Bullet's
//                                      convention -- a tree trunk's stacked
//                                      capsules meet end to end only read this way)
//     3  f32[3] halfExtents
//     4  u16 nVerts, u16 nFaces, u16 nEdges, u16 nIndices, f32[3]*nVerts,
//        (u16 v0, u16 v1, u16 f0, u16 f1)*nEdges     -- edges with their two faces
//        u16*nIndices                                -- EDGE indices, nIndices == 2*nEdges
//        (u16 start, u16 count)*nFaces               -- each face is a run of edges
//        A face's vertex ring is recovered by chaining its edges; the reader does
//        that and hands out vertex polygons like every other type. Read as vertex
//        indices this list overruns the vertex table on the first hull (max 101 of
//        36), which is how the layout was found.
//     5  u16 nVerts, u16 nTris, f32[3]*nVerts, u16[3]*nTris
//     6  u16 nVerts, u16 nTris, u32 nGroups, (u16 materialOrd, u16 lastTri)*nGroups,
//        f32[3]*nVerts, u16[3]*nTris
//     7  f32 radius, f32 halfHeight   cylinder along local Y
//
// Closure over the whole install: 15,841 `.xob`, 14,771 with a COLL chunk, and the
// record walk lands exactly on the chunk end for 14,771 / 14,771 of them (29 files
// carry a type-1 sphere and were the last to close). Record census: 9,672 hulls,
// 8,775 boxes, 3,567 meshes, 2,517 grouped meshes, 1,957 capsules, 1,347
// cylinders, 29 spheres.
//
// The layer preset is what says whether a character collides with the shape. Of the
// 1,132 models `worlds/Eden` places, 1,074 carry at least one shape on a
// character-blocking layer; the 58 that do not are decals, posters, litter, bushes,
// plants and three walk-over pebble clusters -- exactly the things a player walks
// through in Reforger too. Synthesising collision for those would ADD collision the
// source engine does not have, which is why there is no bounding-box fallback here.

namespace Poseidon::Asset::Formats::Enfusion
{

enum class XobCollisionKind : uint8_t
{
    Unknown = 0,
    Sphere = 1,
    Capsule = 2,
    Box = 3,
    ConvexHull = 4,
    TriMesh = 5,
    TriMeshGrouped = 6,
    Cylinder = 7,
};

const char* ToString(XobCollisionKind kind);

//! One record. Everything mesh-like is normalised to `vertices` (LOCAL space) plus
//! `polygons` -- (start, count) into `indices` -- so a consumer walks hulls and
//! meshes with the same loop; a triangle mesh is polygons of count 3.
struct XobCollisionShape
{
    XobCollisionKind kind = XobCollisionKind::Unknown;
    uint8_t typeCode = 0;
    std::string layer;                               //!< layer preset name, e.g. "Building", "FireView"
    std::string name;                                //!< the shape's authored name, e.g. "UBX_Wall_01"
    std::string material;                            //!< `.gamemat` path, GUID stripped
    float rotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; //!< row-major, rows are the local axes
    XobVec3 position;
    float extra = 0.0f; //!< the f32 after position; zero on the whole corpus

    // Sphere / capsule / cylinder.
    float radius = 0.0f;
    float halfHeight = 0.0f;
    // Box.
    XobVec3 halfExtents;
    // Hull / mesh, local space.
    std::vector<XobVec3> vertices;
    std::vector<uint32_t> indices;
    std::vector<std::pair<uint32_t, uint32_t>> polygons; //!< (start, count) into `indices`
    //! Hull only: (v0, v1, f0, f1) per edge, the file's own topology; `polygons`
    //! above are the vertex rings rebuilt from it.
    std::vector<uint32_t> edges;
    //! Grouped mesh only: (material ordinal, last triangle index) per group.
    std::vector<std::pair<uint16_t, uint16_t>> materialGroups;

    //! local -> model, the row-vector convention documented above.
    XobVec3 ToModel(const XobVec3& local) const;

    bool IsMesh() const { return kind == XobCollisionKind::TriMesh || kind == XobCollisionKind::TriMeshGrouped; }
};

struct XobCollision
{
    bool present = false; //!< the file has a COLL chunk at all
    bool closes = false;  //!< the record walk consumed the chunk exactly
    std::vector<XobCollisionShape> shapes;
    std::string error; //!< why the walk stopped early, empty when it closed

    bool valid() const { return !present || closes; }
};

//! Decodes the COLL chunk. Names resolve through the HEAD string table, so the
//! header must have been read first (ReadXobHeader). A file without a COLL chunk
//! returns `present == false` and no error. A record whose type is unknown, or
//! that runs past the chunk, stops the walk: every COMPLETE record before it is
//! kept, the failing one is not, `closes` is false and `error` says where.
XobCollision ReadXobCollision(const void* data, size_t size, const XobHeader& header);

//! Does a shape on this layer preset stop a walking character? Measured layer
//! names, whole corpus: Debris 8491, FireGeo 3047, BuildingFireView 2858,
//! PropFireView 2368, GlassFire 1862, FireView 1636, Building 1537, BuildingView
//! 1222, PropView 1106, Prop 957, VehicleComplex 609, ItemFireView 339, Tree 201,
//! TreePart 198, DoorFireView 197, BuildingFire 183, Door 163, Foliage 155,
//! VehicleSimple 104, Ladder 72, TreeFireView 64, ... Rock 4, Bush 10.
//!
//! The rule: the name starts with one of Building / Prop / Tree / Rock / Door /
//! Ladder / Debris / Terrain / Item / Vehicle / Weapon (any Fire/View/NoNavmesh
//! suffix included -- "BuildingFireView" is a building wall that also stops fire
//! and sight) and does not contain "NoCollide". Everything else -- Fire*, Glass*,
//! Foliage, View*, Char*, Mine*, Interaction, Projectile, Cover, Wheel*, Bush, the
//! empty layer -- is a fire, view, trigger or foliage volume a player walks through.
bool XobLayerBlocksCharacters(const std::string& layer);

} // namespace Poseidon::Asset::Formats::Enfusion
