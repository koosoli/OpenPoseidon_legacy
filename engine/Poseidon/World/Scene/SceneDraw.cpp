#include <Poseidon/Core/Application.hpp>
#include <Poseidon/World/Weather/SnowField.hpp>
#include <Poseidon/Foundation/Algorithms/RadixSort.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Input/InputSubsystem.hpp>
#include <SDL3/SDL_scancode.h>
#include <Random/randomGen.hpp>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <cmath>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Containers/BankArray.hpp>
#include <Poseidon/Foundation/Containers/StaticArray.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Math/MathDefs.hpp>
#include <Poseidon/Foundation/Math/MathOpt.hpp>
#include <Poseidon/Foundation/Memory/CheckMem.hpp>
#include <algorithm>
#include <Poseidon/World/Scene/BenchmarkLodFix.hpp>
#include <Poseidon/World/Scene/ObjectDrawRange.hpp>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using Poseidon::Foundation::IsOutOfMemory;
using Poseidon::Foundation::MStorage;
using Poseidon::Foundation::Time;
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/Dev/Diag/LightingDiag.hpp>
#include <Poseidon/World/Scene/Pass1Submit.hpp>
#include <Poseidon/Foundation/Platform/AppConfig.hpp>
#include <Poseidon/UI/Settings/GameSettingsConfig.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/Graphics/Textures/TexturePreload.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Scene/SurfaceDrawOrder.hpp>
#include <Poseidon/World/Scene/InsideViewPass.hpp>
#include <Poseidon/World/Scene/AlphaSortOrder.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/Graphics/Rendering/Primitives/Poly.hpp>
// MAT-048: `render::GBlendSectionDepthReadOnly`, set around the object-blend branch in
// DrawObjectsAndShadowsPass2.  Previously reached only transitively (via RenderFlags.hpp for
// `render::IsOnSurfaceSpec`); named explicitly now that this TU writes into the header's state.
#include <Poseidon/Graphics/Rendering/BuildRenderPassDescriptor.hpp>
#include <Poseidon/Graphics/Shadow/ShadowMath.hpp>
#include <Poseidon/World/Simulation/Animation/Animation.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/AI/VehicleAI.hpp>
#include <Poseidon/Graphics/Rendering/Primitives/ClipVert.hpp>
#include <Poseidon/World/Entities/Weapons/Shots.hpp>
#include <Poseidon/World/Scene/ObjLine.hpp>
#include <Poseidon/World/Scene/ObjectClasses.hpp>
#include <Poseidon/World/Simulation/FrameInv.hpp>
#include <Poseidon/AI/AI.hpp> // remove dependency
#include <Poseidon/World/World.hpp>
#include <Poseidon/Foundation/Algorithms/Qsort.hpp>
#include <Poseidon/UI/Locale/StringtableExt.hpp>
#include <time.h>
#include <Poseidon/Dev/Diag/DiagModes.hpp>
#include <Poseidon/World/Terrain/Occlusion.hpp>
#include <Poseidon/World/Weather/RainVolume.hpp>

using namespace Poseidon;

#define DRAW_OBJS 1

namespace Poseidon
{
extern bool EnableObjOcc;
SRef<Occlusion>& GetOcclusions();
} // namespace Poseidon

typedef Ref<SortObject> SortObjectItem;

static int CmpRevDistObj(const SortObjectItem* p1, const SortObjectItem* p0)
{
    // the first object is the nearest one
    const SortObject* o0 = *p0;
    const SortObject* o1 = *p1;
    Coord dif = o0->distance2 - o1->distance2;
    if (dif < 0)
    {
        return -1;
    }
    if (dif > 0)
    {
        return +1;
    }
    return 0;
}

// far-to-near by camera-space depth, for the alpha pass (see AlphaSortOrder.hpp)
static int CmpRevAlphaSortObj(const SortObjectItem* p1, const SortObjectItem* p0)
{
    return AlphaSort::CompareAlphaDepth((*p1)->zCoord, (*p0)->zCoord);
}

static int CmpShapeObj(const SortObjectItem* p1, const SortObjectItem* p2)
{
    const SortObject* o1 = *p1;
    const SortObject* o2 = *p2;

    // first sort by pass
    int sDif = o1->passNum - o2->passNum;
    if (sDif)
    {
        return sDif;
    }

    LODShape* s1 = o1->object->GetShape();
    LODShape* s2 = o2->object->GetShape();
    sDif = (intptr_t)s2 - (intptr_t)s1;
    // no invisible LODs here
    if (sDif)
    {
        // Shape complexity (Level(0)->NFaces()) precomputed in AdjustComplexity.
        // sort by shape complexity - first draw simple shapes
        int cDiff = o1->sortComplexity - o2->sortComplexity;
        if (cDiff)
        {
            return cDiff;
        }
        return sDif;
    }
    // sort by LOD - fine LODs (small LOD numbers) last
    sDif = o2->drawLOD - o1->drawLOD;
    if (sDif)
    {
        return sDif;
    }
    // first draw
    // same shape sort by distance, back first (helps to alpha transparency)
    Coord fDif = o2->distance2 - o1->distance2;
    if (fDif < 0)
    {
        return -1;
    }
    if (fDif > 0)
    {
        return +1;
    }
    return 0;
}

// Packed-key object sort. The Pass1/Pass2 object sorts were memory-latency bound:
// sorting the Ref<SortObject> array made every comparison chase two scattered
// SortObjects (plus their Objects, for GetShape()) — several cold cache lines per
// compare, so a comparator doing nothing but int subtractions cost hundreds of ms
// exclusive. SortListByPackedKey copies the compared fields into a contiguous KeyT
// array (one sequential pass), sorts that, then reorders the Ref list to match.
//
// KeyT must carry a `SortObject* obj` payload. QSortWithContext takes the comparator
// as a template functor, so the (non-capturing) comparator lambda INLINES into the
// sort loop — no per-comparison function-pointer call (which dominated the plain-QSort
// cost). The reorder uses SetRef (raw-pointer assignment, no AddRef/Release): it's a
// permutation, so the multiset of held SortObjects is unchanged and each is still
// Released exactly once when the list is cleared — zero refcount traffic. The algorithm
// is QSort's, so as long as the comparator body matches, the order is byte-identical.
template <class KeyT, class ListT, class Extract, class Compare>
static void SortListByPackedKey(ListT& list, Extract extract, Compare comp)
{
    const int n = list.Size();
    if (n < 2)
    {
        return;
    }
    // Render-thread-only scratch, reused across frames (grows once; one static per call site).
    static AutoArray<KeyT> keys;
    keys.Resize(n);
    for (int i = 0; i < n; i++)
    {
        keys[i] = extract(list[i]);
    }
    Foundation::QSortWithContext(keys.Data(), n, 0, comp);
    for (int i = 0; i < n; i++)
    {
        list[i].SetRef(keys[i].obj);
    }
}

// Pass1 shape-sort key — see CmpShapeObj for the ordering rationale.
struct DrawKey
{
    SortObject* obj;   // payload (non-owning; the owning Ref stays in _drawMergers)
    const void* shape; // o->object->GetShape() — primary grouping key
    int passNum;
    int complexity; // o->sortComplexity (precomputed in AdjustComplexity)
    int drawLOD;
    float distance2;
};

// Sort _drawMergers into CmpShapeObj order. The comparator body is a verbatim copy of
// CmpShapeObj's ordering over the packed keys, so the order stays byte-identical to
// QSort(..., CmpShapeObj) (Pass2's unstable-sort tie-breaking depends on it).
static void SortDrawMergersByShape(SortObjectList& mergers)
{
    SortListByPackedKey<DrawKey>(
        mergers,
        [](SortObject* o) -> DrawKey
        {
            return {o,          o->object ? static_cast<const void*>(o->object->GetShape()) : nullptr,
                    o->passNum, o->sortComplexity,
                    o->drawLOD, o->distance2};
        },
        [](const DrawKey* p1, const DrawKey* p2, int) -> int
        {
            int sDif = p1->passNum - p2->passNum;
            if (sDif)
            {
                return sDif;
            }
            sDif = (intptr_t)p2->shape - (intptr_t)p1->shape;
            if (sDif)
            {
                int cDiff = p1->complexity - p2->complexity;
                if (cDiff)
                {
                    return cDiff;
                }
                return sDif;
            }
            sDif = p2->drawLOD - p1->drawLOD;
            if (sDif)
            {
                return sDif;
            }
            float fDif = p2->distance2 - p1->distance2;
            if (fDif < 0)
            {
                return -1;
            }
            if (fDif > 0)
            {
                return +1;
            }
            return 0;
        });
}

// Descending-by-float object sort via an LSD radix sort. `key(o)` extracts the single
// float to sort on; the list ends up largest-key-first (farthest first). Radix is O(n)
// with no comparisons and no payload swaps — the comparison sorts these replace were
// hundreds of ms of QSort on dense scenes. Two callers, both wanting farthest-first:
//   - the occlusion pre-pass (distance2): order is a pure heuristic, tie order irrelevant;
//     the caller iterates in reverse to test/render nearest-first.
//   - the Pass2 alpha pass (zCoord): the primary back-to-front order is exactly the
//     comparison sort's; equal-depth ties composite identically, and the old QSort was
//     unstable anyway, so a different-but-valid tie order can't regress transparency.
template <class ListT, class KeyFn>
static void RadixSortByFloatDesc(ListT& list, KeyFn key)
{
    const int n = list.Size();
    if (n < 2)
    {
        return;
    }
    // Delegate to the shared Foundation algorithm (CWR upstream 8f3de01c/816e615d).
    // The previous inline copy sorted ascending and then read the array BACKWARDS to
    // produce descending order -- which also reverses the relative order of EQUAL keys,
    // i.e. it silently un-stabilised a stable sort. The shared version inverts the key
    // before the stable ascending passes instead, so equal keys keep their input order,
    // and it carries the equal-key regression test this copy never had.
    //
    // Render-thread-only scratch, reused across frames (one pair per instantiation,
    // i.e. per (list type, key function)).
    static Foundation::RadixSortBuffers<SortObject*> buffers;
    static AutoArray<SortObject*> scratch;
    scratch.Resize(n);
    for (int i = 0; i < n; i++)
    {
        scratch[i] = list[i];
    }
    Foundation::RadixSortByFloatDesc(scratch.Data(), n, key, buffers);
    for (int i = 0; i < n; i++)
    {
        list[i].SetRef(scratch[i]);
    }
}

// Packed keys for the Pass2 object sorts. Each carries the `obj` payload plus only the
// fields its comparator reads — see CmpSurfaceObj / CmpRevAlphaSortObj for the ordering,
// mirrored verbatim in the SortListByPackedKey call sites so the order is byte-identical.
struct SurfKey
{
    SortObject* obj;
    int passOrder;     // sortPassOrder (precomputed in AdjustComplexity)
    const void* shape; // GetShape() identity
    float distance2;
};

void Scene::EndObjects()
{
    // sort by screen size
}

void Scene::DrawFlare(ColorVal color, Vector3Par lightPos, bool secondary, float sizeScale, bool emissiveCore, bool analytic)
{
    Color lightColor = emissiveCore ? color : color * GEngine->GetAccomodateEye();
    float oldA = lightColor.A();
    // we can draw flares now
    // flare positions between posLight (0) and screen center (1)
    static const float flarePos[FlareLast + 1 - Flare0] = {0.0f,    -0.2f,   -0.1f,  +0.25f, +0.275f, +0.3f,
                                                           +0.4f,   +0.5f,   +0.65f, +0.7f,  +0.725f, +0.75f,
                                                           +0.875f, +0.885f, +1.0f,  +1.1f};
    // note - z is ignored when doing 2D draw
    // but is significant for decal draw
    float w = GLOB_ENGINE->Width();
    float h = GLOB_ENGINE->Height();
    // calculate screen position of light
    Vector3Val pos = ScaledInvTransform() * lightPos;
    // apply perspective
    Matrix4Val project = GetCamera()->Projection();
    const float thold = 0.6f;
    if (pos[2] < thold)
    {
        return; // no flares
    }
    float vis = (pos[2] - thold) * (1.0f / (1 - thold));
    saturateMin(vis, 1);
    float a = 0.9f * vis * vis;
    if (a > 0.01)
    {
        float invW = 1 / pos[2];
        Vector3 lightPos(project(0, 2) + project(0, 0) * pos[0] * invW, project(1, 2) + project(1, 1) * pos[1] * invW,
                         1);

        float size = 0.1f * std::clamp(sizeScale, 0.01f, 1.0f);
        float sizeX = +project(0, 0) * size * GetCamera()->InvLeft();
        float sizeY = -project(1, 1) * size * GetCamera()->InvTop();

        Vector3 center(0.5f * w, 0.5f * h, 1);
        const int special = NoZBuf | IsLight | ClampU | ClampV | IsAlphaFog;
        // Flare0 is handled specially
        lightColor.SetA(oldA * a * 2);
        Texture* flareSizing = Preloaded(PreloadedTexture(Flare0));
        const float analyticScale = flareSizing ? flareSizing->AWidth() * (1.0f/64) : 1.0f;
        const bool analyticDrawn = analytic && GEngine->DrawLightGlow(
            lightPos,sizeX*analyticScale,sizeY*analyticScale,lightColor,emissiveCore);
        if (!analyticDrawn)
        {
            lightColor.SetA(oldA * a * 2);
            PackedColor color = PackedColor(lightColor);
            Texture* texture = Preloaded(PreloadedTexture(Flare0));
            if (texture)
            {
                MipInfo mip = GLOB_ENGINE->TextBank()->UseMipmap(texture, 0, 0);
                if (mip.IsOK())
                {
                    float sizeCoef = texture->AWidth() * (1.0f / 64);
                    const int layers = emissiveCore ? 8 : 1;
                    for (int layer = 0; layer < layers; ++layer)
                        GEngine->DrawDecal(lightPos, 1, sizeX * sizeCoef, sizeY * sizeCoef, color, mip, special);
                }
            }
        }
        if (!secondary)
        {
            return;
        }
        lightColor.SetA(a * oldA);
        PackedColor color = PackedColor(lightColor);
        for (int s = Flare0 + 1; s <= FlareLast; s++)
        {
            float coef = flarePos[s - Flare0];
            Texture* texture = Preloaded(PreloadedTexture(s));
            if (texture)
            {
                MipInfo mip = GLOB_ENGINE->TextBank()->UseMipmap(texture, 0, 0);
                if (mip.IsOK())
                {
                    Vector3 pos = lightPos + coef * (center - lightPos);
                    float sizeCoef = texture->AWidth() * (1.0f / 64);
                    GEngine->DrawDecal(pos, 1, sizeX * sizeCoef, sizeY * sizeCoef, color, mip, special);
                }
            }
        }
    }
}

// LGT-025: one line per distinct (light slot, verdict) pair, not one per frame -- a per-frame
// line at 60 Hz buries the answer, and a once-only line cannot tell "stopped happening" from
// "never happened".
static void LGT025FlareVerdict(int slot, const char* verdict, float intensity, float occ, float alpha)
{
    static char lastVerdict[16][32] = {};
    if (slot < 0 || slot >= 16)
        return;
    if (std::strncmp(lastVerdict[slot], verdict, sizeof(lastVerdict[0]) - 1) == 0)
        return;
    std::strncpy(lastVerdict[slot], verdict, sizeof(lastVerdict[0]) - 1);
    LOG_INFO(Graphics, "LGT-025 flare: light #{} -> {} (intensity {:.4f} occ {:.3f} alpha {:.4f})", slot,
             verdict, intensity, occ, alpha);
}

