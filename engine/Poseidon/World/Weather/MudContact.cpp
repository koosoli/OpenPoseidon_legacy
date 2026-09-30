#include <Poseidon/World/Weather/MudContact.hpp>
#include <Poseidon/World/Weather/MudField.hpp>
#include <Poseidon/World/Weather/SnowField.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <cstdlib>

namespace Poseidon
{
bool StampMudFootstep(const Landscape& land, Object& actor,
                      Vector3Par foot, Vector3Par direction)
{
    auto& mud = GMud();
    if (!mud.ContactActive() || !std::isfinite(foot.X()) || !std::isfinite(foot.Y()) ||
        !std::isfinite(foot.Z()) || !std::isfinite(direction.X()) || !std::isfinite(direction.Z())) return false;
    const float length = std::hypot(direction.X(), direction.Z());
    if (length < 0.001f || !std::isfinite(length)) return false;

    // Native/A3 masks may mix soft and hard layers inside one palette tile.
    // Until their exact point-mask producer is available, fail closed rather
    // than repurpose an inherited dirt texture or a representative tile label.
    if (land.GetEnfusionSurfaceCount() != 0) return false;
    const float extent = land.GetLandRange() * land.GetLandGrid();
    constexpr float guard = 0.4f; // entire boot kernel plus bilinear support
    for (int z = -1; z <= 1; ++z)
        for (int x = -1; x <= 1; ++x)
        {
            const float east = foot.X() + x * guard, north = foot.Z() + z * guard;
            if (!(east >= 0 && north >= 0 && east < extent && north < extent)) return false;
            const int id = land.GetTexture(int(std::floor(north / land.GetLandGrid())),
                                           int(std::floor(east / land.GetLandGrid())));
            if (land.GetA3TerrainMaterial(id) != nullptr) return false;
            const auto& surface = land.SurfaceAt(east, north);
            if (!StockMudSoil(surface._class.Data(), surface._name.Data(),
                              surface._soundEnv.Data(), surface._character.Data())) return false;
        }
    float dx, dz;
    const float ground = land.SurfaceY(foot.X(), foot.Z(), &dx, &dz);
    if (!std::isfinite(ground) || ground <= land.GetSeaLevel() + 0.15f ||
        !std::isfinite(dx) || !std::isfinite(dz) || std::hypot(dx, dz) > 0.7f ||
        std::abs(foot.Y() - ground) > 0.25f || GSnow().BaseDepthAt(foot.X(), foot.Z()) > 0.002f) return false;
    Object* roadway = nullptr;
    const float support = land.RoadSurfaceY(foot + Vector3(0, 0.5f, 0), nullptr, nullptr, nullptr, &roadway);
    if (roadway != nullptr || !std::isfinite(support) || support > ground + 0.05f) return false;

    // A view roof can shelter even if its fire geometry is empty/bullet-permeable.
    // Keep the footprint store out of dry soil beneath a tent or building.
    CollisionBuffer hits;
    const Vector3 origin = foot + Vector3(0, 0.2f, 0);
    land.ObjectCollision(hits, nullptr, &actor, origin, origin + Vector3(0, 30, 0), 0.05f, ObjIntersectView);
    if (hits.Size() != 0) return false;

    const Vector3 axis(direction.X() / length, 0, direction.Z() / length);
    const Vector3 toe = foot + axis * 0.075f, heel = foot - axis * 0.075f;
    const float depth = mud.ContactDepth();
    const bool toeChanged = mud.Stamp(toe.X(), toe.Z(), 0.125f, depth);
    const bool heelChanged = mud.Stamp(heel.X(), heel.Z(), 0.125f, depth);
    const bool changed = toeChanged || heelChanged;
    static const bool trace = [] {
        const char* value = std::getenv("POSEIDON_MUD_TRACE");
        return value && value[0] == '1';
    }();
    if (trace && changed)
        LOG_INFO(Graphics, "MUD_STEP x={:.3f} z={:.3f} surface=Field files=pol sound=dirt wet={:.4f} depth={:.4f} revision={} chunks={}",
                 foot.X(), foot.Z(), mud.Wetness(), depth, mud.Revision(), mud.Chunks());
    return changed;
}
} // namespace Poseidon
