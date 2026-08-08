#pragma once

#include <cstdint>

namespace Poseidon {
namespace Model {

// VertexFlags: ClipFlags from types.hpp + POINT_* from data3d.h
enum class VertexFlags : uint32_t {
    None = 0,
    
    // Clipping planes (frustum culling)
    ClipFront = 0x0001,
    ClipBack = 0x0002,
    ClipLeft = 0x0004,
    ClipRight = 0x0008,
    ClipBottom = 0x0010,
    ClipTop = 0x0020,
    ClipUser = 0x0040,
    ClipAll = ClipFront | ClipBack | ClipLeft | ClipRight | ClipBottom | ClipTop,
    
    // Surface attributes (land interaction)
    LandNone = 0x0000,
    LandOn = 0x0100,         // Vertex on terrain surface
    LandUnder = 0x0200,      // Vertex below terrain
    LandAbove = 0x0400,      // Vertex above terrain
    LandKeep = 0x0800,       // Keep land position
    LandMask = 0x0F00,
    
    // Decal attributes (texture projection)
    DecalNone = 0x0000,
    DecalNormal = 0x1000,    // Normal decal projection
    DecalVertical = 0x2000,  // Vertical decal projection
    DecalMask = 0x3000,
    
    // Fogging attributes
    FogNormal = 0x0000,
    FogDisable = 0x4000,     // No fog applied
    FogSky = 0x8000,         // Sky fog
    FogShadow = 0xC000,      // Shadow fog
    FogMask = 0xC000,
    
    // Per-vertex lighting attributes
    LightNormal = 0x00000,
    LightSky = 0x10000,      // Sky lighting (ambient + sky color)
    LightCloud = 0x20000,    // Cloud lighting (overcast)
    LightSun = 0x30000,      // Sun lighting (direct)
    LightHalf = 0x80000,     // Half lighting (POINT_HALFLIGHT)
    LightMask = 0xF0000,
    
    // Special attributes
    SpecialHidden = 0x1000000,  // Hidden vertex (not rendered)
    SpecialMask = 0xF000000,
    
    // User-defined flags (game-specific)
    UserMask = 0xFF0000,
    UserStep = 0x010000,
};

// FaceFlags: FACE_* from data3d.h
enum class FaceFlags : uint32_t {
    None = 0,
    
    // Lighting
    NoLight = 0x0001,           // No lighting applied (self-illuminated)
    Ambient = 0x0002,           // Only ambient lighting
    FullLight = 0x0004,         // Full dynamic lighting
    BothSidesLight = 0x0020,    // Light both sides of face
    SkyLight = 0x0080,          // Sky lighting
    ReverseLight = 0x100000,    // Reverse normal for lighting
    FlatLight = 0x200000,       // Flat shading (no smooth interpolation)
    LightMask = 0x3000A7,
    
    // Shadows
    IsShadow = 0x0008,          // This face casts shadows
    NoShadow = 0x0010,          // This face doesn't cast shadows
    ShadowMask = 0x0018,
    
    // Z-buffer bias (fixes z-fighting)
    ZBiasMask = 0x0300,
    ZBiasStep = 0x0100,
    
    // Triangle strip/fan encoding (MLOD optimization)
    BeginFan = 0x10000,         // Start of triangle fan
    BeginStrip = 0x20000,       // Start of triangle strip
    ContinueFan = 0x40000,      // Continue triangle fan
    ContinueStrip = 0x80000,    // Continue triangle strip
    FanStripMask = 0xF0000,
    
    // Rendering hints
    DisableTexMerge = 0x1000000, // Disable texture merging optimization
    
    // User-defined flags (game-specific)
    UserMask = 0xFE000000,
    UserStep = 0x02000000,
    UserShift = 25,
};

// NOTE: Materials have no independent flags; they inherit FaceFlags from their faces.

// RenderHints: orHints/andHints optimization and culling flags
enum class RenderHints : uint32_t {
    None = 0,
    
    // Rendering behavior
    IsShadow = 0x0001,          // Shadow geometry
    IsLight = 0x0002,           // Light/flare geometry
    NoShadows = 0x0004,         // Disable shadow casting
    OnSurface = 0x0008,         // Rendered on terrain surface
    
    // Transparency
    IsTransparent = 0x0010,     // Alpha transparency
    IsAlpha = 0x0020,           // Alpha channel present
    NoZWrite = 0x0040,          // Disable Z-buffer writes
    IsGlass = 0x0080,           // Glass material (special transparency)
    
    // Texture behavior
    NoClamp = 0x2000,           // Texture tiling enabled
    ClampU = 0x4000,            // Clamp U texture coordinate
    ClampV = 0x8000,            // Clamp V texture coordinate
    
