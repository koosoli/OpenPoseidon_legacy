#include <Poseidon/Graphics/Rendering/Effects/UnderwaterBubbles.hpp>

#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/Lights.hpp>
#include <Poseidon/Graphics/Rendering/WaterInteractionBridge.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/World/Entities/Infantry/SwimView.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <algorithm>
#include <cstdlib>
#include <utility>
#include <vector>

namespace Poseidon::render
{
namespace
{
EventRing<BubbleTrail, 256>& Trails()
{
    static EventRing<BubbleTrail, 256> ring;
    return ring;
}
EventRing<BubbleBlast, 16>& Blasts()
{
    static EventRing<BubbleBlast, 16> ring;
    return ring;
}

// A bubble seen in water: a bright rim and a clearer middle, which is what makes a
// sphere of air read as a bubble rather than as a white dot.
Texture* BubbleTexture()
{
    // Never destroyed: a static Ref's destructor runs at process exit, after the graphics
    // engine is gone, and TextureGL33::ReleaseMemory then faults (0xC0000005 after
    // 'Shutdown complete'). The engine's own shutdown releases the texture.
    static Ref<Texture>& texture = *new Ref<Texture>();
    static bool tried = false;
    if (!tried && GEngine && GEngine->TextBank())
    {
        tried = true;
        constexpr int side = 32;
        std::vector<std::uint8_t> pixels(side * side * 4, 255);
        for (int y = 0; y < side; ++y)
            for (int x = 0; x < side; ++x)
            {
                const float px = (float(x) + 0.5f) * (2.0f / side) - 1.0f;
                const float py = (float(y) + 0.5f) * (2.0f / side) - 1.0f;
                const float r = std::sqrt(px * px + py * py);
                const float rim = bubble_detail::Smooth(0.45f, 0.85f, r) * (1.0f - bubble_detail::Smooth(0.85f, 1.0f, r));
                const float glint = std::exp(-((px + 0.35f) * (px + 0.35f) + (py + 0.35f) * (py + 0.35f)) * 30.0f);
                const float alpha = std::clamp(0.25f * (1.0f - bubble_detail::Smooth(0.85f, 1.0f, r)) + 0.75f * rim + glint,
                                               0.0f, 1.0f);
                pixels[std::size_t(y * side + x) * 4 + 3] = std::uint8_t(std::lround(255.0f * alpha));
            }
        texture = GEngine->TextBank()->CreateDynamic(side, side, pixels.data(), std::uint32_t(pixels.size()), true,
                                                     AlphaStats::Blend);
    }
    return texture.GetRef();
}

// A soft disc for the cavity streak, the gas bubble and the flash.
Texture* SoftTexture()
{
    // Never destroyed: a static Ref's destructor runs at process exit, after the graphics
    // engine is gone, and TextureGL33::ReleaseMemory then faults (0xC0000005 after
    // 'Shutdown complete'). The engine's own shutdown releases the texture.
    static Ref<Texture>& texture = *new Ref<Texture>();
    static bool tried = false;
    if (!tried && GEngine && GEngine->TextBank())
    {
        tried = true;
        constexpr int side = 32;
        std::vector<std::uint8_t> pixels(side * side * 4, 255);
        for (int y = 0; y < side; ++y)
            for (int x = 0; x < side; ++x)
            {
                const float px = (float(x) + 0.5f) * (2.0f / side) - 1.0f;
                const float py = (float(y) + 0.5f) * (2.0f / side) - 1.0f;
                const float r2 = px * px + py * py;
                const float edge = std::max(1.0f - r2, 0.0f);
                pixels[std::size_t(y * side + x) * 4 + 3] =
                    std::uint8_t(std::lround(255.0f * std::clamp(std::exp2(-3.0f * r2) * edge, 0.0f, 1.0f)));
            }
        texture = GEngine->TextBank()->CreateDynamic(side, side, pixels.data(), std::uint32_t(pixels.size()), true,
                                                     AlphaStats::Blend);
    }
    return texture.GetRef();
}

// Gas and foam: an irregular, lumpy blob with a slightly brighter edge and no glint.
// A big gas bubble drawn with the small-bubble texture became a cartoon ring.
Texture* GasTexture()
{
    // Never destroyed: a static Ref's destructor runs at process exit, after the graphics
    // engine is gone, and TextureGL33::ReleaseMemory then faults (0xC0000005 after
    // 'Shutdown complete'). The engine's own shutdown releases the texture.
    static Ref<Texture>& texture = *new Ref<Texture>();
    static bool tried = false;
    if (!tried && GEngine && GEngine->TextBank())
    {
        tried = true;
        constexpr int side = 64;
        std::vector<std::uint8_t> pixels(side * side * 4, 255);
        for (int y = 0; y < side; ++y)
            for (int x = 0; x < side; ++x)
            {
                const float px = (float(x) + 0.5f) * (2.0f / side) - 1.0f;
                const float py = (float(y) + 0.5f) * (2.0f / side) - 1.0f;
                const float r = std::sqrt(px * px + py * py);
                const float a = std::atan2(py, px);
                const float edge = 0.82f + 0.08f * std::sin(3.0f * a + 1.0f) + 0.05f * std::sin(7.0f * a + 2.0f)
                    + 0.03f * std::sin(13.0f * a + 0.5f);
                const float lump = 0.85f + 0.15f * std::sin(px * 9.0f + 1.3f) * std::sin(py * 8.0f + 0.4f);
                const float inside = 1.0f - bubble_detail::Smooth(edge - 0.1f, edge, r);
                const float rim = bubble_detail::Smooth(edge * 0.55f, edge * 0.95f, r);
                const float alpha = inside * (0.55f + 0.4f * rim) * lump;
                pixels[std::size_t(y * side + x) * 4 + 3] = std::uint8_t(std::lround(255.0f * std::clamp(alpha, 0.0f, 1.0f)));
            }
        texture = GEngine->TextBank()->CreateDynamic(side, side, pixels.data(), std::uint32_t(pixels.size()), true,
                                                     AlphaStats::Blend);
    }
    return texture.GetRef();
}

// The flash lights the water round an underwater blast. A few pooled lights: blasts
// are rare and a flash lives 110 ms.
void FlashLight(const BubbleBlast& blast)
{
    static Ref<MuzzleFlashLight> pool[4];
    static int next = 0;
    if (!GLOB_SCENE) return;
    Ref<MuzzleFlashLight>& light = pool[next];
    next = (next + 1) % 4;
    if (!light)
    {
        light = new MuzzleFlashLight();
        GLOB_SCENE->AddLight(light);
    }
    light->Trigger(Vector3(blast.centre.x, blast.centre.y, blast.centre.z), 1.0f);
}
} // namespace

bool UnderwaterBubblesEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("POSEIDON_UNDERWATER_BUBBLES");
        return !(value && value[0] == '0');
    }();
    return enabled;
}

