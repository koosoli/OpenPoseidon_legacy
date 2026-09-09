#pragma once

namespace Poseidon::Dev
{
struct TerrainBrushSettings
{
    bool enabled = false;
    bool raise = false;
    float radius = 64.0f;
    float metresPerSecond = 4.0f;
};
TerrainBrushSettings& TerrainBrush();
bool PaintTerrain(float x, float z, float radius, float delta);
bool PaintTerrainAtPixel(float x, float y, float seconds);
void ResetTerrainBrush();
}