void Scene::DrawFlares()
{
    if (!GetLandscape())
    {
        return;
    }
    float vis = GetLandscape()->SkyThrough();
    float night = MainLight()->NightEffect();

    Vector3Val cPos = GetCamera()->Position();

    // WGPU's procedural sky owns its occlusion-aware sun flare.  Drawing the
    // legacy screen decal as well stacks two bright sources at the sun and
    // produces the oversized white orb seen with imported worlds.  Keep the
    // legacy path for GL33, and keep the separate night-time local-light
    // flares below: those are not represented by the procedural sky.
    if (vis > 0.05 && night < 0.95 && !GEngine->ProceduralSkyActive())
    {
        // draw sun flare
        CameraType camType = GWorld->GetCameraType();

        bool secondary = (camType != CamInternal);
        // check HasFlares of camera source
        Object* camObj = GWorld->CameraOn();
        if (camObj)
        {
            secondary = camObj->HasFlares(camType);
        }
        Vector3 lightDir = MainLight()->SunDirection();
        Color lightColor = MainLight()->SunColor() * (1 - night) * vis;

        // fictive sun position
        Vector3 sunPos = cPos - lightDir * ENGINE_CONFIG.horizontZ;
        float sunRadius = 0.02 * ENGINE_CONFIG.horizontZ;

        float visLand = 1;
        if (ENGINE_CONFIG.enableHWTL)
        {
            float t = GLandscape->IntersectWithGroundOrSea(nullptr, cPos, -lightDir, 0, ENGINE_CONFIG.horizontZ * 1.1);
            visLand = t >= ENGINE_CONFIG.horizontZ;
        }
        if (visLand > 0)
        {
            float a = visLand * 0.25;
            // check against occlusion buffer
            float occ = GetOcclusions()->TestSphereWSpace(sunPos, sunRadius);
            a *= occ;
            if (a >= 0.01)
            {
                // check line against view geometries
                CollisionBuffer col;
                Object* camOn = GWorld->CameraOn();
                if (camOn && !GWorld->GetCameraEffect())
                {
                    GLandscape->ObjectCollision(col, camOn, nullptr, cPos, sunPos, 0, ObjIntersectView);
                    // check if any of the objects is not considered
                    for (int i = 0; i < col.Size(); i++)
                    {
                        Object* obj = col[i].object;
                        if (obj && obj->GetShape() && !obj->GetShape()->CanOcclude())
                        {
                            // object is not included in occlusion buffer, check it now
                            a = 0;
                        }
                    }
                }
                if (a >= 0.01)
                {
                    saturateMin(a, 1);
                    lightColor.SetA(a);
                    DrawFlare(lightColor, GetCamera()->Position() - lightDir, secondary);
                }
            }
        }
    }
    // LGT-025 diagnostic. Five separate `continue`s and a multiplicative occlusion term stand
    // between an active light and its halo, and from outside the frame every one of them looks
    // identical: no halo. POSEIDON_BULB_GLOW_DIAG=1 prints which of them fired, once per
    // distinct verdict, so "the sprite is too small" and "the sprite is never drawn" stop being
    // the same observation. It exists because raising the size and alpha caps by 4x moved not
    // one pixel, which no amount of tuning could have explained.
    static const bool glowDiag = []
    { const char* e = std::getenv("POSEIDON_BULB_GLOW_DIAG"); return e && std::atoi(e) != 0; }();
    if (glowDiag)
    {
        static int reported = 0;
        if (reported < 4)
        {
            ++reported;
            LOG_INFO(Graphics, "LGT-025 flare: night {:.2f} activeLights {} maxLights {}", night,
                     _aLights.Size(), ENGINE_CONFIG.maxLights);
        }
    }
    if (night >= 0.2)
    {
        // draw flares from active lights
        Vector3Val camPos = GetCamera()->Position();
        Vector3Val camDir = GetCamera()->Direction();
        for (int i = 0; i < _aLights.Size(); i++)
        {
            Light* light = _aLights[i];
            const bool wgpu = AppConfig::Instance().GetRenderBackend() == "wgpu";
            Vector3 dir = camPos - light->Position();
            if (dir * GetCamera()->Direction() > 0)
            {
                if (glowDiag)
                    LGT025FlareVerdict(i, "behind camera", 0.0f, 0.0f, 0.0f);
                continue; // this one has no flare
            }
            float intensity = light->FlareIntensity(camPos, camDir);
            if (intensity < 0.01)
            {
                if (glowDiag)
                    LGT025FlareVerdict(i, "intensity", intensity, 0.0f, 0.0f);
                continue;
            }
            // light

            // check if light position is visible from camera position
            //
            // LGT-025: the ray was cast along `dir`, which is camPos - lightPos -- i.e. FROM
            // the light AWAY from the camera. So this asked "is there ground behind me",
            // and for any camera near the ground the answer is yes, t <= 1, visLand = 0.
            // The sun arm of this same function has it right (it casts along -lightDir, i.e.
            // TOWARDS the sun); only the local-light arm has the sign inverted.
            //
            // Measured, which is how it was found at all: at the cafe at 23:00 with
            // POSEIDON_BULB_GLOW_DIAG=1, of sixteen active lights the two STRONGEST that the
            // camera could see -- intensity 0.884 and 0.149, the near street lamps -- were
            // both thrown away here, and the only two that ever reached DrawFlare were
            // distant ones at 0.012 and 0.017. Raising the sprite's size and alpha caps by
            // 4x moved not a single pixel, because the lights those caps applied to were a
            // hundredth as bright as the ones this line was discarding.
            //
            // The magnitude convention is unchanged: `toLight` is NOT normalised, so t is in
            // units of the camera-to-light distance and `1.1` is "10% past the light".
            // WGPU always needs this visibility check: its flare decal intentionally
            // has no depth test. The legacy GL33 diagnostic switch is unchanged.
            const Vector3 toLight = (wgpu || GBulbGlow().enabled) ? Vector3(light->Position() - camPos) : dir;
            float visLand = 1;
            if (wgpu || ENGINE_CONFIG.enableHWTL)
            {
                float t = GLandscape->IntersectWithGroundOrSea(nullptr, cPos, toLight, 0, 1.1);
                visLand = t > 1;
            }

            if (visLand <= 0 && glowDiag)
                LGT025FlareVerdict(i, "visLand", intensity, 0.0f, 0.0f);
            if (visLand > 0)
            {
                Color lightColor = light->GetObjectColor();
                // A point/spot emitter is not a second sun. The legacy decal was
                // authored for the fixed-function renderer; on WGPU its large opaque
                // core reads as a solar disc even at a low alpha. Keep it as a compact
                // emitter marker while WGPU's physical point/spot contribution lights
                // nearby surfaces. GL33 retains the original tuned sprite.
                // LGT-025. The paragraph above is why this sprite was shrunk to nothing on
                // wgpu, and the shrinking is the owner's "es muss wirklich glowen": at 0.030
                // the halo is 0.3% of the screen half-height, i.e. two pixels, and at an
                // alpha cap of 0.10 it is invisible against a lit wall. The disc it was
                // protecting against came from the SIZE of an opaque core, not from a halo,
                // and Flare0 is a soft radial texture -- so the answer is a WIDER, softer
                // sprite that is still capped well below opaque, not a return to 2001's.
                //
                // Both are dev-switchable (Lighting tab / POSEIDON_BULB_GLOW*) because both
                // are taste calls, and because the cap is what separates "glow" from "solar
                // disc" and the owner is the one who gets to place that line.
                const BulbGlowSettings& glow = GBulbGlow();
                const float gain = glow.enabled ? glow.strength : 1.0f;
                const float span = glow.enabled ? glow.size : 1.0f;
                float a = visLand * intensity * (wgpu ? 0.18f * gain : 0.35f);
                // LGT-025: a HEADLIGHT is mounted flush in the vehicle it belongs to, so a
                // 0.4 m occlusion sphere centred on the bulb is mostly full of that
                // vehicle's own bonnet and grille -- and the occlusion buffer duly answers
                // "hidden". Measured on the crewed jeep at 20 m: the two headlights pass
                // every other gate (intensity 0.1360 with the off-beam term below) and are
                // killed here, occ 0.000, every frame. Same shape as LGT-016's selfRadius --
                // a light inside a closed mesh occludes itself in every direction.
                //
                // Sample a little way TOWARDS the camera instead: a bulb you can see has
                // clear air in front of it. The offset is capped at half the distance, so a
                // light metres away is not sampled past its own midpoint, and it moves the
                // probe along the line the camera already looks down -- so a lamp genuinely
                // behind a wall stays behind that wall and stays dark.
                //
                // NOT SUFFICIENT, and recorded as such so the next session does not read
                // this as solved: re-measured on the same jeep, the headlights still report
                // occ 0.000 every frame. 0.8 m at 20 m is a fifth of a degree, and the
                // vehicle's silhouette in the occlusion buffer is far coarser than that. It
                // does help lights that are only PARTLY covered (the same capture gained
                // 3,335 changed pixels elsewhere), which is why it stays. The real fix is to
                // let the occlusion query ignore the light's own AttachedOn() object, and
                // that lives in Occlusion::TestSphereWSpace, not here.
                Vector3 occPos = light->Position();
                if (glow.enabled)
                {
                    const Vector3 toCam = camPos - light->Position();
                    const float len = toCam.Size();
                    if (len > 1.0e-3f)
                        occPos = light->Position() + toCam * (floatMin(0.8f, len * 0.5f) / len);
                }
                float occ = GetOcclusions()->TestSphereWSpace(occPos, glow.enabled ? 0.2f : 0.4f);
                if (wgpu)
                {
                    // NoZBuf flares need source visibility for ALL local lights, not
                    // just attached headlights. A coarse shifted sphere can miss a
                    // wall or car covering a street lamp. Keep the attached-light
                    // self exclusion, but do not let the glow style disable occlusion.
                    CollisionBuffer blockers;
                    Object* cameraCarrier = GWorld->GetCameraEffect() ? nullptr : GWorld->CameraOn();
                    GLandscape->ObjectCollision(blockers, cameraCarrier, light->AttachedOn(),
                                                camPos, light->Position(), 0.0f, ObjIntersectView);
                    occ = blockers.Size() == 0 ? 1.0f : 0.0f;
                    if (glowDiag)
                    {
                        static int samples = 0;
                        if (samples++ < 32)
                        {
                            const Vector3 source = light->Position();
                            Object* blocker = blockers.Size() ? blockers[0].object.GetRef() : nullptr;
                            LOG_INFO(Graphics, "LGT-025 geometry: source ({:.2f},{:.2f},{:.2f}) blockers {} first {}",
                                     source.X(), source.Y(), source.Z(), blockers.Size(),
                                     blocker && blocker->GetShape() ? blocker->GetShape()->Name() : "none");
                        }
                    }
                }
                a *= occ;
                // Still a hard ceiling, just a higher one: an alpha of 1 would paint the
                // flare's own core over the bulb as a flat opaque blob, which is exactly the
                // artefact the 0.10 was defending against.
                saturateMin(a, wgpu ? floatMin(0.10f * gain, 0.75f) : 0.2f);
                if (glowDiag)
                    LGT025FlareVerdict(i, a > 0.001f ? "drawn" : "occluded", intensity, occ, a);
                lightColor.SetA(a);
                DrawFlare(lightColor, light->Position(), false,
                          wgpu ? floatMin(0.030f * span, 1.0f) : 0.22f, false, wgpu);
                if (wgpu && glow.enabled && light->HasVisibleBulb() && light->AttachedOn() && occ > 0.0f)
                {
                    const float distance = floatMax(dir.Size(), 0.1f);
                    const float facing = floatMax(0.0f, dir.DotProduct(light->Direction()) / distance);
                    Color core = light->GetObjectColor();
                    const float peak = floatMax(core.R(), floatMax(core.G(), core.B()));
                    if (peak > 1.0e-4f && glow.markerSize > 0.0f)
                    {
                        core = core * (1.0f / peak);
                        core.SetA(floatMin(gain * facing, 1.0f));
                        // A compact screen-facing lens, not the flat HalfLight mesh.
                        // Additive quads avoid the packed-colour radiance ceiling
                        // without another shader, texture or fullscreen pass.
                        DrawFlare(core, light->Position(), false,
                                  glow.markerSize / (0.2f * distance), true, true);
                    }
                }
            }
        }
    }
}

void Scene::DrawRainLevel(float alpha, float yDensity, float xOffset, float yOffset, float z)
{
    Texture* texture = Preloaded(TextureRain);
    Color color(HWhite);
    color.SetA(alpha);
    Draw2DPars pars;
    pars.mip = GLOB_ENGINE->TextBank()->UseMipmap(texture, 0, 0);
    pars.SetU(xOffset - z * 0.5f, xOffset + z * 0.5f);
    pars.SetV(yOffset - z * 0.5f * yDensity, yOffset + z * 0.5f * yDensity);
    pars.SetColor(PackedColor(color));
    pars.spec = NoZWrite | IsAlpha | NoClamp | IsAlphaFog;
    Rect2DAbs rect(0, 0, GLOB_ENGINE->Width(), GLOB_ENGINE->Height());
    GLOB_ENGINE->Draw2D(pars, rect);
}

void Scene::DrawRain()
{
    // start with single level rain
    if (!GetLandscape())
    {
        return;
    }
    // The dev panel's legacy-density override exists so the old overlay can be
    // compared against the particle rain on a clear day; -1 means "follow the weather".
    float density = GRain.LegacyDensityOverride() >= 0.0f ? GRain.LegacyDensityOverride()
                                                          : GetLandscape()->GetRainDensity();
    if (density >= 0.1)
    {
        Vector3Val dir = GetCamera()->Direction();
        float speed = fabs(GetCamera()->Speed() * dir) * 0.05f;
        saturate(speed, 0, 2);
        static float rainOffset;
        static Time rainT;
        float deltaRT = Glob.time - rainT;
        rainT += deltaRT;
        rainOffset += deltaRT;
        float yOffset = -fastFmod(rainOffset, 1);
        float xOffset = atan2(dir.X(), dir.Z()) * 0.3f;
        float yDensity = 0.3f + fabs(dir.Y()) + speed;
        saturate(yDensity, 0.1f, 1);
        // draw all levels
        density *= 0.2f;
        DrawRainLevel(density, yDensity, xOffset, yOffset, 8);
        DrawRainLevel(density, yDensity, xOffset, yOffset, 2);
    }
}

void Scene::ObjectsDrawn()
{
    // RainOff must mute BOTH layers, so the legacy overlay is gated too -- it is no
    // longer unconditional. RainBoth draws both for A/B comparison.
    const RainMode mode = GRain.Mode();
    if (!GSnow().enabled) GSnowFlakes.ClearParticles();
    if (GSnow().enabled)
    {
        RainParams flakes;
        flakes.snowflakes = true;
        flakes.targetDrops = static_cast<int>(700 * GSnow().BoundedFlakeMultiplier());
        flakes.maxDrops = static_cast<int>(900 * GSnow().BoundedFlakeMultiplier());
        flakes.areaRadius = 12.0f;
        flakes.spawnHeight = 8.0f;
        flakes.fallSpeed = 0.65f;
        flakes.speedJitter = 0.45f;
        flakes.streakLength = 0.018f;
        flakes.streakWidth = 0.018f;
        flakes.red = flakes.green = flakes.blue = 0.95f;
        flakes.windResponse = 1.0f;
        flakes.windAcceleration = 3.0f;
        flakes.splashes = false;
        flakes.densityOverride = GSnow().FallingIntensity();
        GSnowFlakes.SetMode(RainParticle);
        GSnowFlakes.SetForceWeatherRain(false);
        GSnowFlakes.SetParams(flakes);
        GSnowFlakes.UpdateAndDraw();
    }
    else if (mode == RainLegacy || mode == RainBoth)
    {
        DrawRain();
    }
    if (!GSnow().enabled && (mode == RainParticle || mode == RainBoth))
    {
        GRain.UpdateAndDraw();
    }

    DrawObjectsAndShadowsPass3();

    // clear working list
    _drawMergers.Resize(0);
    // keep drawObjects for next frame

    DrawFlares();
    _aLights.Resize(0);
}

// The distance the RENDERER culls objects at.
//
// ENGINE_CONFIG.objectsZ is 2/3 of the view distance (ViewDistanceResolver's OFP
// ratio) and doubles as the SIMULATION radius (Simul.cpp:1188/1224), so it cannot
// simply be raised. But as a render cull it is in the wrong place: distance fog is
// anchored to the fog MAX range (frame.wgsl apply_fog: max_dist = fogStart +
// 1/fogInvRange = Scene::GetFogMaxRange(); the legacy flat path uses the same
// anchor), and objectsZ sits at ~2/3 of it. pow(2/3, fogFalloff=3) = 0.30 in the
// aerial path, and 1 - (0.667V-0.3V)/0.7V = 0.48 in the flat path -- i.e. an object
// crossing the cull edge materialises out of nothing at 70% (resp. 52%) of its own
// colour. That instantaneous appearance IS the pop-in; no LOD change can hide it,
// because below the edge the object is not drawn at all.
//
// Anchoring the render cull to the fog max range instead makes the edge coincide
// with fog amount 1.0, where the object is exactly the sky it replaces -- zero pop
// by construction, and no cross-fade machinery needed. What newly appears out there
// is the model's COARSEST authored LOD: the LOD chooser (FindSqrtLevel below) has
// already walked past every finer level by a few hundred metres, so extending the
// cull adds silhouettes, not detail.
//
// WGR_OBJECT_DISTANCE_SCALE is the A/B knob: 1.0 reproduces the legacy cull exactly.
// Keep the legacy minimum only within the terrain reach. Heavy fog may shorten
// that reach below objectsZ; retaining objectsZ then draws trees beyond the ground.
float Scene::GetObjectDrawDistance() const
{
    static const float scale = []() -> float {
        if (const char* e = std::getenv("WGR_OBJECT_DISTANCE_SCALE"))
        {
            const float v = static_cast<float>(atof(e));
            if (v > 0.0f)
            {
                return v;
            }
        }
        return 0.0f; // default: follow the full render range, independently of simulation
    }();

    return ResolveObjectDrawRange(ENGINE_CONFIG.objectsZ, _fogMaxRange, scale);
}

int Scene::LevelFromDistance2(LODShape* shape, float distance2, float oScale, Vector3Par direction,
                              Vector3Par viewDirection)
{
    // if pixel size is lower than 2, object can be considered invisible
    // size in pixels is
    float scale = GetCamera()->Left();
    float diameter = shape->BoundingSphere() * 2 * oScale;

    if (distance2 > Square(GetObjectDrawDistance()))
    {
        return LOD_INVISIBLE;
    }

    float pixelLimit = 0.125;
    float detail2 = distance2 * Square(_lodInvWidth);

    if (Square(diameter) < Square(pixelLimit * scale) * detail2)
    {
        return LOD_INVISIBLE;
    }

    if (shape->NLevels() < 2)
    {
        return 0;
    }

    float resol2 = detail2 * Square(scale);
    // disable decal LODs
    int level = shape->FindSqrtLevel(resol2, true);

    return level;
}

int Scene::LevelShadowFromDistance2(LODShape* shape, float distance2, float oScale, Vector3Par direction,
                                    Vector3Par viewDirection)
{
    distance2 *= 8;

    float scale = GetCamera()->Left();
    float diameter = shape->BoundingSphere() * 2 * oScale;

    float pixelLimit = 0.25;
    float detail2 = distance2 * Square(_lodInvWidth);

    if (Square(diameter) < Square(pixelLimit * scale) * detail2)
    {
        return LOD_INVISIBLE;
    }

    if (shape->NLevels() < 2)
    {
        return 0;
    }
    float limit = ENGINE_CONFIG.shadowLODLimit;
    saturateMax(detail2, Square(diameter * limit));
    float resol2 = detail2 * Square(scale);
    // disable decal LODs
    int level = shape->FindSqrtLevel(resol2, true);

    return level;
}

#define DO_STAT 0

#if DO_STAT

#include <Poseidon/Foundation/Math/Statistics.hpp>

NameStatistics Alpha;
NameStatistics Opaque;
NameStatistics Shadow;
#endif

#if _ENABLE_CHEATS

// advances diagnostics - via scripting
#include <Evaluator/express.hpp>

#define NOTHING GameValue()

#define DIAG_DRAW_MODE_ENUM(type, prefix, XX) \
    XX(type, prefix, Normal)                  \
    XX(type, prefix, Roadway)                 \
    XX(type, prefix, Geometry)                \
    XX(type, prefix, ViewGeometry)            \
    XX(type, prefix, FireGeometry)            \
    XX(type, prefix, Paths)

