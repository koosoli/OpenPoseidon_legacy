#pragma once

// Asynchronous preparation of COLD MODELS for the streamed-object admission path
// (Landscape::UpdateModernObjectResidency, LandSave.cpp).
//
// THE PROBLEM THIS SOLVES. Admission of a streamed object whose model is not resident is
// synchronous: file read + ODOL/MLOD decode + IR compile + ShapeAdapter conversion + vertex
// buffer creation + GPU registration, all on the main thread, ~90+ ms per cold model measured
// on Reforger Everon. The admit ceiling (WGR_OBJECT_STREAM_MAX_MS) can spread a batch over
// frames but cannot bound a frame below `ceiling + one object`, because one object per update
// must always be attempted or the fill never terminates. A single cold model is therefore the
// residual stall, and only taking its cost off the main thread removes it.
//
// WHAT MOVES OFF THE MAIN THREAD, AND WHY ONLY THAT. The cold path was read end to end for
// thread safety (the analysis is in the session handover; the short form is here so the
// boundary is not moved by accident):
//
//   file read + P3D parse + Model::compile   ->  ModelCache::LoadLooseFile. Pure std:: plus the
//                                                header-only readers; no statics, no globals.
//                                                SAFE on a worker. THIS IS WHAT WORKERS DO.
//   ShapeAdapter::convertToLODShape           ->  GlobLoadTexture (global texture bank),
//                                                GTexMaterialBank.New, NewProxyObject /
//                                                NewObject (recursive ShapeBank::New for
//                                                proxies), Texture::LoadHeaders, RStringB
//                                                property tables. MAIN THREAD ONLY.
//   ShapeBank cache insert + OptimizeOneShape ->  AssetCache (unlocked) + GEngine->
//                                                CreateVertexBuffer. MAIN THREAD ONLY.
//   ObjectCreate / AddObject / RegisterGpuModel  MAIN THREAD ONLY.
//   GFileServer / QFBank (PBO-packed files)  ->  FileServerST keeps an unlocked MRU array
//                                                (FileCache::Store / MoveToFront), so a worker
//                                                may NOT read through it. A model that is not
//                                                a loose file is reported NotLoose and admitted
//                                                on the main thread exactly as before.
//
// So a worker produces the compiled IR (std::shared_ptr<Model::Model>) and nothing else; the
// main thread's admission installs it through ShapeBank::NewFromModel, which runs the adapter
// and the vertex-buffer build. That is a partial win by construction: the parse is what moves,
// the adapter and GPU registration stay. The per-model split timer on the admit path
// (`Object stream cold model:` log row) says how the ~90 ms actually divides, so if the parse
// turns out to be the small part this is a measured null result rather than an assumed win.
//
// CONTRACT WITH THE FRAME. Nothing here ever blocks the main thread on a worker: Request
// enqueues, Query reads an atomic, Take moves a pointer under a mutex that workers hold only
// for pointer-sized critical sections (the parse itself runs unlocked). An object whose model
// is not ready is SKIPPED this frame, not waited for. Reset drops everything by bumping a
// generation number and does not join; only the destructor joins.
//
// LEVERS. WGR_OBJECT_STREAM_ASYNC=0 restores the synchronous path exactly (workers are never
// started, the admit loop is the old loop). WGR_OBJECT_STREAM_ASYNC_WORKERS (default 2),
// WGR_OBJECT_STREAM_ASYNC_QUEUE (default 512 outstanding requests),
// WGR_OBJECT_STREAM_ASYNC_READY (default 128 parsed-but-not-yet-installed models held; the
// IR of a large building is megabytes, and a world names ~1,000 models, so this bound is what
// keeps a fast camera from parsing the whole world into RAM ahead of itself. Workers PAUSE
// when it is full rather than dropping anything: the queue and the admit loop share one
// distance order, so the oldest Ready IR is the next one wanted. DropStale, run once per
// recentre, is what evicts IRs the new window no longer wants, so a full ready set can never
// be a full set of unwanted models.)

#include <cstdint>
#include <memory>
#include <string>

namespace Poseidon
{
namespace Model
{
struct Model;
}

class ObjectStreamPreparer
{
  public:
    enum class State : uint8_t
    {
        Unknown = 0, // never requested (or dropped / taken since): the caller may Request it
        Queued,      // in the queue, no worker has picked it up yet
        Parsing,     // a worker is inside LoadLooseFile for it
        Ready,       // IR available; Take() hands it over
        NotLoose,    // sticky: not a loose file, the main thread must load it via the file server
        Failed,      // sticky: loose file exists but did not parse; main thread repeats the old path
    };

