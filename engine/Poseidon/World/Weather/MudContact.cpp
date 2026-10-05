#include <Poseidon/World/Weather/MudContact.hpp>
#include <Poseidon/World/Weather/MudField.hpp>
#include <Poseidon/World/Weather/SnowField.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <cstdlib>

namespace Poseidon
{
static bool AdmitMudContact(const Landscape& land, Object& actor, Vector3Par foot, float tolerance)
{
    auto& mud = GMud();
    if (!mud.ContactActive() || !std::isfinite(foot.X()) || !std::isfinite(foot.Y()) ||
        !std::isfinite(foot.Z())) return false;

    // Native/A3 masks may mix soft and hard layers inside one palette tile.
    // Until their exact point-mask producer is available, fail closed rather
    // than repurpose an inherited dirt texture or a representative tile label.
    if (land.GetEnfusionSurfaceCount() != 0) return false;
    const float landGrid = land.GetLandGrid();
    if (!std::isfinite(landGrid) || landGrid <= 0.0f) return false;
    const float extent = land.GetLandRange() * landGrid;
    constexpr float guard = MudField::MaxContactRadius + 0.075f + MudField::CellSize;
    for (int z = -1; z <= 1; ++z)
        for (int x = -1; x <= 1; ++x)
        {
            const float east = foot.X() + x * guard, north = foot.Z() + z * guard;
            if (!(east >= 0 && north >= 0 && east < extent && north < extent)) return false;
            const int cellX = int(std::floor(east / landGrid)), cellZ = int(std::floor(north / landGrid));
            const int id = land.GetTexture(cellZ, cellX);
            if (land.GetA3TerrainMaterial(id) != nullptr) return false;
            const auto& surface = land.SurfaceAt(east, north);
            if (!StockMudSoil(surface._class.Data(), surface._name.Data(),
                              surface._soundEnv.Data(), surface._character.Data()))
            {
                const Texture* texture = land.GetTexture(id);
                if (!texture || !StockCultivatedMudSoil(texture->GetName().Data(),
                    surface._class.Data(), surface._name.Data(), surface._soundEnv.Data(), surface._character.Data(),
                    east / landGrid - cellX, north / landGrid - cellZ)) return false;
            }
        }
    float dx, dz;
    const float ground = land.SurfaceY(foot.X(), foot.Z(), &dx, &dz);
    // SurfaceY already includes stored mud. Source slope remains the original
    // ground slope so a previous depression cannot veto its own next contact.
    const auto mudGradient = mud.HeightGradientAt(foot.X(), foot.Z());
    dx -= mudGradient[0]; dz -= mudGradient[1];
    if (!std::isfinite(ground) || ground <= land.GetSeaLevel() + 0.15f ||
        !std::isfinite(dx) || !std::isfinite(dz) || std::hypot(dx, dz) > 0.7f ||
        std::abs(foot.Y() - ground) > tolerance || GSnow().BaseDepthAt(foot.X(), foot.Z()) > 0.002f) return false;
    Object* roadway = nullptr;
    const float support = land.RoadSurfaceY(foot + Vector3(0, 0.5f, 0), nullptr, nullptr, nullptr, &roadway);
    if (roadway != nullptr || !std::isfinite(support) || support > ground + 0.05f) return false;

    // A view roof can shelter even if its fire geometry is empty/bullet-permeable.
    // Keep the footprint store out of dry soil beneath a tent or building.
    CollisionBuffer hits;
    const Vector3 origin = foot + Vector3(0, 0.2f, 0);
    land.ObjectCollision(hits, nullptr, &actor, origin, origin + Vector3(0, 30, 0), 0.05f, ObjIntersectView);
    if (hits.Size() != 0) return false;
    return true;
}

bool AdmitMudWheelContact(const Landscape& land, Object& actor, Vector3Par point)
{
    return AdmitMudContact(land, actor, point, 0.5f);
}

bool StampMudWheelContact(const Landscape& land, Object& actor, Vector3Par point, MudWheelProfile profile)
{
    // The car calls this only for an identified wheel with actual solid-ground
    // collision. Allow its penetration tolerance; still reject sea/roads/roofs.
    if (!profile.enabled || !AdmitMudWheelContact(land, actor, point)) return false;
    auto& mud = GMud();
    const float depth = std::min(profile.maxDepth, mud.ContactDepth() * profile.depthScale);
    const bool changed = mud.Stamp(point.X(), point.Z(), MudField::ContactRadius(depth), depth, 0.8f);
    static const bool trace = [] {
        const char* value = std::getenv("POSEIDON_MUD_TRACE");
        return value && value[0] == '1';
    }();
    if (trace && changed)
        LOG_INFO(Physics, "MUD_WHEEL x={:.3f} z={:.3f} wet={:.4f} capacity={:.4f} stored={:.4f} revision={}",
                 point.X(), point.Z(), mud.Wetness(), depth,
                 mud.HeightOffsetAt(point.X(), point.Z()), mud.Revision());
    // Admission, not mutation: a saturated rut remains a valid trail anchor.
    return true;
}

bool StampMudFootstep(const Landscape& land, Object& actor,
                      Vector3Par foot, Vector3Par direction)
{
    if (!std::isfinite(direction.X()) || !std::isfinite(direction.Z())) return false;
    const float length = std::hypot(direction.X(), direction.Z());
    if (length < 0.001f || !std::isfinite(length) || !AdmitMudContact(land, actor, foot, 0.25f)) return false;
    auto& mud = GMud();
    const float landGrid = land.GetLandGrid();

    const Vector3 axis(direction.X() / length, 0, direction.Z() / length);
    const Vector3 toe = foot + axis * 0.075f, heel = foot - axis * 0.075f;
    const float depth = mud.ContactDepth();
    const bool toeChanged = mud.CompressBoot(toe.X(), toe.Z());
    const bool heelChanged = mud.CompressBoot(heel.X(), heel.Z());
    const bool changed = toeChanged || heelChanged;
    static const bool trace = [] {
        const char* value = std::getenv("POSEIDON_MUD_TRACE");
        return value && value[0] == '1';
    }();
    if (trace && changed)
    {
        const auto& source = land.SurfaceAt(foot.X(), foot.Z());
        const int id = land.GetTexture(int(std::floor(foot.Z() / landGrid)), int(std::floor(foot.X() / landGrid)));
        const Texture* texture = land.GetTexture(id);
        LOG_INFO(Graphics, "MUD_STEP x={:.3f} z={:.3f} surface={} files={} sound={} texture={} wet={:.4f} depth={:.4f} revision={} chunks={} layer={:.4f} stored={:.4f} radius={:.4f}",
                 foot.X(), foot.Z(), source._class.Data(), source._name.Data(), source._soundEnv.Data(),
                 texture ? texture->GetName().Data() : "", mud.Wetness(), depth, mud.Revision(), mud.Chunks(),
                 mud.LayerDepth(), mud.HeightOffsetAt(foot.X(), foot.Z()), MudField::ContactRadius(depth));
    }
    return changed;
}
} // namespace Poseidon