DECLARE_DEFINE_ENUM(DiagDrawMode, DDM, DIAG_DRAW_MODE_ENUM)

namespace Poseidon::Dev
{
DEFINE_ENUM(DiagEnable, DE, DIAG_ENABLE_ENUM)
}

DiagDrawMode DiagDrawModeState = DDMNormal;
namespace Poseidon::Dev
{
int DiagMode;
}

static GameValue SetDiagDrawMode(const GameState* state, GameValuePar oper)
{
    const char* modeStr = (RString)oper;
    DiagDrawMode mode = GetEnumValue<DiagDrawMode>(modeStr);
    if ((int)mode == -1)
        return NOTHING;
    DiagDrawModeState = mode;
    return NOTHING;
}

static GameValue SetDiagEnable(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    const char* modeStr = (RString)oper1;
    bool onOff = oper2;
    int modeMask = ~0;
    if (strcmpi(modeStr, "all"))
    {
        DiagEnable mode = GetEnumValue<DiagEnable>(modeStr);
        if ((int)mode == -1)
            return NOTHING;
        modeMask = 1 << mode;
    }

    if (onOff)
        DiagMode |= modeMask;
    else
        DiagMode &= ~modeMask;
    return NOTHING;
}

static GameValue SetDiagToggle(const GameState* state, GameValuePar oper1)
{
    const char* modeStr = (RString)oper1;
    DiagEnable mode = GetEnumValue<DiagEnable>(modeStr);
    if ((int)mode == -1)
        return NOTHING;
    int modeMask = 1 << mode;

    DiagMode ^= modeMask;
    return NOTHING;
}

#include <Poseidon/Foundation/Modules/Modules.hpp>

static const GameFunction ObjUnary[] = {
    GameFunction(GameNothing, "diag_drawmode", SetDiagDrawMode, GameString),
    GameFunction(GameNothing, "diag_toggle", SetDiagToggle, GameString),
};
static const GameOperator ObjBinary[] = {
    GameOperator(GameNothing, "diag_enable", function, SetDiagEnable, GameString, GameBool),
};

INIT_MODULE(GameStateObj, 3)
{
    GGameState.NewOperators(ObjBinary, sizeof(ObjBinary) / sizeof(*ObjBinary));
    GGameState.NewFunctions(ObjUnary, sizeof(ObjUnary) / sizeof(*ObjUnary));
};

#endif

int Scene::AdjustComplexity(SortObjectList& objs)
{
    int totalComplexity = 0;
    for (int i = 0; i < objs.Size(); i++)
    {
        SortObject* oi = objs[i];
        Object* obj = oi->object;
        if (!obj)
        {
            Fail("No obj in SortObject info");
            continue;
        }
        LODShape* shape = oi->shape;
        if (oi->forceDrawLOD >= 0)
        {
            oi->drawLOD = oi->forceDrawLOD;
        }
        else
        {
            int drawLevel =
                LevelFromDistance2(shape, oi->distance2, obj->Scale(), obj->Direction(), _camera->Direction());
            if (drawLevel != LOD_INVISIBLE)
            {
#if _ENABLE_CHEATS
                if (DiagDrawModeState != DDMNormal)
                {
                    int geom = -1;
                    switch (DiagDrawModeState)
                    {
                        case DDMGeometry:
                            geom = shape->FindGeometryLevel();
                            break;
                        case DDMViewGeometry:
                            geom = shape->FindViewGeometryLevel();
                            break;
                        case DDMFireGeometry:
                            geom = shape->FindFireGeometryLevel();
                            break;
                        case DDMRoadway:
                            geom = shape->FindRoadwayLevel();
                            break;
                        case DDMPaths:
                            geom = shape->FindPaths();
                            break;
                    }
                    if (geom >= 0)
                        drawLevel = geom;
                }
#endif
            }
            oi->drawLOD = drawLevel;
        }
        // Decorate the frame's sort keys once, now that drawLOD is final. Both are
        // otherwise re-derived per comparison in CmpShapeObj / CmpSurfaceObj: the
        // complexity is Level(0)->NFaces() (a pointer chase) and PassOrder() is a
        // virtual call. Level(0) is drawLOD-independent; PassOrder ignores its lod arg,
        // but pass the real drawLOD so this stays byte-identical to the comparators.
        {
            LODShapeWithShadow* srtShape = obj->GetShape();
            oi->sortComplexity = (srtShape && srtShape->NLevels() > 0) ? srtShape->Level(0)->NFaces() : 0;
            oi->sortPassOrder = obj->PassOrder(oi->drawLOD);
        }
        // check number of faces in given level
        if (oi->drawLOD != LOD_INVISIBLE)
        {
            oi->passNum = obj->PassNum(oi->drawLOD);
#if _ENABLE_CHEATS
            if (CHECK_DIAG(DETransparent))
            {
                // all geometries are drawn transparent
                if (oi->drawLOD == shape->FindGeometryLevel())
                {
                    if (oi->passNum < 2)
                        oi->passNum = 2;
                }
            }
#endif
            totalComplexity += obj->GetComplexity(oi->drawLOD, *obj);
        }
    }
    return totalComplexity;
}

static int ShadowFactor(Scene* scene)
{
    float addLightsFactor = scene->MainLight()->NightEffect();
    float skyCoef = floatMin(scene->GetLandscape()->SkyThrough(), 0.6f);
    float shadowFactor = skyCoef + 0.1f;
    return toIntFloor(shadowFactor * (1 - addLightsFactor) * 255);
}

// WGR_SHADOW_LOD_STATS=1 dumps, once per Scene::AdjustComplexity (i.e. once per frame), how many
// objects the complexity re-scan visited and how many shadow-LOD property queries that cost. The
// re-scan is a loop of up to five iterations plus five more call pairs on the exit paths, so the
// object count multiplies; the query count is what the LODShape memo removes the string work from.
static bool ShadowLodStatsEnabled()
{
    static const bool on = []
    {
        const char* v = std::getenv("WGR_SHADOW_LOD_STATS");
        return v && std::strcmp(v, "0") != 0;
    }();
    return on;
}
static long long GShadowLodStatObjects = 0;
static long long GShadowLodStatQueries = 0;
static long long GShadowLodStatPasses = 0;

int Scene::AdjustShadowComplexity(SortObjectList& objs)
{
#if 1
    int totalComplexity = 0;
    const bool statsOn = ShadowLodStatsEnabled();
    if (statsOn)
    {
        GShadowLodStatPasses++;
        GShadowLodStatObjects += objs.Size();
    }

    int shadowFactorI = ShadowFactor(this);

    for (int i = 0; i < objs.Size(); i++)
    {
        SortObject* oi = objs[i];
        Object* obj = oi->object;
        LODShape* shape = oi->shape;
        if (!shape)
        {
            continue;
        }
        if (!(shape->Special() & NoShadow) && oi->distance2 < Square(_shadowFogMaxRange) && shadowFactorI >= 12 &&
            obj->CastShadow())
        {
            int level = LevelShadowFromDistance2(shape, oi->distance2, obj->Scale(), obj->Direction(),
                                                 _mainLight->ShadowDirection());
            if (level != LOD_INVISIBLE)
            {
                if (statsOn)
                {
                    GShadowLodStatQueries++;
                }
                level = shape->FindNearestWithoutProperty(level, "lodnoshadow");
                if (level < 0)
                {
                    level = LOD_INVISIBLE;
                }
            }
            oi->shadowLOD = level;
        }
        else
        {
            oi->shadowLOD = LOD_INVISIBLE;
        }
        if (oi->shadowLOD != LOD_INVISIBLE)
        {
            // check number of faces in given level
            Shape* level = shape->Level(oi->shadowLOD);
            totalComplexity += level->NFaces();
        }
    }
    return totalComplexity;
#else

    float addLightsFactor = MainLight()->NightEffect();
    float skyCoef = floatMin(GetLandscape()->SkyThrough(), 0.6f);
    float shadowFactor = skyCoef + 0.1f;
    int shadowFactorI = toIntFloor(shadowFactor * (1 - addLightsFactor) * 255);

    for (int i = 0; i < objs.Size(); i++)
    {
        SortObject* oi = objs[i];
        Object* obj = oi->object;
        LODShape* shape = oi->shape;
        if (!(shape->Special() & NoShadow) && oi->distance2 < Square(_shadowFogMaxRange) && shadowFactorI >= 12)
        {
            int level = 0;
            if (level != LOD_INVISIBLE)
            {
                level = shape->FindNearestWithoutProperty(level, "lodnoshadow");
                if (level < 0)
                    level = LOD_INVISIBLE;
            }
            oi->shadowLOD = level;
        }
        else
        {
            oi->shadowLOD = LOD_INVISIBLE;
        }
    }
    return 1000;
#endif
}

inline void CheckMinMaxIter(Vector3& min, Vector3& max, Vector3Par val)
{
#if __ICL
    if (min[0] > val[0])
        min[0] = val[0];
    if (max[0] < val[0])
        max[0] = val[0];
    if (min[1] > val[1])
        min[1] = val[1];
    if (max[1] < val[1])
        max[1] = val[1];
    if (min[2] > val[2])
        min[2] = val[2];
    if (max[2] < val[2])
        max[2] = val[2];
#else
    if (min[0] > val[0])
    {
        min[0] = val[0];
    }
    else if (max[0] < val[0])
    {
        max[0] = val[0];
    }
    if (min[1] > val[1])
    {
        min[1] = val[1];
    }
    else if (max[1] < val[1])
    {
        max[1] = val[1];
    }
    if (min[2] > val[2])
    {
        min[2] = val[2];
    }
    else if (max[2] < val[2])
    {
        max[2] = val[2];
    }
#endif
}

static bool FarEnoughForOcclusion(const SortObject* oi)
{
    // do not occlude things that are very near
    // the test would be very slow and is very like to fail
    const float occNearest = 20;
    if (oi->distance2 > Square(occNearest + 50))
    {
        return true;
    }
    else
    {
        float distNear = oi->distance2 * InvSqrt(oi->distance2) - oi->radius;
        if (distNear > occNearest)
        {
            return true;
        }
    }
    return false;
}

#if !DENSITY_LOD

// WGR_LOD_GOVERNOR_TARGET selects the target-point error metric described at the use site.
// It was OFF by default, pending measurement on more than the world that motivated it (+36%
// on Everon). ON since 2026-09-02 (REN-VEG-003): with the governor's set point moved to 60 fps
// and the retained set in its estimate, the distance form's range-as-gain behaviour is what
// made the loop overshoot to the 6-triangle tree card on Chernarus+; the target form is the
// one that treats the range as the clamp it is documented to be. =0 restores the distance form.
static bool LodGovernorTargetDrive()
{
    static const bool on = []
    {
        const char* v = std::getenv("WGR_LOD_GOVERNOR_TARGET");
        return !(v && v[0] == '0');
    }();
    return on;
}

