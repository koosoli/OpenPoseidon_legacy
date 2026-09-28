#include <Poseidon/Core/Application.hpp>
#include <Poseidon/IO/FileServer.hpp>
#include <string.h>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Memory/FastAlloc.hpp>
#include <Poseidon/Foundation/Memory/MemFreeReq.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/Foundation/Threads/WatchDog.hpp>
#include <Poseidon/Foundation/platform.hpp>
#ifdef _WIN32
#include <process.h>
#include <Poseidon/IO/Streams/FileOverlapped.hpp>
#endif
#include <Poseidon/IO/FileServerMT.hpp>
#include <Poseidon/IO/Streams/PatnikArchiveReason.hpp>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Dev/Diag/StreamingDiag.hpp> // file-cache hit/miss + miss-latency counters
#include <chrono>

#if _ENABLE_CHEATS
#define ENABLE_OVERLAPPED_IO 0
#endif

bool GEnableCaching = true;

namespace Poseidon
{
Ref<FileServer> GFileServer;

FileCache::FileCache(int size, int files) : _maxSize(size), _maxFiles(files), _size(0)
{
    RegisterMemoryFreeOnDemand(this);
}

FileCache::~FileCache() = default;

int FileCache::Find(const char* name)
{
    // Roadmap 4.1. The header has said MAIN THREAD ONLY since 2026-08-30; this is the
    // line that makes it checkable. Find is the funnel -- Use, Store and the eviction
    // path all reach the MRU through it -- so a worker that reads a PBO-packed file
    // through GFileServer reports here, before it can race MoveToFront's array shuffle.
    // A report, not a lock: locking this cache would hide a call that should not be on
    // a worker at all (the pure loose-file readers exist for exactly that case).
    POSEIDON_MAIN_THREAD_ONLY("FileCache (GFileServer's unlocked MRU) -- workers must use "
                              "ModelCache::LoadLooseFile / ReadPAABlockChain / ReadEmatFileLoose");
    for (int i = 0; i < _cache.Size(); i++)
    {
        const FileInCache* file = _cache[i];
        if (!strcmp(file->_name, name))
        {
            return i;
        }
    }
    return -1;
}

void FileCache::MoveToFront(int index)
{
    SRef<FileInCache> temp = _cache[index];
    _cache.Delete(index);
    _cache.Insert(0, temp);
}

void FileCache::DeleteLast()
{
    if (_cache.Size() <= 0)
    {
        return;
    }
    FileInCache* file = _cache[_cache.Size() - 1];
    PoseidonAssert(file->_data.tellg() == 0);
    _size -= file->_data.rest();
    PoseidonAssert(_size >= 0);
    _cache.Delete(_cache.Size() - 1);
}

//! average expected file size is 64 KB
const int FileCacheItemMemSize = 64 * 1024;

size_t FileCache::FreeOneItem()
{
    if (_cache.Size() <= 0)
    {
        return 0;
    }
    size_t sizeBefore = _size;
    DeleteLast();
    size_t sizeAfter = _size;
    return sizeBefore - sizeAfter;
}

float FileCache::Priority()
{
    // estimated CPU time to read typical file
    // due to background loading it is much less that actual time neeed
    const float itemTime = 300000 * 0.1;
    // estimated time per byte
    return itemTime / FileCacheItemMemSize;
}

int FileCache::Load(const char* name)
{
    // search the cache
    int index = Find(name);
    if (index >= 0)
    {
        if (PatnikArchiveReason::Enabled() && PatnikArchiveReason::Matches(name) &&
            PatnikArchiveReason::ReserveRow())
        {
            const IFileBuffer* buffer = _cache[index]->_data.GetBuffer();
            LOG_INFO(Core, "patnik_archive_reason stage=fileCache reason=hit bound={}",
                buffer && static_cast<bool>(buffer->GetArchiveSourceBinding()));
        }
        if (index != 0)
        {
            MoveToFront(index);
        }
        Poseidon::Dev::GFileServerCounters().hits.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    // not cached yet -- this branch is the real file read (open + decompress into the
    // MRU), i.e. the "file-read latency" the roadmap's Phase 3 list asks for. Queue
    // depth has no counter because there is no queue: the server is synchronous
    // (ENABLE_OVERLAPPED_IO compiles out above), so depth is 0 by construction and the
    // latency of a miss lands on whoever asked, on the main thread, immediately.
    const auto missBegan = std::chrono::steady_clock::now();
    // Watched because of the sentence above: the miss is a synchronous read that lands on
    // whoever asked, on the main thread. There is no queue to inspect and no worker to
    // blame, so a read that never returns -- a bank on a disconnected network path, a
    // handle the OS never completes -- is invisible except as a stopped frame counter. The
    // scope is inside the miss branch, so a cache HIT (the overwhelming majority of calls)
    // does not touch the ring at all.
    Poseidon::WatchDogItem watch = Poseidon::WatchScopeFor("io.read", name ? name : "");
    FileInCache* newEntry = new FileInCache(name);
    if (PatnikArchiveReason::Enabled() && PatnikArchiveReason::Matches(name) &&
        PatnikArchiveReason::ReserveRow())
    {
        const IFileBuffer* buffer = newEntry->_data.GetBuffer();
        LOG_INFO(Core, "patnik_archive_reason stage=fileCache reason=miss failed={} bound={}",
            newEntry->_data.fail(), buffer && static_cast<bool>(buffer->GetArchiveSourceBinding()));
    }
    if (newEntry->_data.fail())
    {
        delete newEntry;
        // RptF is a no-op in release builds — surface the missing file through the
        // normal log so callers can see WHICH file the subsequent Cache failure is for.
        LOG_WARN(Core, "FileCache: file '{}' not found", name);
        return -1;
    }
    _cache.Insert(0, newEntry);
    PoseidonAssert(newEntry->_data.tellg() == 0);
    // maintain size statistics
    _size += newEntry->_data.rest();
    Maintain();
    {
        Poseidon::Dev::FileServerCounters& c = Poseidon::Dev::GFileServerCounters();
        const uint64_t us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - missBegan)
                .count());
        c.misses.fetch_add(1, std::memory_order_relaxed);
        c.missUs.fetch_add(us, std::memory_order_relaxed);
        uint64_t prev = c.missUsMax.load(std::memory_order_relaxed);
        while (us > prev && !c.missUsMax.compare_exchange_weak(prev, us, std::memory_order_relaxed))
        {
        }
    }
    return 0;
}

