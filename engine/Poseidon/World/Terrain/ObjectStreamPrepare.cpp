// Asynchronous cold-model preparation for streamed object admission. See the header for the
// thread-safety boundary; this file is the mechanism only.

#include <Poseidon/World/Terrain/ObjectStreamPrepare.hpp>
#include <Poseidon/World/Terrain/ObjectStreamTextureNames.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>

#include <Poseidon/Asset/Formats/Material/EmatMaterialAdapter.hpp> // ReadEmatFileLoose (pure, worker-safe)
#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <Poseidon/Foundation/Threads/PoThread.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/Foundation/Threads/WatchDog.hpp>
#include <Poseidon/Foundation/Platform/FpEnvironment.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>      // ReadPAABlockChain (pure, worker-safe)
#include <Poseidon/Graphics/Textures/PreparedTextures.hpp> // the worker->main-thread mip-chain store
#include <Poseidon/Graphics/Textures/DdsImport.hpp>
#include <Poseidon/Dev/Diag/StreamingDiag.hpp>
#include <Poseidon/Asset/Formats/Enfusion/EnfusionMount.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/World/Model/ModelMemory.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/World/Model/ModelCache.hpp>
#include <Poseidon/World/Model/ModelDerivedCache.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace Poseidon
{

namespace
{
template<class Reserve, class Release>
bool PrepareNativeDds(const std::string& key, uint64_t generation, size_t& bytes,
                      bool prepareBc3, bool bc3Only, const Bc3EncodingObserver* observer,
                      Reserve&& reserve, Release&& release)
{
    try
    {
        const auto& mount = Asset::Formats::Enfusion::EnfusionMount::Instance();
        const auto mountGeneration = mount.Generation();
        auto source = std::make_unique<TextureSourceDDS>();
        PacLevelMem mips[16];
        const auto options = CaptureDdsPreparationOptions();
        const bool loaded = source->InitFromReader(key.c_str(), mips, 16,
            [&](const char* name, std::vector<uint8_t>& out) {
                if (mount.Generation() != mountGeneration) return false;
                const auto request = mount.MakeReadRequest(name);
                if (!request || mount.Generation() != mountGeneration) return false;
                std::string error;
                return request->Read(out, error);
            }, options);
        if (!loaded || mount.Generation() != mountGeneration) return false;
        const size_t encodeReservation = prepareBc3 ? source->CompositeBc3Reservation() : 0;
        if (encodeReservation && reserve(encodeReservation))
        {
            const auto began = std::chrono::steady_clock::now();
            bool encoded = false;
            bool reservationActive = true;
            auto finish = [&](bool published) {
                reservationActive = false; // never subtract twice if the release callback throws
                release(encodeReservation, published,
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count());
            };
            try
            {
                encoded = source->PrepareCompositeBc3(encodeReservation, observer);
                if (bc3Only && !encoded)
                {
                    source.reset();
                    finish(false);
                    return false;
                }
                // A mount/world may have changed during this comparatively long
                // pure CPU operation. reserve(0) validates the model job only.
                if (mount.Generation() != mountGeneration || !reserve(0))
                {
                    source.reset();
                    finish(false);
                    return false;
                }
                bytes = source->PreparedByteSize();
                const bool stored = render::PreparedTextureStore::Instance().PutDdsPrepared(
                    key, std::move(source), options, generation);
                finish(stored && encoded);
                return stored;
            }
            catch (...)
            {
                if (reservationActive)
                {
                    source.reset(); // destroy outstanding pixels before releasing their charge
                    finish(false);
                }
                throw;
            }
        }
        // Narrow mode never retains ordinary DDS when the sidecar was ineligible
        // or its optional scratch reservation was refused. Owner fallback remains.
        if (bc3Only || mount.Generation() != mountGeneration || !reserve(0)) return false;
        bytes = source->PreparedByteSize();
        return render::PreparedTextureStore::Instance().PutDdsPrepared(key, std::move(source), options, generation);
    }
    catch (...)
    {
        // Optional preparation may fail; keep the parsed model and let its
        // ordinary main-thread material path report/retry. Never lose a worker.
        return false;
    }
}

// Native model conversion keeps the source .emat path in its IR. Reading that material through
// the mounted archive is safe here: EnfusionMount returns an owned request, and the parser plus
// inheritance fold are pure. This deliberately does not touch the texture bank or GPU state.
bool ReadNativeEmat(const Asset::Formats::Enfusion::EnfusionMount& mount, const std::string& path,
                    Asset::Material::EmatMaterial& out, bool& complete)
{
    complete = false;
    const auto request = mount.MakeReadRequest(path);
    if (!request)
        return false;
    std::vector<uint8_t> bytes;
    std::string error;
    if (!request->Read(bytes, error) || bytes.empty())
        return false;
    namespace Mat = Asset::Material;
    out = Mat::ParseEmat(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (!out.valid())
        return false;
    complete = Mat::ResolveEmatInheritance(out,
                                [&mount](const std::string& parentPath, std::string& text)
                                {
                                    const auto parent = mount.MakeReadRequest(parentPath);
                                    if (!parent)
                                        return false;
                                    std::vector<uint8_t> parentBytes;
                                    std::string parentError;
                                    if (!parent->Read(parentBytes, parentError) || parentBytes.empty())
                                        return false;
                                    text.assign(reinterpret_cast<const char*>(parentBytes.data()), parentBytes.size());
                                    return true;
                                });
    return true;
}
}

// ------------------------------------------------------------------------------------------
// Levers. All read once; the admit path logs which arm it is running.
// ------------------------------------------------------------------------------------------

bool ObjectStreamPreparer::AsyncEnabled()
{
    static const bool on = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_ASYNC");
        // Default ON. The worker touches nothing but std:: and the header-only P3D readers
        // (see LoadLooseFile and the header note); the parts that are not thread-safe were
        // left on the main thread rather than made safe. WGR_OBJECT_STREAM_ASYNC=0 is the
        // A/B and restores the synchronous admit loop exactly.
        return !(v && std::strcmp(v, "0") == 0);
    }();
    return on;
}

uint32_t ObjectStreamPreparer::WorkerCount()
{
    static const uint32_t n = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_ASYNC_WORKERS");
        // Two, not "one per core": the parse is memory-bandwidth-bound and shares the machine
        // with the render thread, the enkiTS TaskPool and the audio mixer. Two keeps a
        // 30 m/s camera fed on Everon (a recentre needs a few hundred distinct models over
        // ~2 s, at tens of ms each) without contending for the cores the frame is using.
        const long parsed = v ? std::strtol(v, nullptr, 10) : 2;
        return static_cast<uint32_t>(std::clamp<long>(parsed, 1, 16));
    }();
    return n;
}

