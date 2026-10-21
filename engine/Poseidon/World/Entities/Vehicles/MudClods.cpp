#include <Poseidon/World/Entities/Vehicles/MudClods.hpp>

#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/World.hpp>
#include <Random/randomGen.hpp>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>

#include <array>
#include <cmath>

void RegisterVBShape(LODShapeWithShadow* shape);

namespace Poseidon
{
DEFINE_FAST_ALLOCATOR(MudClod)

namespace
{
int GMudClodsAlive = 0;

VehicleNonAIType* MudClodType()
{
    static Ref<VehicleNonAIType> type;
    if (!type) type = VehicleTypes.New("smoke"); // same plain non-AI type as cloudlets
    return type;
}

// Lumpy unit blob: an icosahedron with every vertex pushed in or out a little,
// smooth normals so it reads as soft wet soil rather than a cut stone.
// `variant` picks one of a few fixed shapes; `texture` is its mud colour.
constexpr int ClodVariants = 4;

LODShapeWithShadow* BuildClodShape(Texture* texture, int variant)
{
    static const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    const Vector3 base[12] = {
        Vector3(-1, t, 0), Vector3(1, t, 0), Vector3(-1, -t, 0), Vector3(1, -t, 0),
        Vector3(0, -1, t), Vector3(0, 1, t), Vector3(0, -1, -t), Vector3(0, 1, -t),
        Vector3(t, 0, -1), Vector3(t, 0, 1), Vector3(-t, 0, -1), Vector3(-t, 0, 1)};
    static const int tris[20][3] = {
        {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6},
        {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10},
        {8, 6, 7}, {9, 8, 1}};

    Ref<LODShapeWithShadow> lShape = new LODShapeWithShadow();
    Shape* shape = new Shape;
    lShape->AddShape(shape, 0);
    shape->ReallocTable(12);
    unsigned seed = 2166136261u ^ unsigned(variant * 7919 + 13);
    auto next = [&seed]() {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        return float(seed & 0xffffu) / 65535.0f;
    };
    // Squashed a little (clods are flatter than balls) and lumpy.
    const float squash = 0.7f + 0.15f * next();
    for (int i = 0; i < 12; ++i)
    {
        Vector3 p = base[i].Normalized();
        const float bump = 0.75f + 0.45f * next();
        p = Vector3(p.X() * bump, p.Y() * bump * squash, p.Z() * bump);
        shape->SetPos(i) = p;
        shape->SetNorm(i) = p.Normalized();
        shape->SetClip(i, ClipAll);
        const float u = 0.03f * next(), v = 0.03f * next();
        if (texture)
            shape->SetUV(i, texture->UToPhysical(u), texture->VToPhysical(v));
        else
            shape->SetUV(i, u, v);
    }
    shape->CalculateHints();
    lShape->CalculateHints();
    for (const auto& tri : tris)
    {
        Poly face;
        face.Init();
        face.SetN(3);
        // Shape faces wind the other way round from the icosahedron table.
        face.Set(0, tri[2]);
        face.Set(1, tri[1]);
        face.Set(2, tri[0]);
        face.SetTexture(texture);
        face.SetSpecial(NoShadow | IsColored);
        shape->AddFace(face);
    }
    shape->OrSpecial(NoShadow | IsColored);
    lShape->OrSpecial(NoShadow | IsColored);
    lShape->CalculateMinMax(true);
    shape->FindSections();
    shape->ConvertToVBuffer(VBStatic);
    RegisterVBShape(lShape); // released/restored with the other dynamic shapes
    lShape->AddRef();        // kept for the whole run in the cache below
    return lShape;
}

// Wet mud colours. The first version wore the ground's own texture, but field
// textures are mostly crop and grass, so clods came out green. These are flat
// procedural colours (generated from the name, no file needed), lit by the
// lumpy normals: dark wet brown, mid brown, reddish clay, grey-brown silt.
constexpr int ClodColours = 4;
const char* const ClodColourNames[ClodColours] = {
    "#(argb,8,8,3)color(0.20,0.14,0.09,1)",
    "#(argb,8,8,3)color(0.27,0.19,0.12,1)",
    "#(argb,8,8,3)color(0.30,0.18,0.10,1)",
    "#(argb,8,8,3)color(0.25,0.21,0.16,1)",
};

LODShapeWithShadow* ClodShape(int colour, int variant)
{
    static std::array<std::array<LODShapeWithShadow*, ClodVariants>, ClodColours> cache{};
    static std::array<Ref<Texture>, ClodColours> textures{};
    colour = std::clamp(colour, 0, ClodColours - 1);
    variant = std::clamp(variant, 0, ClodVariants - 1);
    if (!textures[colour]) textures[colour] = GlobLoadTexture(ClodColourNames[colour]);
    auto& shape = cache[colour][variant];
    if (!shape) shape = BuildClodShape(textures[colour], variant);
    return shape;
}

float Random01() { return GRandGen.RandomValue(); }
} // namespace

MudClod::MudClod(LODShapeWithShadow* shape, const MudClodState& state)
    : Vehicle(shape, MudClodType(), -1), _state(state)
{
    Object::_type = TypeTempVehicle;
    ++GMudClodsAlive;
    // (The wgpu renderer ignores constant colour on lit meshes; the colour is in
    // the texture. Kept for the GL33 path.)
    const float shade = 0.85f + 0.15f * Random01();
    SetConstantColor(PackedColor(Color(shade, shade, shade, 1.0f)));
    Simulate(0.0f, SimulateCamera);
}

MudClod::~MudClod()
{
    --GMudClodsAlive;
}

void MudClod::Simulate(float deltaT, SimulationImportance prec)
{
    const float ground = GLandscape ? GLandscape->SurfaceY(_state.px, _state.pz) : -1e6f;
    StepMudClod(_state, deltaT, ground);
    if (MudClodExpired(_state))
    {
        SetDelete();
        return;
    }
    const float scale = _state.radius * MudClodVisibleScale(_state);
    if (scale <= 0.002f)
    {
        SetDelete();
        return;
    }
    // Tumbling in flight; once landed it keeps the pose it hit the ground in.
    Matrix3 rotY, rotX;
    rotY.SetRotationY(_state.spin * 0.7f + _state.radius * 900.0f);
    rotX.SetRotationX(_state.spin);
    Matrix4 trans;
    trans.SetIdentity();
    trans.SetOrientation(rotY * rotX * scale);
    trans.SetPosition(Vector3(_state.px, _state.py, _state.pz));
    SetTransform(trans);
}

int MudClodEmitter::Take(float rate, float deltaT)
{
    if (!std::isfinite(rate) || !std::isfinite(deltaT) || rate <= 0.0f || deltaT <= 0.0f)
    {
        carry = 0.0f;
        return 0;
    }
    carry += rate * deltaT;
    const int whole = int(std::floor(carry));
    carry -= float(whole);
    return std::min(whole, 12);
}

void SpawnMudClods(Vector3Par contact, Vector3Par forward, Vector3Par velocity, float driveSign,
                   float groundSpeed, float spin, int count)
{
    if (count <= 0 || !GLandscape || !GLOB_WORLD || !GLOB_SCENE || !GLOB_SCENE->GetCamera()) return;
    // Clods are small; past ~120 m nobody sees them.
    if (contact.Distance2(GLOB_SCENE->GetCamera()->Position()) > 120.0f * 120.0f) return;
    for (int i = 0; i < count && GMudClodsAlive < MudClodMaxAlive; ++i)
    {
        const float r[5] = {Random01(), Random01(), Random01(), Random01(), Random01()};
        const MudClodLaunch launch = PlanMudClodLaunch(forward.X(), forward.Z(), driveSign, velocity.X(),
            velocity.Y(), velocity.Z(), groundSpeed, spin, r);
        MudClodState state;
        // Leaves the top rear of the tyre: a little behind and above the contact patch.
        const float behind = -(driveSign < 0.0f ? -1.0f : 1.0f) * (0.25f + 0.2f * Random01());
        state.px = contact.X() + forward.X() * behind + (Random01() - 0.5f) * 0.25f;
        state.pz = contact.Z() + forward.Z() * behind + (Random01() - 0.5f) * 0.25f;
        state.py = contact.Y() + 0.25f + 0.35f * Random01();
        state.vx = launch.vx; state.vy = launch.vy; state.vz = launch.vz;
        state.radius = launch.radius;
        state.life = launch.life;
        state.spinRate = launch.spinRate;
        state.spin = Random01() * 6.28f;
        const int variant = int(Random01() * ClodVariants) % ClodVariants;
        const int colour = int(Random01() * ClodColours) % ClodColours;
        GLOB_WORLD->AddCloudlet(new MudClod(ClodShape(colour, variant), state));
    }
}
} // namespace Poseidon
