#include <Poseidon/Foundation/platform.hpp>
#include <Poseidon/Foundation/Memory/FastAlloc.hpp>
#include <Poseidon/Foundation/Containers/SmallArray.hpp>
#include <Poseidon/Foundation/Containers/CacheList.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>

namespace Poseidon::Foundation
{
FastAlloc::FastAlloc(size_t n, const char* name, int alignOffset)
    : _name(name), _alignOffset(alignOffset),
      esize(n + sizeof(void*) < sizeof(Link) ? sizeof(Link) : n + sizeof(void*)), // Chunk pointer header
      allocated(1)
{
    //  if structure is smaller than 32 it is very likely
    //  it does not have any special alignment requirements
    //  to conserve memory we will align it to 4B only
    const int alignMem = esize > 32 ? 16 : 4;
    esize = (esize + alignMem - 1) & ~(alignMem - 1);

    int chSize = Chunk::size - _alignOffset;
    _nElemPerChunk = static_cast<int>(chSize / esize);
}

FastAlloc::~FastAlloc()
{
    // note: automatic destruction of links takes place

    // check if all chunks may be freed now
    if (--allocated == 0)
    {
        FreeChunks();
        PoseidonAssert(!chunksBusy.First());
        PoseidonAssert(!chunksFree.First());
    }
    else
    {
        LOG_DEBUG(Core, "FastAlloc {} not free when destructed.", Name());
    }
}

FastAlloc::Chunk* FastAlloc::NewChunk()
{
    Chunk* chunk = new Chunk;
    return chunk;
}
void FastAlloc::DeleteChunk(Chunk* chunk)
{
    delete chunk;
}

void FastAlloc::FreeChunks()
{
    Chunk* n;
    while ((n = chunksFree.First()) != nullptr)
    {
        // all chunks should be empty
        PoseidonAssert(n->allocated == 0);
        chunksFree.Delete(n);
        DeleteChunk(n);
    }

    // there should be no busy chunks now
    PoseidonAssert(chunksBusy.First() == nullptr);
    while ((n = chunksBusy.First()) != nullptr)
    {
        chunksBusy.Delete(n);
        DeleteChunk(n);
    }
}

void* FastAlloc::AllocCounted(size_t n)
{
    allocated++;

    PoseidonAssert(n <= esize);
    if (chunksFree.First() == nullptr)
    {
        Grow();
    }
    Chunk* ch = chunksFree.First();

    if (!ch)
    {
        return nullptr;
    }

    Link* p = ch->head;

    ch->head = p->next;
    ch->allocated++;

    if (!ch->head)
    {
        // chunk is now full — move it to the busy list
        PoseidonAssert(ch->allocated == _nElemPerChunk);
        chunksFree.Delete(ch);
        chunksBusy.Insert(ch);
    }

    *(Chunk**)p = ch; // store the chunk back-pointer in the block header
    return p;
}

void FastAlloc::FreeCounted(void* pAlloc, bool fast)
{
    if (!pAlloc)
    {
        return;
    }

    Chunk* ch = *(Chunk**)pAlloc;

    if (ch->null1)
    {
        Fail("FastAlloc chunk header corrupted.");
        RptF("Chunk %x, null %x", ch, ch->null1);
        return;
    }
    Link* p = static_cast<Link*>(pAlloc);

    // add to head of free links in this chunk
    p->next = ch->head;
    bool wasBusy = ch->head == nullptr;
    ch->head = p;

    // mark which chunk do we belong to
    p->chunk = ch;

    if (wasBusy)
    {
        // chunk was full; move it back to the free list
        chunksBusy.Delete(ch);
        chunksFree.Insert(ch);
    }

    --ch->allocated;
    if (ch->allocated == 0)
    {
        if (!fast)
        {
            ChunkRemove(ch);
        }
    }

    // check if all chunks may be freed now
    if (--allocated == 0)
    {
        FreeChunks();
    }
}

void* FastAlloc::Alloc(size_t n)
{
    PoseidonAssert(n + sizeof(void*) <= esize); // Account for chunk pointer
    char* counted = (char*)AllocCounted(n);
    if (!counted)
    {
        return nullptr;
    }
    return counted + sizeof(void*); // skip chunk pointer header
}

void FastAlloc::Free(void* pAlloc)
{
    if (!pAlloc)
    {
        return;
    }
    char* counted = (char*)pAlloc - sizeof(void*); // Go back to chunk pointer
    // free but do not clean chunks yet
    FreeCounted(counted);
}

FastAlloc* FastAlloc::WhichAllocator(void* pAlloc)
{
    if (!pAlloc)
    {
        return nullptr;
    }
    Chunk* ch = *(Chunk**)pAlloc;
    if (ch->null1 != 0)
    {
        return nullptr;
    }
    return ch->allocator;
}

void FastAlloc::CleanUp()
{
    // drop every empty free chunk; busy chunks can't be reclaimed, so skip them
    for (Chunk* ch = chunksFree.First(); ch;)
    {
        Chunk* next = chunksFree.Next(ch);
        if (ch->allocated <= 0)
        {
            PoseidonAssert(ch->allocated == 0);
            ChunkRemove(ch);
        }
        ch = next;
    }
}

// Deliberate-corruption diagnostic used by the genuinity check on failure:
// Dammage_FastAlloc damages the allocator's internal structures so a later, hard
// to trace failure surfaces rather than an immediate one at the check site.

bool FastAlloc::CheckIfFree(Chunk* chunk, void* data) const
{
    // check if data are from this chunk
    // traverse free list and check if data is in it
    Link* p = chunk->head;
    while (p)
    {
        if (data >= p && data < (char*)p + esize)
        {
            return true;
        }
        p = p->next;
    }
    return false;
}

void Dammage_FastAlloc(FastAlloc* alloc)
{
    FastAlloc::Chunk* chunk = alloc->chunksFree.First(); // chunk with some free links
    if (!chunk || chunk->allocated == 0)
    {
        // no free chunk, or it holds nothing allocated — fall back to a busy chunk
        chunk = alloc->chunksBusy.First();
    }
    if (!chunk)
    {
        return;
    }

    // we have to find some non-free block of the chunk
    // use first busy block of the chunk
    // it will become free, but the memory will still be used,
    // which should lead to strange effects soon
    void* p = chunk->mem;
    while (alloc->CheckIfFree(chunk, p))
    {
        // a free block can't be freed again — it isn't linked back to its chunk
        p = (char*)p + alloc->ItemSize();
        if (p >= chunk->mem + FastAlloc::Chunk::size)
        {
            // no damage can be done
            return;
        }
    }
    alloc->FreeCounted(p);
}

void FastAlloc::ChunkRemove(Chunk* chunk)
{
    chunksFree.Delete(chunk);
    // remove all items from this chunk from free list
    // no need to remove links - they are not stored anywhere

    DeleteChunk(chunk);
}

void FastAlloc::Grow()
{
    Chunk* n = NewChunk();
    if (!n)
    {
        return;
    }
    n->null1 = 0;

    n->allocated = 0;
    n->allocator = this;

    chunksFree.Insert(n);

    char* start = n->mem + _alignOffset;

    const int nelem = _nElemPerChunk;
    PoseidonAssert(nelem > 0 && "FastAlloc: element size exceeds chunk capacity");

    char* last = &start[(nelem - 1) * esize];
    for (char* p = start; p < last; p += esize)
    {
        Link* link = reinterpret_cast<Link*>(p);

        link->next = reinterpret_cast<Link*>(p + esize);
        link->chunk = n;
    }
    Link* lastLink = reinterpret_cast<Link*>(last);
    lastLink->next = nullptr;
    lastLink->chunk = n;

    n->head = reinterpret_cast<Link*>(start);
}

// keep track of all FastCAlloc - use Meyers Singleton to avoid SIOF
static VerySmallArray<InitPtr<FastCAlloc>, 1024 * sizeof(InitPtr<FastCAlloc>)>& GetAllInstances()
{
    [[clang::no_destroy]] static VerySmallArray<InitPtr<FastCAlloc>, 1024 * sizeof(InitPtr<FastCAlloc>)> instances;
    return instances;
}

FastCAlloc::FastCAlloc(size_t n, const char* name)
    // CAlloc returns (element + sizeof(void*)) — the element carries a chunk-pointer
    // header. mem[] is 16B-aligned and esize is a multiple of 16 for objects > 32B, so
    // offsetting the first element by (16 - sizeof(void*)) makes every user pointer
    // 16B-aligned: 12 on 32-bit, 8 on 64-bit. (A hardcoded 12 leaves x64 user pointers
    // at +4 mod 16 — misaligned — which is what crashed FastAlloc on x64.)
    : base(n, name, 16 - (int)sizeof(void*))
{
    GetAllInstances().Add(this);
}

FastCAlloc::~FastCAlloc()
{
    for (int i = 0; i < GetAllInstances().Size(); i++)
    {
        FastCAlloc* ii = GetAllInstances()[i];
        if (ii == this)
        {
            GetAllInstances().Delete(i);
            return;
        }
    }
}

void* FastCAlloc::CAlloc(size_t n)
{
    PoseidonAssert(n + sizeof(void*) <= esize); // Account for chunk pointer
    char* counted = (char*)FastAlloc::AllocCounted(n);
    if (!counted)
    {
        return nullptr;
    }
    return counted + sizeof(void*); // skip chunk pointer header
}

void FastCAlloc::CFree(void* pAlloc)
{
    if (!pAlloc)
    {
        return;
    }
    char* counted = (char*)pAlloc - sizeof(void*); // Go back to chunk pointer
    // free but do not clean chunks yet
    FastAlloc::FreeCounted(counted);
}

void FastCAlloc::CleanUpAll()
{
    for (int i = 0; i < GetAllInstances().Size(); i++)
    {
        GetAllInstances()[i]->CleanUp();
    }
}

} // namespace Poseidon::Foundation