size_t ObjectStreamPreparer::QueueLimit()
{
    static const size_t n = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_ASYNC_QUEUE");
        const long parsed = v ? std::strtol(v, nullptr, 10) : 512;
        return static_cast<size_t>(std::clamp<long>(parsed, 1, 100000));
    }();
    return n;
}

size_t ObjectStreamPreparer::ReadyLimit()
{
    static const size_t n = []
    {
        const char* v = std::getenv("WGR_OBJECT_STREAM_ASYNC_READY");
        const long parsed = v ? std::strtol(v, nullptr, 10) : 128;
        return static_cast<size_t>(std::clamp<long>(parsed, 1, 100000));
    }();
    return n;
}

// ------------------------------------------------------------------------------------------
// State shared between the main thread and the workers.
//
// LOCKING. `mutex` guards everything below except `states[]`, which are atomics so that the
// main thread's per-placement Query in the admit loop is a load and nothing else. Every
// transition of a state is still made under the mutex, so a Query that reads Ready is
// guaranteed to find the IR in `models[]` on the following Take (which locks). The parse
// itself runs with the mutex released; the critical sections are pointer moves and counter
// bumps. Freeing an IR (megabytes) is always done with the mutex released, too.
// ------------------------------------------------------------------------------------------
uint64_t ObjectStreamPreparer::ReadyByteBudget()
{
    static const uint64_t bytes = [] {
        const char* value = std::getenv("WGR_OBJECT_STREAM_READY_MB");
        const long mb = value ? std::strtol(value, nullptr, 10) : 512;
        return static_cast<uint64_t>(std::clamp<long>(mb, 0, 16384)) * 1024 * 1024;
    }();
    return bytes;
}

struct ObjectStreamPreparer::Impl
{
    using State = ObjectStreamPreparer::State;
    using ModelPtr = std::shared_ptr<Model::Model>;

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::thread> workers;
    bool stop = false;
    Bc3EncodingObserver encodingObserver; // immutable copy, set before workers start
    // Bumped by Reset. A worker that finishes a parse started under an older generation
    // discards its result: the slot it would write belongs to a different world now.
    // Generation tokens distinguish workers from an earlier world after Reset;
    // all writes and cancellation checks also hold the queue mutex.
    std::atomic<uint64_t> generation{0};

    size_t count = 0;
    std::vector<std::string> paths;
    std::vector<uint8_t> prepareCompositeBc3; // written only by owner Request; workers capture before unlocking
    struct RadiusSlot
    {
        float radius = 0.0f;
        RadiusProvenance provenance = RadiusProvenance::None;
    };
    std::vector<RadiusSlot> radiusCertificates; // no retained IR, shape, or duplicate path strings

    // Caller holds mutex. RadiusSlot belongs to this generation's inventory;
    // Reset destroys/reinitializes the slots, while payload retirement leaves them alone.
    bool PublishRadius(uint32_t index, uint64_t gen, const std::string& identity,
                       float radius, RadiusProvenance provenance)
    {
        if (gen != generation.load(std::memory_order_relaxed) || index >= count || paths[index] != identity)
            return false;
        if (!std::isfinite(radius) || radius < 0.0f)
        {
            radiusCertificates[index] = {}; // a current failed measurement invalidates old evidence
            return false;
        }
        // An already installed/adapted bound outranks a later speculative parse.
        if (provenance == RadiusProvenance::ParsedODOL &&
            radiusCertificates[index].provenance == RadiusProvenance::AdaptedShape)
            return true;
        radiusCertificates[index] = {radius, provenance};
        return true;
    }
    std::vector<std::optional<BankReadRequest>> packedReads;
    std::unique_ptr<std::atomic<uint8_t>[]> states;
    std::vector<ModelPtr> models;
    // Protected by mutex. In-flight slots stay occupied until their worker exits.
    std::vector<bool> stale;
    std::deque<uint32_t> queue;
    // Stage 3: conversion jobs and their results. `convertedShapes[i]` is owned here
    // between the worker finishing and TakeConverted; Reset/DropStale delete stale
    // ones OUTSIDE the mutex (a fresh shape holds no vertex buffers, so its
    // destructor touches only refcounted textures and the locked fast allocator).
    struct PackedTextureRead
    {
        std::string key;
        BankReadRequest read;
    };
    static constexpr size_t PackedTextureLimit = 16 * 1024 * 1024;
    struct ConvertJob
    {
        std::vector<PackedTextureRead> textures;
        uint64_t textureGeneration = 0;
        uint32_t index = 0;
        bool radiusIdentityAccepted = false;
        uint64_t radiusGeneration = 0;
        std::string radiusIdentity; // copied owner token, never a borrowed model/shape
        uint64_t reservation = 0;
        ModelPtr model;
        std::shared_ptr<const Model::ShapeAdapter::AdapterBankTables> tables;
    };
    std::deque<ConvertJob> convertQueue;
    std::vector<LODShapeWithShadow*> convertedShapes;
    std::vector<ModelPtr> convertedModels;
    std::vector<uint64_t> conversionBytes;
    Stats stats;
    std::vector<uint64_t> payloadBytes;
    uint64_t ParseReservation() const
    {
        uint64_t estimate = 16ull * 1024 * 1024;
        if (!queue.empty() && queue.front() < packedReads.size() && packedReads[queue.front()])
            estimate += packedReads[queue.front()]->bytes; // owned source buffer overlaps parsing
        return stats.readyByteBudget ? std::min(estimate, stats.readyByteBudget) : estimate;
    }
    bool HasParseByteRoom() const
    {
        if (!stats.readyByteBudget) return true;
        uint64_t remaining = stats.readyByteBudget;
        for (const uint64_t charge : {stats.readyPayloadBytes, stats.parseReservedBytes,
                                     stats.conversionReservedBytes, stats.textureEncodeReservedBytes})
        {
            if (charge > remaining) return false;
            remaining -= charge;
        }
        return ParseReservation() <= remaining;
    }