bool FileCache::IsLoaded(const char* name)
{
    int index = Find(name);
    return (index >= 0);
}

void FileCache::Open(QIFStream& stream, const char* name)
{
    if (!name || !*name)
    {
        // Opening an empty/null name is a benign "no resource" request — e.g. a
        // profile with no custom squad logo / voice sample set. The stream is left
        // in its failed state for the caller to handle; this is not an asset error,
        // so it must not be logged at ERROR (which would abort the app under --strict
        // on a perfectly normal profile open).
        LOG_DEBUG(Core, "FileCache::Open: empty name — no resource requested (no-op)");
        return;
    }

    if (GEnableCaching)
    {
        // note: multiple threads may open file same time
        // make sure file is in cache
        int index = Load(name);
        if (index >= 0)
        {
            // it must be the first file
            PoseidonAssert(index == 0);
            PoseidonAssert(!strcmp(name, _cache[index]->_name));
            // share cached data
            stream = _cache[index]->_data;
        }
        else
        {
            // Load already logged a "file not found" warning with the same name;
            // include it here too so the error line carries enough context on its own.
            LOG_ERROR(Core, "FileCache: Cache failure loading '{}' (file not found or stream fail)", name);
        }
    }
    else
    {
        QIFStreamB temp;
        temp.AutoOpen(name);
        stream = temp;
    }
}

void FileCache::Store(QIFStreamB& stream, const char* name)
{
    // search the cache
    int index = Find(name);
    if (index >= 0)
    {
        if (index != 0)
        {
            MoveToFront(index);
        }
        return;
    }
    // not cached yet
    FileInCache* newEntry = new FileInCache(stream, name);
    _cache.Insert(0, newEntry);
    PoseidonAssert(newEntry->_data.tellg() == 0);
    _size += newEntry->_data.rest();
    Maintain();
}
void FileCache::Maintain()
{
    while ((_size > _maxSize || _cache.Size() > _maxFiles) && _cache.Size() > 1)
    {
        DeleteLast();
    }
}

