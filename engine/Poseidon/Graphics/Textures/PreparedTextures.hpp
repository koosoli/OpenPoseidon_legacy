#pragma once

// The hand-off between the object-stream WORKER threads and the texture UPLOAD on the main
// thread (texture streaming, stage 2 of the streamed-admission work; stage 1 was the model
// parse in ObjectStreamPreparer).
//
// WHAT CROSSES THE BOUNDARY. A worker that has parsed a cold model's IR knows the model's
// texture PATHS (Model::Material::texturePath, embeddedStages[].texturePath). For each that is a
// loose block-compressed PAA it runs ReadPAABlockChain (pure: ifstream + LZO, no engine
// globals) and Puts the raw mip chain here, keyed by the same lower-cased name the texture bank
// will be asked for. When TextureWgpu::EnsureUploaded later runs on the main thread for that
// texture -- inside RegisterGpuModel, on the admission of the object -- it Takes the chain
// instead of opening the file, validates it against the header the bank already parsed
// (format, level count, per-level dimensions and byte size), and uploads. Any mismatch, or no
// entry, is exactly the old path. Nothing here ever creates a Texture, touches the bank, or
// uploads: workers produce bytes; the main thread owns every GPU and bank object.
//
// WHAT DOES NOT CROSS. Textures a material resolves at registration time (.rvmat/.emat
// stages: normal/specular/mask maps) are not in the IR; they are read the old way and are
// counted as misses so the coverage is measured, not assumed.
//
// BOUNDS. Bytes are capped (WGR_OBJECT_STREAM_TEXTURE_PREPARE_MB, default 512): a worker that
// finds the store full skips, it never blocks and never evicts something the main thread is
// about to want (entries are consumed in the same distance order they were produced). Entries
// that will never be consumed -- a texture that was already GPU-resident when the worker looked,
// or one belonging to a model the camera left behind -- are the leak this design has to close:
// (1) the main thread marks every texture that reaches a real upload (or a failed one) as
// `Uploaded`, and workers skip those names; (2) an entry older than
// WGR_OBJECT_STREAM_TEXTURE_PREPARE_TTL_S (default 30) is dropped at the next Put; (3) the whole
// store is cleared on world change (Landscape's preparer Reset).
//
// THREADING. One mutex around a name->entry map plus an insertion-ordered list, and a name set
// for the uploaded marks. Every critical section is a map operation or a pointer move; the
// file read and LZO happen outside it. Take moves the vector out, so the main thread never
// copies a mip chain under the lock.

#include <Poseidon/Graphics/Textures/PAADecoder.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace Poseidon::render
{

class PreparedTextureStore
{
  public:
    struct Stats
    {
        uint64_t puts = 0;          // chains stored
        uint64_t putBytes = 0;      // ...their bytes
        uint64_t takes = 0;         // chains handed to the main thread (any validity)
        uint64_t takeBytes = 0;
        uint64_t misses = 0;        // Take found nothing (texture not prepared, or already consumed)
        uint64_t rejectedFull = 0;  // Put refused: byte budget
        uint64_t rejectedDup = 0;   // Put refused: already present
        uint64_t expired = 0;       // entries dropped by the TTL sweep
        uint64_t skippedUploaded = 0; // ShouldPrepare said no: main thread already uploaded it
        uint64_t uploadedMarks = 0; // names currently marked Uploaded
        // The RGBA8 side (non-DXT PAAs -- the Reforger mask population): whole-file
        // decodes done on a worker so the main thread's fallback path uploads instead
        // of decoding. Counted separately because the byte shapes differ wildly.
        uint64_t decodedPuts = 0;
        uint64_t decodedTakes = 0;
        size_t entries = 0;         // current
        size_t bytes = 0;           // current
    };

    static PreparedTextureStore& Instance();

    // Read once from the environment. Enabled() is the master switch
    // (WGR_OBJECT_STREAM_TEXTURE_PREPARE, default 1); the others are the bounds above.
    static bool Enabled();
    static size_t ByteBudget();
    static double TtlSeconds();

    // Lower-case, backslash-normalised key -- the texture bank's spelling of a model texture
    // (ShapeAdapter lower-cases Model::Material::texturePath before GlobLoadTexture; the bank
    // stores the name verbatim). Both sides key through this so they cannot drift.
    static std::string Key(const char* name);
    static std::string Key(const std::string& name) { return Key(name.c_str()); }

    // Worker or main thread. False if the name is already Uploaded, already stored, or the
    // store is over budget: the worker should not spend a file read on it. Advisory (a
    // concurrent Put may still be refused as a duplicate).
    bool ShouldPrepare(const std::string& key);
    // Worker. Stores a chain; false if refused (full / duplicate / disabled).
    bool Put(const std::string& key, PAABlockChain&& chain);
    // Main thread. Moves the entry out if present.
    bool Take(const std::string& key, PAABlockChain& out);

    // The RGBA8 twin pair, for textures whose format has no DXT chain (the fallback
    // decode population): a worker decodes the whole file with the pure DecodePAABuffer
    // and stores the image; the main thread's fallback path takes it and only uploads.
    // Same budget, TTL, dedup and Uploaded-mark rules as the chain entries.
    bool PutDecoded(const std::string& key, DecodedImage&& image);
    bool TakeDecoded(const std::string& key, DecodedImage& out);

    // Main thread. A texture reached a real upload attempt (success or failure): its bytes are
    // no longer wanted from a worker; also drops any pending entry for it.
    void MarkUploaded(const std::string& key);
    // Main thread. The GPU copy was evicted; a later re-upload may want a prepared chain again.
    void MarkEvicted(const std::string& key);

    // Main thread. World change.
    void Clear();
    Stats SnapshotStats() const;

  private:
    struct Impl;
    Impl* _impl;
    PreparedTextureStore();
    ~PreparedTextureStore();
    PreparedTextureStore(const PreparedTextureStore&) = delete;
    PreparedTextureStore& operator=(const PreparedTextureStore&) = delete;
};

} // namespace Poseidon::render
