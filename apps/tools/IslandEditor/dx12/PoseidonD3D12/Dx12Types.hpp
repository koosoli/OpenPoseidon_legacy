#pragma once

// State, format and layout names of the Direct3D 12 backend's device layer
// (Dx12::Device).  The engine's drawing code sets render and sampler states one
// at a time and addresses textures by a small format list; these are the names it
// uses.  Dx12::Device turns them into Direct3D 12 state objects and DXGI formats.
//
// The numbering of the comparison functions and stencil operations matches
// D3D12_COMPARISON_FUNC / D3D12_STENCIL_OP, and the colour-write bits match
// D3D12_COLOR_WRITE_ENABLE; Dx12Device.cpp relies on that.

#include <windows.h>

#include <cstdint>

// Render states (index into Dx12::Device's state table; < 256)
enum GxRenderState : uint32_t
{
    GXRS_ZENABLE = 7,
    GXRS_ZWRITEENABLE = 14,
    GXRS_ALPHATESTENABLE = 15,
    GXRS_SRCBLEND = 19,
    GXRS_DESTBLEND = 20,
    GXRS_CULLMODE = 22,
    GXRS_ZFUNC = 23,
    GXRS_ALPHABLENDENABLE = 27,
    GXRS_FOGENABLE = 28,
    GXRS_STENCILENABLE = 52,
    GXRS_STENCILFAIL = 53,
    GXRS_STENCILZFAIL = 54,
    GXRS_STENCILPASS = 55,
    GXRS_STENCILFUNC = 56,
    GXRS_STENCILREF = 57,
    GXRS_STENCILMASK = 58,
    GXRS_STENCILWRITEMASK = 59,
    GXRS_CLIPPING = 136,
    GXRS_LIGHTING = 137,
    GXRS_MULTISAMPLEANTIALIAS = 161,
    GXRS_COLORWRITEENABLE = 168,
    GXRS_SCISSORTESTENABLE = 174,
    GXRS_SLOPESCALEDEPTHBIAS = 175,
    GXRS_SRGBWRITEENABLE = 194,
    GXRS_DEPTHBIAS = 195,
};

// Sampler states, per texture stage
enum GxSamplerState : uint32_t
{
    GXSAMP_ADDRESSU = 1,
    GXSAMP_ADDRESSV = 2,
    GXSAMP_MAGFILTER = 5,
    GXSAMP_MINFILTER = 6,
    GXSAMP_MIPFILTER = 7,
    GXSAMP_MAXANISOTROPY = 10,
    GXSAMP_SRGBTEXTURE = 11,
};

// Texture / surface formats
enum GxFormat : uint32_t
{
    GXFMT_DXT1 = 0x31545844, // 'DXT1'
    GXFMT_DXT3 = 0x33545844, // 'DXT3'
    GXFMT_DXT5 = 0x35545844, // 'DXT5'
    GXFMT_UNKNOWN = 0,
    GXFMT_A8R8G8B8 = 21,
    GXFMT_X8R8G8B8 = 22,
    GXFMT_R5G6B5 = 23,
    GXFMT_A1R5G5B5 = 25,
    GXFMT_A4R4G4B4 = 26,
    GXFMT_L8 = 50,
    GXFMT_A8L8 = 51,
    GXFMT_D24S8 = 75,
    GXFMT_D24X8 = 77,
    GXFMT_INDEX16 = 101,
    GXFMT_A16B16G16R16F = 113,
    GXFMT_R32F = 114,
};

// Texture filters
enum GxTextureFilter : uint32_t
{
    GXTEXF_NONE = 0,
    GXTEXF_POINT = 1,
    GXTEXF_LINEAR = 2,
    GXTEXF_ANISOTROPIC = 3,
};

// Texture addressing
enum : uint32_t
{
    GXTADDRESS_WRAP = 1,
    GXTADDRESS_MIRROR = 2,
    GXTADDRESS_CLAMP = 3,
    GXTADDRESS_BORDER = 4,
    GXTADDRESS_MIRRORONCE = 5,
};

// Blend factors
enum : uint32_t
{
    GXBLEND_ZERO = 1,
    GXBLEND_ONE = 2,
    GXBLEND_SRCCOLOR = 3,
    GXBLEND_INVSRCCOLOR = 4,
    GXBLEND_SRCALPHA = 5,
    GXBLEND_INVSRCALPHA = 6,
    GXBLEND_DESTALPHA = 7,
    GXBLEND_INVDESTALPHA = 8,
    GXBLEND_DESTCOLOR = 9,
    GXBLEND_INVDESTCOLOR = 10,
    GXBLEND_SRCALPHASAT = 11,
};