    // Animation
    IsAnimated = 0x10000,       // Has animated texture
    IsAlphaOrdered = 0x20000,   // Alpha order important - cannot reorder
    
    // Performance hints
    NoDropdown = 0x40000,       // Disable speed dropdown
    IsAlphaFog = 0x80000,       // Use alpha blending instead of fog
    FogDisabled = 0x100000,     // Fog disabled for entire object
    IsColored = 0x200000,       // Object constant color is set
    
    // Visibility
    IsHidden = 0x400000,        // Face hidden
    IsHiddenProxy = 0x10000000, // Face hard-hidden (proxy)
    
    // Quality hints
    BestMipmap = 0x800000,      // Best mipmap forced
    DetailTexture = 0x1000000,  // Use default detail texture
    SpecularTexture = 0x2000000,// Use default specular texture
    
    // Z-buffer bias (same as FaceFlags for consistency)
    ZBiasMask = 0xC000000,
    ZBiasStep = 0x4000000,
    
    // Advanced
    NoTexMerger = 0x20000000,   // Disable texture merging
    SpecLighting = 0x40000000,  // Lighting unsuitable for D3D
    DisableSun = 0x80000000,    // Disable sun lighting
};

// SpecialFlags: per-face/LOD special properties.
// Bit values intentionally overlap with RenderHints (same origin in types.hpp).
enum class SpecialFlags : uint32_t {
    None = 0,
    IsShadow = 0x0001,
    IsLight = 0x0002,
    NoShadows = 0x0004,
    OnSurface = 0x0008,
    IsTransparent = 0x0010,
    IsAlpha = 0x0020,
    NoZWrite = 0x0040,
    IsGlass = 0x0080,
    NoClamp = 0x2000,
    ClampU = 0x4000,
    ClampV = 0x8000,
    IsAnimated = 0x10000,
    IsAlphaOrdered = 0x20000,
    NoDropdown = 0x40000,
    IsAlphaFog = 0x80000,
    FogDisabled = 0x100000,
    IsColored = 0x200000,
    IsHidden = 0x400000,
    BestMipmap = 0x800000,
    DetailTexture = 0x1000000,
    SpecularTexture = 0x2000000,
    ZBiasMask = 0xC000000,
    ZBiasStep = 0x4000000,
    IsHiddenProxy = 0x10000000,
    NoTexMerger = 0x20000000,
    SpecLighting = 0x40000000,
    DisableSun = 0x80000000,
    
