#pragma once

// Pool allocator: fixed-size blocks carved from pre-allocated chunks, for the hot
// classes that opt in via USE_FAST_ALLOCATOR. Algorithm from Stroustrup,
// "The C++ Programming Language", 3rd ed.
//
//   struct MyClass { USE_FAST_ALLOCATOR };
//   DEFINE_FAST_ALLOCATOR(MyClass);

#include <Poseidon/Foundation/Containers/CacheList.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <cstdint>


namespace Poseidon::Foundation
{
class FastCAlloc; // used for classes
class FastAlloc;

// USE_FAST_ALLOCATOR macro - defines custom operator new/delete for pool allocation
#define USE_FAST_ALLOCATOR_ID(x)                     \
  private:                                           \
    static ::Poseidon::Foundation::FastCAlloc& _allocator##x##_instance();   \
    void* operator new[](size_t n);                  \
    void operator delete[](void* ptr, size_t n);     \
                                                     \
  public:                                            \
    void* operator new(size_t n)                     \
    {                                                \
        return _allocator##x##_instance().CAlloc(n); \
    }                                                \
    void operator delete(void* ptr)                  \
    {                                                \
        _allocator##x##_instance().CFree(ptr);       \
    }

// Function-local static (Meyers singleton) — initialized on first access, not at
// program startup, so it sidesteps the static-init order fiasco.
#define DEFINE_FAST_ALLOCATOR_ID(className, x)                                                        \
    Poseidon::Foundation::FastCAlloc& className::_allocator##x##_instance()                              \
    {                                                                                                    \
        [[clang::no_destroy]] static Poseidon::Foundation::FastCAlloc allocator(sizeof(className), #className); \
        return allocator;                                                                                \
    }
#define USE_FAST_ALLOCATOR USE_FAST_ALLOCATOR_ID(F)
#define DEFINE_FAST_ALLOCATOR(className) DEFINE_FAST_ALLOCATOR_ID(className, F)

enum
{
#if INTPTR_MAX == INT64_MAX
    FastAllocChunkSize = 16 * 1024 - 8
#else
    FastAllocChunkSize = 8 * 1024 - 8
#endif
};

// Base for the fixed-block pool allocators.
class FastAlloc
{
  public:
    enum
    {
        chunkSize = FastAllocChunkSize
    };
    struct Chunk;

    FastAlloc(size_t n, const char* name = "", int alignOffset = 0);
    ~FastAlloc();

    // Counted allocation only — uncounted variants fragment too badly.
    void* Alloc(size_t n);
    void Free(void* pAlloc);

    // free unused chunks
    void CleanUp();

    static FastAlloc* WhichAllocator(void* pAlloc);
    void* AllocCounted(size_t n);
    void FreeCounted(void* pAlloc, bool fast = true);

    const char* Name() const { return _name; }
    size_t ItemSize() const { return esize; }
    static int ChunkSize() { return sizeof(Chunk); }

  protected:
    struct Link
    {
        Link* next;   // next free in chunk
        Chunk* chunk; // which chunk is it in
    };
    struct ChunkHead
    {
        int null1;            // null - to distinguish this block type
        FastAlloc* allocator; // which allocator this chunk serves for
        int allocated;        // number of allocated items
        // align mem to 16 B boundary
        // each block will have short 4B description on the beginning
        Link* head;
        void* align[2];
    };

  public:
    struct Chunk : public ChunkHead, public CLDLink
    {
        enum
        {
            size = chunkSize - sizeof(ChunkHead) - sizeof(CLDLink)
        };
        char mem[size];
    };

  protected:
    // no free items - add new chunk
    void Grow();

    // remove a chunk and all its items; the chunk must hold none allocated
    void ChunkRemove(Chunk* chunk);

    // overridable hook to the parent memory manager
    virtual Chunk* NewChunk();
    virtual void DeleteChunk(Chunk* chunk);

    virtual void FreeChunks();

    CLList<Chunk> chunksFree; // some Link in chunk is free
    CLList<Chunk> chunksBusy; // all Links busy
    const char* _name;        // for debugging purposes
    int _alignOffset;         // align each chunk
    unsigned int esize;
    int _nElemPerChunk;

    int allocated; // how much is allocated

    friend void Dammage_FastAlloc(FastAlloc* alloc);

    bool CheckIfFree(Chunk* chunk, void* data) const;
};

// Fixed-block pool with automatic cleanup; defines per-class allocators (see USE_FAST_ALLOCATOR).
class FastCAlloc : public FastAlloc
{
    typedef FastAlloc base;

  public:
    FastCAlloc(size_t n, const char* name = "");
    ~FastCAlloc();

    void* CAlloc(size_t n);
    void CFree(void* pAlloc);

    static void CleanUpAll(); // free unused chunks in all FastCAlloc instances

  private: // hide parent members
    static FastAlloc* WhichAllocator(void* pAlloc);
    void* AllocCounted(size_t n);
    void FreeCounted(void* pAlloc, bool fast = true);
};

} // namespace Poseidon::Foundation

// Global operator declarations — must be at global scope
void* CCALL operator new(size_t size, const char* file, int line);
void* CCALL operator new(size_t size);
void CCALL operator delete(void* ptr) noexcept;

