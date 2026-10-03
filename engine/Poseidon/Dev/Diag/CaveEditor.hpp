#pragma once
#include <cstddef>

namespace Poseidon::Dev
{
struct CaveEditorSettings
{
    bool enabled = false;
    float width = 3.0f;
    float height = 2.5f;
    float length = 16.0f;
    float floorOffset = 0.0f;
};
CaveEditorSettings& CaveEditor();
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
