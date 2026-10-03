#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Poseidon::Dev
{
enum class ExcavationTool
{
    Cave,
    InfantryTrench,
    TankTrench
};
struct CaveEditorSettings
{
    bool enabled = false;
    ExcavationTool tool = ExcavationTool::Cave;
    float width = 3.0f;
    float height = 2.5f;
    float length = 16.0f;
    float floorOffset = 0.0f;
};
CaveEditorSettings& CaveEditor();
void SetExcavationTool(ExcavationTool tool);
const char* ExcavationToolName();
bool CreateEditorTrench(float x, float y, float z, float heading, float width, float depth, float length, bool tank);
// XYZ metres, heading in degrees clockwise from north. The entrance is at XYZ,
// the interior extends horizontally along heading. No terrain samples change.
bool CreateEditorCave(float x, float y, float z, float heading, float width, float height, float length);
bool CreateEditorCaveAtPixel(float x, float y);
bool RemoveLastEditorCave();
void ClearEditorCaves();
std::size_t EditorCaveCount();
const char* EditorCaveStatus();
void ResizeEditorCave(float wheelSteps);
struct EditorCaveInfo
{
    std::uint32_t id = 0;
    int objectId = -1;
    ExcavationTool tool = ExcavationTool::Cave;
    std::string sourceName;
    float x = 0, y = 0, z = 0, heading = 0, width = 0, height = 0, length = 0;
};
// Generated records only. IDs survive rotation and never mean a list index.
std::vector<EditorCaveInfo> EditorCaves();
std::uint32_t SelectedEditorCaveId();
bool SelectEditorCave(std::uint32_t id);
bool RotateEditorCave(std::uint32_t id, float heading);
bool DeleteEditorCave(std::uint32_t id);
// CPU geometry helpers shared by admission/occupancy and focused tests.
bool EditorCaveFootprintContains(const EditorCaveInfo& info, float x, float z);
bool EditorCaveFootprintsOverlap(const EditorCaveInfo& a, const EditorCaveInfo& b);
bool EditorCaveSupportOccupied(const EditorCaveInfo& info, float minY, float rawTerrainY, bool newFootprint = false);
} // namespace Poseidon::Dev
