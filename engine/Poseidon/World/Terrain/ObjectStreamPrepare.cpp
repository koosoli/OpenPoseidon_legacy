// Asynchronous cold-model preparation for streamed object admission. See the header for the
// thread-safety boundary; this file is the mechanism only.

#include <Poseidon/World/Terrain/ObjectStreamPrepare.hpp>

#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <Poseidon/Foundation/Threads/PoThread.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>      // ReadPAABlockChain (pure, worker-safe)
#include <Poseidon/Graphics/Textures/PreparedTextures.hpp> // the worker->main-thread mip-chain store
#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/World/Model/ModelCache.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace Poseidon
{

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
struct ObjectStreamPreparer::Impl
{
    using State = ObjectStreamPreparer::State;
    using ModelPtr = std::shared_ptr<Model::Model>;

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::thread> workers;
    bool stop = false;
    // Bumped by Reset. A worker that finishes a parse started under an older generation
    // discards its result: the slot it would write belongs to a different world now.
    // Atomic because the texture-prepare loop polls it OUTSIDE the mutex (to stop reading
    // files for a world that is gone); every WRITE still happens under the mutex.
    std::atomic<uint64_t> generation{0};

    size_t count = 0;
    std::vector<std::string> paths;
    std::unique_ptr<std::atomic<uint8_t>[]> states;
    std::vector<ModelPtr> models;
    std::deque<uint32_t> queue;
    Stats stats;
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
    // Below-normal priority: this thread must never win a core from the render thread. The
    // Foundation helper's Win32 arm passes a thread ID where a HANDLE is needed and so is a
    // no-op there; call the platform API directly on Windows and the helper elsewhere.
#ifdef _WIN32
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
    Foundation::poSetMyPriority(-1);
#endif
    (void)workerIndex;

    std::unique_lock<std::mutex> lock(mutex);
    for (;;)
    {
        // BACK-PRESSURE, NOT DROPPING. The queue is in distance order and the main thread
        // consumes Ready IRs in the same order, so if the workers run more than ReadyLimit
        // ahead, the right thing is to pause here until Take() makes room -- not to discard
        // the oldest Ready (the nearest, most-needed models) or the newest (parsed for
        // nothing). Take() and Reset() notify.
        cv.wait(lock, [&] { return stop || (!queue.empty() && stats.ready < ObjectStreamPreparer::ReadyLimit()); });
        if (stop)
            return;
        const uint32_t index = queue.front();
        queue.pop_front();
        --stats.queued;
        if (index >= count)
            continue;
        const uint64_t gen = generation.load(std::memory_order_relaxed);
        const std::string path = paths[index];
        Set(index, State::Parsing);
        lock.unlock();

        // ---- unlocked: the parse ------------------------------------------------------
        const auto start = std::chrono::steady_clock::now();
        ModelPtr model;
        std::string error;
        bool opened = false;
        try
        {
            model = ModelCache::LoadLooseFile(path, &error, &opened);
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
            render::PreparedTextureStore& store = render::PreparedTextureStore::Instance();
            std::vector<std::string> names;
            auto noteName = [&](const std::string& raw)
            {
                if (raw.empty())
                    return;
                // Same extension gate as the main thread's block path (BcFormatFor): only
                // .paa/.pac can carry a DXT chain this reader understands.
                const size_t dot = raw.find_last_of('.');
                if (dot == std::string::npos)
                    return;
                std::string ext = raw.substr(dot);
                for (char& c : ext)
                    if (c >= 'A' && c <= 'Z')
                        c = static_cast<char>(c - 'A' + 'a');
                if (ext != ".paa" && ext != ".pac")
                    return;
                std::string key = render::PreparedTextureStore::Key(raw);
                if (std::find(names.begin(), names.end(), key) == names.end())
                    names.push_back(std::move(key));
            };
            for (const Model::LODLevel& level : model->lodLevels)
            {
                for (const Model::Material& material : level.mesh.materials)
                {
                    noteName(material.texturePath);
                    for (const Model::MaterialStage& stage : material.embeddedStages)
                        noteName(stage.texturePath);
                }
            }
            for (const std::string& key : names)
            {
                if (gen != generation.load(std::memory_order_relaxed)) // Reset ran: dead world, stop reading
                    break;
                if (!store.ShouldPrepare(key))
                {
                    ++texSkipped;
                    continue;
                }
                PAABlockChain chain;
                if (!ReadPAABlockChain(key, chain))
                {
                    ++texUnreadable; // not loose, not DXT, or malformed: main thread's problem
                    continue;
                }
                const size_t bytes = chain.blocks.size();
                if (store.Put(key, std::move(chain)))
                {
                    ++texPrepared;
                    texPreparedBytes += bytes;
                }
                else
                {
                    ++texSkipped; // raced with another worker or the budget: fine
                }
            }
            texMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - texStart).count();
        }
        // ---------------------------------------------------------------------------------

        lock.lock();
        stats.texPrepared += texPrepared;
        stats.texPreparedBytes += texPreparedBytes;
        stats.texSkipped += texSkipped;
        stats.texUnreadable += texUnreadable;
        stats.texMs += texMs;
        if (gen != generation.load(std::memory_order_relaxed))
        {
            // Reset ran while we were parsing: the slot is another world's now.
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

ObjectStreamPreparer::ObjectStreamPreparer() : _impl(std::make_unique<Impl>()) {}

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
    std::vector<Impl::ModelPtr> freeLater;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        ++_impl->generation;
        _impl->stats.dropped += _impl->stats.ready;
        _impl->stats.queued = 0;
        _impl->stats.ready = 0;
        _impl->queue.clear();
        for (Impl::ModelPtr& m : _impl->models)
            if (m)
                freeLater.push_back(std::move(m));
        _impl->models.assign(modelCount, nullptr);
        _impl->requestedAtUs.assign(modelCount, 0);
        _impl->paths.assign(paths, paths + modelCount);
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
}

bool ObjectStreamPreparer::Request(uint32_t index)
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    if (index >= _impl->count || _impl->workers.empty())
        return false;
    if (_impl->Get(index) != State::Unknown)
        return false;
    if (_impl->queue.size() >= QueueLimit())
    {
        ++_impl->stats.rejectedFull;
        return false;
    }
    _impl->Set(index, State::Queued);
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
    // Room in the ready set: wake a worker parked on the ready-limit predicate.
    _impl->cv.notify_one();
    return model;
}

size_t ObjectStreamPreparer::DropStale(const uint32_t* epochs, size_t count, uint32_t currentEpoch)
{
    std::vector<Impl::ModelPtr> freeLater;
    size_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        const size_t n = std::min(count, _impl->count);
        bool queueTouched = false;
        for (size_t i = 0; i < n; ++i)
        {
            if (epochs[i] == currentEpoch)
                continue;
            const State state = _impl->Get(static_cast<uint32_t>(i));
            if (state == State::Ready)
            {
                freeLater.push_back(std::move(_impl->models[i]));
                _impl->models[i].reset();
                _impl->Set(static_cast<uint32_t>(i), State::Unknown);
                --_impl->stats.ready;
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
        _impl->stats.dropped += dropped;
    }
    if (dropped > 0)
        _impl->cv.notify_all(); // room in the ready set, or a shorter queue: let parked workers re-check
    freeLater.clear();
    return dropped;
}

ObjectStreamPreparer::Stats ObjectStreamPreparer::SnapshotStats() const
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    return _impl->stats;
}

} // namespace Poseidon