    // Per model index: when Request() enqueued it, for the request->ready / request->taken
    // latency figures. Written under the mutex; a DropStale->re-Request simply restamps.
    std::vector<uint64_t> requestedAtUs;

    static uint64_t NowUs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    }

    State Get(uint32_t index) const
    {
        return static_cast<State>(states[index].load(std::memory_order_acquire));
    }
    void Set(uint32_t index, State s) { states[index].store(static_cast<uint8_t>(s), std::memory_order_release); }

    void WorkerMain(uint32_t workerIndex);
};

void ObjectStreamPreparer::Impl::WorkerMain(uint32_t workerIndex)
{
    // Below-normal priority: this thread must never win a core from the render thread.
    //
    // This was a #ifdef _WIN32 direct SetThreadPriority call, working around the Foundation
    // helper passing a thread ID where a HANDLE is needed. The helper is fixed (PoThread.cpp,
    // 2026-08-31), so the workaround is gone -- keeping it would have left the underlying bug
    // in place for every other caller while looking like this one was fine.
    Foundation::poSetMyPriority(-1);

    // Roadmap 8.1. This thread runs full ShapeAdapter geometry conversion whose output is
    // installed verbatim on the main thread, so it must compute in the main thread's
    // floating-point environment -- a new thread inherits the platform default MXCSR, not
    // ours, and with ENGINE_CONFIG.enablePIII the two differ in flush-to-zero. Workers are
    // created long after InitFPU, so the capture is always live by the time we get here.
    Foundation::ApplyMainFpEnvironment();

    (void)workerIndex;

    std::unique_lock<std::mutex> lock(mutex);
    for (;;)
    {
        // BACK-PRESSURE, NOT DROPPING. The queue is in distance order and the main thread
        // consumes Ready IRs in the same order, so if the workers run more than ReadyLimit
        // ahead, the right thing is to pause here until Take() makes room -- not to discard
        // the oldest Ready (the nearest, most-needed models) or the newest (parsed for
        // nothing). Take() and Reset() notify.
        // Count the park BEFORE waiting, and only when the ready bound is the thing doing
        // the blocking: queue work exists, no conversion to run instead, and the ready set
        // is at its limit. An empty queue is "nothing to do", not back-pressure, and must
        // not be counted as the bound biting.
        if (!stop && convertQueue.empty() && !queue.empty() && stats.ready >= ObjectStreamPreparer::ReadyLimit())
            ++stats.readyFullParks;
        if (!stop && convertQueue.empty() && !queue.empty() && !HasParseByteRoom())
            ++stats.readyByteParks;
        cv.wait(lock, [&] {
            return stop || !convertQueue.empty() ||
                   (!queue.empty() && stats.ready < ObjectStreamPreparer::ReadyLimit() && HasParseByteRoom());
        });
        if (stop)
            return;
        // Conversions first: each one unblocks an install the admit loop is waiting
        // on, and the set is bounded by what the main thread submitted.
        if (!convertQueue.empty())
        {
            ConvertJob job = std::move(convertQueue.front());
            convertQueue.pop_front();
            // Only active workers allocate source/decode scratch. Queued and ready
            // jobs keep their model charge, not dozens of nonexistent 64 MiB buffers.
            const uint64_t textureScratch = job.textures.empty() ? 0 : 4 * PackedTextureLimit + 8192;
            stats.conversionReservedBytes += textureScratch;
            stats.peakConversionReservedBytes = std::max(stats.peakConversionReservedBytes, stats.conversionReservedBytes);
            const uint64_t jobGen = generation.load(std::memory_order_relaxed);
            const std::string conversionPath = paths[job.index];
            lock.unlock();

            Poseidon::WatchDogItem watch = Poseidon::WatchScopeFor("objconvert", conversionPath);
            const auto convStart = std::chrono::steady_clock::now();
            LODShapeWithShadow* shape = nullptr;
            try
            {
                shape = Model::ShapeAdapter::convertToLODShape(*job.model, false, job.tables.get(),
                                                               /*finishTail=*/false);
            }
            catch (...)
            {
                shape = nullptr; // main thread will repeat synchronously via Failed-like fallback
            }
            const double convMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - convStart).count();
            if (shape)
            {
                // ODOL's deferred tail never changes this scalar; MLOD already
                // ran its ordinary all-LOD/frame bounding-sphere computation.
                // Publish before optional texture work, without another vertex scan.
                std::lock_guard<std::mutex> radiusLock(mutex);
                if (jobGen == generation.load(std::memory_order_relaxed) &&
                    job.index < count && !stale[job.index] && paths[job.index] == conversionPath &&
                    job.radiusIdentityAccepted && job.radiusGeneration == jobGen &&
                    (job.model->sourceFormat == "ODOL" || job.model->sourceFormat == "MLOD"))
                    PublishRadius(job.index, jobGen, job.radiusIdentity,
                                  shape->BoundingSphere(), RadiusProvenance::AdaptedShape);
            }
            uint64_t prepared = 0, preparedBytes = 0, skipped = 0, unreadable = 0;
            const auto textureStart = std::chrono::steady_clock::now();
            if (shape)
            {
                auto& store = render::PreparedTextureStore::Instance();
                for (const auto& texture : job.textures)
                {
                    {
                        std::lock_guard<std::mutex> pendingLock(mutex);
                        if (stop || jobGen != generation.load(std::memory_order_relaxed) || stale[job.index]) break;
                    }
                    if (!store.ShouldPrepare(texture.key)) { ++skipped; continue; }
                    try
                    {
                        PAABlockChain chain;
                        {
                            std::vector<char> source;
                            if (!texture.read.Read(source) || !ReadPAABlockChainBuffer(source.data(), source.size(),
                                    chain, nullptr, PackedTextureLimit)) { ++unreadable; continue; }
                        }
                        const size_t bytes = chain.blocks.size();
                        if (store.Put(texture.key, std::move(chain), job.textureGeneration))
                        { ++prepared; preparedBytes += bytes; }
                        else ++skipped;
                    }
                    catch (...) { ++unreadable; } // ordinary owner-thread decode remains the fallback
                }
            }
            const double textureMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - textureStart).count();

            lock.lock();
            stats.conversionReservedBytes -= textureScratch;
            if (jobGen != generation.load(std::memory_order_relaxed) || stale[job.index])
            {
                if (jobGen == generation.load(std::memory_order_relaxed))
                    Set(job.index, State::Unknown);
                ++stats.dropped;
                stats.conversionReservedBytes -= job.reservation;
                cv.notify_all();
                lock.unlock();
                delete shape;
                job.model.reset();
                job.tables.reset();
                lock.lock();
                continue;
            }
            stats.texPrepared += prepared;
            stats.texPreparedBytes += preparedBytes;
            stats.texSkipped += skipped;
            stats.texUnreadable += unreadable;
            stats.texMs += textureMs;
            stats.convertMs += convMs;
            stats.convertMaxMs = std::max(stats.convertMaxMs, convMs);
            if (shape && job.index < count)
            {
                conversionBytes[job.index] = job.reservation;
                convertedShapes[job.index] = shape;
                shape = nullptr; // Ownership is now in the published slot.
                convertedModels[job.index] = std::move(job.model);
                Set(job.index, State::Converted);
                ++stats.converted;
            }
            else
            {
                stats.conversionReservedBytes -= job.reservation;
                cv.notify_all();
                // Conversion failed: sticky Failed, the admit loop falls back to the
                // synchronous path for this model exactly like a parse failure.
                if (job.index < count)
                    Set(job.index, State::Failed);
                ++stats.failed;
            }
            // Last references can release bank resources or large model storage.
            // Their destructors must never run under the preparer's queue mutex.
            lock.unlock();
            delete shape;
            job.model.reset();
            job.tables.reset();
            lock.lock();
            continue;
        }
        const uint32_t index = queue.front();
        const uint64_t reservation = ParseReservation();
        queue.pop_front();
        --stats.queued;
        if (index >= count)
            continue;
        const uint64_t gen = generation.load(std::memory_order_relaxed);
        const std::string path = paths[index];
        const bool prepareBc3 = prepareCompositeBc3[index] != 0;
        const auto packedRead = packedReads[index];
        Set(index, State::Parsing);
        stats.parseReservedBytes += reservation;
        lock.unlock();

        // ---- unlocked: the parse ------------------------------------------------------
        // Watched for the whole unlocked span -- parse AND texture prepare -- because a
        // worker that wedges here is the quietest failure this file can produce. The main
        // thread does not wait on it: the model simply stays Parsing forever, the admit
        // loop keeps falling back, and the only visible symptom is that streaming quietly
        // stops making progress. Nothing is logged today, and there is no timer on the
        // path because every existing timer here is recorded on completion.
        //
        // Reachability, stated because it is not obvious and a scope that never runs reads
        // as "nothing stalled here": these workers exist only for the modern-OPRW object
        // table (Landscape::EnsureModernObjectPreparer), so a classic CWA world never
        // enters this loop at all. Verified 2026-09-01 -- a full Eden mission load with a
        // 1 ms watchdog deadline produced no `objstream` report, because the preparer was
        // never constructed. The scope covers the A2/A3-lineage worlds, where this is the
        // async path and the stall would otherwise be silent.
        Poseidon::WatchDogItem watch = Poseidon::WatchScopeFor("objstream", path);
        const auto start = std::chrono::steady_clock::now();
        ModelPtr model;
        std::string error;
        bool opened = false;
        bool nativeArchiveModel = false;
        try
        {
            model = ModelCache::LoadLooseFile(path, &error, &opened);
            if (!opened && packedRead)
            {
                std::vector<char> bytes;
                opened = true;
                if (packedRead->Read(bytes))
                    model = ModelCache::LoadOwnedBytes(bytes.data(), bytes.size(), path, error);
                else
                    error = "owned archive read failed; retry through VFS";
            }
            // RFG-103: a native `.xob` is not a loose file -- it lives in a Reforger pak. The
            // world loader's external loader (ModelCache::SetExternalLoader) reads the pak
            // (one ifstream per read) and runs the pure converter, so it is safe here; without
            // this every native model was "not loose" and fell back to the main thread
            // (measured: prep=0/503, sync=503, 226 ms parse hitches per house).
            if (!opened && !model)
            {
                const size_t n = path.size();
                const bool xob = n > 4 && (path[n - 4] == '.') && (path[n - 3] == 'x' || path[n - 3] == 'X') &&
                                 (path[n - 2] == 'o' || path[n - 2] == 'O') && (path[n - 1] == 'b' || path[n - 1] == 'B');
                if (xob)
                    if (ModelCache::ExternalLoader external = ModelCache::GetExternalLoader())
                    {
                        std::string why;
                        model = external(path, why);
                        nativeArchiveModel = model != nullptr;
                        opened = true; // it was ours to load; a null model is a parse failure, not "not loose"
                        if (!model)
                            error = why.empty() ? "external loader declined" : why;
                    }
            }
        }
        catch (const std::exception& e)
        {
            // LoadLooseFile catches around the readers; this is for the allocation of the
            // Model itself or anything else outside that try. A worker must never unwind
            // out of its loop.
            model.reset();
            opened = true;
            error = e.what();
        }
        catch (...)
        {
            model.reset();
            opened = true;
            error = "unknown exception";
        }
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

        // ---- unlocked: the texture prepare (stage 2) ------------------------------------
        // The IR names its face textures (Model::Material::texturePath / embeddedStages);
        // read the block-compressed loose PAAs among them NOW, on this thread, so the main
        // thread's EnsureUploaded inside RegisterGpuModel takes ready blocks instead of a
        // file open + LZO per texture. Everything used here is pure (ReadPAABlockChain:
        // ifstream + LZO; PreparedTextureStore: its own mutex): no texture bank, no file
        // server, no GPU. Textures the store refuses (already uploaded, already stored,
        // over budget) are skipped; textures that are not loose DXT PAAs simply fail to
        // read here and take the old path on the main thread -- counted, never fatal.
        // Materials resolved at registration time (.rvmat/.emat maps) are not in the IR and
        // are NOT prepared; the admit split's texture timers measure what that costs.
        uint64_t texPrepared = 0, texPreparedBytes = 0, texSkipped = 0, texUnreadable = 0;
        double texMs = 0.0;
        if (model && render::PreparedTextureStore::Enabled())
        {
            const auto texStart = std::chrono::steady_clock::now();
            try
            {
                render::PreparedTextureStore& store = render::PreparedTextureStore::Instance();
                const uint64_t textureGeneration = store.Generation();
                const bool bc3Only = nativeArchiveModel && render::PreparedTextureStore::NativeDdsBc3Only();
                const auto& mount = Asset::Formats::Enfusion::EnfusionMount::Instance();
                const auto names = CollectObjectStreamTextureNames(*model, nativeArchiveModel,
                    render::PreparedTextureStore::NativeDdsEnabled(),
                    [&](const std::string& path) {
                        // Loose files may be case-sensitive. Native archive entries
                        // have a canonical identity, without any filesystem fallback.
                        return nativeArchiveModel ? mount.CanonicalPath(path).value_or(std::string{}) : path;
                    },
                    [&](const std::string& path, Asset::Material::EmatMaterial& material, bool& complete) {
                        if (nativeArchiveModel) return ReadNativeEmat(mount, path, material, complete);
                        // Preserve the loose reader's route and partial-inheritance
                        // fallback, while exposing whether a retry can still add maps.
                        std::string text;
                        if (!Asset::Material::ReadEmatTextLoose(path, text)) return false;
                        material = Asset::Material::ParseEmat(text);
                        if (!material.valid()) return false;
                        complete = Asset::Material::ResolveEmatInheritance(material,
                            [](const std::string& parent, std::string& parentText) {
                                return Asset::Material::ReadEmatTextLoose(parent, parentText);
                            });
                        return true;
                    });
                for (const std::string& key : names)
                {
                    // Filter before store lookups or source reads. Raw material
                    // stages and nested tint wrappers stay on their ordinary path.
                    if (bc3Only && (!prepareBc3 || !key.starts_with("enfa|")))
                        continue;
                    {
                        // Poll between textures, never hold the queue mutex during I/O.
                        // Check generation first: Reset may have replaced/shrunk stale.
                        std::lock_guard<std::mutex> pendingLock(mutex);
                        if (stop || gen != generation.load(std::memory_order_relaxed) || stale[index])
                            break;
                    }
                    if (!store.ShouldPrepare(key))
                    {
                        ++texSkipped;
                        continue;
                    }
                    if (nativeArchiveModel && render::PreparedTextureStore::NativeDdsEnabled() && key.size() >= 4 &&
                        (key.ends_with(".edds") || key.ends_with(".dds")))
                    {
                        size_t bytes = 0;
                        if (PrepareNativeDds(key, textureGeneration, bytes, prepareBc3, bc3Only,
                            encodingObserver.afterTopMip ? &encodingObserver : nullptr,
                            [&](size_t needed) {
                                std::lock_guard<std::mutex> budgetLock(mutex);
                                if (stop || gen != generation.load(std::memory_order_relaxed) ||
                                    index >= count || stale[index]) return false;
                                if (stats.readyByteBudget)
                                {
                                    uint64_t remaining = stats.readyByteBudget;
                                    for (const uint64_t charge : {stats.readyPayloadBytes, stats.parseReservedBytes,
                                                                 stats.conversionReservedBytes, stats.textureEncodeReservedBytes})
                                        remaining -= std::min(remaining, charge);
                                    if (needed > remaining) { ++stats.workerBc3BudgetSkipped; return false; }
                                }
                                stats.textureEncodeReservedBytes += needed;
                                stats.peakTextureEncodeReservedBytes = std::max(stats.peakTextureEncodeReservedBytes,
                                                                              stats.textureEncodeReservedBytes);
                                return true;
                            },
                            [&](size_t reserved, bool prepared, double ms) {
                                std::lock_guard<std::mutex> budgetLock(mutex);
                                stats.textureEncodeReservedBytes -= reserved;
                                stats.workerBc3Prepared += prepared;
                                stats.workerBc3Ms += ms;
                                cv.notify_all();
                            }))
                        {
                            ++texPrepared;
                            texPreparedBytes += bytes;
                        }
                        else
                        {
                            ++texUnreadable;
                        }
                        continue;
                    }
                    PAABlockChain chain;
                    if (!ReadPAABlockChain(key, chain))
                    {
                        // Not a DXT chain. If it is still a READABLE loose PAA (the Reforger
                        // corpus's PacARGB8888 *_mask.paa population -- 390 whole-file decodes
                        // costing ~1.1 s of admission time on the Everon traverse), decode it
                        // HERE with the pure buffer decoder and store the RGBA image, so the
                        // main thread's fallback path uploads instead of decoding.
                        std::ifstream file(key, std::ios::binary);
                        if (!file)
                        {
                            ++texUnreadable; // not loose at all: main thread's problem
                            continue;
                        }
                        std::vector<char> bytesIn((std::istreambuf_iterator<char>(file)),
                                                  std::istreambuf_iterator<char>());
                        const bool isPaa = key.size() >= 4 && key.compare(key.size() - 4, 4, ".paa") == 0;
                        DecodedImage image = DecodePAABuffer(bytesIn.data(), bytesIn.size(), isPaa);
                        if (!image.valid())
                        {
                            ++texUnreadable; // malformed: main thread repeats and reports
                            continue;
                        }
                        const size_t imageBytes = image.rgba.size();
                        if (store.PutDecoded(key, std::move(image), textureGeneration))
                        {
                            ++texPrepared;
                            texPreparedBytes += imageBytes;
                        }
                        else
                        {
                            ++texSkipped;
                        }
                        continue;
                    }
                    const size_t bytes = chain.blocks.size();
                    if (store.Put(key, std::move(chain), textureGeneration))
                    {
                        ++texPrepared;
                        texPreparedBytes += bytes;
                    }
                    else
                    {
                        ++texSkipped; // raced with another worker or the budget: fine
                    }
                }
            }
            catch (...)
            {
                // Texture-name collection and loose decodes can allocate too.
                // Keep this optional stage from unwinding out of the worker loop.
                ++texUnreadable;
            }
            texMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - texStart).count();
        }
        // ---------------------------------------------------------------------------------

        const uint64_t payload = model ? Model::ResidentPayloadBytes(*model) : 0;
        lock.lock();
        stats.parseReservedBytes -= reservation;
        cv.notify_all(); // failed/stale completion also releases its reservation
        stats.texPrepared += texPrepared;
        stats.texPreparedBytes += texPreparedBytes;
        stats.texSkipped += texSkipped;
        stats.texUnreadable += texUnreadable;
        stats.texMs += texMs;
        if (gen != generation.load(std::memory_order_relaxed) || stale[index])
        {
            // Do not republish work discarded by a world or camera-window change.
            if (gen == generation.load(std::memory_order_relaxed))
                Set(index, State::Unknown);
            ++stats.dropped;
            lock.unlock();
            model.reset();
            lock.lock();
            continue;
        }
        stats.workerMs += ms;
        stats.workerMaxMs = std::max(stats.workerMaxMs, ms);
        const bool parseFailed = !model && opened;
        if (model)
        {
            // Canonical ODOL parses preserve the serialized engine sphere exactly
            // through adaptation/tail. IR MLOD/Xob bbox spheres are not equivalent.
            if (!nativeArchiveModel && model->sourceFormat == "ODOL")
                PublishRadius(index, gen, model->sourcePath, model->boundingSphere.radius,
                              RadiusProvenance::ParsedODOL);
            payloadBytes[index] = payload;
            stats.readyPayloadBytes += payload;
            stats.peakReadyPayloadBytes = std::max(stats.peakReadyPayloadBytes, stats.readyPayloadBytes);
            if (stats.readyByteBudget && payload > stats.readyByteBudget) ++stats.oversizedPayloads;
            models[index] = std::move(model);
            Set(index, State::Ready);
            ++stats.prepared;
            ++stats.ready;
            if (index < requestedAtUs.size() && requestedAtUs[index] != 0)
            {
                const uint64_t latency = NowUs() - requestedAtUs[index];
                stats.readyLatencyUsTotal += latency;
                stats.readyLatencyUsMax = std::max(stats.readyLatencyUsMax, latency);
            }
        }
        else if (!opened)
        {
            Set(index, State::NotLoose);
            ++stats.notLoose;
        }
        else
        {
            Set(index, State::Failed);
            ++stats.failed;
        }
        if (parseFailed)
        {
            lock.unlock();
            // The main thread will repeat this load through ShapeBank::New and hit the same
            // failure; say once, here, what it was, so the row is not a mystery.
            LOG_WARN(World,
                     "Object stream prepare: {} did not parse on the worker ({}); main thread will retry "
                     "synchronously",
                     path, error);
            lock.lock();
        }
    }
}