void Scene::AdjustComplexity()
{
    // POSEIDON_LOD_TRACE: once per frame, after the whole object walk (Pass1) has offered
    // every visible object and before the governor moves _lodInvWidth, so the value logged
    // is the one that selected the level. No-op unless the env var is set.
    LodTraceFlush();

    // Opt-in benchmark density shared by CPU draw/shadow selection and the
    // existing GPU presentation snapshot. Ordinary governor remains unchanged.
    static const float lodFix = ParseBenchmarkLodFix(std::getenv("POSEIDON_LOD_FIX"));
    if (lodFix != 0)
    {
        _lodInvWidth = lodFix > 0 ? lodFix : _minLodInvWidth;
        saturate(_lodInvWidth, _minLodInvWidth, _maxLodInvWidth);
        AdjustComplexity(_drawObjects);
        AdjustShadowComplexity(_drawObjects);
        return;
    }

    float oldLodInvWidth = _lodInvWidth;

    // adjust lodInvWidth so we are on the line given by points
    // (MinTargetFrameDuration, minLodInvWidth), (MaxTargetFrameDuration, maxLodInvWidth):
    // l is _lodInvWidth, t is estimated time. Line equation lineL*l+lineT*t+lineC = 0.
    float lMin = _minLodInvWidth;
    float lMax = _maxLodInvWidth;

    float tMin = _minTargetFrameDuration;
    float tMax = _maxTargetFrameDuration;

    float lineL = tMin - tMax;
    float lineT = lMax - lMin;
    // normalize lineL +  lineT
    float invLineTLSize = InvSqrt(lineL * lineL + lineT * lineT);
    lineL *= invLineTLSize;
    lineT *= invLineTLSize;

    float lineC = -lineL * lMin - lineT * tMin;
#define DIAG_LOD 0
#if DIAG_LOD
    LOG_DEBUG(Graphics, "t range {:.1f}..{:.1f}", tMin, tMax);
    LOG_DEBUG(Graphics, "l range {:.3f}..{:.3f}", lMin, lMax);
#endif

    int targetGeom = ENGINE_CONFIG.maxObjects * 75;
    int complexity = 0;
    int treshold = 8;
    // THE RETAINED SET IS PART OF THE FRAME (REN-VEG-002, 2026-09-02). This governor estimates
    // the frame's cost from the complexity of the objects the CPU is about to draw and steers
    // _lodInvWidth until the estimate fits the target frame duration. The GPU-driven cull
    // reads the SAME _lodInvWidth -- but the objects it draws left _drawObjects when they
    // were diverted, so their triangles left this sum, and the governor sat blind to a
    // GPU-bound frame: handing Chernarus+'s trees to the retained path dropped this sum by
    // 4,000 objects, kept _lodInvWidth fine, and drew 30.5 M triangles (REN-VEG-001).
    // The previous frame's retained main-view triangle count (the cull's own readback) is
    // the missing term, in the same units as GetComplexity (faces). It is a one-frame-old
    // number and constant across the five iterations below, which is right: the loop can
    // re-pick the CPU objects' LODs per iteration and cannot re-cull the GPU's, so the
    // GPU term converges over frames instead, at the governor's usual cadence.
    // First measured with the 2001 set point (frameRate=15, a 10..20 fps band): on Chernarus+ the
    // term changed nothing because 55 ms was not slow to that band, and on Everon it coarsened a
    // frame that was not slow either -- the band, not the term, was wrong. ON by default since
    // the set point moved to 60 fps and the loop drives on the target point (REN-VEG-003).
    // WGR_LOD_GOVERNOR_RETAINED=0 removes the term.
    int retainedComplexity = 0;
    {
        static const bool includeRetained = []
        {
            const char* v = std::getenv("WGR_LOD_GOVERNOR_RETAINED");
            return !(v && v[0] == '0');
        }();
        // STICKY. The cull readback is not valid on every frame (it is a GPU round trip), and a
        // term that reads 0 on the frames in between poisons the four-frame history: the
        // estimate then compares a full sum against a half-full average and calls a 13 ms
        // frame 22 ms (trace2-chern-retained, REN-VEG-004). Keep the last valid count.
        Engine::ObjectStatsOut retained;
        if (includeRetained && GEngine && GEngine->GetObjectStats(retained) && retained.valid)
        {
            _governorRetainedLast = static_cast<int>(std::min<uint32_t>(retained.mainTris, 1u << 30));
            // THE WHAT-IF LADDER (REN-VEG-004). The cull also reports what the same survivors
            // would cost at four alternative detail multipliers. Kept as (scale, triangles)
            // points around the multiplier the count was taken at, so the loop below can ask
            // "what does the retained set cost at the l I am about to try" instead of assuming
            // last frame's answer -- the assumption that walked a 700x vegetation ladder blind.
            _governorWhatIfBase = _lodInvWidth;
            for (int k = 0; k < 4; ++k)
            {
                _governorWhatIfScale[k] = retained.whatIfScale[k];
                _governorWhatIfTris[k] = static_cast<int>(std::min<uint32_t>(retained.mainWhatIfTris[k], 1u << 30));
            }
        }
        retainedComplexity = includeRetained ? _governorRetainedLast : 0;
    }
    // Retained cost at a candidate detail multiplier: piecewise-linear in log(scale) through
    // the five known points (the four what-ifs and the measured count at scale 1), clamped at
    // the ends. Falls back to the flat count when the what-if words are absent (older DLL).
    auto retainedAt = [&](float l) -> int
    {
        if (retainedComplexity <= 0 || _governorWhatIfBase <= 0.0f || _governorWhatIfScale[3] <= 0.0f)
            return retainedComplexity;
        float xs[5];
        float ys[5];
        // ascending in scale: 0.5, 0.707, 1.0, 1.414, 2.0
        xs[0] = _governorWhatIfScale[0]; ys[0] = static_cast<float>(_governorWhatIfTris[0]);
        xs[1] = _governorWhatIfScale[1]; ys[1] = static_cast<float>(_governorWhatIfTris[1]);
        xs[2] = 1.0f;                    ys[2] = static_cast<float>(retainedComplexity);
        xs[3] = _governorWhatIfScale[2]; ys[3] = static_cast<float>(_governorWhatIfTris[2]);
        xs[4] = _governorWhatIfScale[3]; ys[4] = static_cast<float>(_governorWhatIfTris[3]);
        const float f = l / _governorWhatIfBase;
        if (f <= xs[0]) return static_cast<int>(ys[0]);
        if (f >= xs[4]) return static_cast<int>(ys[4]);
        const float lf = std::log(f);
        for (int i = 0; i < 4; ++i)
        {
            if (f <= xs[i + 1])
            {
                const float a = std::log(xs[i]);
                const float b = std::log(xs[i + 1]);
                const float t = b > a ? (lf - a) / (b - a) : 0.0f;
                return static_cast<int>(ys[i] + (ys[i + 1] - ys[i]) * t);
            }
        }
        return retainedComplexity;
    };
    for (int iters = 5; --iters >= 0;)
    {
        // calculate complexity
        complexity = AdjustComplexity(_drawObjects);
        complexity += AdjustShadowComplexity(_drawObjects);
        complexity += retainedAt(_lodInvWidth);
        // include landscape complexity
        complexity += toLargeInt(Square(ENGINE_CONFIG.horizontZ * (1.0f / 50)));

        float frameDuration = GEngine->GetAvgFrameDuration(4);

        // estimate duration of this frame
        float avgComplexity = 0;
        for (int i = 0; i < NStoreComplexities; i++)
        {
            avgComplexity += _lastComplexity[i];
        }
        avgComplexity *= 1.0f / NStoreComplexities;
        int estDuration = 200; // no estimate yet
        if (avgComplexity > 0)
        {
            estDuration = toLargeInt(frameDuration * complexity / avgComplexity);
        }
        // estimate some minimal complexity this computer is able to render
        float triCoef = 200; // good estimation for Voodoo2
        int maxDuration = toLargeInt(complexity / float(targetGeom) * triCoef);

        saturateMin(estDuration, maxDuration);
        _governorTrace.frameMs = frameDuration;
        _governorTrace.estimatedMs = static_cast<float>(estDuration);
        _governorTrace.complexity = complexity;
        _governorTrace.retained = retainedComplexity;
        _governorTrace.avgComplexity = avgComplexity;
        for (int i = 0; i < NStoreComplexities && i < 4; i++)
            _governorTrace.history[i] = _lastComplexity[i];
        _governorTrace.iterations = 5 - iters;
        _governorTrace.targetMinMs = tMin;
        _governorTrace.targetMaxMs = tMax;
        _governorTrace.lodMin = lMin;
        _governorTrace.lodMax = lMax;

        // react to quick change in complexity
        // estimate frame rate based on previous complexity and time

        // calculate l and t
        // l is _lodInvWidth, t is estimated time
        // line normal is (lineL,lineT)
        float l = _lodInvWidth;
        float t = estDuration;
        float dist = lineL * l + lineT * t + lineC;
        // WGR_LOD_GOVERNOR_TARGET=1 drives on the TARGET POINT instead of the perpendicular
        // distance, which is what makes the range a clamp again rather than a gain.
        //
        // `dist` above is the signed distance from (l,t) to the target line measured with the
        // NORMALIZED normal (lineL, lineT) -- and that normal is built from lMax - lMin, i.e.
        // from the range. So the range does not only bound the loop, it rescales its error
        // term and therefore its fixed point and its deadband (the thresholds 8 and 4 are
        // absolute). That is why WGR_LOD_GOVERNOR_RANGE=4 lands COARSER than 16 instead of
        // finer, which is backwards for something documented as a coarseness bound: measured
        // on Everon at a near pose, range 16 puts 9,186 instances at LOD 0 and none past
        // LOD 2, range 4 puts 697 at LOD 0 and the mass at LOD 3-4.
        //
        // The line is the same line; only the error metric changes. Solve it for the l the
        // loop actually wants at this frame time, then drive on l - lTarget expressed as a
        // FRACTION OF THE RANGE. A fraction is dimensionless, so the same frame time gives
        // the same relative operating point at any range, and shrinking the range now does
        // what it says: it lowers the coarse end and everything below it is untouched.
        if (LodGovernorTargetDrive())
        {
            const float tSpan = tMax - tMin;
            float lTarget = lMin;
            if (tSpan > 1e-6f)
            {
                lTarget = lMin + (t - tMin) * (lMax - lMin) / tSpan;
            }
            saturate(lTarget, lMin, lMax);
            const float lSpan = lMax - lMin;
            // Same sign convention as the distance form: positive means "too slow, coarsen".
            // Scaled so a full-range error reads 100, keeping the 8/4 thresholds meaningful
            // as percentages of the range rather than as absolutes in a rescaled space.
            dist = lSpan > 1e-9f ? (lTarget - l) / lSpan * 100.0f : 0.0f;
            // The distance form multiplies by 200 below; pre-divide so one code path applies.
            dist *= 1.0f / 200.0f;
        }
// check distance from line
// note: we might be in area out of tmin,tmax
// find nearest point on line
#if DIAG_LOD
        LOG_DEBUG(Graphics, "  t {:.1f}, l {:.3f}, dist {:.2f}", t, l, dist);
#endif
        float tDiff = dist * 200;

        if (tDiff > treshold)
        {
            if (_lodInvWidth >= _maxLodInvWidth)
            {
                break; // fast path - min reached
            }
            treshold = 4; // allow smoother change
            // scale - we are too slow
            float change = tDiff * -0.005f; // change is negative
            saturate(change, -0.2f, +0.2f);
            _lodInvWidth /= Square(1 + change);
            if (_lodInvWidth > _maxLodInvWidth)
            {
                // extreme reached - limit
                _lodInvWidth = _maxLodInvWidth;
                complexity = AdjustComplexity(_drawObjects);
                complexity += AdjustShadowComplexity(_drawObjects);
                complexity += retainedAt(_lodInvWidth); // the history below must carry the same sum the loop used
                complexity += toLargeInt(Square(ENGINE_CONFIG.horizontZ * (1.0f / 50)));
                break;
            }
        }
        else if (tDiff < -treshold)
        {
            if (_lodInvWidth <= _minLodInvWidth)
            {
                break; // fast path - max reached
            }
            treshold = 4; // allow smoother change
            // scale - we are too fast
            float change = tDiff * -0.005f; // change is possitive
            saturate(change, -0.2f, +0.2f);

            _lodInvWidth /= Square(1 + change);

            if (_lodInvWidth < _minLodInvWidth)
            {
                // extreme reached - limit
                _lodInvWidth = _minLodInvWidth;
                complexity = AdjustComplexity(_drawObjects);
                complexity += AdjustShadowComplexity(_drawObjects);
                complexity += retainedAt(_lodInvWidth); // the history below must carry the same sum the loop used
                complexity += toLargeInt(Square(ENGINE_CONFIG.horizontZ * (1.0f / 50)));
                break;
            }
        }
        else
        {
            // wanted state reached - terminate loop
            break;
        }
    }

    if (oldLodInvWidth > _lodInvWidth)
    {
        // going better - check for oscilation
        if (_lastScaleWorseTime > Glob.uiTime - 5)
        {
            // avoid change in this direction - restore state
            _lodInvWidth = oldLodInvWidth;
            complexity = AdjustComplexity(_drawObjects);
            complexity += AdjustShadowComplexity(_drawObjects);
            complexity += retainedAt(_lodInvWidth);
        }
        _lastScaleBetterTime = Glob.uiTime;
    }
    if (oldLodInvWidth < _lodInvWidth)
    {
        // going worse - check for oscilation
        if (_lastScaleBetterTime > Glob.uiTime - 0.5)
        {
            // avoid change in this direction - restore state
            _lodInvWidth = oldLodInvWidth;
            complexity = AdjustComplexity(_drawObjects);
            complexity += AdjustShadowComplexity(_drawObjects);
            complexity += retainedAt(_lodInvWidth);
        }
        _lastScaleWorseTime = Glob.uiTime;
    }

    // store complexity to complexity history
    for (int i = 1; i < NStoreComplexities; i++)
    {
        _lastComplexity[i - 1] = _lastComplexity[i];
    }
    _lastComplexity[NStoreComplexities - 1] = complexity;

    float avgComplexity = 0;
    for (int i = 0; i < NStoreComplexities; i++)
    {
        avgComplexity += _lastComplexity[i];
    }
    avgComplexity *= 1.0f / NStoreComplexities;
    if (ShadowLodStatsEnabled())
    {
        LOG_INFO(Graphics, "SHADOW_LOD_STATS frame passes={} objectVisits={} propQueries={} drawObjects={}",
                 GShadowLodStatPasses, GShadowLodStatObjects, GShadowLodStatQueries, _drawObjects.Size());
        GShadowLodStatPasses = 0;
        GShadowLodStatObjects = 0;
        GShadowLodStatQueries = 0;
    }
}
float Scene::GetSmokeGeneralization() const
{
    return _lodInvWidth * 0.1f;
}

#else

static inline float CoveredArea(Object* obj, float dist2, float oScale)
{
    LODShape* shape = obj->GetShape();
    if (!shape)
        return 0;
    float radius = shape->BoundingSphere() * oScale;

    Camera* cam = GScene->GetCamera();
    // return area in pixels
    float areaK = cam->InvLeft() * cam->InvTop() * GEngine->Width() * GEngine->Height();

    const float maxArea = 0.5f;
    // if (Square(radius)/dist2>maxArea)
    if (Square(radius) > dist2 * maxArea)
    {
        return maxArea * areaK;
    }
    return Square(radius) * areaK / dist2;
}

static int CmpCoveredAreaObj(const SortObjectItem* p1, const SortObjectItem* p0)
{
    // the first object is the nearest one
    const SortObject* o0 = *p0;
    const SortObject* o1 = *p1;
    float dif = (CoveredArea(o0->object, o0->distance2, o0->object->Scale()) -
                 CoveredArea(o1->object, o1->distance2, o1->object->Scale()));
    if (dif < 0)
        return -1;
    if (dif > 0)
        return +1;
    // make sure ordering is stable
    dif = o0->distance2 - o1->distance2;
    if (dif < 0)
        return +1;
    if (dif > 0)
        return -1;
    return CmpShapeObj(p1, p0);
}

static inline int Complexity(LODShape* lShape, int level)
{
    return lShape->Level(level)->NFaces();
}

static int FindLevelWithComplexity(LODShape* lShape, float complexity, float maxDif)
{
    PoseidonAssert(complexity >= 0);
    int bestI = -1;
    float bestDif = maxDif;
    for (int i = 0; i < lShape->NLevels(); i++)
    {
        float resol = lShape->Resolution(i);
        if (resol > 900)
            break;
        int lComplex = Complexity(lShape, i);
        float dif = fabs(complexity - lComplex);
        if (bestDif > dif)
        {
            bestDif = dif;
            bestI = i;
        }
    }
    return bestI;
}

static int FindShadowLevelWithComplexity(LODShape* lShape, float complexity, float maxDif)
{
    int i = FindLevelWithComplexity(lShape, complexity, maxDif);
    if (i < 0)
        return i;
    return lShape->FindNearestWithoutProperty(i, "lodnoshadow");
}

void Scene::AdjustComplexity()
{
#if _ENABLE_CHEATS
    static int displayComplexityLimit = 100000;
#endif

    // set _lodInvWidth in case any needs it
    // development only: it should not be used when Density Lod system is active
    _lodInvWidth = (_minLodInvWidth + _maxLodInvWidth) * 0.5f;

    int shadowFactorI = ShadowFactor(this);

#if _ENABLE_CHEATS
    static float maxDensity = 0.3; // max. 5 polygons per pixel wanted
    static float shadowAreaCoef = 1.0f / 32;
    // static float invShadowAreaCoef = 1/shadowAreaCoef;
    auto& input = InputSubsystem::Instance();
    if (input.GetCheat2ToDo(SDL_SCANCODE_LEFTBRACKET))
    {
        maxDensity /= 1.2f;
    }
    if (input.GetCheat2ToDo(SDL_SCANCODE_RIGHTBRACKET))
    {
        maxDensity *= 1.2f;
    }

    if (input.GetCheat1ToDo(SDL_SCANCODE_LEFTBRACKET))
    {
        shadowAreaCoef /= 1.5f;
        // invShadowAreaCoef = 1/shadowAreaCoef;
    }
    if (input.GetCheat1ToDo(SDL_SCANCODE_RIGHTBRACKET))
    {
        shadowAreaCoef *= 1.5f;
        // invShadowAreaCoef = 1/shadowAreaCoef;
    }
#else
    const float maxDensity = 0.3; // max. 5 polygons per pixel wanted
    const float shadowAreaCoef = 1.0f / 32;
// const float invShadowAreaCoef = 1/shadowAreaCoef;
#endif

    // calculate total covered area
    float totalArea = 0;
    // sum complexity of objects that are excluded from lod management
    int complexityUsed = 0;
    for (int i = 0; i < _drawObjects.Size(); i++)
    {
        SortObject* oi = _drawObjects[i];
        Object* obj = oi->object;
        if (oi->forceDrawLOD >= 0)
        {
            // count complexity as used
            if (oi->forceDrawLOD != LOD_INVISIBLE)
            {
                complexityUsed += obj->GetComplexity(oi->forceDrawLOD, *obj);
            }
        }
        else
        {
            float area = CoveredArea(obj, oi->distance2, obj->Scale());
            totalArea += area;
        }

        LODShape* shape = obj->GetShape();
        if (shape && !(shape->Special() & NoShadow) && oi->distance2 < Square(_shadowFogMaxRange) &&
            shadowFactorI >= 12 && obj->CastShadow())
        {
            float area = CoveredArea(obj, oi->distance2, obj->Scale());
            totalArea += area * shadowAreaCoef;
        }
        else
        {
            oi->shadowLOD = LOD_INVISIBLE;
        }
    }

    // if total area is zero, there are no controlled visible objects and density does not matter
    // we have total area, we know how much complexity we want - wa may calculate density now
    const int wantedComplexity = 30000;
    complexityUsed += toLargeInt(Square(ENGINE_CONFIG.horizontZ * (1.0f / 50)));

    int complexity = complexityUsed;

    float density = (wantedComplexity - complexity) / floatMax(totalArea, 1e-20);
    saturate(density, 0, maxDensity);

    float density0 = density;
    float totalArea0 = totalArea;
    // note: if too many polygons were already used, density may be negative

    float minArea = totalArea * 1e-4f;
    // first pass: select object which use lod 0
    // such objects often do not use available complexity
    for (int i = 0; i < _drawObjects.Size(); i++)
    {
        SortObject* oi = _drawObjects[i];
        Object* obj = oi->object;
        if (oi->forceDrawLOD < 0)
        {
            LODShape* lShape = obj->GetShape();
            float area = CoveredArea(obj, oi->distance2, obj->Scale());
            float oComplexity = area * density;
            int level = FindLevelWithComplexity(lShape, oComplexity, oComplexity * 1.5f + 200);
            if (level == 0)
            {
                int levelComplexity = Complexity(lShape, level);
#if _ENABLE_CHEATS
                if (abs(oComplexity - levelComplexity) > displayComplexityLimit)
                {
                    LOG_DEBUG(Graphics, "{} - complexity {}, wanted {:.0f}", (const char*)lShape->GetName(),
                              levelComplexity, oComplexity);
                }
#endif
                complexity += levelComplexity;
                oi->passNum = obj->PassNum(level);
                oi->drawLOD = level;

                totalArea -= area;
            }
        }
        else
        {
            if (oi->forceDrawLOD != LOD_INVISIBLE)
            {
                oi->passNum = obj->PassNum(oi->forceDrawLOD);
            }
            oi->drawLOD = oi->forceDrawLOD;
        }
        // similiar for shadows
        if (oi->shadowLOD < 0)
        {
            LODShape* lShape = obj->GetShape();
            float area = CoveredArea(obj, oi->distance2, obj->Scale());
            float oComplexity = area * density * shadowAreaCoef;
            int level = FindShadowLevelWithComplexity(lShape, oComplexity, oComplexity * 1.5f + 200);
            if (level == 0) // lShape->_minShadow should be used here instead
            {
                int levelComplexity = Complexity(lShape, level);
                complexity += levelComplexity;
                oi->shadowLOD = level;

                totalArea -= area * shadowAreaCoef;
            }
        }
    }

    // when we rendered almost everything, there is no need to update density any more
    if (totalArea > minArea)
    {
        density = (wantedComplexity - complexity) / floatMax(totalArea, 1e-20);
        saturate(density, 0, maxDensity);
    }

    // selects lods for all other objects
    for (int i = 0; i < _drawObjects.Size(); i++)
    {
        SortObject* oi = _drawObjects[i];
        Object* obj = oi->object;
        if (oi->drawLOD < 0)
        {
            LODShape* lShape = obj->GetShape();
            float area = CoveredArea(obj, oi->distance2, obj->Scale());
            float oComplexity = area * density;
            int level = FindLevelWithComplexity(lShape, oComplexity, oComplexity * 1.5f + 200);
            if (level < 0)
            {
                oi->drawLOD = LOD_INVISIBLE;
            }
            else
            {
                int levelComplexity = Complexity(lShape, level);
#if _ENABLE_CHEATS
                if (abs(oComplexity - levelComplexity) > displayComplexityLimit)
                {
                    LOG_DEBUG(Graphics, "{} - complexity {}, wanted {:.0f}", (const char*)lShape->GetName(),
                              levelComplexity, oComplexity);
                }
#endif
                complexity += levelComplexity;
                oi->passNum = obj->PassNum(level);
                oi->drawLOD = level;
            }
        }
        if (oi->shadowLOD < 0)
        {
            LODShape* lShape = obj->GetShape();
            float area = CoveredArea(obj, oi->distance2, obj->Scale());
            float oComplexity = area * density * shadowAreaCoef;
            int level = FindShadowLevelWithComplexity(lShape, oComplexity, oComplexity * 1.5f + 200);
            if (level < 0)
            {
                oi->shadowLOD = LOD_INVISIBLE;
            }
            else
            {
                int levelComplexity = Complexity(lShape, level);
#if _ENABLE_CHEATS
                if (abs(oComplexity - levelComplexity) > displayComplexityLimit)
                {
                    LOG_DEBUG(Graphics, "{} - complexity {}, wanted {:.0f}", (const char*)lShape->GetName(),
                              levelComplexity, oComplexity);
                }
#endif
                complexity += levelComplexity;
                oi->shadowLOD = level;
            }
        }
    }

    // store complexity to complexity history
    for (int i = 1; i < NStoreComplexities; i++)
    {
        _lastComplexity[i - 1] = _lastComplexity[i];
    }
    _lastComplexity[NStoreComplexities - 1] = complexity;

    float avgComplexity = 0;
    for (int i = 0; i < NStoreComplexities; i++)
    {
        avgComplexity += _lastComplexity[i];
    }
    avgComplexity *= 1.0f / NStoreComplexities;
#if 1
    GlobalShowMessage(
        500,
        "Complex %8d, ~ %8.0f, density %0.5f (%0.5f) < %0.5f, totArea %0.f (%0.f), check %0.1f, cUsed %d, shadC %.2f",
        complexity, avgComplexity, density, density0, maxDensity, totalArea, totalArea0, density0 * totalArea0,
        complexityUsed, 1 / shadowAreaCoef);
#endif
}
float Scene::GetSmokeGeneralization() const
{
    return (_minLodInvWidth + _maxLodInvWidth) * 0.5f * 0.1f;
}
#endif

