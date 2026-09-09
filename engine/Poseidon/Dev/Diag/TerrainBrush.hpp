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
struct RocketCraterSettings
{
    bool enabled = false;
    float radius = 12.0f;
    float depth = 1.0f;
};
RocketCraterSettings& RocketCraters();
void ResetRocketCraters();
bool RocketTerrainImpact(float x, float y, float z);
}