// ------------------------------------------------------------------------------------------

ObjectStreamPreparer::ObjectStreamPreparer(uint64_t readyByteBudget, const Bc3EncodingObserver* observer)
    : _impl(std::make_unique<Impl>())
{
    _impl->stats.readyByteBudget = readyByteBudget;
    if (observer) _impl->encodingObserver = *observer;
}

ObjectStreamPreparer::~ObjectStreamPreparer()
{
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        _impl->stop = true;
    }
    _impl->cv.notify_all();
    for (std::thread& worker : _impl->workers)
        if (worker.joinable())
            worker.join();
    _impl->workers.clear();
    // SIM-807. `convertedShapes` holds RAW LODShapeWithShadow pointers that this
    // class owns between the worker finishing and TakeConverted. Reset and
    // DropStale both delete them; destruction did not, so every shape converted
    // but not yet installed was leaked when the preparer went away. The vector's
    // own destructor cannot help -- it destroys the pointers, not the objects --
    // and the symptom is invisible short of a leak checker, because a shape that
    // was never installed is one nothing else references either.
    // Safe here and only here: the workers are joined above, so no one can be
    // writing the slot.
    for (LODShapeWithShadow*& shape : _impl->convertedShapes)
    {
        delete shape;
        shape = nullptr;
    }
    _impl->convertedShapes.clear();
}

