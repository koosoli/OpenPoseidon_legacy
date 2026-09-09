#pragma once

#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>

#include <vector>

namespace Poseidon
{

class BuildingType;
class Object;

// ---------------------------------------------------------------------------
// Proactive indoor containment for smoke (roadmap SMOKE-rooms-and-portals).
//
// Reactive collision ("stop when you hit a wall") can never be complete: a puff
// is metres wide, a wall is decimetres thin, and some fire geometry has gaps.
// This is the proactive answer: which room is a point in, and which openings
// (portals) lead out of it. A particle that is in a room cannot leave except
// through a portal, by construction, and no wall ever has to be hit.
//
// Everything expensive is derived once per BuildingType and shared by every
// instance of that type; placement transforms the lookup. The data comes from
// what enterable buildings already carry:
//
//   doors      named points In1..InN in the Paths LOD (BuildingType::_exits)
//   rooms      connected clusters of the Paths graph with the In nodes removed
//              (BuildingType::_connections / _positions)
//   shell      Fire Geometry LOD, plus View Geometry where it closes a hole the
//              fire LOD leaves open (glass)
//   floors     implied by the shell; storeys separate where the graph does
//
// Rooms are also voxelised at 0.5 m against that shell, which turns "which
// room" into a table lookup and makes leaks (gappy fire LODs) visible at build
// time instead of at runtime.
//
// ALL COORDINATES IN THIS CLASS ARE MODEL SPACE unless a name says World.
// ---------------------------------------------------------------------------

enum InteriorPortalKind
{
	PortalDoor,   ///< an In node of the Paths LOD
	PortalWindow, ///< an opening found by the scan, small enough to be a window
	PortalBreach, ///< an opening too large to be authored -- usually a gappy fire LOD
};

struct InteriorPortal
{
	Vector3 centre{VZero};  ///< model space, centre of the opening
	Vector3 normal{VZero};  ///< unit; points OUT of the room the portal serves
	float radius = 1.0f;    ///< rough radius of the opening, m
	int room = -1;          ///< room this portal serves
	int otherRoom = -1;     ///< room on the far side, or -1 for open air
	InteriorPortalKind kind = PortalDoor;
};

/// Which openings forgive a crossing, and how generously. Rain is the caller
/// that cares: a portal disc is a rough stand-in for a real hole, and every
/// metre of slack it is given is a metre of wall that rain falls through.
/// Doors are AUTHORED (In1..InN in the Paths LOD) and trustworthy; windows and
/// breaches come from the voxel scan's heuristic and are off by default.
/// `radiusScale` shrinks every portal disc -- 1 is the inferred radius, and the
/// smaller it gets the closer to the exact opening a crossing has to be.
struct InteriorPortalFilter
{
	float radiusScale = 0.75f;
	bool doors = true;
	bool windows = false;
	bool breaches = false;

	bool Accepts(InteriorPortalKind kind) const
	{
		switch (kind)
		{
			case PortalDoor: return doors;
			case PortalWindow: return windows;
			case PortalBreach: return breaches;
		}
		return false;
	}
};

/// Result of one proactive containment step. When `blocked`, `position` is the
/// last point provably inside the room and `normal` the outward normal of the
/// boundary that was about to be crossed -- enough for the caller to respond as
/// if a surface had been hit.
struct InteriorStepResult
{
	bool blocked = false;
	Vector3 position{VZero};
	Vector3 normal{VZero};
	int room = -1; ///< room the particle ends this step in
};

class BuildingInterior
{
public:
	/// Lazily built, cached per type. Returns null for types with no usable
	/// shell (no fire geometry) -- callers must fall back to reactive sweeps.
	static BuildingInterior const* GetFor(BuildingType const* type);
	static void ClearCache();

	bool Valid() const { return _valid; }
	int RoomCount() const { return _roomCount; }
	const AutoArray<InteriorPortal> &Portals() const { return _portals; }

	/// Room id containing a model-space point, or -1 for outside / unknown.
	int RoomOfModel(Vector3Par pos) const;

