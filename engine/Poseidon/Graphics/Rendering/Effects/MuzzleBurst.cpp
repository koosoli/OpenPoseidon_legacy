#include <Poseidon/Graphics/Rendering/Effects/MuzzleBurst.hpp>

#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/World/Scene/Camera/Camera.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Scene/Scene.hpp>
#include <cstdlib>
#include <vector>

namespace Poseidon::render
{
namespace
{
MuzzleBurstRing& Ring()
{
    static MuzzleBurstRing ring;
    return ring;
}

// The particle sprite: a soft, slightly lumpy disc, white so the vertex colour
// carries the cooling ramp. Built once; the engine owns it from then on.
Texture* SpriteTexture()
{
    static Ref<Texture> texture;
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
                const float angle = std::atan2(py, px);
                // Low-order wobble keeps a puff from reading as a perfect dot.
                const float r2 = (px * px + py * py) *
                    (1.0f + 0.18f * std::sin(3.0f * angle + 0.7f) + 0.10f * std::sin(5.0f * angle + 2.1f));
                const float edge = std::max(1.0f - r2, 0.0f);
                const float alpha = std::exp2(-4.0f * r2) * edge * edge;
                pixels[std::size_t(y * side + x) * 4 + 3] = std::uint8_t(std::lround(255.0f * std::clamp(alpha, 0.0f, 1.0f)));
            }
        texture = GEngine->TextBank()->CreateDynamic(side, side, pixels.data(), std::uint32_t(pixels.size()), true,
                                                     AlphaStats::Blend);
    }
    return texture.GetRef();
}
} // namespace

bool MuzzleBurstEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("POSEIDON_MUZZLE_BURST");
        return !(value && value[0] == '0');
    }();
    return enabled;
}

void RecordMuzzleBurst(const MuzzleBurstShot& shot)
{
    if (!MuzzleBurstEnabled()) return;
    const auto finite = [](MuzzleBurstVec v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
    if (!finite(shot.muzzle) || !finite(shot.forward) || !finite(shot.carrier)) return;
    Ring().Record(shot);
}

void DrawMuzzleBursts()
{
    if (!MuzzleBurstEnabled() || !GScene || !GEngine || !GScene->GetCamera()) return;
    Texture* texture = SpriteTexture();
    if (!texture) return;
    const MipInfo mip = GEngine->TextBank()->UseMipmap(texture, 0, 0);
    if (!mip.IsOK()) return;

    const Camera& camera = *GScene->GetCamera();
    Matrix4Val project = camera.Projection();
    Matrix4Val toView = GScene->ScaledInvTransform();
    const float nearest = camera.Near() * 10.0f;
    const float invLeft = camera.InvLeft();
    const Vector3 eye = camera.Position();
    // Interpolated render time, not Glob.time: the clock only steps once per tick, and
    // a burst shorter than a tick would otherwise show one frozen frame.
    const double now = Object::RenderTimeMs();
    // Additive light sprite, tested against the world, never writing depth; the
    // caller folds fog into alpha (IsAlphaFog), which also enables the HDR lane.
    constexpr int specials = ClampU | ClampV | NoZWrite | IsAlpha | IsAlphaFog | NoShadow | IsLight;
    constexpr float maxDistance = 350.0f;

    int drawn = 0;
    Ring().ForEachLive(now, [&](const MuzzleBurstShot& shot) {
        const Vector3 muzzle(shot.muzzle.x, shot.muzzle.y, shot.muzzle.z);
        const float distance2 = (muzzle - eye).SquareSize();
        if (distance2 > maxDistance * maxDistance) return;
        const float fog = GScene->Fog8(distance2) * (1.0f / 255.0f);
        if (fog >= 0.99f) return;
        MuzzleBurstParticles(shot, now, 9.81f, [&](const MuzzleParticle& p) {
            const Vector3 head(p.head.x, p.head.y, p.head.z);
            const Vector3 tail(p.tail.x, p.tail.y, p.tail.z);
            const Vector3 headView = toView * head;
            const Vector3 tailView = toView * tail;
            if (headView.Z() < nearest || tailView.Z() < nearest) return;
            const Vector3 mid = (head + tail) * 0.5f;
            const Vector3 midView = toView * mid;
            const float invHead = 1.0f / headView.Z();
            const float invTail = 1.0f / tailView.Z();
            const float invMid = 1.0f / midView.Z();
            const float hx = project(0, 2) + project(0, 0) * headView.X() * invHead;
            const float hy = project(1, 2) + project(1, 1) * headView.Y() * invHead;
            const float tx = project(0, 2) + project(0, 0) * tailView.X() * invTail;
            const float ty = project(1, 2) + project(1, 1) * tailView.Y() * invTail;
            const float pxPerMetre = std::fabs(project(0, 0)) * invMid * invLeft;
            const float halfWidth = p.radius * pxPerMetre;
            if (halfWidth < 0.15f) return; // sub-pixel at this range
            float dirX = hx - tx, dirY = hy - ty;
            const float streak = std::sqrt(dirX * dirX + dirY * dirY);
            if (streak < 1e-3f) { dirX = 0.0f; dirY = 1.0f; }
            const float halfLength = halfWidth + streak * 0.5f;

            const float nearPos = floatMax(midView.Z() - std::min(p.radius * 2.0f, 0.05f), nearest);
            const float invNear = 1.0f / nearPos;
            Vector3 screen;
            screen[0] = project(0, 2) + project(0, 0) * midView.X() * invMid;
            screen[1] = project(1, 2) + project(1, 1) * midView.Y() * invMid;
            screen[2] = project(2, 2) + project.Position()[2] * invNear;

            // A streak spreads the same light over a longer quad; keep its energy.
            const float spread = halfWidth / halfLength;
            Color colour(p.r, p.g, p.b, (1.0f - fog));
            const float radiance = std::max(p.radiance * std::max(spread, 0.25f), 1e-3f);
            GEngine->DrawDecalRotated(screen, invNear, std::max(halfWidth, 0.5f), std::max(halfLength, 0.5f),
                                      PackedColor(colour), mip, specials, dirX, dirY, radiance);
            ++drawn;
        });
    });
    static const bool trace = [] {
        const char* value = std::getenv("POSEIDON_MUZZLE_FLASH_TRACE");
        return value && value[0] == '1';
    }();
    static int lastLogged = -1;
    if (trace && drawn > 0 && drawn != lastLogged)
    {
        lastLogged = drawn;
        LOG_INFO(Graphics, "MUZZLE_BURST timeMs={:.1f} particles={}", now, drawn);
    }
}
} // namespace Poseidon::render