bool ObjectStreamPreparer::Running() const
{
    return !_impl->workers.empty();
}

size_t ObjectStreamPreparer::ModelCount() const
{
    // Written only by Reset, on the main thread, which is the only caller of this too.
    return _impl->count;
}

void ObjectStreamPreparer::Reset(const std::string* paths, size_t modelCount)
{
    std::vector<std::optional<BankReadRequest>> packedReads(modelCount);
    const char* packedFlag = std::getenv("WGR_OBJECT_STREAM_PBO");
    size_t packedCount = 0;
    if (packedFlag && packedFlag[0] == '1' && GUseFileBanks && !ModelDerivedCache::Instance().Enabled())
    {
        for (size_t i = 0; i < modelCount; ++i)
        {
            const auto& path = paths[i];
            if (path.size() < 4 || path.substr(path.size() - 4) != ".p3d") continue;
            if (auto* bank = QIFStreamB::AutoBank(path.c_str()))
            {
                packedReads[i] = bank->CaptureReadRequest(path.c_str() + bank->GetPrefix().GetLength());
                packedCount += packedReads[i].has_value();
            }
        }
    }
    if (packedCount) LOG_INFO(World, "Object stream prepare: {} owned PBO read requests", packedCount);
    std::vector<Impl::ModelPtr> freeLater;
    std::vector<LODShapeWithShadow*> freeShapes;
    std::deque<Impl::ConvertJob> freeConversions;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        ++_impl->generation;
        _impl->stats.dropped += _impl->stats.ready;
        _impl->stats.queued = 0;
        _impl->stats.ready = 0;
        _impl->stats.readyPayloadBytes = 0;
        // Do not clear parseReservedBytes: old-generation workers still own their reservation.
        _impl->payloadBytes.assign(modelCount, 0);
        _impl->queue.clear();
        // Jobs own model IR and bank-table references, not just queue indices.
        // Their last-reference destructors must not run under the queue mutex.
        freeConversions.swap(_impl->convertQueue);
        for (const auto& job : freeConversions)
            _impl->stats.conversionReservedBytes -= job.reservation;
        for (const uint64_t bytes : _impl->conversionBytes)
            _impl->stats.conversionReservedBytes -= bytes;
        // Active jobs retain their local reservation across generation changes.
        _impl->conversionBytes.assign(modelCount, 0);
        for (Impl::ModelPtr& m : _impl->models)
            if (m)
                freeLater.push_back(std::move(m));
        for (LODShapeWithShadow*& sh : _impl->convertedShapes)
            if (sh)
            {
                freeShapes.push_back(sh);
                sh = nullptr;
            }
        for (Impl::ModelPtr& m : _impl->convertedModels)
            if (m)
                freeLater.push_back(std::move(m));
        _impl->convertedShapes.assign(modelCount, nullptr);
        _impl->convertedModels.assign(modelCount, nullptr);
        _impl->models.assign(modelCount, nullptr);
        _impl->stale.assign(modelCount, false);
        _impl->requestedAtUs.assign(modelCount, 0);
        _impl->paths.assign(paths, paths + modelCount);
        std::vector<uint8_t>(modelCount, 0).swap(_impl->prepareCompositeBc3);
        _impl->stats.bc3PolicyBytes = _impl->prepareCompositeBc3.capacity();
        // Old workers keep their encoding reservation until publication/destruction.
        // Fresh capacity tracks this inventory, rather than retaining a previous
        // world's larger certificate allocation after Reset.
        std::vector<Impl::RadiusSlot>(modelCount).swap(_impl->radiusCertificates);
        _impl->stats.radiusCertificateBytes =
            _impl->radiusCertificates.capacity() * sizeof(Impl::RadiusSlot);
        _impl->packedReads = std::move(packedReads);
        _impl->count = modelCount;
        _impl->states = modelCount > 0 ? std::make_unique<std::atomic<uint8_t>[]>(modelCount) : nullptr;
        for (size_t i = 0; i < modelCount; ++i)
            _impl->states[i].store(static_cast<uint8_t>(State::Unknown), std::memory_order_relaxed);
        if (modelCount > 0 && _impl->workers.empty() && AsyncEnabled())
        {
            const uint32_t n = WorkerCount();
            _impl->workers.reserve(n);
            for (uint32_t i = 0; i < n; ++i)
                _impl->workers.emplace_back([impl = _impl.get(), i] { impl->WorkerMain(i); });
            _impl->stats.workers = n;
        }
    }
    // Workers parked on the ready-limit predicate must re-evaluate: the ready set is empty now.
    _impl->cv.notify_all();
    freeLater.clear();
    for (LODShapeWithShadow* sh : freeShapes)
        delete sh;
}