// Cockpit pass routing — the one policy site.  The camera vehicle is
// queued at its inside-view LOD (World::Draw); only that queue ever
// selects an object's InsideLOD level, so equality here means "this is
// the first-person interior" — soldier hands/body, vehicle cockpit.
// Those draws carry PassKindHint::Cockpit so the descriptor build picks
// the Cockpit* pass family explicitly instead of inferring it from the
// shape's NoDropdown faces.
static void DrawSortObject(SortObject* oi)
{
    Object* obj = oi->object;
    // GPU-driven rendering (docs/gpu-culling-and-depth-plan.md §12 / Stage 3b): objects handed
    // to the GPU retained scene are culled + drawn by the compute/indirect path. Full-coverage
    // objects are diverted out of the draw list entirely (Scene::ObjectForDrawing never creates a
    // SortObject for them), so they normally never reach here — this Full guard is a defensive
    // backstop. Partial-coverage objects (buildings — the GPU draws the opaque geometry, but the
    // CPU still paints interior furniture proxies + blend/decal sections) DO keep their CPU draw
    // with GSkipGpuOwnedSections set, so Shape::Draw drops the GPU-owned sections and never
    // repaints them. No-op unless WGR_GPU_DRIVEN is on and obj registered.
    const GpuDrawCoverage cov = GEngine->GpuDrivenCoverage(obj);
    if (cov == GpuDrawCoverage::Full)
    {
        return;
    }
    // RAII: set the owned-section skip for a Partial object's draw, restore on every exit.
    struct SkipOwnedGuard
    {
        bool prev;
        explicit SkipOwnedGuard(bool v) : prev(GSkipGpuOwnedSections) { GSkipGpuOwnedSections = v; }
        ~SkipOwnedGuard() { GSkipGpuOwnedSections = prev; }
    } skipOwnedGuard(cov == GpuDrawCoverage::Partial);
    // REN-TEMP-001H: identity bracket for motion vectors. Placed HERE and not inside
    // Object::Draw because DrawProxies re-enters Object::Draw on proxy Objects, which are
    // shape-shared across every placement of a model — the parent must own the whole
    // scope. Instanced runs call this for the head object only, but their gate requires
    // Static(), so no moving rigid object is ever mis-attributed. RAII like the guards
    // above: this function has several exits.
    struct DrawObjectGuard
    {
        const void* prev;
        explicit DrawObjectGuard(const Object* obj) : prev(GEngine->GetDrawObject())
        {
            GEngine->SetDrawObject(obj);
        }
        ~DrawObjectGuard() { GEngine->SetDrawObject(prev); }
    } drawObjectGuard(obj);
    // Cockpit identity and explicit inside-view selection must both agree.
    const bool cockpit = GWorld && obj == GWorld->DrawInsideVehicle() &&
        ExplicitInsideViewDraw(oi->drawLOD, oi->forceDrawLOD, obj->InsideLOD(GWorld->GetCameraType()));
    // REN-INTERP-001: moving objects draw at their interpolated frame; everything else at
    // its own (RenderFrame returns *obj for those, one branch).
    FrameBase interpFrame;
    const FrameBase& drawFrame = obj->RenderFrame(interpFrame);
    if (!cockpit)
    {
        obj->Draw(oi->drawLOD, oi->orClip, drawFrame);
        return;
    }
    const render::PassKindHint savedHint = GEngine->GetPassKindHint();
    // Both hints route into the Cockpit* pass family; they differ only in
    // whether the backend treats the draw as an enclosed interior for ambient.
    GEngine->SetPassKindHint(obj->InsideLODIsEnclosed() ? render::PassKindHint::Cockpit
                                                        : render::PassKindHint::FirstPersonBody);
    obj->Draw(oi->drawLOD, oi->orClip, drawFrame);
    GEngine->SetPassKindHint(savedHint);
}

// Storage for the Pass1 submission sub-split declared in Pass1Submit.hpp. Defined here
// because SceneDraw.cpp owns the loop that turns it on; Object.cpp and ShapeDraw.cpp only
// contribute to it.
namespace Poseidon
{
namespace Pass1Submit
{
bool gActive = false;
int gDepth = 0;
std::atomic<uint64_t> gScanNs{0};
std::atomic<uint64_t> gAnimateNs{0};
std::atomic<uint64_t> gClipFogNs{0};
std::atomic<uint64_t> gLightsNs{0};
std::atomic<uint64_t> gProxiesNs{0};
std::atomic<uint64_t> gTexturesNs{0};
std::atomic<uint64_t> gShapeDrawNs{0};
std::atomic<uint64_t> gRuns{0};
std::atomic<uint64_t> gRunObjs{0};
std::atomic<uint64_t> gScalarObjs{0};
std::atomic<uint64_t> gSkipped{0};
std::atomic<uint64_t> gObjDraws{0};
std::atomic<uint64_t> gProxyDraws{0};
std::atomic<uint64_t> gSectionsSeen{0};
std::atomic<uint64_t> gSectionsDrawn{0};
std::atomic<uint64_t> gHeads{0};
std::atomic<uint64_t> gVetoLights{0};
std::atomic<uint64_t> gVetoNotStatic{0};
std::atomic<uint64_t> gVetoProxies{0};
std::atomic<uint64_t> gVetoRouting{0};
std::atomic<uint64_t> gRunsShort{0};
std::atomic<uint64_t> gRunShortLen{0};
std::atomic<uint64_t> gSecClassCalls{0};
std::atomic<uint64_t> gSecClassMisses{0};
std::atomic<uint64_t> gSecTLNs{0};
std::atomic<uint64_t> gSecTLCalls{0};
std::atomic<uint64_t> gSecBindNs{0};
std::atomic<uint64_t> gSecBindCalls{0};
std::atomic<uint64_t> gSecReflNs{0};
std::atomic<uint64_t> gSecDescNs{0};
std::atomic<uint64_t> gSecAlphaNs{0};
std::atomic<uint64_t> gSecLightNs{0};
std::atomic<uint64_t> gSecBindFast{0};
std::atomic<uint64_t> gSecBindResolves{0};
std::atomic<uint64_t> gWouldRuns{0};
std::atomic<uint64_t> gWouldObjs{0};
std::atomic<uint64_t> gWouldMaxLen{0};

// Same env var as Pass1Stats on purpose: the sub-split is only ever read next to the
// six-way split it refines, and a second switch would let the two be enabled apart, which
// is how a share gets quoted against a total that was never measured.
bool Enabled()
{
    static const bool enabled = std::getenv("WGR_PASS1_STATS") != nullptr;
    return enabled;
}
} // namespace Pass1Submit
} // namespace Poseidon

namespace
{

// ---------------------------------------------------------------------------------------
// Pass1 sub-phase attribution.
//
// `land:obj` IS this function, and on a settled Stratis it is 83% of an 87 ms CPU frame —
// but as one number it names no line of code. This splits it six ways.
//
// MEASURED ANSWER (Stratis, settled, governor 16, 1024 frames, 11,974 objects/frame):
//
//   compact 0.046 | complexity 1.040 | mergers 0.197 | occlusion 0.000 | sort 0.857
//   | submit 35.453   ms/frame
//
// Submission is 35.45 of 37.59 ms — 94% of the phase. Two leads die on that line:
// AdjustComplexity is 1.04 ms, so memoising its per-LOD property scan cannot be worth
// more than ~1 ms of 37; and the occlusion pre-pass is 0.000 with 0 TestBBox calls,
// confirming by measurement what the WGR_OBJ_OCCLUSION comment below establishes by
// inspection. The target is the ~3 us per object in the submission loop.
//
// AND IT IS NOT SUPERLINEAR — an earlier version of this comment said it was, from
// comparing the two governor arms (4,337 -> 12,477 instances for 67.4 -> 309.5 ms, x2.88
// for x4.59). That inference was wrong: those arms differ in LOD as well as count, so the
// objects are not merely more numerous but individually heavier (238 -> 298 triangles
// each). Within ONE arm, submit cost per object is 3.27 -> 3.03 -> 2.96 us as the set
// grows from 3,986 to 11,974 — flat, if anything falling as batching improves. Pass1 is
// linear; it is simply expensive per object.
//
// Deliberately NOT a FrameProfiler phase. FrameProfiler::Mark is a boundary model — the time
// lands in whichever phase is current — so a sub-phase inside PhaseDrawLandObjects would be
// SUBTRACTED from land:obj rather than nested inside it, and would change the phase list that
// farfield-compare.py reads. This is a separate cumulative tally that cannot disturb either.
//
// WGR_PASS1_STATS=1 turns it on. Off, it costs one cached bool test per frame.
struct Pass1Stats
{
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> compactUs{0};   // the notUsed sweep
    std::atomic<uint64_t> complexityUs{0};// AdjustComplexity — up to 7 passes over the set
    std::atomic<uint64_t> mergersUs{0};   // _drawMergers rebuild
    std::atomic<uint64_t> occlusionUs{0}; // software occlusion pre-pass — SKIPPED under gpu_driven
    std::atomic<uint64_t> sortUs{0};      // SortDrawMergersByShape
    std::atomic<uint64_t> submitUs{0};    // instanced-run batching + DrawSortObject
    std::atomic<uint64_t> objects{0};     // _drawMergers.Size() summed, to divide by
    std::atomic<uint64_t> occTests{0};    // Occlusions::TestBBox calls
    std::atomic<uint64_t> occRenders{0};  // Occlusions::RenderShape calls (occluders ADDED)
    // THE COVERAGE SPLIT — why an object is in this phase at ALL.
    //
    // Tallied per frame in Scene::ObjectForDrawing and, until now, readable only from the
    // one-shot PERF-shapes dump that `triPerfDumpShapes` arms — i.e. never from a
    // non-interactive benchmark, which is the only way these worlds get measured. Summed
    // here so the question "why does Stratis put 11,974 objects through Pass1 when Everon
    // puts 1" is answered by a number instead of by inference from the object counts.
    //
    // full = diverted onto the GPU-driven path, no SortObject, out of this walk entirely.
    // partial = GPU owns the opaque geometry, the CPU still draws the blend/decal/proxy
    // complement. none = fully CPU.
    std::atomic<uint64_t> covFull{0};
    std::atomic<uint64_t> covPartial{0};
    std::atomic<uint64_t> covNone{0};
    // PASS 2, the other half of the object draw, split the same way and reported on the
    // same line. It is the dominant CPU cost on the two worlds Pass1 does NOT explain:
    // Arma 1 Sahrani (24.424 ms of a 33.7 ms drw:land) and DayZ Chernarus (23.2 ms of a
    // 37.9 ms CPU frame). Four blocks, and any one of them could be all of it:
    //   surfSort  - the road/decal PassOrder sort
    //   surfDraw  - road/decal blend sections, drawn before the shadow pass on purpose
    //   shadow    - the projected shadow accumulator, or the shadow-map depth pass
    //   alphaSort - the back-to-front radix over every merger
    //   alphaDraw - whole-alpha objects and the blend sections of opaque ones
    std::atomic<uint64_t> p2SurfSortUs{0};
    std::atomic<uint64_t> p2SurfDrawUs{0};
    std::atomic<uint64_t> p2ShadowUs{0};
    std::atomic<uint64_t> p2AlphaSortUs{0};
    std::atomic<uint64_t> p2AlphaDrawUs{0};
    std::atomic<uint64_t> p2ShadowCasters{0};
    std::atomic<uint64_t> p2AlphaDrawn{0};
};

Pass1Stats GPass1Stats;

bool Pass1StatsEnabled()
{
    static const bool enabled = std::getenv("WGR_PASS1_STATS") != nullptr;
    return enabled;
}

uint64_t Pass1NowUs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// A scope that bills its lifetime to one counter, and nothing when stats are off.
struct Pass1Scope
{
    std::atomic<uint64_t>* sink;
    uint64_t began;
    explicit Pass1Scope(std::atomic<uint64_t>& into)
        : sink(Pass1StatsEnabled() ? &into : nullptr), began(sink ? Pass1NowUs() : 0)
    {
    }
    // End the measurement early, where a preprocessor block makes a brace-scoped lifetime
    // awkward. Idempotent, so the destructor cannot double-count after an explicit Close.
    void Close()
    {
        if (sink)
        {
            sink->fetch_add(Pass1NowUs() - began, std::memory_order_relaxed);
            sink = nullptr;
        }
    }
    ~Pass1Scope() { Close(); }
};

// Averages per frame and per object, on a power-of-two frame count. Per-OBJECT is the column
// that matters: a phase whose us/object is flat is linear and uninteresting no matter how big
// it is, and the one whose us/object climbs with the set is the one to fix.
void ReportPass1Stats()
{
    const uint64_t frames = GPass1Stats.frames.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((frames & (frames - 1)) != 0)
    {
        return;
    }
    const double f = static_cast<double>(frames);
    const double objs = static_cast<double>(GPass1Stats.objects.load(std::memory_order_relaxed));
    const auto per = [&](const std::atomic<uint64_t>& v) { return v.load(std::memory_order_relaxed) / f / 1000.0; };
    const double full = GPass1Stats.covFull.load(std::memory_order_relaxed) / f;
    const double partial = GPass1Stats.covPartial.load(std::memory_order_relaxed) / f;
    const double none = GPass1Stats.covNone.load(std::memory_order_relaxed) / f;
    const double covTotal = full + partial + none;
    LOG_INFO(Graphics,
             "PERF pass1: frames={} objs/frame={:.0f} | compact={:.3f} complexity={:.3f} mergers={:.3f} "
             "occlusion={:.3f} sort={:.3f} submit={:.3f} ms/frame | occlusion={:.2f} submit={:.2f} us/object "
             "| occTests/frame={:.0f} occRenders/frame={:.0f} "
             "| coverage full={:.0f} ({:.1f}%) partial={:.0f} none={:.0f}",
             frames, objs / f, per(GPass1Stats.compactUs), per(GPass1Stats.complexityUs), per(GPass1Stats.mergersUs),
             per(GPass1Stats.occlusionUs), per(GPass1Stats.sortUs), per(GPass1Stats.submitUs),
             objs > 0 ? GPass1Stats.occlusionUs.load(std::memory_order_relaxed) / objs : 0.0,
             objs > 0 ? GPass1Stats.submitUs.load(std::memory_order_relaxed) / objs : 0.0,
             GPass1Stats.occTests.load(std::memory_order_relaxed) / f,
             GPass1Stats.occRenders.load(std::memory_order_relaxed) / f, full,
             covTotal > 0.0 ? full * 100.0 / covTotal : 0.0, partial, none);
    // The submission sub-split. Reported on its own line, with a count beside every
    // bucket, and with the per-object column that says whether a big number is many cheap
    // items or few dear ones. `probe` is the sum of the sub-buckets against the `submit`
    // number on the line above: the gap between them is this instrumentation's own clock
    // reads plus whatever of DrawSortObject falls outside a bucket, and quoting a share
    // without it would overstate every part.
    {
        namespace P1S = Poseidon::Pass1Submit;
        const auto ns = [&](const std::atomic<uint64_t>& v)
        { return v.load(std::memory_order_relaxed) / f / 1e6; };
        const auto cnt = [&](const std::atomic<uint64_t>& v) { return v.load(std::memory_order_relaxed) / f; };
        const double objDraws = static_cast<double>(P1S::gObjDraws.load(std::memory_order_relaxed));
        const auto perObjUs = [&](const std::atomic<uint64_t>& v)
        { return objDraws > 0.0 ? v.load(std::memory_order_relaxed) / objDraws / 1000.0 : 0.0; };
        LOG_INFO(Graphics,
                 "PERF submit: scan={:.3f} animate={:.3f} clipfog={:.3f} lights={:.3f} proxies={:.3f} "
                 "textures={:.3f} shapeDraw={:.3f} ms/frame | per objDraw us: animate={:.2f} clipfog={:.2f} "
                 "lights={:.2f} proxies={:.2f} textures={:.2f} shapeDraw={:.2f}",
                 ns(P1S::gScanNs), ns(P1S::gAnimateNs), ns(P1S::gClipFogNs), ns(P1S::gLightsNs),
                 ns(P1S::gProxiesNs), ns(P1S::gTexturesNs), ns(P1S::gShapeDrawNs), perObjUs(P1S::gAnimateNs),
                 perObjUs(P1S::gClipFogNs), perObjUs(P1S::gLightsNs), perObjUs(P1S::gProxiesNs),
                 perObjUs(P1S::gTexturesNs), perObjUs(P1S::gShapeDrawNs));
        LOG_INFO(Graphics,
                 "PERF submit counts/frame: runs={:.0f} runObjs={:.0f} scalarObjs={:.0f} skipped={:.0f} "
                 "objDraws={:.0f} proxyDraws={:.0f} sectionsSeen={:.0f} sectionsDrawn={:.0f} "
                 "| sections/objDraw={:.1f} proxies/objDraw={:.2f}",
                 cnt(P1S::gRuns), cnt(P1S::gRunObjs), cnt(P1S::gScalarObjs), cnt(P1S::gSkipped),
                 cnt(P1S::gObjDraws), cnt(P1S::gProxyDraws), cnt(P1S::gSectionsSeen), cnt(P1S::gSectionsDrawn),
                 objDraws > 0.0 ? P1S::gSectionsSeen.load(std::memory_order_relaxed) / objDraws : 0.0,
                 objDraws > 0.0 ? P1S::gProxyDraws.load(std::memory_order_relaxed) / objDraws : 0.0);
        LOG_INFO(Graphics,
                 "PERF submit batching/frame: heads={:.0f} vetoLights={:.0f} vetoNotStatic={:.0f} "
                 "vetoProxies={:.0f} vetoRouting={:.0f} runsShort={:.0f} shortLen/run={:.2f}",
                 cnt(P1S::gHeads), cnt(P1S::gVetoLights), cnt(P1S::gVetoNotStatic), cnt(P1S::gVetoProxies),
                 cnt(P1S::gVetoRouting), cnt(P1S::gRunsShort),
                 P1S::gRunsShort.load(std::memory_order_relaxed) > 0
                     ? static_cast<double>(P1S::gRunShortLen.load(std::memory_order_relaxed)) /
                           static_cast<double>(P1S::gRunsShort.load(std::memory_order_relaxed))
                     : 0.0);
        LOG_INFO(Graphics,
                 "PERF submit batchable/frame: wouldRuns={:.0f} wouldObjs={:.0f} ofObjDraws={:.1f}% "
                 "meanRunLen={:.1f} maxRunLen={}",
                 cnt(P1S::gWouldRuns), cnt(P1S::gWouldObjs),
                 objDraws > 0.0 ? P1S::gWouldObjs.load(std::memory_order_relaxed) * 100.0 / objDraws : 0.0,
                 P1S::gWouldRuns.load(std::memory_order_relaxed) > 0
                     ? static_cast<double>(P1S::gWouldObjs.load(std::memory_order_relaxed)) /
                           static_cast<double>(P1S::gWouldRuns.load(std::memory_order_relaxed))
                     : 0.0,
                 P1S::gWouldMaxLen.load(std::memory_order_relaxed));
        // PERF-015. Says whether the per-section classification cache was ON and how well it
        // held, so a null A/B result cannot be read as "caching does not help" when what
        // actually happened is that the lever was off in both arms. `calls=0` means the cache
        // was never entered at all.
        const uint64_t ccCalls = P1S::gSecClassCalls.load(std::memory_order_relaxed);
        const uint64_t ccMiss = P1S::gSecClassMisses.load(std::memory_order_relaxed);
        LOG_INFO(Graphics,
                 "PERF section class cache: enabled={} calls/frame={:.0f} misses/frame={:.2f} hitRate={:.3f}% "
                 "(cumulative calls={} misses={})",
                 ccCalls > 0 ? 1 : 0, ccCalls / f, ccMiss / f,
                 ccCalls > 0 ? (ccCalls - ccMiss) * 100.0 / static_cast<double>(ccCalls) : 0.0, ccCalls, ccMiss);
        // PERF-016. The DrawSectionTL subdivision, with per-call microseconds against the
        // gSecTLCalls denominator so "many cheap sections" and "few dear ones" stay
        // distinguishable. `other` is the body minus the named buckets: the early header,
        // the trace getenv tests, the memcpy/conform fill and the push_backs -- plus this
        // instrumentation's own inner clock reads, so it is an upper bound on the
        // unattributed remainder. The bindCache pair is the WGR_SECTION_BIND_CACHE lever
        // counting itself: fast+resolve = 0 means the lever was OFF, not ineffective.
        const uint64_t tlNs = P1S::gSecTLNs.load(std::memory_order_relaxed);
        const uint64_t namedNs = P1S::gSecBindNs.load(std::memory_order_relaxed) +
                                 P1S::gSecReflNs.load(std::memory_order_relaxed) +
                                 P1S::gSecDescNs.load(std::memory_order_relaxed) +
                                 P1S::gSecAlphaNs.load(std::memory_order_relaxed) +
                                 P1S::gSecLightNs.load(std::memory_order_relaxed);
        const double tlCalls = static_cast<double>(P1S::gSecTLCalls.load(std::memory_order_relaxed));
        const auto perCallUs = [&](uint64_t v) { return tlCalls > 0.0 ? v / tlCalls / 1000.0 : 0.0; };
        LOG_INFO(Graphics,
                 "PERF drawSection: calls/frame={:.0f} bindCalls/frame={:.0f} | total={:.3f} bind={:.3f} "
                 "refl={:.3f} desc={:.3f} alpha={:.3f} light={:.3f} other={:.3f} ms/frame | per call us: "
                 "total={:.3f} bind={:.3f} refl={:.3f} desc={:.3f} alpha={:.3f} light={:.3f} other={:.3f} "
                 "| bindCache fast/frame={:.0f} resolves={}",
                 cnt(P1S::gSecTLCalls), cnt(P1S::gSecBindCalls), ns(P1S::gSecTLNs), ns(P1S::gSecBindNs),
                 ns(P1S::gSecReflNs), ns(P1S::gSecDescNs), ns(P1S::gSecAlphaNs), ns(P1S::gSecLightNs),
                 (tlNs - std::min(tlNs, namedNs)) / f / 1e6, perCallUs(tlNs),
                 perCallUs(P1S::gSecBindNs.load(std::memory_order_relaxed)),
                 perCallUs(P1S::gSecReflNs.load(std::memory_order_relaxed)),
                 perCallUs(P1S::gSecDescNs.load(std::memory_order_relaxed)),
                 perCallUs(P1S::gSecAlphaNs.load(std::memory_order_relaxed)),
                 perCallUs(P1S::gSecLightNs.load(std::memory_order_relaxed)),
                 perCallUs(tlNs - std::min(tlNs, namedNs)),
                 cnt(P1S::gSecBindFast), P1S::gSecBindResolves.load(std::memory_order_relaxed));
    }
    LOG_INFO(Graphics,
             "PERF pass2: surfSort={:.3f} surfDraw={:.3f} shadow={:.3f} alphaSort={:.3f} alphaDraw={:.3f} ms/frame "
             "| shadowCasters={:.0f} alphaDrawn={:.0f}",
             per(GPass1Stats.p2SurfSortUs), per(GPass1Stats.p2SurfDrawUs), per(GPass1Stats.p2ShadowUs),
             per(GPass1Stats.p2AlphaSortUs), per(GPass1Stats.p2AlphaDrawUs),
             GPass1Stats.p2ShadowCasters.load(std::memory_order_relaxed) / f,
             GPass1Stats.p2AlphaDrawn.load(std::memory_order_relaxed) / f);
}

} // namespace

