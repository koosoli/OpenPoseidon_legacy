#include <Poseidon/World/Weather/SandContact.hpp>
#include <Poseidon/World/Weather/SandField.hpp>
#include <Poseidon/World/Weather/SnowField.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <cmath>
#include <cstdlib>

namespace Poseidon
{
bool StampSandFootstep(const Landscape& land, Object& actor,
                       Vector3Par foot, Vector3Par direction)
{
    (void)actor; // Roofs alone do not stop compaction of actual dry sand below.
    auto& sand = GSand();
    if (!sand.ContactActive() || !std::isfinite(foot.X()) || !std::isfinite(foot.Y()) ||
        !std::isfinite(foot.Z()) || !std::isfinite(direction.X()) || !std::isfinite(direction.Z())) return false;
    if (land.GetEnfusionSurfaceCount() != 0) return false;
    const float grid = land.GetLandGrid();
    if (!std::isfinite(grid) || grid <= 0.0f) return false;
    const float extent = land.GetLandRange() * grid;
    if (!(foot.X() >= 0 && foot.Z() >= 0 && foot.X() < extent && foot.Z() < extent)) return false;
    const auto& centre = land.SurfaceAt(foot.X(), foot.Z());
    if (!StockSandSurface(centre._class.Data(), centre._name.Data(),
                          centre._soundEnv.Data(), centre._character.Data())) return false;
    int checkedX[9], checkedZ[9], checked = 0;
    constexpr float guard = 0.6f; // capsule/rim 41.5cm + one 12.5cm bilinear cell
    for (int z = -1; z <= 1; ++z)
        for (int x = -1; x <= 1; ++x)
        {
            const float east = foot.X() + x * guard, north = foot.Z() + z * guard;
            if (!(east >= 0 && north >= 0 && east < extent && north < extent)) return false;
            const int cx = int(std::floor(east / grid)), cz = int(std::floor(north / grid));
            const int id = land.GetTexture(cz, cx);
            if (land.GetA3TerrainMaterial(id) != nullptr ||
                !SandSourceInterior(east / grid - cx, north / grid - cz)) return false;
            // The raised rim and bilinear support also belong to sand. Do not
            // stamp across a road, snow edge or submerged shore merely because
            // the centre of the boot still has admissible support.
            const float supportGround = land.SurfaceY(east, north);
            if (!std::isfinite(supportGround) || supportGround <= land.GetSeaLevel() + 0.02f ||
                GSnow().BaseDepthAt(east, north) > 0.002f) return false;
            Object* supportRoadway = nullptr;
            const float roadHeight = land.RoadSurfaceY(Vector3(east, supportGround + 0.5f, north),
                nullptr, nullptr, nullptr, &supportRoadway);
            if (supportRoadway != nullptr || !std::isfinite(roadHeight) || roadHeight > supportGround + 0.05f) return false;
            bool alreadyChecked = false;
            for (int i = 0; i < checked; ++i)
                if (checkedX[i] == cx && checkedZ[i] == cz) alreadyChecked = true;
            if (alreadyChecked) continue;
            checkedX[checked] = cx; checkedZ[checked++] = cz;
            // A cached point quadrant is not proof that the remaining tile is
            // pure sand. Check all four exact quadrants, avoiding mixed tiles.
            for (const float v : {0.25f, 0.75f})
                for (const float u : {0.25f, 0.75f})
                {
                    const auto& source = land.SurfaceAt((cx + u) * grid, (cz + v) * grid);
                    if (!StockSandSurface(source._class.Data(), source._name.Data(),
                                          source._soundEnv.Data(), source._character.Data())) return false;
                }
        }
    float dx, dz;
    const float ground = land.SurfaceY(foot.X(), foot.Z(), &dx, &dz);
    const auto sandGradient = sand.HeightGradientAt(foot.X(), foot.Z());
    dx -= sandGradient[0]; dz -= sandGradient[1];
    if (!std::isfinite(ground) || ground <= land.GetSeaLevel() + 0.02f ||
        !std::isfinite(dx) || !std::isfinite(dz) || std::hypot(dx, dz) > 0.7f ||
        std::abs(foot.Y() - ground) > 0.25f || GSnow().BaseDepthAt(foot.X(), foot.Z()) > 0.002f) return false;
    const bool changed = sand.StampBoot(foot.X(), foot.Z(), direction.X(), direction.Z(),
                                       sand.ContactDepth(), sand.ContactRim());
    static const bool trace = [] {
        const char* value = std::getenv("POSEIDON_SAND_TRACE");
        return value && value[0] == '1';
    }();
    if (trace && changed)
    {
        const auto& source = land.SurfaceAt(foot.X(), foot.Z());
        const int id = land.GetTexture(int(std::floor(foot.Z() / grid)), int(std::floor(foot.X() / grid)));
        const Texture* texture = land.GetTexture(id);
        LOG_INFO(Graphics, "SAND_STEP x={:.3f} z={:.3f} surface={} files={} sound={} texture={} wet={:.4f} depth={:.4f} rim={:.4f} revision={} chunks={}",
                 foot.X(), foot.Z(), source._class.Data(), source._name.Data(), source._soundEnv.Data(),
                 texture ? texture->GetName().Data() : "", sand.Wetness(), sand.ContactDepth(), sand.ContactRim(),
                 sand.Revision(), sand.Chunks());
    }
    return changed;
}
} // namespace Poseidon