    IsOnSurface = 0x0100,       // confirmed on surface (OnSurface = should be placed on surface)
    ShadowDisabled = 0x0200,    // Shadow rendering disabled for this geometry
};

// VertexFlags operators
inline VertexFlags operator|(VertexFlags a, VertexFlags b) {
    return static_cast<VertexFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline VertexFlags operator&(VertexFlags a, VertexFlags b) {
    return static_cast<VertexFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline VertexFlags operator~(VertexFlags a) {
    return static_cast<VertexFlags>(~static_cast<uint32_t>(a));
}

inline VertexFlags& operator|=(VertexFlags& a, VertexFlags b) {
    return a = a | b;
}

inline VertexFlags& operator&=(VertexFlags& a, VertexFlags b) {
    return a = a & b;
}

inline bool HasFlag(VertexFlags flags, VertexFlags test) {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(test)) != 0;
}

inline uint32_t GetMasked(VertexFlags flags, VertexFlags mask) {
    return static_cast<uint32_t>(flags) & static_cast<uint32_t>(mask);
}

// FaceFlags operators
inline FaceFlags operator|(FaceFlags a, FaceFlags b) {
    return static_cast<FaceFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline FaceFlags operator&(FaceFlags a, FaceFlags b) {
    return static_cast<FaceFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline FaceFlags operator~(FaceFlags a) {
    return static_cast<FaceFlags>(~static_cast<uint32_t>(a));
}

inline FaceFlags& operator|=(FaceFlags& a, FaceFlags b) {
    return a = a | b;
}

inline FaceFlags& operator&=(FaceFlags& a, FaceFlags b) {
    return a = a & b;
}

inline bool HasFlag(FaceFlags flags, FaceFlags test) {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(test)) != 0;
}

inline uint32_t GetMasked(FaceFlags flags, FaceFlags mask) {
    return static_cast<uint32_t>(flags) & static_cast<uint32_t>(mask);
}

// RenderHints operators
inline RenderHints operator|(RenderHints a, RenderHints b) {
    return static_cast<RenderHints>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline RenderHints operator&(RenderHints a, RenderHints b) {
    return static_cast<RenderHints>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline RenderHints operator~(RenderHints a) {
    return static_cast<RenderHints>(~static_cast<uint32_t>(a));
}

inline RenderHints& operator|=(RenderHints& a, RenderHints b) {
    return a = a | b;
}

inline RenderHints& operator&=(RenderHints& a, RenderHints b) {
    return a = a & b;
}

inline bool HasFlag(RenderHints flags, RenderHints test) {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(test)) != 0;
}

inline uint32_t GetMasked(RenderHints flags, RenderHints mask) {
    return static_cast<uint32_t>(flags) & static_cast<uint32_t>(mask);
}

// SpecialFlags operators
inline SpecialFlags operator|(SpecialFlags a, SpecialFlags b) {
    return static_cast<SpecialFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline SpecialFlags operator&(SpecialFlags a, SpecialFlags b) {
    return static_cast<SpecialFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline SpecialFlags operator~(SpecialFlags a) {
    return static_cast<SpecialFlags>(~static_cast<uint32_t>(a));
}

inline SpecialFlags& operator|=(SpecialFlags& a, SpecialFlags b) {
    return a = a | b;
}

inline SpecialFlags& operator&=(SpecialFlags& a, SpecialFlags b) {
    return a = a & b;
}

inline bool HasFlag(SpecialFlags flags, SpecialFlags test) {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(test)) != 0;
}

inline uint32_t GetMasked(SpecialFlags flags, SpecialFlags mask) {
    return static_cast<uint32_t>(flags) & static_cast<uint32_t>(mask);
}

inline VertexFlags VertexFlagsFromUint32(uint32_t value) {
    return static_cast<VertexFlags>(value);
}

inline FaceFlags FaceFlagsFromUint32(uint32_t value) {
    return static_cast<FaceFlags>(value);
}

inline RenderHints RenderHintsFromUint32(uint32_t value) {
    return static_cast<RenderHints>(value);
}

inline SpecialFlags SpecialFlagsFromUint32(uint32_t value) {
    return static_cast<SpecialFlags>(value);
}

inline uint32_t ToUint32(VertexFlags flags) {
    return static_cast<uint32_t>(flags);
}

inline uint32_t ToUint32(FaceFlags flags) {
    return static_cast<uint32_t>(flags);
}

inline uint32_t ToUint32(RenderHints flags) {
    return static_cast<uint32_t>(flags);
}

inline uint32_t ToUint32(SpecialFlags flags) {
    return static_cast<uint32_t>(flags);
}

// AST-018 -- which bits this build claims to understand.
//
// The loaders already keep the source word intact: every flag field is a raw
// cast, so no bit is dropped. What was missing is the other half of "preserve
// raw bits plus decoded known flags" -- a way to ask which bits were decoded and
// which were merely carried. Without it, a face whose word contains a bit no
// enum names is indistinguishable from one that is fully understood, and the
// difference only turns up as a rendering surprise.
//
// These masks are the union of the named values, deliberately including the
// user-defined ranges: a user bit is a bit the format reserves for content, so
// it is known even though its meaning is content's business.
constexpr uint32_t kKnownVertexFlagBits =
    static_cast<uint32_t>(VertexFlags::ClipAll) | static_cast<uint32_t>(VertexFlags::ClipUser) |
    static_cast<uint32_t>(VertexFlags::LandMask) | static_cast<uint32_t>(VertexFlags::DecalMask) |
    static_cast<uint32_t>(VertexFlags::FogMask) | static_cast<uint32_t>(VertexFlags::LightMask) |
    static_cast<uint32_t>(VertexFlags::SpecialMask) | static_cast<uint32_t>(VertexFlags::UserMask);

constexpr uint32_t kKnownFaceFlagBits =
    static_cast<uint32_t>(FaceFlags::LightMask) | static_cast<uint32_t>(FaceFlags::ShadowMask) |
    static_cast<uint32_t>(FaceFlags::ZBiasMask) | static_cast<uint32_t>(FaceFlags::FanStripMask) |
    static_cast<uint32_t>(FaceFlags::DisableTexMerge) | static_cast<uint32_t>(FaceFlags::UserMask);

inline uint32_t UnknownBits(VertexFlags flags) {
    return static_cast<uint32_t>(flags) & ~kKnownVertexFlagBits;
}

inline uint32_t UnknownBits(FaceFlags flags) {
    return static_cast<uint32_t>(flags) & ~kKnownFaceFlagBits;
}

// True when the word carries something this build cannot name. Not an error:
// the bits are still there to be interpreted later. It is a fact a consumer --
// or a conformance report -- is entitled to know.
inline bool HasUnknownBits(VertexFlags flags) { return UnknownBits(flags) != 0; }
inline bool HasUnknownBits(FaceFlags flags) { return UnknownBits(flags) != 0; }

} // namespace Model
} // namespace Poseidon