    struct Stats
    {
        uint64_t requested = 0; // Request() calls that actually enqueued
        uint64_t rejectedFull = 0; // Request() refused because the queue was at its limit
        uint64_t prepared = 0;  // parses that produced an IR
        uint64_t taken = 0;     // IRs handed to the main thread (installed)
        uint64_t notLoose = 0;
        uint64_t failed = 0;
        uint64_t dropped = 0;    // requests / Ready IRs discarded by DropStale, Reset, or a stale generation
        double workerMs = 0.0;   // wall time workers spent inside LoadLooseFile, cumulative
        double workerMaxMs = 0.0; // the single slowest parse
        // Request() -> Ready and Request() -> Take() wall time: cumulative microseconds and
        // the single slowest, over `prepared` / `taken` respectively. This preparer is the
        // one asset path that is asynchronous today, so this is the one place "time from
        // request to residency" (roadmap Phase 3) is a real queue measurement rather than
        // the duration of a synchronous call. Includes queue wait by design -- that is the
        // number an admit loop experiences.
        uint64_t readyLatencyUsTotal = 0;
        uint64_t readyLatencyUsMax = 0;
        uint64_t takeLatencyUsTotal = 0;
        uint64_t takeLatencyUsMax = 0;
        size_t queued = 0;       // current queue depth
        size_t ready = 0;        // current Ready-and-not-taken count
        uint32_t workers = 0;
        // Texture prepare (stage 2, WGR_OBJECT_STREAM_TEXTURE_PREPARE): after a successful
        // parse the same worker reads the model's block-compressed loose PAAs into the
        // PreparedTextureStore so the main thread's upload skips the file read + LZO. All
        // still on the worker, all outside any lock.
        uint64_t texPrepared = 0;   // chains stored
        uint64_t texPreparedBytes = 0;
        uint64_t texSkipped = 0;    // ShouldPrepare said no (already uploaded/stored/full)
        uint64_t texUnreadable = 0; // named by the IR but not a readable loose DXT PAA
        double texMs = 0.0;         // wall time in ReadPAABlockChain + Put
    };

    // WGR_OBJECT_STREAM_ASYNC. Read once.
    static bool AsyncEnabled();
    static uint32_t WorkerCount();
    static size_t QueueLimit();
    static size_t ReadyLimit();

    ObjectStreamPreparer();
    ~ObjectStreamPreparer(); // joins the workers; the only blocking call in this class

    ObjectStreamPreparer(const ObjectStreamPreparer&) = delete;
    ObjectStreamPreparer& operator=(const ObjectStreamPreparer&) = delete;

    // Main thread. Adopt a world's model table: `paths[i]` is the P3D path of model index i,
    // exactly the string ShapeBank::New would be given. Drops every outstanding request and
    // result of the previous world (in-flight parses finish on the worker and are discarded
    // there). Starts the workers on first use. modelCount == 0 is a plain flush.
    void Reset(const std::string* paths, size_t modelCount);
    bool Running() const;
    // Main thread. Size of the model table adopted by the last Reset (0 after a flush).
    size_t ModelCount() const;

    // Main thread. Ask for model `index` to be parsed. Returns true if it was enqueued now;
    // false if it is already tracked (any state but Unknown) or the queue is full.
    bool Request(uint32_t index);
    // Main thread. Lock-free.
    State Query(uint32_t index) const;
    // Main thread. If Ready, returns the IR and returns the slot to Unknown; else null.
    std::shared_ptr<Model::Model> Take(uint32_t index);
    // Main thread. For every model whose `epochs[index] != currentEpoch`: a Queued request is
    // removed from the queue and a Ready IR is freed, both back to Unknown (Parsing ones finish
    // and are dropped at the next call). Called once per recentre with the desired set's
    // stamps, so the bounded ready set never fills with models nobody will Take. Returns how
    // many were dropped. Sticky NotLoose / Failed are left alone.
    size_t DropStale(const uint32_t* epochs, size_t count, uint32_t currentEpoch);
    // Main thread. Snapshot of the counters (takes the mutex briefly).
    Stats SnapshotStats() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace Poseidon