bool ObjectStreamPreparer::Request(uint32_t index)
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    if (index >= _impl->count || _impl->workers.empty())
        return false;
    _impl->stale[index] = false;
    const State current = _impl->Get(index);
    if (current != State::Unknown)
    {
        // The dedup hit. Split so the reported rate is the rate of asks that actually
        // saved a parse -- see the comment on Stats::coalesced.
        if (current == State::NotLoose || current == State::Failed)
            ++_impl->stats.coalescedStuck;
        else
            ++_impl->stats.coalesced;
        return false;
    }
    if (_impl->queue.size() >= QueueLimit())
    {
        ++_impl->stats.rejectedFull;
        return false;
    }
    _impl->Set(index, State::Queued);
    // Only the captured owner may read the mutable developer checkbox.
    // Other callers retain ordinary preparation without this optional sidecar.
    _impl->prepareCompositeBc3[index] = render::PreparedTextureStore::NativeDdsEnabled() &&
                                      Foundation::IsMainThread() &&
                                      Poseidon::Dev::GResidencyLevers().compressComposites;
    _impl->queue.push_back(index);
    if (index < _impl->requestedAtUs.size())
        _impl->requestedAtUs[index] = Impl::NowUs();
    ++_impl->stats.queued;
    ++_impl->stats.requested;
    _impl->cv.notify_one();
    return true;
}

