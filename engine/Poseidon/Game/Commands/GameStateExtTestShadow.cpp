#include <Evaluator/express.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/UI/Settings/GameSettingsConfig.hpp>
#include <Poseidon/Dev/Debug/DebugOverlay.hpp>
#include <Poseidon/Dev/Diag/PhysicsProbe.hpp>
#include <Poseidon/Dev/Diag/TerrainBrush.hpp>
#include <Poseidon/World/Entities/Infantry/SoldierOld.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>

#include <cstdio>
#include <cstring>

using namespace Poseidon::Dev;
using namespace Poseidon;

GameValue TriFreeFlyPose(const GameState*, GameValuePar arg)
{
    const GameStringType pose = static_cast<GameStringType>(arg);
    return GameValue(DebugOverlay::SetFreeFlyTestPose(pose) ? "OK" : "REFUSED");
}

GameValue TriPhysicsShowcase(const GameState*, GameValuePar arg)
{
    const GameStringType command = static_cast<GameStringType>(arg);
    if (std::strncmp(command, "corpse-", 7) == 0) return GameValue(Man::CorpsePoseProbe(command));
    return GameValue(PhysicsShowcaseCommand(command) ? "OK" : "REFUSED");
}

GameValue TriTerrainBrushMode(const GameState*, GameValuePar arg)
{
    const GameStringType mode = static_cast<GameStringType>(arg);
    if (strcmp(mode,"restore") == 0) return GameValue(RestoreEditedTerrain() ? "OK" : "REFUSED");
    if (strcmp(mode,"off") == 0) { TerrainBrush().enabled=false; return GameValue("OK"); }
    if (strcmp(mode,"raise") != 0 && strcmp(mode,"lower") != 0) return GameValue("REFUSED");
    TerrainBrush().enabled = true;
    TerrainBrush().raise = strcmp(mode,"raise") == 0;
    return GameValue("OK");
}

GameValue TriTerrainPaint(const GameState*, GameValuePar arg)
{
    const GameArrayType& args = arg;
    if (args.Size() != 4) return GameValue("REFUSED");
    for (int i=0; i<4; ++i) if (args[i].GetType() != GameScalar) return GameValue("REFUSED");
    return GameValue(PaintTerrain(float(args[0]),float(args[1]),float(args[2]),float(args[3])) ? "OK" : "REFUSED");
}

GameValue TriTerrainHeight(const GameState*, GameValuePar arg)
{
    const GameArrayType& args = arg;
    if (!GLandscape || args.Size()!=2 || args[0].GetType()!=GameScalar || args[1].GetType()!=GameScalar)
        return GameValue(-1000000.0f);
    const float x=args[0], z=args[1];
    if (!std::isfinite(x) || !std::isfinite(z)) return GameValue(-1000000.0f);
    return GameValue(GLandscape->SurfaceY(x,z));
}

GameValue TriRocketCraters(const GameState*, GameValuePar arg)
{
    const GameStringType mode = static_cast<GameStringType>(arg);
    if (strcmp(mode,"reset") == 0) ResetRocketCraters();
    else if (strcmp(mode,"on") == 0) RocketCraters().enabled = true;
    else if (strcmp(mode,"off") == 0) RocketCraters().enabled = false;
    else return GameValue("REFUSED");
    return GameValue("OK");
}

namespace Poseidon
{
extern int gSmDepthCachedCasters;
extern int gShadowFrozenCasters;
} // namespace Poseidon

// Visibility apply (global free fn, defined in GameStateExtUi.cpp).
extern void SetVisibility(float distance);
// Proxy shadow-caster vert count from the last depth pass (SceneShadowPass.cpp).
extern int LastShadowProxyVertCount();

// Shadow-map + visibility-distance tri verbs — split out of GameStateExtTest.cpp
// to keep it under the file-size limit. GLOBAL scope like the rest of the tri verb
// surface (registered, with forward decls, in GameStateExtTestAudio.cpp).