void Scene::DrawObjectsAndShadowsPass1()
{
    // select first objects - those with highest visual priority

    int s = 0, t = 0;
    {
        Pass1Scope scope(GPass1Stats.compactUs);
        for (; s < _drawObjects.Size(); s++)
        {
            SortObject* sObj = _drawObjects[s];
            if (!sObj->notUsed)
            {
                if (s != t)
                {
                    _drawObjects[t] = sObj;
                }
                t++;
            }
            else
            {
                sObj->object->SetInList(nullptr); // removed from the list
            }
        }
        _drawObjects.Resize(t);
    }

#if DO_STAT
    Alpha.Clear();
    Opaque.Clear();
    Shadow.Clear();
#endif

    // remove all objects that should not be used

    {
        Pass1Scope scope(GPass1Stats.complexityUs);
        AdjustComplexity();
    }

    // copy objects to working list (mergers)
    // do not copy objects that are not drawn
    {
        Pass1Scope scope(GPass1Stats.mergersUs);
        // make smaller only when really necessary
        int objNeed = _drawObjects.Size();
        int objHave = _drawMergers.MaxSize();
        if (objNeed >= objHave)
        {
            _drawMergers.Reserve(objNeed, objNeed);
        }
        else if (objNeed * 2 < objHave && objHave > 1024)
        {
            // no need to keep it big now - make it smaller
            _drawMergers.Realloc(objNeed);
        }
        _drawMergers.Resize(0);
        for (int s = 0; s < _drawObjects.Size(); s++)
        {
            SortObject* sObj = _drawObjects[s];
            if (sObj->drawLOD == LOD_INVISIBLE && sObj->shadowLOD == LOD_INVISIBLE)
            {
                continue;
            }
            if (!sObj->object || !sObj->shape)
            {
                continue;
            }
            _drawMergers.Add(sObj);
        }
    }
    // One-shot shape census (perf campaign): triPerfDumpShapes arms this;
    // the next Pass1 logs the top repeated shapes — the instancing-candidate
    // histogram (how many draws are N copies of the same tree/fence/house).
    extern bool gPerfDumpShapesOnce;
    if (gPerfDumpShapesOnce)
    {
        gPerfDumpShapesOnce = false;
        struct Bucket
        {
            const char* name;
            int count;
        };
        std::map<std::string, int> hist;
        for (int s = 0; s < _drawMergers.Size(); s++)
        {
            SortObject* so = _drawMergers[s];
            if (so && so->shape)
                hist[std::string((const char*)so->shape->GetName())]++;
        }
        std::vector<std::pair<int, std::string>> top;
        for (auto& kv : hist)
            top.push_back({kv.second, kv.first});
        std::sort(top.rbegin(), top.rend());
        LOG_INFO(Graphics, "PERF shapes: {} objects, {} unique shapes", _drawMergers.Size(), (int)hist.size());
        // Coverage split (tallied this frame in Scene::ObjectForDrawing): how much the GPU-render
        // divert removed (Full) vs what still rides the whole Pass1 walk (Partial complement / None
        // = fully CPU). EnableObjOcc: whether the software occlusion (Occlusion::TestBBox) is still
        // running — should be OFF (GPU Hi-Z replaces it) when GPU occlusion is active.
        extern int gCovFull, gCovPartial, gCovNone;
        extern bool EnableObjOcc;
        LOG_INFO(Graphics, "PERF coverage: Full(diverted)={} Partial(cpu-complement)={} None(cpu)={} | EnableObjOcc={}",
                 gCovFull, gCovPartial, gCovNone, EnableObjOcc ? 1 : 0);
        for (size_t i = 0; i < top.size() && i < 20; i++)
            LOG_INFO(Graphics, "PERF shape[{}]: {} x{}", (int)i, top[i].second.c_str(), top[i].first);
    }

    // if we sort by shape, we group object with similiar textures
    // sort by distance

#if _ENABLE_CHEATS
    if (InputSubsystem::Instance().GetCheat2ToDo(SDL_SCANCODE_H))
    {
        EnableObjOcc = !EnableObjOcc;
        GlobalShowMessage(500, "Object occlusions %s", EnableObjOcc ? "On" : "Off");
    }
#endif

    // WGR_OBJ_OCCLUSION forces the software occlusion pre-pass off (0) or ON (1).
    //
    // THE PRE-PASS DOES NOT RUN IN THE BENCHMARKED CONFIGURATION, so do not reach for it as
    // the explanation of the superlinearity above. EngineWgpu::PushSceneCamera executes
    // `EnableObjOcc = !_cullDebug.occlusion` EVERY FRAME while the GPU-driven path is on, and
    // CullDebugSettings::occlusion defaults to true, and _cullDebug is only ever written by
    // the dev panel's Culling tab, which a non-interactive benchmark never opens. Every
    // Stratis and Chernarus arm logs `gpu_driven=true`, so `EnableObjOcc` is false in all of
    // them and this whole block is skipped. Established by inspection, not by a run.
    //
    // That is why `1` exists and not only `0`. Pass1 runs after PushSceneCamera, so a value
    // set here wins for the frame either way — but an off-only switch could only disable
    // something already disabled, which would have measured as "no effect" and been misread
    // as "the pre-pass is cheap". It is not cheap; it is absent.
    //
    // Not a revert of anything: the cheat key (Shift-H) already toggles EnableObjOcc at
    // runtime. This only makes the same switch reachable from a non-interactive run, in both
    // directions, so the pre-pass can be priced if anyone wants it back for a CPU-driven path.
    {
        static const int occForce = []
        {
            const char* v = std::getenv("WGR_OBJ_OCCLUSION");
            return v ? (v[0] == '0' ? 0 : 1) : -1;
        }();
        if (occForce >= 0)
        {
            EnableObjOcc = occForce != 0;
        }
    }
    if (EnableObjOcc)
    {
        Pass1Scope scope(GPass1Stats.occlusionUs);
        // sort only what needs to checked/drawn for occlusion
        // this will remove especially cloudlets from occlusion testing
        // it also helps to maintain _drawMergers sorted
        // because sort is performed in different array
        // therefore _drawMergers sort may be performed incrementally
        AUTO_STATIC_ARRAY(Ref<SortObject>, occSort, 2048);
        for (int i = 0; i < _drawMergers.Size(); i++)
        {
            SortObject* oi = _drawMergers[i];
            if (oi->drawLOD == LOD_INVISIBLE)
            {
                continue;
            }
            Object* obj = oi->object;
            if (!obj)
            {
                continue;
            }
            LODShape* shape = oi->shape;
            if (!shape)
            {
                continue;
            }

            if (shape->CanBeOccluded() || shape->CanOcclude() && obj->OcclusionView())
            {
                occSort.Add(_drawMergers[i]);
            }
        }
        RadixSortByFloatDesc(occSort, [](const SortObject* o) { return o->distance2; });
// before drawing anything draw cockpit occlusion
// check if we are in internal view
#if 1
        if (GWorld->GetCameraType() == CamInternal && !GWorld->GetCameraEffect())
        {
            Object* obj = GWorld->CameraOn();
            if (obj)
            {
                int view = obj->InsideViewGeomLOD(CamInternal);
                if (view != LOD_INVISIBLE && view >= 0)
                {
                    // draw that particular view geometry
                    obj->AnimateComponentLevel(view);
                    // if object can occlude something, render its components

                    LODShape* lShape = obj->GetShape();

                    Shape* shape = lShape->Level(view);
                    const ConvexComponents* cc = lShape->GetConvexComponents(view);
                    if (cc)
                    {
                        GetOcclusions()->RenderShape(*obj, shape, *cc, ClipAll);
                    }
                    else
                    {
                        LOG_DEBUG(Graphics, "Inconsistent view geom in {}", (const char*)obj->GetDebugName());
                    }
                    obj->DeanimateComponentLevel(view);
                }
            }
        }
#endif

        // draw occlusions front to back
        for (int i = occSort.Size(); --i >= 0;)
        {
            SortObject* oi = occSort[i];
            Object* obj = oi->object;
            LODShape* shape = oi->shape;
            if (oi->drawLOD == LOD_INVISIBLE)
            {
                continue;
            }
            // select view geometry
            // no occlusions for or by camera vehicle
            if (obj == GWorld->CameraOn())
            {
                continue;
            }
            // if object is small and simple if should not be tested nor rendered
            bool occluded = false;
            // check object distance
            // if the objects is very near, do not check occlusion

            if (shape->CanBeOccluded())
            {
                // do not occlude things that are very near
                // the test would be very slow and is very like to fail
                if (FarEnoughForOcclusion(oi))
                {
                    Vector3 minMax[2];
                    obj->AnimatedMinMax(oi->drawLOD, minMax);
                    // check if object is occluded by objects
                    GPass1Stats.occTests.fetch_add(1, std::memory_order_relaxed);
                    if (!GetOcclusions()->TestBBox(*oi->object, minMax, oi->orClip))
                    {
                        occluded = true;
                    }
                }
            }
            if (!occluded)
            {
                if (shape->CanOcclude() && obj->OcclusionView())
                {
                    Shape* view = shape->ViewGeometryLevel();
                    if (!view)
                    {
                        continue;
                    }
                    // test if object is near enough to make a some occlusion
                    float areaNom = Square(oi->radius * _camera->InvLeft());
                    float areaDenom = oi->distance2;
                    if (areaNom > 0.001 * areaDenom)
                    {
                        obj->AnimateViewGeometry();
                        // if object can occlude something, render its components
                        // Counted: every one of these makes every SUBSEQUENT TestBBox above
                        // more expensive, which is the proposed superlinear mechanism.
                        GPass1Stats.occRenders.fetch_add(1, std::memory_order_relaxed);
                        GetOcclusions()->RenderShape(*oi->object, view, shape->GetViewComponents(), oi->orClip);
                        obj->DeanimateViewGeometry();
                    }
                }
            }
            else
            {
                // occluded - do not draw, do not render into occlusion buffer
                oi->drawLOD = LOD_INVISIBLE;
            }
        }
    }

    // Byte-identical to QSort(..., CmpShapeObj) but sorts a packed key array instead
    // of the scattered Ref<SortObject> array (see SortDrawMergersByShape).
    {
        Pass1Scope scope(GPass1Stats.sortUs);
        SortDrawMergersByShape(_drawMergers);
    }
    if (Pass1StatsEnabled())
    {
        // Batchability census (Pass1Submit.hpp gWould*). Placed HERE, outside the submitUs
        // scope, so it inflates neither `submit` nor any of its sub-buckets -- it does add
        // to the land:obj wall clock of a stats-enabled run, which is why the timing arms
        // and the census arm are reported from different runs.
        //
        // The clauses are the batcher's own, MINUS the scene-wide light veto and minus the
        // backend's capacity refusal, because the question is what the geometry offers, not
        // what today's code accepts.
        namespace P1S = Poseidon::Pass1Submit;
        uint64_t maxLen = 0;
        for (int i = 0; i < _drawMergers.Size();)
        {
            SortObject* oi = _drawMergers[i];
            if (oi->drawLOD == LOD_INVISIBLE || !oi->object || !oi->object->GetShape())
            {
                i++;
                continue;
            }
            LODShape* shape = oi->object->GetShape();
            Shape* sShape = shape->LevelOpaque(oi->drawLOD);
            if (!sShape || oi->passNum > 1 || !oi->object->Static() || sShape->NProxies() != 0)
            {
                i++;
                continue;
            }
            const int headSpecial = sShape->Special() | oi->object->GetObjSpecial();
            const float d2lo = oi->distance2 * 0.90f;
            const float d2hi = oi->distance2 * 1.10f;
            int runEnd = i + 1;
            while (runEnd < _drawMergers.Size())
            {
                SortObject* oj = _drawMergers[runEnd];
                if (!oj->object || oj->object->GetShape() != shape || oj->drawLOD != oi->drawLOD ||
                    oj->passNum != oi->passNum || !oj->object->Static() ||
                    (sShape->Special() | oj->object->GetObjSpecial()) != headSpecial || oj->distance2 < d2lo ||
                    oj->distance2 > d2hi)
                {
                    break;
                }
                runEnd++;
            }
            const uint64_t runLen = static_cast<uint64_t>(runEnd - i);
            if (runLen >= 4)
            {
                P1S::gWouldRuns.fetch_add(1, std::memory_order_relaxed);
                P1S::gWouldObjs.fetch_add(runLen, std::memory_order_relaxed);
                if (runLen > maxLen)
                {
                    maxLen = runLen;
                }
            }
            i = runEnd;
        }
        uint64_t prevMax = P1S::gWouldMaxLen.load(std::memory_order_relaxed);
        while (maxLen > prevMax && !P1S::gWouldMaxLen.compare_exchange_weak(prevMax, maxLen))
        {
        }
    }
    if (Pass1StatsEnabled())
    {
        GPass1Stats.objects.fetch_add(static_cast<uint64_t>(_drawMergers.Size()), std::memory_order_relaxed);
        // Sampled here, at the end of the same frame's walk, because ObjectForDrawing zeroes
        // these when the visible set is rebuilt — read any earlier and they are last frame's.
        extern int gCovFull, gCovPartial, gCovNone;
        GPass1Stats.covFull.fetch_add(static_cast<uint64_t>(gCovFull < 0 ? 0 : gCovFull), std::memory_order_relaxed);
        GPass1Stats.covPartial.fetch_add(static_cast<uint64_t>(gCovPartial < 0 ? 0 : gCovPartial),
                                         std::memory_order_relaxed);
        GPass1Stats.covNone.fetch_add(static_cast<uint64_t>(gCovNone < 0 ? 0 : gCovNone), std::memory_order_relaxed);
    }
    // first of all draw non-alpha objects

#if DRAW_OBJS
    {
        Pass1Scope scope(GPass1Stats.submitUs);
        // Submission sub-split (Pass1Submit.hpp). `scan` is derived by subtraction --
        // whole-loop elapsed minus the time billed to the draw calls -- rather than by a
        // Scope opened and closed twice per iteration, which would have doubled the clock
        // reads in the hottest loop in the phase being measured.
        namespace P1S = Poseidon::Pass1Submit;
        const bool subOn = P1S::Enabled();
        const uint64_t loopT0 = subOn ? P1S::NowNs() : 0;
        uint64_t drawNsLocal = 0;
        P1S::gActive = subOn;
        P1S::gDepth = 0;
        // Instanced runs (perf effort 08): _drawMergers is shape-sorted, so
        // identical static shapes arrive contiguously. A batchable run draws
        // the head once inside Begin/EndInstancedRun — every TL section then
        // renders all K instances from the WorldInstances matrix array. The
        // predicate keeps per-object state out of batches: static, proxy-free,
        // not OnSurface/IsColored, equal obj-special, no local lights in the
        // scene, and a tight distance band so the head's constant-fog value
        // is representative for the whole batch.
        const bool noLocalLights = NLights() == 0;
        for (int i = 0; i < _drawMergers.Size();)
        {
            SortObject* oi = _drawMergers[i];
            LODShape* shape = oi->object->GetShape();
            if (oi->drawLOD == LOD_INVISIBLE)
            {
                if (subOn)
                    P1S::gSkipped.fetch_add(1, std::memory_order_relaxed);
                i++;
                continue;
            }
            PoseidonAssert(oi->drawLOD >= 0);
            Shape* sShape = shape->LevelOpaque(oi->drawLOD);
            if (!sShape)
            {
                if (subOn)
                    P1S::gSkipped.fetch_add(1, std::memory_order_relaxed);
                i++;
                continue;
            }
            if (oi->passNum > 1)
            {
                if (subOn)
                    P1S::gSkipped.fetch_add(1, std::memory_order_relaxed);
                i++;
                continue;
            }

            int runEnd = i + 1;
            const int headSpecial = sShape->Special() | oi->object->GetObjSpecial();
            const render::LegacySpec headSpec = render::SplitLegacy(headSpecial);
            // DeformsSharedShape: a destroyed building stays Static(), so Static() alone does
            // not keep it out of a run. Its Animate rewrites the SHARED Shape, and an
            // instanced run draws the head once for all K transforms -- so one wreck in the
            // run would give every intact copy of that model the wreck's geometry.
            const bool headBatchable =
                noLocalLights && oi->object->Static() && !oi->object->DeformsSharedShape(oi->drawLOD) &&
                sShape->NProxies() == 0 && !render::Has(headSpec.routing, render::Routing::OnSurface) &&
                !render::Has(headSpec.routing, render::Routing::IsColored) && oi->object != GWorld->CameraOn();
            if (subOn)
            {
                // Not mutually exclusive by construction -- an object can fail several
                // clauses at once -- so these are tallied independently and must not be
                // read as a partition. The scene-wide one is the one to read first: if
                // gVetoLights equals gHeads, no per-object property matters this frame.
                P1S::gHeads.fetch_add(1, std::memory_order_relaxed);
                if (!noLocalLights)
                    P1S::gVetoLights.fetch_add(1, std::memory_order_relaxed);
                if (!oi->object->Static())
                    P1S::gVetoNotStatic.fetch_add(1, std::memory_order_relaxed);
                if (sShape->NProxies() != 0)
                    P1S::gVetoProxies.fetch_add(1, std::memory_order_relaxed);
                if (render::Has(headSpec.routing, render::Routing::OnSurface) ||
                    render::Has(headSpec.routing, render::Routing::IsColored))
                    P1S::gVetoRouting.fetch_add(1, std::memory_order_relaxed);
            }
            if (headBatchable)
            {
                GEngine->InstancedRunReset();
                if (GEngine->InstancedRunAdd(oi->object->Transform()))
                {
                    // Fog band: keep members within ~5% of the head's distance so
                    // the head's per-object constant fog approximates all of them.
                    const float d2lo = oi->distance2 * 0.90f;
                    const float d2hi = oi->distance2 * 1.10f;
                    while (runEnd < _drawMergers.Size())
                    {
                        SortObject* oj = _drawMergers[runEnd];
                        if (oj->object->GetShape() != shape || oj->drawLOD != oi->drawLOD ||
                            oj->passNum != oi->passNum || !oj->object->Static() ||
                            oj->object->DeformsSharedShape(oj->drawLOD) ||
                            (sShape->Special() | oj->object->GetObjSpecial()) != headSpecial || oj->distance2 < d2lo ||
                            oj->distance2 > d2hi)
                        {
                            break;
                        }
                        if (!GEngine->InstancedRunAdd(oj->object->Transform()))
                        {
                            break;
                        }
                        runEnd++;
                    }
                }
            }

            const int runLen = runEnd - i;
            GSectionFilter = SectionClassFilter::OpaqueAndCutout;
            const uint64_t drawT0 = subOn ? P1S::NowNs() : 0;
            if (headBatchable && runLen >= 4)
            {
                if (subOn)
                {
                    P1S::gRuns.fetch_add(1, std::memory_order_relaxed);
                    P1S::gRunObjs.fetch_add(static_cast<uint64_t>(runLen), std::memory_order_relaxed);
                }
                GEngine->BeginInstancedRunUpload();
                DrawSortObject(oi);
                if (!GEngine->EndInstancedRun())
                {
                    // Vertex-soup sections can't instance — those drew only for the
                    // head; redraw the rest scalar (TL overdraw is z-equal opaque).
                    for (int k = i + 1; k < runEnd; k++)
                    {
                        DrawSortObject(_drawMergers[k]);
                    }
                }
            }
            else
            {
                if (subOn)
                {
                    P1S::gScalarObjs.fetch_add(static_cast<uint64_t>(runLen), std::memory_order_relaxed);
                    if (headBatchable)
                    {
                        P1S::gRunsShort.fetch_add(1, std::memory_order_relaxed);
                        P1S::gRunShortLen.fetch_add(static_cast<uint64_t>(runLen), std::memory_order_relaxed);
                    }
                }
                for (int k = i; k < runEnd; k++)
                {
                    DrawSortObject(_drawMergers[k]);
                }
            }
            if (subOn)
            {
                drawNsLocal += P1S::NowNs() - drawT0;
            }
            GSectionFilter = SectionClassFilter::All;
#if DO_STAT
            Opaque.Count(shape->Name());
#endif
            i = runEnd;
        }
        if (subOn)
        {
            // Whole-loop elapsed minus what the draws cost. Clamped: the two clock
            // sequences are read at different points and a negative here would mean the
            // probe, not the code, so report zero rather than an impossible number.
            const uint64_t loopNs = P1S::NowNs() - loopT0;
            P1S::gScanNs.fetch_add(loopNs > drawNsLocal ? loopNs - drawNsLocal : 0, std::memory_order_relaxed);
        }
        P1S::gActive = false;
    }
#endif
    if (Pass1StatsEnabled())
    {
        ReportPass1Stats();
    }
}