	/// Three-way classification: -1 outside, -2 inside SHELL GEOMETRY (a wall,
	/// a roof), otherwise the room id. Rain uses this to die at the roofline
	/// instead of falling through the shell to the room beneath.
	int ClassifyModel(Vector3Par pos) const;
	/// Cached topmost shell voxel in this vertical model-space column.
	bool CoveredFromAboveModel(Vector3Par pos) const;

	/// First non-outside classification touched by a model-space segment. This
	/// samples at half-cell intervals, so a fast rain drop cannot step from air
	/// above a thin roof to air below it without observing the solid voxel.
	/// Returns -1 when the complete segment remains outside the cached interior.
	int ClassifySegmentModel(Vector3Par from, Vector3Par to) const;

	/// Does the model-space segment pass through any portal opening? The test
	/// is the exact point-to-segment distance against each portal centre,
	/// against a TIGHTENED radius (three quarters): portals sit on the wall
	/// plane and some inferred windows/breaches are generous, and a loose test
	/// forgave crossings metres away from the actual hole -- rain through
	/// walls. Rain asks this when its segment crossed the shell or a room
	/// boundary; a crossing AT an opening is rain coming IN through a door or
	/// window -- exactly where smoke leaves -- and must be allowed.
	bool SegmentPassesPortalModel(Vector3Par from, Vector3Par to) const
	{
		return SegmentPassesPortalModel(from, to, InteriorPortalFilter{});
	}
	bool SegmentPassesPortalModel(Vector3Par from, Vector3Par to, const InteriorPortalFilter &filter) const;

	/// Conservative distance from a point in `roomId` to the nearest room
	/// boundary, capped by `maxDistance`. Used to keep the drawn radius of a
	/// soft smoke billboard inside the room even before its centre touches a
	/// wall or ceiling. Returns 0 for a stale/outside point.
	float ClearanceModel(Vector3Par pos, int roomId, float maxDistance) const;

	/// One simulation step inside `roomId`: from -> to. See InteriorStepResult.
	InteriorStepResult ContainedStepModel(Vector3Par from, Vector3Par to, int roomId) const;

	// -- world-space conveniences (transform through `obj`) ------------------
	int RoomOfWorld(Object const* obj, Vector3Par pos) const;
	InteriorStepResult ContainedStepWorld(Object const* obj, Vector3Par from, Vector3Par to, int roomId) const;

	/// Dev overlay: room cells as coloured points, portals as markers (green
	/// door, cyan window, red breach). Draws through GScene; safe to call
	/// during draw with GScene non-null. This tool stays in the codebase on
	/// purpose -- it doubles as a fire-geometry QA view.
	void DebugDraw(Object const* obj) const;

private:
	friend struct BuildingInteriorTestAccess;
	void Build(BuildingType const* type);

	bool _valid = false;
	int _roomCount = 0;
	AutoArray<InteriorPortal> _portals;

	// Voxel grid over the shell's bounding box, model space. Cell labels:
	// 0xFE solid (inside fire or view geometry), 0xFF free air reached by no
	// room (= outside), 0..N-1 room ids.
	Vector3 _gridMin{VZero};
	float _cell = 0.5f;
	int _dim[3] = {0, 0, 0};
	std::vector<unsigned char> _labels;
	std::vector<int> _roofTop;
	void BuildRoofColumns();

	int CellIndex(int x, int y, int z) const { return (x * _dim[1] + y) * _dim[2] + z; }
	bool CellSolid(int x, int y, int z) const
	{
		return x < 0 || y < 0 || z < 0 || x >= _dim[0] || y >= _dim[1] || z >= _dim[2] ||
			   _labels[CellIndex(x, y, z)] == 0xFE;
	}
	int CellLabel(int x, int y, int z) const
	{
		if (x < 0 || y < 0 || z < 0 || x >= _dim[0] || y >= _dim[1] || z >= _dim[2])
		{
			return -1;
		}
		const unsigned char label = _labels[CellIndex(x, y, z)];
		return label == 0xFE || label == 0xFF ? -1 : static_cast<int>(label);
	}
};

/// Resolve the building containing `pos` (fire geometry first, view geometry as
/// fallback) and draw its interior overlay. Cheap enough to call once a frame
/// from a debug path; does nothing outdoors.
void DebugDrawInteriorNear(Vector3Par pos);

} // namespace Poseidon