ObjectStreamPreparer::State ObjectStreamPreparer::Query(uint32_t index) const
{
    if (index >= _impl->count)
        return State::Unknown;
    return _impl->Get(index);
}

ObjectStreamPreparer::RadiusCertificate ObjectStreamPreparer::QueryRadius(uint32_t index) const
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    RadiusCertificate out;
    out.generation = _impl->generation.load(std::memory_order_relaxed);
    if (index >= _impl->count) return out;
    out.modelIdentity = _impl->paths[index];
    const auto& slot = _impl->radiusCertificates[index];
    out.provenance = slot.provenance;
    out.radius = slot.radius;
    if (slot.provenance != RadiusProvenance::None) out.state = RadiusState::Certified;
    return out;
}

bool ObjectStreamPreparer::RecordAdaptedRadius(uint32_t index, uint64_t generation,
                                             const std::string& modelIdentity, float radius)
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    return _impl->PublishRadius(index, generation, modelIdentity, radius, RadiusProvenance::AdaptedShape);
}

std::shared_ptr<Model::Model> ObjectStreamPreparer::Take(uint32_t index)
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    if (index >= _impl->count || _impl->Get(index) != State::Ready)
        return nullptr;
    std::shared_ptr<Model::Model> model = std::move(_impl->models[index]);
    _impl->models[index].reset();
    _impl->Set(index, State::Unknown);
    if (index < _impl->requestedAtUs.size() && _impl->requestedAtUs[index] != 0)
    {
        const uint64_t latency = Impl::NowUs() - _impl->requestedAtUs[index];
        _impl->stats.takeLatencyUsTotal += latency;
        _impl->stats.takeLatencyUsMax = std::max(_impl->stats.takeLatencyUsMax, latency);
        _impl->requestedAtUs[index] = 0;
    }
    ++_impl->stats.taken;
    --_impl->stats.ready;
    _impl->stats.readyPayloadBytes -= _impl->payloadBytes[index];
    _impl->payloadBytes[index] = 0;
    // Room in the ready set: wake a worker parked on the ready-limit predicate.
    _impl->cv.notify_one();
    return model;
}