// Comparison functions
enum : uint32_t
{
    GXCMP_EQUAL = 3,
    GXCMP_LESSEQUAL = 4,
    GXCMP_ALWAYS = 8,
};

// Cull modes (by winding of the culled triangles)
enum : uint32_t
{
    GXCULL_NONE = 1,
    GXCULL_CW = 2,
    GXCULL_CCW = 3,
};

// Stencil operations
enum : uint32_t
{
    GXSTENCILOP_KEEP = 1,
    GXSTENCILOP_REPLACE = 3,
    GXSTENCILOP_INCR = 7,
};

// Depth test on / off
enum : uint32_t
{
    GXZB_FALSE = 0,
    GXZB_TRUE = 1,
};

// Primitive types
enum GxPrimitiveType : uint32_t
{
    GXPT_POINTLIST = 1,
    GXPT_LINELIST = 2,
    GXPT_LINESTRIP = 3,
    GXPT_TRIANGLELIST = 4,
    GXPT_TRIANGLESTRIP = 5,
};

// Sample counts
enum GxMultisample : uint32_t
{
    GXMULTISAMPLE_NONE = 0,
};

// Back buffer selector
enum GxBackBufferType : uint32_t
{
    GXBACKBUFFER_TYPE_MONO = 0,
};

// Resource pools (kept for the call shape; all resources live on the GPU)
enum GxPool : uint32_t
{
    GXPOOL_DEFAULT = 0,
    GXPOOL_MANAGED = 1,
    GXPOOL_SYSTEMMEM = 2,
};

// Resource kinds
enum : uint32_t
{
    GXRTYPE_SURFACE = 1,
};

// Vertex element types
enum : uint32_t
{
    GXDECLTYPE_FLOAT1 = 0,
    GXDECLTYPE_FLOAT2 = 1,
    GXDECLTYPE_FLOAT3 = 2,
    GXDECLTYPE_FLOAT4 = 3,
    GXDECLTYPE_COLOR = 4,
};

// Vertex element semantics
enum : uint32_t
{
    GXDECLUSAGE_POSITION = 0,
    GXDECLUSAGE_NORMAL = 3,
    GXDECLUSAGE_TEXCOORD = 5,
    GXDECLUSAGE_COLOR = 10,
};

// Vertex element method
enum : uint32_t
{
    GXDECLMETHOD_DEFAULT = 0,
};

// Clear flags
enum : uint32_t
{
    GXCLEAR_STENCIL = 0x00000004l,
    GXCLEAR_TARGET = 0x00000001l,
    GXCLEAR_ZBUFFER = 0x00000002l,
};

// Resource usage flags
enum : uint32_t
{
    GXUSAGE_DYNAMIC = (0x00000200L),
    GXUSAGE_RENDERTARGET = (0x00000001L),
    GXUSAGE_WRITEONLY = (0x00000008L),
};

// Lock flags
enum : uint32_t
{
    GXLOCK_READONLY = 0x00000010,
    GXLOCK_NOOVERWRITE = 0x00001000,
    GXLOCK_DISCARD = 0x00002000,
};

// Colour write mask bits
enum : uint32_t
{
    GXCOLORWRITEENABLE_RED = (1L<<0),
};

using GxColor = uint32_t; // 0xAARRGGBB
#define GX_COLOR_ARGB(a, r, g, b) \
    (static_cast<GxColor>((((a) & 0xff) << 24) | (((r) & 0xff) << 16) | (((g) & 0xff) << 8) | ((b) & 0xff)))
#define GX_COLOR_XRGB(r, g, b) GX_COLOR_ARGB(0xff, r, g, b)

struct GxViewport
{
    uint32_t X, Y, Width, Height;
    float MinZ, MaxZ;
};

struct GxRect
{
    long x1, y1, x2, y2;
};

struct GxLockedRect
{
    int Pitch;
    void* pBits;
};

struct GxSurfaceDesc
{
    GxFormat Format;
    uint32_t Type;
    uint32_t Usage;
    GxPool Pool;
    GxMultisample MultiSampleType;
    uint32_t MultiSampleQuality;
    uint32_t Width, Height;
};

// One element of a vertex layout; a list ends with GXDECL_END().
struct GxVertexElement
{
    uint16_t Stream;
    uint16_t Offset;
    uint8_t Type;
    uint8_t Method;
    uint8_t Usage;
    uint8_t UsageIndex;
};
#define GXDECL_END() {0xFF, 0, 0, 0, 0, 0}