// A surface overlay (road / decal): OnSurface-routed geometry drawn as a
// polygon-offset decal over the terrain, detected from the draw-LOD level
// spec exactly as Object::PassNum classifies surface objects.
static bool IsSurfaceSortObject(const SortObject* oi)
{
    if (!oi->object)
        return false;
    LODShape* lShape = oi->object->GetShape();
    if (!lShape)
        return false;
    Shape* s = lShape->Level(oi->drawLOD);
    if (!s)
        return false;
    const int spec = s->Special() | oi->object->GetObjSpecial();
    return render::IsOnSurfaceSpec(spec);
}

// On-surface ordering: PassOrder (roads before decals), then shape, then
// back-to-front within a shape — see Scene/SurfaceDrawOrder.hpp.
static int CmpSurfaceObj(const SortObjectItem* p1, const SortObjectItem* p2)
{
    const SortObject* o1 = *p1;
    const SortObject* o2 = *p2;
    // PassOrder precomputed in AdjustComplexity (was a virtual call per comparison).
    const Poseidon::SurfaceDraw::SurfaceDrawKey k1{
        o1->object ? o1->sortPassOrder : 0, o1->object ? static_cast<const void*>(o1->object->GetShape()) : nullptr,
        o1->distance2};
    const Poseidon::SurfaceDraw::SurfaceDrawKey k2{
        o2->object ? o2->sortPassOrder : 0, o2->object ? static_cast<const void*>(o2->object->GetShape()) : nullptr,
        o2->distance2};
    return Poseidon::SurfaceDraw::CompareSurfaceDraw(k1, k2);
}

// MAT-048 escape hatch.  The safe behaviour (blend sections draw depth-test-only) is the
// default per house rules; `WGR_COCKPIT_BLEND_LEGACY=1` restores the pre-MAT-048 state exactly —
// depth-write left ON for the object-blend branch below — so an A/B of the cockpit-glass fix
// needs one binary and one env var rather than two builds.  Read once: this is queried inside the
// per-object alpha loop, and a `getenv` per drawn vehicle per frame is not free.
static bool CockpitBlendLegacy()
{
    static const bool on = []
    {
        const char* v = std::getenv("WGR_COCKPIT_BLEND_LEGACY");
        return v && std::strcmp(v, "0") != 0;
    }();
    return on;
}

// WGR_SHADOW_GATE_TRACE=1 also reports THIS gate, on transition. The backend's own trace
// (EngineWgpu "Wgpu shadow gate") can only say that the cascades were invalid; it cannot say
// whether the depth pass ran at all, and those are different bugs with different fixes. Logged
// on change, not per frame, for the same reason the backend's is.
static void ShadowPassTrace(bool mapsEnabled, float shadowFactor, int nDraw)
{
    static const bool on = []
    {
        const char* v = std::getenv("WGR_SHADOW_GATE_TRACE");
        return v && std::strcmp(v, "0") != 0;
    }();
    if (!on)
    {
        return;
    }
    const int state = (mapsEnabled ? 1 : 0) | (shadowFactor > 0.01f ? 2 : 0) | (nDraw > 0 ? 4 : 0);
    static int last = -1;
    if (state == last)
    {
        return;
    }
    last = state;
    LOG_INFO(Graphics, "Scene shadow pass gate: mapsEnabled={} shadowFactor={:.3f} nDraw={} ran={}", mapsEnabled,
             shadowFactor, nDraw, mapsEnabled && shadowFactor > 0.01f);
}