/// triShadowSetDarkness <0..1> — lit-colour multiplier where shadowed (lower =
/// darker). Drives the same knob as the dev panel. Returns "OK:<value>".
GameValue TriShadowSetDarkness(const GameState* /*state*/, GameValuePar arg)
{
    if (!GEngine)
        return GameValue("FAIL:no engine");
    Engine::ShadowMapTuning t = GEngine->GetShadowMapTuning();
    t.darkness = static_cast<float>(static_cast<GameScalarType>(arg));
    GEngine->SetShadowMapTuning(t);
    char buf[48];
    snprintf(buf, sizeof(buf), "OK:%.3f", t.darkness);
    return GameValue(buf);
}

/// triShadowSetBias <base> — per-cascade depth bias base (base*(i+1)^2). "OK:<v>".
GameValue TriShadowSetBias(const GameState* /*state*/, GameValuePar arg)
{
    if (!GEngine)
        return GameValue("FAIL:no engine");
    Engine::ShadowMapTuning t = GEngine->GetShadowMapTuning();
    t.biasBase = static_cast<float>(static_cast<GameScalarType>(arg));
    GEngine->SetShadowMapTuning(t);
    char buf[48];
    snprintf(buf, sizeof(buf), "OK:%.6f", t.biasBase);
    return GameValue(buf);
}

// Match the dev-panel ranges for reproducible receiver/filter ablations.
GameValue TriShadowSetNormalOffset(const GameState*, GameValuePar arg)
{
    if (!GEngine) return GameValue("FAIL:no engine");
    auto t = GEngine->GetShadowMapTuning();
    const float value = static_cast<float>(static_cast<GameScalarType>(arg));
    if (!(value >= 0 && value <= 4)) return GameValue("FAIL:range");
    t.normalOffset = value;
    GEngine->SetShadowMapTuning(t);
    return GameValue("OK");
}

GameValue TriShadowSetPcf(const GameState*, GameValuePar arg)
{
    if (!GEngine) return GameValue("FAIL:no engine");
    auto t = GEngine->GetShadowMapTuning();
    const float value = static_cast<float>(static_cast<GameScalarType>(arg));
    if (!(value >= 0 && value <= 3)) return GameValue("FAIL:range");
    t.pcf = value;
    GEngine->SetShadowMapTuning(t);
    return GameValue("OK");
}

/// triShadowSetCascades <1..4> - view-frustum cascade count. Returns "OK:<v>".
GameValue TriShadowSetCascades(const GameState* /*state*/, GameValuePar arg)
{
    if (!GEngine)
        return GameValue("FAIL:no engine");
    Engine::ShadowMapTuning t = GEngine->GetShadowMapTuning();
    t.cascadeCount = static_cast<int>(static_cast<GameScalarType>(arg));
    GEngine->SetShadowMapTuning(t);
    char buf[48];
    snprintf(buf, sizeof(buf), "OK:%d", t.cascadeCount);
    return GameValue(buf);
}

/// triShadowSetDistance <coef> — shadow far distance as a fraction of the view
/// distance (FP shadowDistance). Returns "OK:<v>".
GameValue TriShadowSetDistance(const GameState* /*state*/, GameValuePar arg)
{
    if (!GEngine)
        return GameValue("FAIL:no engine");
    Engine::ShadowMapTuning t = GEngine->GetShadowMapTuning();
    t.distanceCoef = static_cast<float>(static_cast<GameScalarType>(arg));
    GEngine->SetShadowMapTuning(t);
    char buf[48];
    snprintf(buf, sizeof(buf), "OK:%.3f", t.distanceCoef);
    return GameValue(buf);
}

/// triShadowSetShadowDistance <metres> — explicit cascade reach, decoupled from the
/// 250 m shadowsZ clamp (0 = use the game shadow slider). Returns "OK:<v>".
GameValue TriShadowSetShadowDistance(const GameState* /*state*/, GameValuePar arg)
{
    if (!GEngine)
        return GameValue("FAIL:no engine");
    Engine::ShadowMapTuning t = GEngine->GetShadowMapTuning();
    t.shadowDistance = static_cast<float>(static_cast<GameScalarType>(arg));
    GEngine->SetShadowMapTuning(t);
    char buf[48];
    snprintf(buf, sizeof(buf), "OK:%.1f", t.shadowDistance);
    return GameValue(buf);
}