void FileCache::FlushBank(QFBank* bank)
{
    // release all files from this bank

    for (int i = 0; i < _cache.Size(); i++)
    {
        const FileInCache* file = _cache[i];
        const QIFStreamB& stream = file->_data;
        if (!stream.IsFromBank(bank))
        {
            continue;
        }
        // Match DeleteLast's accounting before the owning cache reference dies.
        // Otherwise a selective flush leaves phantom bytes that cause Maintain
        // to evict unrelated entries on the next successful open/store.
        PoseidonAssert(stream.tellg() == 0);
        _size -= stream.rest();
        PoseidonAssert(_size >= 0);
        _cache.Delete(i);
        i--;
    }
}

DEFINE_FAST_ALLOCATOR(FileInCache)

bool FileRequest::operator==(const FileRequest& with) const
{
    return (!strcmpi(_filename, with._filename) && _from == with._from && _to == with._to);
}
bool FileRequest::Contains(const FileRequest& with) const
{
    return (!strcmpi(_filename, with._filename) && _from <= with._from && _to >= with._to);
}

int FileServerST::RequestPresentAndDone(const FileRequest& req) const
{
    for (int i = 0; i < _done.Size(); i++)
    {
        if (_done[i].Contains(req))
        {
            return i;
        }
    }
    return -1;
}
int FileServerST::RequestPresentAndNotDone(const FileRequest& req) const
{
    for (int i = 0; i < _queue.Size(); i++)
    {
        if (_queue[i].Contains(req))
        {
            return i;
        }
    }
    return -1;
}

bool FileServerST::RequestPresent(const FileRequest& req) const
{
    if (RequestPresentAndNotDone(req) >= 0)
    {
        return true;
    }
    if (RequestPresentAndDone(req) >= 0)
    {
        return true;
    }
    return false;
}

void FileServerST::Request(const char* name, float time, int from, int to)
{
#if ENABLE_OVERLAPPED_IO
    if (_cache.IsLoaded(name))
    {
        Update();
        return;
    }
    // check if given request is already queued
    DWORD itime = GlobalTickCount() + toInt(1000 * time);
    FileRequest request(name, itime, from, to);
    // check
    if (!RequestPresent(request))
    {
        _queue.HeapInsert(request);
    }
    Update();
#endif
}

void FileServerST::CancelRequest(const char* name, int from, int to) {}

void FileServerST::Update()
{
    // check if current operation is terminated
    // if it is, process next request
    while (_queue.Size() > 0)
    {
        // get heap head
        // check if operation already started
        FileRequest& head = _queue[0];
        if (!head._filebuf)
        {
            QFBank* bank = QIFStreamB::AutoBank(head._filename);
            if (bank)
            {
                const char* name = head._filename;
                name += bank->GetPrefix().GetLength();
                head._filebuf = bank->ReadOverlapped(name);
            }
            else
            {
                QIFStreamB temp;
                temp.AutoOpen(head._filename);
                head._filebuf = head._in.GetBuffer();
            }
        }
        // check if operation terminated
        if (!head._filebuf->IsReady())
        {
            break;
        }
        FileRequest req;
        _queue.HeapRemoveFirst(req);
        req._in.OpenBuffer(head._filebuf);
        _done.Add(req);
    }
}

void FileServerST::Open(QIFStream& stream, const char* name)
{
#if ENABLE_OVERLAPPED_IO
    Update();
    // try to locate it in cache
    if (_cache.IsLoaded(name))
    {
        _cache.Open(stream, name);
        return;
    }
    // try to locate it in _done queue
    FileRequest req(name, 0);
    int done = RequestPresentAndDone(req);
    if (done >= 0)
    {
        _cache.Store(_done[done]._in, name);
        stream = _done[done]._in;
        return;
    }
    int queue = RequestPresentAndNotDone(req);
    if (queue >= 0)
    {
        // wait until it is loaded
        while (RequestPresentAndNotDone(req) >= 0)
        {
            Update();
        }
        int done = RequestPresentAndDone(req);
        if (done >= 0)
        {
            _cache.Store(_done[done]._in, name);
            stream = _done[done]._in;
            _done.Delete(done);
            return;
        }
        else
        {
            LOG_DEBUG(Core, "Request failed: {}", name);
            _cache.Open(stream, name);
            return;
        }
    }
#endif
    _cache.Open(stream, name);
}

void FileServerST::FlushBank(QFBank* bank)
{
    // scan all files and release those comming this bank
    _cache.FlushBank(bank);
}

} // namespace Poseidon