void RecordBubbleTrail(const BubbleTrail& trail)
{
    if (!UnderwaterBubblesEnabled()) return;
    Trails().Record(trail);
    static const bool trace = [] {
        const char* value = std::getenv("POSEIDON_UNDERWATER_TRACE");
        return value && value[0] == '1';
    }();
    if (trace)
        LOG_INFO(Graphics, "UNDERWATER_TRAIL from={:.2f},{:.2f},{:.2f} to={:.2f},{:.2f},{:.2f} speed={:.0f} timeMs={}",
                 trail.from.x, trail.from.y, trail.from.z, trail.to.x, trail.to.y, trail.to.z, trail.speed, trail.birthMs);
}

void RecordBubbleBlast(const BubbleBlast& blast)
{
    if (!UnderwaterBubblesEnabled()) return;
    Blasts().Record(blast);
    FlashLight(blast);
}

void DrawUnderwaterBubbles()
{
    if (!UnderwaterBubblesEnabled() || !GScene || !GEngine || !GScene->GetCamera()) return;
    const Camera& camera = *GScene->GetCamera();
    const Vector3 eye = camera.Position();
    // Bubbles are only drawn from inside the water: seen from above they would sit on
    // top of the surface, which the sprites do not depth-test against.
    // The surface as the renderer draws it near the camera, where it knows it: in a swell
    // the predictor and the drawn crest differ, and a camera in a trough would draw
    // bubbles in the sky.
    float surface = SwimWaterSurfaceY(eye.X(), eye.Z());
    float drawnSurface = 0.0f;
    if (DrawnWaterHeightAt(eye.X(), eye.Z(), drawnSurface)) surface = drawnSurface;
    if (!(eye.Y() < surface - 0.05f)) return;
    Texture* bubbleTexture = BubbleTexture();
    Texture* softTexture = SoftTexture();
    Texture* gasTexture = GasTexture();
    if (!bubbleTexture || !softTexture || !gasTexture) return;
    const MipInfo bubbleMip = GEngine->TextBank()->UseMipmap(bubbleTexture, 0, 0);
    const MipInfo softMip = GEngine->TextBank()->UseMipmap(softTexture, 0, 0);
    const MipInfo gasMip = GEngine->TextBank()->UseMipmap(gasTexture, 0, 0);
    if (!bubbleMip.IsOK() || !softMip.IsOK() || !gasMip.IsOK()) return;

    Matrix4Val project = camera.Projection();
    Matrix4Val toView = GScene->ScaledInvTransform();
    const float nearest = camera.Near() * 10.0f;
    const float invLeft = camera.InvLeft();
    const double now = Object::RenderTimeMs();
    // Bubbles reflect the daylight that reaches them; the same daylight scale the
    // snowflakes use, so they are silver by day and nearly invisible at night
    // (except in a blast's own flash).
    float daylightScale = 0.02f;
    if (GScene->MainLight())
    {
        const auto sky = GEngine->GetSkySettings();
        const float daylight = 1.0f - std::clamp(GScene->MainLight()->NightEffect(), 0.0f, 1.0f);
        daylightScale = std::max(0.02f, sky.sunIntensity * sky.exposure * 0.30f * daylight);
    }

    constexpr int alphaSpecials = ClampU | ClampV | NoZWrite | IsAlpha | IsAlphaFog | NoShadow;
    constexpr int lightSpecials = alphaSpecials | IsLight;
    constexpr float maxDistance = 60.0f; // underwater visibility is tens of metres at best

    int drawn = 0;
    const auto draw = [&](const BubbleSprite& b) {
        const Vector3 head(b.head.x, b.head.y, b.head.z);
        const Vector3 tail(b.tail.x, b.tail.y, b.tail.z);
        const Vector3 headView = toView * head;
        const Vector3 tailView = toView * tail;
        if (headView.Z() < nearest || tailView.Z() < nearest) return;
        const Vector3 midView = toView * ((head + tail) * 0.5f);
        const float invMid = 1.0f / midView.Z();
        const float hx = project(0, 2) + project(0, 0) * headView.X() / headView.Z();
        const float hy = project(1, 2) + project(1, 1) * headView.Y() / headView.Z();
        const float tx = project(0, 2) + project(0, 0) * tailView.X() / tailView.Z();
        const float ty = project(1, 2) + project(1, 1) * tailView.Y() / tailView.Z();
        // Keep a minimum on-screen size: a bubble a pixel wide is lost to the depth
        // cutout and to MSAA, and a real bubble trail reads as a line at any range.
        float halfWidth = b.radius * std::fabs(project(0, 0)) * invMid * invLeft;
        if (halfWidth < 0.15f) return;
        halfWidth = std::max(halfWidth, 1.2f);
        float dirX = hx - tx, dirY = hy - ty;
        const float streak = std::sqrt(dirX * dirX + dirY * dirY);
        if (streak < 1e-3f) { dirX = 0.0f; dirY = 1.0f; }
        const float nearPos = floatMax(midView.Z() - std::min(b.radius, 0.5f), nearest);
        const float invNear = 1.0f / nearPos;
        Vector3 screen;
        screen[0] = project(0, 2) + project(0, 0) * midView.X() * invMid;
        screen[1] = project(1, 2) + project(1, 1) * midView.Y() * invMid;
        screen[2] = project(2, 2) + project.Position()[2] * invNear;
        const float distance = (head - eye).Size();
        const float murk = std::clamp(1.0f - distance / maxDistance, 0.0f, 1.0f);
        if (b.kind == BubbleKind::Flash)
        {
            // Water absorbs red first: an underwater flash reads white-green.
            Color colour(0.85f, 1.0f, 0.9f, b.alpha * murk);
            GEngine->DrawDecalRotated(screen, invNear, std::max(halfWidth, 0.5f), std::max(halfWidth, 0.5f),
                                      PackedColor(colour), softMip, lightSpecials, dirX, dirY, 14.0f);
        }
        else if (b.kind == BubbleKind::Bubble)
        {
            Color colour(0.85f, 0.95f, 1.0f, b.alpha * murk);
            GEngine->DrawDecalRotated(screen, invNear, std::max(halfWidth, 0.5f), std::max(halfWidth, 0.5f),
                                      PackedColor(colour), bubbleMip, alphaSpecials, dirX, dirY,
                                      std::max(daylightScale * 1.1f, 1e-3f));
        }
        else if (b.kind == BubbleKind::GasBubble || b.kind == BubbleKind::Foam)
        {
            Color colour(0.8f, 0.86f, 0.9f, b.alpha * murk);
            GEngine->DrawDecalRotated(screen, invNear, std::max(halfWidth, 0.5f), std::max(halfWidth, 0.5f),
                                      PackedColor(colour), gasMip, alphaSpecials, dirX, dirY,
                                      std::max(daylightScale * 0.55f, 1e-3f));
        }
        else
        {
            // Cavity streak: the air channel behind the round, a white line.
            const float halfLength = halfWidth + streak * 0.5f;
            Color colour(0.9f, 0.97f, 1.0f, b.alpha * murk);
            GEngine->DrawDecalRotated(screen, invNear, std::max(halfWidth, 0.5f), std::max(halfLength, 0.5f),
                                      PackedColor(colour), softMip, alphaSpecials, dirX, dirY,
                                      std::max(daylightScale * 1.0f, 1e-3f));
        }
        ++drawn;
    };
    const auto near = [&](MuzzleBurstVec p) {
        const float dx = p.x - eye.X(), dy = p.y - eye.Y(), dz = p.z - eye.Z();
        return dx * dx + dy * dy + dz * dz < (maxDistance + 5.0f) * (maxDistance + 5.0f);
    };
    // The underwater compositor fogs every pixel by its depth: bubbles must put their
    // own distance in the depth buffer or they take the murk behind them and vanish.
    // Gathered first and drawn far to near: each sprite writes depth (see above), so a
    // farther one drawn after a nearer one would be cut off along the nearer's edge.
    static std::vector<std::pair<float, BubbleSprite>> sprites;
    sprites.clear();
    const auto collect = [&](const BubbleSprite& b) {
        const float dx = b.head.x - eye.X(), dy = b.head.y - eye.Y(), dz = b.head.z - eye.Z();
        sprites.emplace_back(dx * dx + dy * dy + dz * dz, b);
    };
    Trails().ForEach([&](const BubbleTrail& trail) {
        if (near(trail.from) || near(trail.to)) BubbleTrailSprites(trail, now, collect);
    });
    Blasts().ForEach([&](const BubbleBlast& blast) {
        if (near(blast.centre)) BubbleBlastSprites(blast, now, collect);
    });
    std::sort(sprites.begin(), sprites.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    GEngine->SetDecalDepthCutout(true);
    for (const auto& entry : sprites) draw(entry.second);
    GEngine->SetDecalDepthCutout(false);
    static const bool trace = [] {
        const char* value = std::getenv("POSEIDON_UNDERWATER_TRACE");
        return value && value[0] == '1';
    }();
    static int last = -1;
    if (trace && drawn != last && (drawn == 0) != (last == 0))
    {
        last = drawn;
        LOG_INFO(Graphics, "UNDERWATER_BUBBLES timeMs={:.1f} sprites={}", now, drawn);
    }
}
} // namespace Poseidon::render