/// triShadowSetSplit <coef> — PSSM split blend 0=uniform..1=log. Returns "OK:<v>".
GameValue TriShadowSetSplit(const GameState* /*state*/, GameValuePar arg)
{
    if (!GEngine)
        return GameValue("FAIL:no engine");
    Engine::ShadowMapTuning t = GEngine->GetShadowMapTuning();
    t.splitCoef = static_cast<float>(static_cast<GameScalarType>(arg));
    GEngine->SetShadowMapTuning(t);
    char buf[48];
    snprintf(buf, sizeof(buf), "OK:%.2f", t.splitCoef);
    return GameValue(buf);
}

/// triShadowSetOmni <count> — number of leading camera-sphere (omni) tiers, for
/// all-direction near coverage (0 = pure frustum). Returns "OK:<v>".
GameValue TriShadowSetOmni(const GameState* /*state*/, GameValuePar arg)
{
    if (!GEngine)
        return GameValue("FAIL:no engine");
    Engine::ShadowMapTuning t = GEngine->GetShadowMapTuning();
    t.omniCount = static_cast<int>(static_cast<GameScalarType>(arg));
    GEngine->SetShadowMapTuning(t);
    char buf[48];
    snprintf(buf, sizeof(buf), "OK:%d", t.omniCount);
    return GameValue(buf);
}

/// triShadowSetOmniR0 <coef> — omni tier-0 sphere radius as a fraction of the
/// shadow range. Returns "OK:<v>".
GameValue TriShadowSetOmniR0(const GameState* /*state*/, GameValuePar arg)
{
    if (!GEngine)
        return GameValue("FAIL:no engine");
    Engine::ShadowMapTuning t = GEngine->GetShadowMapTuning();
    t.omniCoef0 = static_cast<float>(static_cast<GameScalarType>(arg));
    GEngine->SetShadowMapTuning(t);
    char buf[48];
    snprintf(buf, sizeof(buf), "OK:%.3f", t.omniCoef0);
    return GameValue(buf);
}

/// triShadowSetOmniR1 <coef> — omni tier-1 sphere radius as a fraction of the
/// shadow range. Returns "OK:<v>".
GameValue TriShadowSetOmniR1(const GameState* /*state*/, GameValuePar arg)
{
    if (!GEngine)
        return GameValue("FAIL:no engine");
    Engine::ShadowMapTuning t = GEngine->GetShadowMapTuning();
    t.omniCoef1 = static_cast<float>(static_cast<GameScalarType>(arg));
    GEngine->SetShadowMapTuning(t);
    char buf[48];
    snprintf(buf, sizeof(buf), "OK:%.3f", t.omniCoef1);
    return GameValue(buf);
}

/// triShadowSetFade <metres> — far-edge fade width. Returns "OK:<v>".
GameValue TriShadowSetFade(const GameState* /*state*/, GameValuePar arg)
{
    if (!GEngine)
        return GameValue("FAIL:no engine");
    Engine::ShadowMapTuning t = GEngine->GetShadowMapTuning();
    t.fadeRange = static_cast<float>(static_cast<GameScalarType>(arg));
    GEngine->SetShadowMapTuning(t);
    char buf[48];
    snprintf(buf, sizeof(buf), "OK:%.1f", t.fadeRange);
    return GameValue(buf);
}

/// triShadowSetRes <pixels> — square depth-map size (recreated next pass).
/// Returns "OK:<value>".
GameValue TriShadowSetRes(const GameState* /*state*/, GameValuePar arg)
{
    if (!GEngine)
        return GameValue("FAIL:no engine");
    Engine::ShadowMapTuning t = GEngine->GetShadowMapTuning();
    t.resolution = static_cast<int>(static_cast<GameScalarType>(arg));
    GEngine->SetShadowMapTuning(t);
    char buf[48];
    snprintf(buf, sizeof(buf), "OK:%d", t.resolution);
    return GameValue(buf);
}

/// triShadowTuning — read back the current shadow-map tuning as the same one-line
/// summary the dev panel shows, so a hand-tuned look can be copied back verbatim:
/// "enabled=.. darkness=.. bias=.. radius=.. res=..".
GameValue TriShadowTuning(const GameState* /*state*/)
{
    if (!GEngine)
        return GameValue("FAIL:no engine");
    Engine::ShadowMapTuning t = GEngine->GetShadowMapTuning();
    char buf[200];
    snprintf(buf, sizeof(buf),
             "enabled=%s darkness=%.3f cascades=%d omni=%d/%.3f/%.3f dist=%.3f shadowDist=%.1f split=%.2f bias=%.6f "
             "fade=%.1f res=%d",
             t.enabled ? "true" : "false", t.darkness, t.cascadeCount, t.omniCount, t.omniCoef0, t.omniCoef1,
             t.distanceCoef, t.shadowDistance, t.splitCoef, t.biasBase, t.fadeRange, t.resolution);
    return GameValue(buf);
}