void Scene::DrawObjectsAndShadowsPass2()
{
    // must be sorted by distance
    // then draw shadows
    // note: some polygons can not be ordered
    int nDraw = _drawMergers.Size();
#if DRAW_OBJS
    if (GetLandscape())
    {
        // BeginShadowPass / EndShadowPass bracket the projected shadow
        // loop; on GL33 they flush the triangle queues so the per-poly
        // shadows commit between the world geometry and the alpha pass.

        // Surface-overlay blend sections (roads/decals) commit BEFORE the
        // shadow pass so the projected shadows darken on top of the road
        // instead of the road's blend section repainting over the shadowed
        // road pixels.  Their opaque sections already drew in pass 1; only
        // the OnSurface blend section (the visible asphalt) is drawn here.
        GEngine->FlushQueues();
        GEngine->EnableReorderQueues(false);
        // PassOrder, not distance: a fresh decal carries distance2~=0
        // (SetAutoCenter(false)) and would tie the road tile under the vehicle,
        // letting the road repaint over it.  Re-sorted by distance below.
        {
            Pass1Scope scope(GPass1Stats.p2SurfSortUs);
            SortListByPackedKey<SurfKey>(
                _drawMergers,
                [](SortObject* o) -> SurfKey
                {
                    return {o, o->object ? o->sortPassOrder : 0,
                            o->object ? static_cast<const void*>(o->object->GetShape()) : nullptr, o->distance2};
                },
                [](const SurfKey* p1, const SurfKey* p2, int) -> int
                {
                    const Poseidon::SurfaceDraw::SurfaceDrawKey k1{p1->passOrder, p1->shape, p1->distance2};
                    const Poseidon::SurfaceDraw::SurfaceDrawKey k2{p2->passOrder, p2->shape, p2->distance2};
                    return Poseidon::SurfaceDraw::CompareSurfaceDraw(k1, k2);
                });
        }
        Pass1Scope surfDrawScope(GPass1Stats.p2SurfDrawUs);
        for (int i = 0; i < nDraw; i++)
        {
            SortObject* oi = _drawMergers[i];
            if (oi->drawLOD == LOD_INVISIBLE)
                continue;
            // Mirror exactly the post-shadow blend branch that skips these
            // objects (passNum<=1 && HasBlendSections && surface).  With grass
            // enabled a surface object is passNum==2 (whole-alpha) and is drawn
            // by the passNum==2 branch below; drawing it here too would
            // double-draw it and re-overwrite the shadow.  The cheap passNum
            // gate also avoids the per-object Special() probe for non-surface
            // objects.
            if (oi->passNum > 1)
                continue;
            if (!IsSurfaceSortObject(oi))
                continue;
            LODShape* shp = oi->object->GetShape();
            Shape* lvl = shp ? shp->LevelOpaque(oi->drawLOD) : nullptr;
            if (!lvl || !lvl->HasBlendSections())
                continue;
            GSectionFilter = SectionClassFilter::BlendOnly;
            DrawSortObject(oi);
            GSectionFilter = SectionClassFilter::All;
        }
        GEngine->FlushQueues();
        GEngine->EnableReorderQueues(true);
        surfDrawScope.Close(); // the surface block ends here, before the shadow pass

        Pass1Scope shadowScope(GPass1Stats.p2ShadowUs);
        // Shadow-map depth pass (durable shadow fix; off by default).  Render the
        // visible casters' geometry from the sun into the cascade depth maps.
        // The lit shaders sample it; exclusive with the projected accumulator
        // below — never both.  Body in SceneShadowPass.cpp (keeps
        // Scene.cpp under the file-size limit).
        if (GEngine->ShadowMapsEnabled())
        {
            // Sun shadows fade out at dusk and vanish at night (no sun above the
            // horizon = no sun shadow), as the projected path and OFP/ArmA/FP do.
            // NightEffect is 0 in daylight, ramps through twilight, 1 at night. The
            // lit shaders fade the darkness by this factor; skip the depth pass once
            // it is fully dark (nothing would be visible, and a stale map is hidden
            // because the darkness factor is 0).
            float sunFactor = 1.0f - _mainLight->NightEffect();
            sunFactor = floatMax(0.0f, floatMin(1.0f, sunFactor));
            // ...unless the MOON is lighting the scene. LightSun swings ShadowDirection() onto
            // the moon while MoonLightAmount() > 0 (phase and horizon ramps folded in), and the
            // wgpu sky-lit path adds the moon's radiance to the directional term -- so the same
            // cascades, from the same direction, are moon shadows for free. Skipping the pass
            // here was the one thing that kept a full moon from casting anything (owner request
            // 2026-08-16: "a bluish light that casts shadows").
            const float moonFactor = floatMax(0.0f, floatMin(1.0f, _mainLight->MoonLightAmount()));
            const float shadowFactor = floatMax(sunFactor, moonFactor);
            GEngine->SetShadowMapSunFactor(shadowFactor);
            // LGT-010, the last of six gates that all encode "the shadow pass is the SUN's".
            // At night with no moon this factor is zero and the entire caster collection was
            // skipped -- so a local light's depth map was allocated, cleared, and left empty,
            // which against a LessEqual compare reads as "lit everywhere". The shadow could
            // not exist for exactly the hours a lamp or a headlight exists for.
            //
            // Keyed on the LEVER rather than on a live light count: this is the collection
            // that decides what a shadow map can contain, and it runs before the renderer has
            // chosen which lights get a view. With the lever off the condition is unchanged.
            const bool localShadowsWanted =
                Poseidon::Dev::GLocalShadowSettings().enabled && Poseidon::Dev::GLocalShadowSettings().maxLights > 0;
            if (shadowFactor > 0.01f || localShadowsWanted)
            {
                RenderShadowMapDepthPass(nDraw);
            }
            ShadowPassTrace(true, shadowFactor, nDraw);
        }
        else
        {
            ShadowPassTrace(false, 0.0f, nDraw);
        }

        // Projected accumulator shadows — the fallback path.  When shadow maps are
        // on, the lit shaders darken receivers from the depth map instead, so skip
        // this to avoid two overlapping shadows.
        if (!GEngine->ShadowMapsEnabled())
        {
            // Frozen-pose caster accounting for the projected-shadow cache: Object::PrepareShadow
            // bumps gShadowFrozenRouted each time a settled corpse / stopped vehicle is served from
            // the cache instead of re-projected.  Reset per pass, then publish the count the
            // triShadowAssertFrozenCasts verb reads.
            extern int gShadowFrozenCasters;
            extern int gShadowFrozenRouted;
            GEngine->BeginShadowPass();
            gShadowFrozenRouted = 0;
            for (int i = 0; i < nDraw; i++)
            {
                SortObject* oi = _drawMergers[i];
                if (oi->shadowLOD == LOD_INVISIBLE)
                {
                    continue;
                }
                DrawExShadow(oi);
                GPass1Stats.p2ShadowCasters.fetch_add(1, std::memory_order_relaxed);
            }
            GEngine->EndShadowPass();
            gShadowFrozenCasters = gShadowFrozenRouted;
        }
        shadowScope.Close();
    }
#endif
    // draw alpha parts of roads
    GEngine->FlushQueues();
    GEngine->EnableReorderQueues(false);
    // Back-to-front by camera-space depth (farthest first). Single float key => radix
    // (see RadixSortByFloatDesc). CmpRevAlphaSortObj / CompareAlphaDepth produced the same
    // descending-zCoord order; this is O(n) instead of O(n log n).
    {
        Pass1Scope scope(GPass1Stats.p2AlphaSortUs);
        RadixSortByFloatDesc(_drawMergers, [](const SortObject* o) { return o->zCoord; });
    }
    Pass1Scope alphaDrawScope(GPass1Stats.p2AlphaDrawUs);
    // last draw alpha objects (not roads - they are already drawn)
    for (int i = 0; i < nDraw; i++)
    {
        SortObject* oi = _drawMergers[i];
        LODShape* shape = oi->object->GetShape();
        if (oi->drawLOD == LOD_INVISIBLE)
        {
            continue;
        }
        PoseidonAssert(oi->drawLOD >= 0);
        Shape* sShape = shape->LevelOpaque(oi->drawLOD);
        if (!sShape)
        {
            continue;
        }
        if (oi->passNum == 2)
        {
            // whole-alpha object (cloudlets, <0xd0 objects): draw all of it here
            GPass1Stats.p2AlphaDrawn.fetch_add(1, std::memory_order_relaxed);
            // NAME them, once per shape, under WGR_PASS1_STATS. On Arma 1 Sahrani these 778
            // objects are 27.9 ms of a 47 ms CPU frame at ~36 us each, and the counter alone
            // cannot say whether they are foliage that should be on the retained cutout path
            // or genuinely un-divertable blend geometry. Those want opposite fixes, so the
            // list decides it rather than a guess about what an Arma 1 tree is.
            if (Pass1StatsEnabled() && shape)
            {
                static std::unordered_set<std::string> loggedAlpha;
                const char* nm = shape->Name();
                if (nm && loggedAlpha.size() < 60 && loggedAlpha.insert(std::string(nm)).second)
                {
                    LOG_INFO(Graphics, "PERF pass2 alpha shape: {} passNum={} lod={}", nm, oi->passNum,
                             oi->drawLOD);
                }
            }
            DrawSortObject(oi);
#if DO_STAT
            Alpha.Count(shape->Name());
#endif
        }
        else if (oi->passNum <= 1 && sShape->HasBlendSections())
        {
            // Surface overlays (roads/decals: asfaltka/cesta) own their visible
            // surface as a blend section; those drew before the shadow pass so
            // the shadow darkens on top of them — skip here (see the pre-shadow
            // surface-blend pass above).
            if (IsSurfaceSortObject(oi))
            {
                continue;
            }
            // opaque object that owns translucent (blend) sections — e.g. a vehicle's
            // glass. Its opaque+cutout sections drew in pass 1; revisit it here, in
            // back-to-front order, drawing only the blend sections so they blend over
            // the already-drawn scene behind them.
            //
            // MAT-048: depth-TEST on, depth-WRITE off for the duration of this draw. The
            // previous code left the write on and called it "original parity"; it is not
            // parity with anything, and it is what makes a cockpit lose panes. This loop's
            // back-to-front order is per-OBJECT, but a cockpit is ONE object whose glass
            // spans the whole view depth, and Shape::Draw walks that object's sections in
            // mesh index order with no intra-object sort (ShapeDraw.cpp:217). So the pane
            // with the lower section index wrote depth first and the pane BEHIND it was
            // then rejected by the depth test outright — seven OH-58 canopy panes, and
            // from the pilot seat looking across the cockpit you saw one of every two.
            // Stable per model, because section index does not change. Full reasoning and
            // the accepted dust-ordering cost are at
            // Graphics/Rendering/BuildRenderPassDescriptor.hpp (`GBlendSectionDepthReadOnly`).
            //
            // Set per object and cleared per object, not hoisted out of the loop: the
            // passNum==2 whole-alpha branch above shares this loop and must keep its own
            // depth behaviour, and so must the surface-overlay BlendOnly pass at the top of
            // this function.
            //
            // FlushQueues before clearing is for GL33, not wgpu. wgpu's DrawSectionTL builds
            // the descriptor synchronously (EngineWgpu.cpp:2078), but GL33 defers it to
            // ApplyPassState at queue-flush time (EngineGL33_Queue.cpp:255) — a queue opened
            // during this draw and flushed after the flag was cleared would silently get the
            // old state. FlushQueues is a no-op on backends that do not queue
            // (Graphics/Core/Engine.hpp:494 defines it as an empty default), so this costs
            // wgpu nothing.
            const bool blendDepthReadOnly = !CockpitBlendLegacy();
            render::GBlendSectionDepthReadOnly = blendDepthReadOnly;
            GSectionFilter = SectionClassFilter::BlendOnly;
            GPass1Stats.p2AlphaDrawn.fetch_add(1, std::memory_order_relaxed);
            // MAT-051 diagnostic (temporary): name each object the blend-revisit
            // pass actually draws, so a glass-owning shape that never appears
            // here names the gap.
            if (const char* trace = std::getenv("WGR_BLEND_PASS_TRACE"); trace && *trace == '1')
            {
                static std::unordered_set<std::string> logged;
                const char* nm = shape ? shape->Name() : nullptr;
                if (nm && logged.size() < 40 && logged.insert(std::string(nm)).second)
                    LOG_WARN(Graphics, "Blend revisit: {} lod={} passNum={}", nm, oi->drawLOD, oi->passNum);
            }
            DrawSortObject(oi);
            if (blendDepthReadOnly)
            {
                GEngine->FlushQueues();
            }
            GSectionFilter = SectionClassFilter::All;
            render::GBlendSectionDepthReadOnly = false;
#if DO_STAT
            Alpha.Count(shape->Name());
#endif
        }
    }
    alphaDrawScope.Close();
    GEngine->EnableReorderQueues(true);
}

#include <Poseidon/Foundation/Common/Filenames.hpp>

void Scene::DrawObjectsAndShadowsPass3()
{
#if DRAW_OBJS
    int i, nDraw = _drawMergers.Size();
    // pass 3 - draw cockpits after all external alpha effects
    for (i = 0; i < nDraw; i++)
    {
        SortObject* oi = _drawMergers[i];
        LODShape* shape = oi->object->GetShape();
        if (oi->drawLOD != LOD_INVISIBLE)
        {
            PoseidonAssert(oi->drawLOD >= 0);
            Shape* sShape = shape->LevelOpaque(oi->drawLOD);
            if (!sShape)
            {
                continue;
            }
            if (oi->passNum == 3)
            {
                {
                    DrawSortObject(oi);
                }
            }
        }
    }
#endif

#if DO_STAT
    LOG_DEBUG(Graphics, "Alpha objects");
    Alpha.Report();
    LOG_DEBUG(Graphics, "Opaque objects");
    Opaque.Report();
    LOG_DEBUG(Graphics, "Shadow objects");
    Shadow.Report();
#endif
}

void Scene::DrawReflections(const WaterLevel& water) {}

void Scene::DrawCollisionStar(Vector3Par pos, float size, PackedColor color)
{
    if (_collisionStar.NotNull() && size >= 0.01)
    {
        Ref<Object> star = new ObjectColored(_collisionStar->GetShape(), -1);
        star->SetTransform(M4Identity);
        star->SetPosition(pos);
        star->SetScale(size * 4);
        star->SetConstantColor(color);
        GetLandscape()->ShowObject(star);
    }
}

void Scene::DrawDiagModel(Vector3Par pos, LODShapeWithShadow* shape, float size, PackedColor color)
{
    Ref<Object> star = new ObjectColored(shape, -1);
    star->SetTransform(M4Identity);
    star->SetPosition(pos);
    star->SetScale(size / shape->BoundingSphere());
    star->SetConstantColor(color);
    GetLandscape()->ShowObject(star);
}

void Scene::DrawVolumeLight(LODShapeWithShadow* shape, PackedColor color, const Frame& pos, float size,
                            float minAngular, float maxWorld)
{
    if (!shape)
    {
        static bool warned = false;
        if (!warned)
        {
            LOG_ERROR(Graphics, "Skipping visible light volume because its shape failed to load");
            warned = true;
        }
        return;
    }

    // StreetLamp creates a HalfLight object for the legacy fixed-function
    // "volume" around a bulb.  At its original world-space size WGPU's HDR
    // blend turns that helper into a sun-sized white disc (particularly for
    // imported A3 halogen lamps).  It is only an emitter marker: the actual
    // LightPoint still provides the terrain/geometry illumination.  Keep a
    // compact, readable marker on WGPU instead of discarding it entirely so
    // ordinary CWA street lamps do not vanish.
    if (AppConfig::Instance().GetRenderBackend() == "wgpu" && shape == Preloaded(HalfLight))
    {
        size *= 0.035f;
    }

    // LGT-018. The marker above is a world-space object a few centimetres across, and that is
    // the whole reason a lit village looks black from the air and a truck's headlight reads as
    // off: at any distance it falls below a pixel, and the ILLUMINATION cannot stand in for it
    // because a lamp's inverse-square falloff from a small core is genuinely almost nothing at
    // 200 m. Real lamps are visible at that range as the GLARE OF THE BULB, not as lit ground.
    // So the marker gets a minimum angle: it never shrinks below a couple of pixels, exactly
    // as a glare sprite does not. Near the camera the floor is below the authored size and
    // nothing changes, so this cannot alter the look the owner already accepted.
    // POSEIDON_LIGHT_GLARE scales the angular floor (0 disables it, 1 is the shipped value);
    // POSEIDON_LIGHT_GLARE_DIAG=1 reports what the marker actually resolved to, because "the
    // halo is too small" and "the halo is never drawn" look identical from outside.
    static const float glareScale = []
    {
        const char* e = std::getenv("POSEIDON_LIGHT_GLARE");
        const double d = e ? std::atof(e) : -1.0;
        return d >= 0.0 ? static_cast<float>(d) : 1.0f;
    }();
    minAngular *= glareScale;
    maxWorld *= glareScale;
    if (minAngular > 0.0f && _camera)
    {
        const float dist = (pos.Position() - _camera->Position()).Size();
        const float floorWorld = dist * minAngular;
        if (floorWorld > size)
        {
            size = floorWorld;
        }
        if (maxWorld > 0.0f && size > maxWorld)
        {
            size = maxWorld;
        }
    }

    static const bool glareDiag = [] { const char* e = std::getenv("POSEIDON_LIGHT_GLARE_DIAG"); return e && std::atoi(e) != 0; }();
    if (glareDiag)
    {
        static int seen = 0;
        static float lastSize = -1.0f;
        // Report on a CHANGE, not once: a single line at frame 1 cannot tell "never drawn
        // again" from "drawn every frame at the same size".
        if (++seen <= 3 || std::fabs(size - lastSize) > lastSize * 0.25f)
        {
            lastSize = size;
            const float dist = _camera ? (pos.Position() - _camera->Position()).Size() : -1.0f;
            LOG_INFO(Graphics, "LGT-018 glare: marker #{} size {:.3f} m at {:.1f} m (minAngular {:.4f})", seen, size,
                     dist, minAngular);
        }
    }
    Ref<Object> object = new ObjectColored(shape, -1);
    // draw light shape
    object->SetTransform(pos.Transform());
    object->SetPosition(object->PositionModelToWorld(shape->BoundingCenter()));
    // ObjectForDrawing(object,-1,(ENGINE_CONFIG.reflections&REFL_OBJECT)!=0);

    object->SetConstantColor(color);
    object->SetScale(size);
    ObjectForDrawing(object);
}

void Scene::DrawExShadow(SortObject* oi)
{
    // calculate position of the shadow of the object top
    Object* obj = oi->object;
    if (!obj)
    {
        return;
    }
    int level = oi->shadowLOD;
    LODShapeWithShadow* shape = oi->shape;
    Vector3Val objPos = obj->Position();
    Point3 shadowPos = objPos;
    // REN-INTERP-001: the shadow must sit where the body is drawn, not where the tick left it.
    FrameBase interpFrame;
    const FrameBase& pos = obj->RenderFrame(interpFrame);

    if (!ShadowPos(objPos, shadowPos, _mainLight))
    {
        return;
    }

    Ref<Shape> sShape = obj->PrepareShadow(level, shadowPos, pos);
    // check bbox clipping and occlusion

    // per object clipping possible - bounding sphere was recalculated
    if (!sShape)
    {
        return; // no shadow
    }
    if (sShape->NPos() <= 0)
    {
        return;
    }

    float bRadius = sShape->BSphereRadius() * pos.Scale();
    Vector3Val bCenterM = sShape->BSphereCenter();
    Vector3Val bCenter = pos.PositionModelToWorld(bCenterM);
    // try to clip bounding sphere
    // if whole bounding sphere is out, object is already skipped
    if (GScene->GetCamera()->IsClipped(bCenter, bRadius, 1))
    {
        return;
    }
    ClipFlags clipFlags = GScene->GetCamera()->MayBeClipped(bCenter, bRadius, 1);
    if (EnableObjOcc)
    {
        bool occluded = false;
        if (shape->CanBeOccluded())
        {
            if (FarEnoughForOcclusion(oi))
            {
                // check occlusion - bbox recalculation
                Vector3 minMax[2];
                Vector3Val pos0 = sShape->Pos(0);
                minMax[0] = pos0;
                minMax[1] = pos0;
                for (int i = 0; i < sShape->NPos(); i++)
                {
                    Vector3Val posI = sShape->Pos(i);
                    CheckMinMaxIter(minMax[0], minMax[1], posI);
                }

                // check if object is occluded by objects
                if (!GetOcclusions()->TestBBox(pos, minMax, clipFlags))
                {
                    occluded = true;
                }
            }
        }
        if (occluded)
        {
            return;
        }
    }

    // first draw shadows of some proxies?
    for (int i = 0; i < obj->GetProxyCount(level); i++)
    {
        if (!obj->CastProxyShadow(level, i))
        {
            continue;
        }
        // REN-INTERP-001: proxies' shadows follow the DRAWN parent frame (pos), like the body.
        Matrix4 trans = pos.Transform(), invTrans = pos.GetInvTransform();

        LODShapeWithShadow* pshape = nullptr;
        Object* proxy = obj->GetProxy(pshape, level, trans, invTrans, *obj, i);
        if (!proxy)
        {
            continue;
        }
        // note: it is not sure we need to draw shadows of all proxies
        // proxy shadow must be always calculated dynamically
        // we do not want to draw proxies of complex objects like vehicle crews

        if (!pshape)
        {
            continue;
        }
        float dist2 = trans.Position().Distance2(GetCamera()->Position());
        int plevel =
            LevelShadowFromDistance2(pshape, dist2, trans.Scale(), trans.Direction(), GetCamera()->Direction());
        if (plevel != LOD_INVISIBLE)
        {
            plevel = pshape->FindNearestWithoutProperty(plevel, "lodnoshadow");
            if (plevel < 0)
            {
                plevel = LOD_INVISIBLE;
            }
        }
        if (plevel == LOD_INVISIBLE)
        {
            continue;
        }

        proxy->Animate(plevel);

        FrameWithInverse pframe(trans, invTrans);
        if (proxy->RecalcShadow(plevel, pframe, true, pshape))
        {
            LODShape* shadowShape = pshape->Shadow();
            Ref<Shape> ret = shadowShape->LevelOpaque(plevel);
            // we can draw shape ret now as shadow
            proxy->DrawShadow(ret, shadowPos, ClipAll, pframe);
        }

        proxy->Deanimate(plevel);
    }
    /**/

    // obj->DrawShadow(shadow,shadowPos,ClipAll,pos);
    obj->DrawShadow(sShape, shadowPos, clipFlags, pos);

#if DO_STAT
    Shadow.Count(oi->shape->Name());
#endif
}

// different global variables

void ManCleanUp();

void ClearShapes()
{
    VehicleTypes.Clear();
    // WeaponInfos.Clear();
    RoadTypes.Clear();
    // log any shape still resident in the cache at shutdown (refs would
    // be from cache itself plus any holdouts on the entity side).
    Shapes.ForEach([](LODShapeWithShadow& shape)
                   { Log("Shape %s not released (%d refs).", shape.Name(), shape.RefCounter()); });
    Shapes.Clear();

    MagazineTypes.Clear();
    WeaponTypes.Clear();
}

bool Scene::ShadowPos(Vector3Par pos, Vector3& aprox, LightSun* light)
{ //
    Vector3Val dir = light->ShadowDirection();
    if (dir.Y() > 0)
    {
        return false; // shadow casted up - no real shadow
    }
    const float maxShadow = 300;
    float t = GetLandscape()->IntersectWithGround(&aprox, pos, dir, 0, maxShadow * 1.1);
    if (t > maxShadow)
    {
        // no intersection
        return false;
    }
    if (t <= 0.01f)
    {
        // up -> on surface required
        aprox[1] = GLandscape->SurfaceY(aprox[0], aprox[2]);
    }
    return true;
}
