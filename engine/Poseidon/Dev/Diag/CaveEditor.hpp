#pragma once
#include <cstddef>

namespace Poseidon::Dev
{
enum class ExcavationTool { Cave, InfantryTrench, TankTrench };
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
}
