#pragma once

namespace Poseidon::Dev
{
struct TerrainBrushSettings
{
    bool enabled = false;
    bool raise = false;
    bool showRadius = true;
    float radius = 8.0f;
    float metresPerSecond = 4.0f;
};
TerrainBrushSettings& TerrainBrush();
struct TerrainBrushTarget
{
    float x = 0, y = 0, z = 0, radius = 0;
    bool editable = false;
};
bool TerrainBrushTargetAtPixel(float x, float y, TerrainBrushTarget& target);
bool PaintTerrain(float x, float z, float radius, float delta);
bool PaintTerrainAtPixel(float x, float y, float seconds, bool invert = false, bool fast = false);
void ResetTerrainBrush();
bool CaptureTerrainEditBaseline(int x, int z, int width, int height);
bool HasTerrainEditBaseline();
bool RestoreEditedTerrain();
void ClearTerrainEditBaseline();
struct RocketCraterSettings
{
    bool enabled = true;
    float radius = 12.0f;
    float depth = 1.0f;
};
RocketCraterSettings& RocketCraters();
void ResetRocketCraters();
bool RocketTerrainImpact(float x, float y, float z, float indirectHit);
}
