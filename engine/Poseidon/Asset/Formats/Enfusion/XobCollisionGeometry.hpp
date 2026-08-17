#pragma once

#include <Poseidon/Asset/Formats/Enfusion/XobCollision.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODWriter.hpp>

#include <cstddef>
#include <cstdint>

// COL-001: a Geometry LOD for a converted Reforger model, from the `.xob`'s own
// COLL shapes.
//
// What the engine needs (ShapeAdapter.cpp, LODShape::InitConvexComponents): a LOD at
// resolution 1e13 whose faces are grouped into named selections `Component01..NN`,
// each of which is a CONVEX polytope -- the engine turns every face of a selection
// into a half-space and the component is their intersection -- plus a `#Mass#`
// tagg whose total is at least 10 kg, or Object::IsPassable() is true and a
// soldier's collision response skips the object.
//
// What the `.xob` carries (XobCollision.hpp): boxes, capsules, cylinders, spheres,
// convex hulls and triangle meshes on named layer presets. The first five are convex
// already and become one component each. The meshes are not: a house's "Building"
// mesh is one closed shell with rooms, doors and windows, and a house is exactly the
// case where one hull would be wrong -- the interior must stay enterable. So a mesh
// is decomposed:
//
//   1. split into connected pieces (shared edges);
//   2. a CLOSED piece whose own face planes already bound a near-convex volume --
//      every vertex within `nearConvexTolerance` x radius (capped at
//      `nearConvexMaxTolerance`) of the inside of every plane, and the inward
//      normals positively spanning space so the intersection is bounded -- becomes
//      ONE component from its own faces (a wall block, a post, a solid boulder).
//      An OPEN piece qualifies only with `solidMesh` (rocks: their undersides are
//      not modelled) and then gets a bottom cap so the intersection closes; for a
//      building an open near-convex piece is a sheet-walled room with a door, and
//      using it whole would seal the door;
//   3. a concave piece of a SOLID mesh (a lumpy boulder, a cliff) is kd-split into
//      cells -- along the longest axis at the median, recursively, each cell
//      taking every triangle that touches it -- until a cell's triangles are
//      near-convex, and every cell becomes one component: the cell box cut by its
//      triangles' inward half-spaces (inward from the mesh's own winding). Cells
//      of air come out empty and are not written; the notch of an L stays open;
//   4. anything else (the house shell) is split into planar patches, each patch's
//      triangles are merged greedily into convex polygons, and every polygon is
//      extruded into a thin slab: inward for a closed shell (its winding says
//      which way is in), straight down for a horizontal sheet (a floor must not
//      become a step), and symmetrically for any other open sheet.
//
// The result for a Reforger house is a few hundred slab components -- one per wall
// panel, floor plate and roof pitch -- which is more than an authored OFP house
// carries but is the same shape: door openings stay open because no slab covers
// them. `maxComponents` caps the count under InitConvexComponents' 999 (it stops at
// the first missing ComponentNN); the smallest slabs by area go first, since they
// are the trims and bevels.
//
// Measured over the 1,132 models Everon places (2026-08-17): 1,074 get a geometry
// LOD, 58 have nothing character-blocking; median 4 components per model, p90 227,
// 24 models at the cap (the church, the factory hall, the castle tower, the silage
// store, ... -- 5,000-7,000-triangle building meshes); every rock under the cap.
// The whole set decomposes in about four seconds.
//
// Mass is the caller's number, spread evenly over the LOD's points; the engine sums
// it back (LODShape::CalculateMass).

namespace Poseidon::Asset::Formats::Enfusion
{

struct GeometryLodOptions
{
    float totalMass = 1000.0f;           //!< kg, spread over the geometry points
    float slabHalfThickness = 0.08f;     //!< metres each side of an open sheet's polygon
    float nearConvexTolerance = 0.15f;   //!< fraction of a piece's radius a vertex may sit outside a face plane
    float nearConvexMaxTolerance = 0.5f; //!< metres; the fraction above is capped here so a house shell never passes
    //! The meshes are solid bodies (rocks). Two things follow: an OPEN piece may be
    //! used whole when it is near-convex (boulders are modelled without an
    //! underside), and a concave piece is split into spatial chunks each used
    //! whole inside its bounding box, instead of into per-face slabs. Wrong for a
    //! building, where an open near-convex piece is a sheet-walled room with a
    //! door and a chunk around a room would fill the room. The converter sets this
    //! from the model's class.
    bool solidMesh = false;
    size_t maxComponents = 990; //!< InitConvexComponents stops at Component999
    int roundSides = 8;         //!< polygon sides for a capsule / cylinder / sphere
    //! Ignore the layer preset and use every shape. For inspection only: with it, a
    //! bush's foliage cylinder and a house's fire-geometry mesh both become walls.
    bool allLayers = false;
};

struct GeometryLodStats
{
    size_t shapesTotal = 0;         //!< records in COLL
    size_t shapesBlocking = 0;      //!< of which on a character-blocking layer (or all, with allLayers)
    size_t shapesUsed = 0;          //!< of which produced at least one component
    size_t componentsPrimitive = 0; //!< boxes, capsules, cylinders, spheres
    size_t componentsHull = 0;      //!< authored convex hulls
    size_t componentsPiece = 0;     //!< near-convex mesh pieces used whole
    size_t componentsPrism = 0;     //!< slabs from planar patches
    size_t componentsDropped = 0;   //!< over maxComponents, or fewer than four faces
    size_t meshTriangles = 0;       //!< triangles in the meshes that were decomposed
    size_t meshPieces = 0;          //!< connected pieces those meshes split into
    size_t meshPiecesClosed = 0;    //!< of which closed (every edge shared by two triangles)
    size_t meshPiecesConcave = 0;   //!< candidates for whole use refused on the tolerance -> slabs
    size_t meshPiecesUnbounded = 0; //!< near-convex but their planes do not close a volume -> slabs
    size_t meshChunks = 0;          //!< solid-mesh cells (concave rock pieces split spatially)
    size_t meshChunksEmpty = 0;     //!< of which enclosed no solid (a cell in a notch) and were not written
    size_t points = 0;
    size_t faces = 0;

    size_t Components() const { return componentsPrimitive + componentsHull + componentsPiece + componentsPrism; }
};

//! Fills `out` (points, normals, faces, selections, mass, resolution = 1e13). Returns
//! false, leaving `out` empty, when no shape yields a component -- the caller then
//! writes no geometry LOD, which is the correct file for a decal or a bush.
bool BuildGeometryLod(const XobCollision& collision, const GeometryLodOptions& options, MLOD::WriteLod& out,
                      GeometryLodStats& stats);

} // namespace Poseidon::Asset::Formats::Enfusion