/// triSetViewDistance <m> — set the master view / terrain distance (tacticalZ) and
/// re-derive objectsZ / shadowsZ from their prefs. Returns the effective tacticalZ.
GameValue TriSetViewDistance(const GameState* /*state*/, GameValuePar arg)
{
    if (!GScene)
        return GameValue("FAIL:no scene");
    SetVisibility(static_cast<float>(static_cast<GameScalarType>(arg)));
    char buf[48];
    snprintf(buf, sizeof(buf), "OK:%.0f", ENGINE_CONFIG.tacticalZ);
    return GameValue(buf);
}

/// triAssertVisibility "<expected>" — assert the live effective visibility distances
/// "vd=<m> obj=<m> shadow=<m>" (what the dev-panel Game tab shows). "OK"/"FAIL:..".
GameValue TriAssertVisibility(const GameState* /*state*/, GameValuePar arg)
{
    GameStringType exp = static_cast<GameStringType>(arg);
    const char* expected = (const char*)exp;
    char got[96];
    snprintf(got, sizeof(got), "vd=%.0f obj=%.0f shadow=%.0f", ENGINE_CONFIG.tacticalZ, ENGINE_CONFIG.objectsZ,
             ENGINE_CONFIG.shadowsZ);
    if (expected && strcmp(got, expected) == 0)
        return GameValue("OK");
    char buf[200];
    snprintf(buf, sizeof(buf), "FAIL:got='%s' exp='%s'", got, expected ? expected : "");
    return GameValue(buf);
}

/// triDevPanelShow <0|1> — show / hide the ImGui dev panel. Visibility stays
/// gated on --dev (SetVisible refuses otherwise), so the result reports the
/// state actually reached: "OK:1" / "OK:0".
GameValue TriDevPanelShow(const GameState* /*state*/, GameValuePar arg)
{
    const bool want = static_cast<GameScalarType>(arg) != 0.0f;
    DebugOverlay::SetVisible(want);
    return GameValue(DebugOverlay::IsVisible() ? "OK:1" : "OK:0");
}

/// triDevPanelSelectShadows — force the dev panel's "Shadows" tab to be the
/// selected one on the next drawn frame, so a screenshot captures its contents
/// deterministically. Returns "OK".
GameValue TriDevPanelSelectShadows(const GameState* /*state*/)
{
    DebugOverlay::SelectShadowsTab();
    return GameValue("OK");
}

/// triDevPanelSelectMemory — force the dev panel's "Memory" tab to the front so a
/// screenshot captures the process-heap + per-subsystem residency view. Returns "OK".
GameValue TriDevPanelSelectMemory(const GameState* /*state*/)
{
    DebugOverlay::SelectMemoryTab();
    return GameValue("OK");
}

// Day/night sun-shadow factor = 1 - NightEffect (1 in daylight, 0 at night). The
// shadow-map path fades shadow darkness by this and skips the depth pass when dark,
// so sun shadows vanish at night like the projected path + OFP/ArmA/FP.
static float ShadowSunFactorNow()
{
    if (!GScene || !GScene->MainLight())
        return -1.0f;
    float f = 1.0f - GScene->MainLight()->NightEffect();
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

/// triShadowSunFactor — read the live day/night sun-shadow factor. "OK:<v>".
GameValue TriShadowSunFactor(const GameState* /*state*/)
{
    float f = ShadowSunFactorNow();
    if (f < 0.0f)
        return GameValue("FAIL:no scene");
    char buf[48];
    snprintf(buf, sizeof(buf), "OK:%.2f", f);
    return GameValue(buf);
}

/// triShadowProxyVerts — solid shadow-caster vertices contributed by proxies
/// (weapon/crew) on the last depth pass. "OK:<n>".
GameValue TriShadowProxyVerts(const GameState* /*state*/)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "OK:%d", LastShadowProxyVertCount());
    return GameValue(buf);
}
