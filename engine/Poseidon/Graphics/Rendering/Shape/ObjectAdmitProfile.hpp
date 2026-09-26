#pragma once

// Where one streamed-object ADMISSION spends its time, below ObjectCreate.
//
// The per-cold-model row in Landscape::UpdateModernObjectResidency (LandSave.cpp) splits a
// cold admission into parse / adapt / opt / create. Measured on Reforger Everon (591 cold
// models, .tmp-shadow/reforger3/owner-trees.log): parse=0.0 adapt=0.6-15.8 opt=0.2-2.5
// create=26 median / 208 worst ms -- so `create` is the whole cost, and `create` is one opaque
// number: ObjectCreate -> NewObject -> AddObject -> Engine::SceneObjectCreated ->
// RegisterGpuModel (LOD/section conversion into pool meshes, section classification, texture
// upload, material resolution, the register_model FFI) -> proxies -> marker lights.
//
// This is the split of THAT number. The world side (LandSave.cpp) opens a scope around one
// ObjectCreate; the graphics side (EngineWgpu.cpp, TextureWgpu.cpp) accumulates into the
// buckets while a scope is open and does nothing at all otherwise (one bool load per site).
// It lives here, in the shape library both sides already include, because Engine.hpp's vtable
// is slot-locked and this must not add a virtual to it.
//
// Not thread-safe by design: every accumulating site is on the thread that runs ObjectCreate.
// Buckets are INCLUSIVE where noted: `sceneMs` contains `registerMs` and `proxiesMs`;
// `registerMs` and `proxiesMs` each contain their own share of meshBuild/meshCreate/section/
// modelRegister; `sectionMs` contains the texture and alpha-scan buckets. The disjoint
// partition the row prints is derived in LandSave.cpp from these.

#include <cstdint>

namespace Poseidon::render
{

struct ObjectAdmitProfile
{
    bool active = false;

    // Engine::SceneObjectCreated, whole (top-level object only; proxy children are inside).
    double sceneMs = 0.0;
    // RegisterGpuModel of the object's own shape (inclusive of everything it does).
    double registerMs = 0.0;
    // EmitGpuProxies (inclusive: nested RegisterGpuModel of each proxy shape + instance adds).
    double proxiesMs = 0.0;
    // Inside RegisterGpuModel (any depth):
    double meshBuildMs = 0.0;     // BuildVertices/BuildOrigVertices/BuildIndices/BuildSections
    double meshCreateMs = 0.0;    // wgr_mesh_create (geometry into the pool)
    double sectionMs = 0.0;       // the per-section loop: classify + material resolve + texture upload
    double modelRegisterMs = 0.0; // wgr_model_register FFI
    // Inside the section loop, from TextureWgpu (real uploads only; the resident fast path
    // costs nothing here):
    // TextureBankWgpu::Load misses during the admission, at ANY point (the adapter's face
    // texture resolution AND the section loop's material stages): each is a TextureWgpu
    // allocation + Init(), and Init opens the file through GFileServer to parse the PAA
    // header/mip table. With the mip-chain READS now worker-prepared, this header open is
    // the file-server touch left on the admission path -- and whether the church-sized
    // `adapt` numbers are these opens or real geometry work is exactly what this bucket
    // answers.
    double texHeaderLoadMs = 0.0;
    uint32_t texHeaderLoads = 0;
    double textureReadMs = 0.0;     // ITextureSource::GetMipmapChain (file open + read + LZO)
    double textureCreateMs = 0.0;   // wgr_texture_create (staging copy + write_texture)
    double textureWindowWaitMs = 0.0; // subset of texture work: waiting for render ownership
    double textureEncodeMs = 0.0;   // subset of fallback: CPU-only BC3 mip/encoding work
    double textureFallbackMs = 0.0; // whole-file RGBA8 decode path (non-block formats)
    double alphaScanMs = 0.0;       // GetAlphaClass scans that touched blocks or the file

    uint32_t meshes = 0;          // pool meshes created
    uint32_t sections = 0;        // sections classified as GPU-owned
    uint32_t textureUploads = 0;  // EnsureUploaded calls that reached a real load
    uint32_t texturePrepared = 0; // ...of which the mip chain came from the prepared store
    uint32_t alphaScans = 0;
    uint32_t proxyModels = 0;     // proxy shapes registered
    uint32_t parkHits = 0;        // RegisterGpuModel served from the parked (evict-cache) set
    uint32_t parkStale = 0;       // parked entry found but its textures had moved: rebuilt

    void Reset()
    {
        const bool wasActive = active;
        *this = ObjectAdmitProfile{};
        active = wasActive;
    }
};

// The one instance. Read/written on the main thread only.
extern ObjectAdmitProfile GObjectAdmitProfile;

} // namespace Poseidon::render