size_t ObjectStreamPreparer::DropStale(const uint32_t* epochs, size_t count, uint32_t currentEpoch)
{
    std::vector<Impl::ModelPtr> freeLater;
    std::vector<LODShapeWithShadow*> freeShapes;
    std::deque<Impl::ConvertJob> freeConversions;
    size_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        const size_t n = std::min(count, _impl->count);
        bool queueTouched = false;
        for (size_t i = 0; i < n; ++i)
        {
            _impl->stale[i] = epochs[i] != currentEpoch;
            if (epochs[i] == currentEpoch)
                continue;
            const State state = _impl->Get(static_cast<uint32_t>(i));
            if (state == State::Ready)
            {
                freeLater.push_back(std::move(_impl->models[i]));
                _impl->models[i].reset();
                _impl->Set(static_cast<uint32_t>(i), State::Unknown);
                --_impl->stats.ready;
                _impl->stats.readyPayloadBytes -= _impl->payloadBytes[i];
                _impl->payloadBytes[i] = 0;
                ++dropped;
            }
            else if (state == State::Converted)
            {
                _impl->stats.conversionReservedBytes -= _impl->conversionBytes[i];
                _impl->conversionBytes[i] = 0;
                if (_impl->convertedShapes[i])
                {
                    freeShapes.push_back(_impl->convertedShapes[i]);
                    _impl->convertedShapes[i] = nullptr;
                }
                freeLater.push_back(std::move(_impl->convertedModels[i]));
                _impl->convertedModels[i].reset();
                _impl->Set(static_cast<uint32_t>(i), State::Unknown);
                ++dropped;
            }
            else if (state == State::Queued)
            {
                _impl->Set(static_cast<uint32_t>(i), State::Unknown);
                queueTouched = true;
                ++dropped;
            }
        }
        if (queueTouched)
        {
            // Rebuild rather than erase-in-place: the queue holds indices, and an index whose
            // state is no longer Queued is exactly one we un-queued above.
            std::deque<uint32_t> kept;
            for (uint32_t index : _impl->queue)
                if (index < _impl->count && _impl->Get(index) == State::Queued)
                    kept.push_back(index);
            _impl->queue.swap(kept);
            _impl->stats.queued = _impl->queue.size();
        }
        // Queued conversions own IR and bank tables too. Release them outside the
        // mutex; active conversions observe stale on completion instead.
        std::deque<Impl::ConvertJob> keptConversions;
        for (auto& job : _impl->convertQueue)
        {
            if (_impl->stale[job.index])
            {
                _impl->Set(job.index, State::Unknown);
                _impl->stats.conversionReservedBytes -= job.reservation;
                freeConversions.push_back(std::move(job));
                ++dropped;
            }
            else
                keptConversions.push_back(std::move(job));
        }
        _impl->convertQueue.swap(keptConversions);
        _impl->stats.dropped += dropped;
    }
    if (dropped > 0)
        _impl->cv.notify_all(); // room in the ready set, or a shorter queue: let parked workers re-check
    freeLater.clear();
    for (LODShapeWithShadow* sh : freeShapes)
        delete sh;
    return dropped;
}

bool ObjectStreamPreparer::SubmitConvert(uint32_t index, std::shared_ptr<Model::Model> model,
                                         std::shared_ptr<const Model::ShapeAdapter::AdapterBankTables> tables,
                                         const RadiusCertificate* inventoryToken)
{
    if (!model || !tables)
        return false;
    // Owner-thread snapshot only. Workers receive immutable archive spans and names,
    // never Texture pointers, bank handles, ParamFile state or the file-server MRU.
    std::vector<Impl::PackedTextureRead> textures;
    const uint64_t textureGeneration = render::PreparedTextureStore::Instance().Generation();
    const char* flag = std::getenv("WGR_OBJECT_STREAM_PBO_TEXTURES");
    if (flag && flag[0] == '1' && GUseFileBanks && render::PreparedTextureStore::Enabled())
    {
        auto note = [&](const std::string& raw)
        {
            if (textures.size() >= 64) return; // bounded per-model metadata; remaining inputs use VFS
            const std::string key = render::PreparedTextureStore::Key(raw);
            if (key.size() < 4 || (key.substr(key.size()-4) != ".paa" && key.substr(key.size()-4) != ".pac")) return;
            if (!render::PreparedTextureStore::Instance().ShouldPrepare(key)) return;
            if (std::any_of(textures.begin(), textures.end(), [&](const auto& t) { return t.key == key; })) return;
            if (auto* bank = QIFStreamB::AutoBank(key.c_str()))
                if (auto read = bank->CaptureReadRequest(key.c_str() + bank->GetPrefix().GetLength());
                    read && read->bytes <= Impl::PackedTextureLimit)
                    textures.push_back({key, std::move(*read)});
        };
        for (const auto& level : model->lodLevels)
            for (const auto& material : level.mesh.materials)
            {
                note(material.texturePath);
                for (const auto& stage : material.embeddedStages) note(stage.texturePath);
            }
        for (const auto& level : tables->textures)
            for (const auto& texture : level)
                if (texture) note(static_cast<const char*>(texture->GetName()));
    }
    std::lock_guard<std::mutex> lock(_impl->mutex);
    if (index >= _impl->count || _impl->workers.empty())
        return false;
    if (_impl->Get(index) != State::Unknown)
        return false;
    _impl->stale[index] = false;
    _impl->Set(index, State::Converting);
    Impl::ConvertJob job;
    job.index = index;
    job.radiusGeneration = _impl->generation.load(std::memory_order_relaxed);
    // Admission lowercases sourcePath for ShapeBank after taking the IR. Only
    // an exact owner inventory token authorizes this otherwise ambiguous mapping.
    // A rejected explicit token never falls back to inference from sourcePath.
    job.radiusIdentityAccepted = inventoryToken
        ? inventoryToken->generation == job.radiusGeneration && inventoryToken->modelIdentity == _impl->paths[index]
        : model->sourcePath == _impl->paths[index];
    if (job.radiusIdentityAccepted) job.radiusIdentity = _impl->paths[index];
    // Retain the IR charge plus an equal workspace/output allowance. This is a
    // scheduling estimate, not a measurement of Shape allocations. Useful
    // conversions always progress; their debt parks subsequent speculative parses.
    job.reservation = 2 * Model::ResidentPayloadBytes(*model);
    // Scratch is charged when a worker starts, and released before publishing.
    // Retained chains are charged separately by PreparedTextureStore.
    job.textures = std::move(textures);
    job.textureGeneration = textureGeneration;
    _impl->stats.conversionReservedBytes += job.reservation;
    _impl->stats.peakConversionReservedBytes = std::max(
        _impl->stats.peakConversionReservedBytes, _impl->stats.conversionReservedBytes);
    job.model = std::move(model);
    job.tables = std::move(tables);
    _impl->convertQueue.push_back(std::move(job));
    _impl->cv.notify_one();
    return true;
}

ObjectStreamPreparer::ConvertedShape ObjectStreamPreparer::TakeConverted(uint32_t index)
{
    ConvertedShape out;
    std::lock_guard<std::mutex> lock(_impl->mutex);
    if (index >= _impl->count || _impl->Get(index) != State::Converted)
        return out;
    _impl->stats.conversionReservedBytes -= _impl->conversionBytes[index];
    _impl->conversionBytes[index] = 0;
    _impl->cv.notify_all();
    out.shape = _impl->convertedShapes[index];
    _impl->convertedShapes[index] = nullptr;
    out.model = std::move(_impl->convertedModels[index]);
    _impl->convertedModels[index].reset();
    _impl->Set(index, State::Unknown);
    return out;
}

ObjectStreamPreparer::Stats ObjectStreamPreparer::SnapshotStats() const
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    return _impl->stats;
}

} // namespace Poseidon
