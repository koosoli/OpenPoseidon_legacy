#pragma once

// Direct3D 12 device layer.
//
// The engine's drawing code sets render and sampler states one at a time,
// addresses shader constants by register and fills textures and buffers through
// lock / unlock.  This layer offers those call shapes and implements them on
// Direct3D 12:
//   - one command list, recorded as the calls arrive and submitted at Present;
//     three frames may be in flight, each with its own allocator, upload memory
//     and descriptor tables;
//   - render states are collected and turned into cached pipeline state objects
//     at draw time (shaders, vertex layout, blend, depth-stencil, rasterizer,
//     target formats);
//   - shader constants live in two CPU register blocks (c0..c255); a changed
//     block is copied into the frame's upload memory and bound as a root
//     constant buffer.  The HLSL globals keep their register(cN) slots, which
//     fxc maps to offset N*16 in $Globals;
//   - textures are bound by sampler stage; each pixel shader carries a
//     sampler-stage -> texture-slot table read from its reflection data.  The
//     textures of a draw are copied into a descriptor table of the frame;
//   - every resource carries the state the command list last left it in, and is
//     moved to the state its next use needs;
//   - released resources are kept until the frames that may still use them are
//     through;
//   - the "back buffer" is an offscreen colour target (multisampled when MSAA
//     is on) that Present copies or resolves into the flip-model swap chain.
//
// State and format names: Dx12Types.hpp.

#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_5.h>
#include <PoseidonD3D12/Dx12Types.hpp>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <string>
#include <vector>

namespace Dx12
{
using Microsoft::WRL::ComPtr;

class Device;
class Texture;

// Intrusive reference count with the COM method names the carried-over code uses.
class RefObject
{
  public:
    ULONG AddRef() { return static_cast<ULONG>(++_refs); }
    ULONG Release()
    {
        const long left = --_refs;
        if (left == 0)
            delete this;
        return static_cast<ULONG>(left);
    }

  protected:
    RefObject() = default;
    virtual ~RefObject() = default;
    RefObject(const RefObject&) = delete;
    RefObject& operator=(const RefObject&) = delete;

  private:
    long _refs = 1;
};

// A GPU resource and the state the command list last left it in.
struct Tracked
{
    ComPtr<ID3D12Resource> res;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

// A render target, a depth-stencil buffer, one level of a texture, or a
// system-memory image used for read-back.
class Surface : public RefObject
{
  public:
    HRESULT GetDesc(GxSurfaceDesc* desc) const;
    // System-memory surfaces only.
    HRESULT LockRect(GxLockedRect* locked, const RECT* rect, DWORD flags);
    HRESULT UnlockRect() { return S_OK; }

    UINT Width() const { return _w; }
    UINT Height() const { return _h; }
    UINT Samples() const { return _samples; }

  private:
    friend class Device;
    friend class Texture;
    ~Surface() override;

    Tracked _own;            // a target or depth buffer of its own
    Tracked* _trk = nullptr; // the resource: &_own, or the texture's for a texture level
    UINT _rtv = ~0u, _dsv = ~0u; // descriptor numbers
    Texture* _owner = nullptr;   // texture level: keeps the texture alive
    UINT _level = 0;
    UINT _w = 0, _h = 0;
    UINT _samples = 1;
    GxFormat _format = GXFMT_UNKNOWN;
    DXGI_FORMAT _dxgi = DXGI_FORMAT_UNKNOWN;
    std::vector<uint8_t> _sysmem; // system-memory surface
    UINT _sysPitch = 0;
};

class Texture : public RefObject
{
  public:
    // Hands out a CPU buffer for the level; UnlockRect uploads it.
    HRESULT LockRect(UINT level, GxLockedRect* locked, const RECT* rect, DWORD flags);
    HRESULT UnlockRect(UINT level);
    HRESULT GetSurfaceLevel(UINT level, Surface** out);
    HRESULT GetLevelDesc(UINT level, GxSurfaceDesc* desc) const;
    DWORD GetLevelCount() const { return _levels; }

  private:
    friend class Device;
    friend class Surface;
    ~Texture() override;

    Tracked _trk;
    UINT _srv = ~0u; // descriptor number
    UINT _w = 0, _h = 0, _levels = 1;
    DWORD _usage = 0;
    GxFormat _format = GXFMT_UNKNOWN; // format the caller asked for (lock layout)
    DXGI_FORMAT _dxgi = DXGI_FORMAT_UNKNOWN;
    bool _expand = false; // caller's format is expanded to BGRA8 on upload
    std::vector<std::vector<uint8_t>> _lock; // per level, alive between Lock and Unlock
    std::vector<Surface*> _levelSurface;     // weak; a surface clears its slot when it dies
};

// One of the large blocks static buffers are cut out of (Dx12Device.cpp).
struct BufferSlab;

class Buffer : public RefObject
{
  public:
    HRESULT Lock(UINT offset, UINT size, void** data, DWORD flags);
    HRESULT Unlock();

  private:
    friend class Device;
    ~Buffer() override;
    D3D12_GPU_VIRTUAL_ADDRESS Address() const;

    // Static buffers: a piece of a shared block (_slab, from _slabOffset), or, the big ones, a
    // resource of their own (_trk).
    Tracked _trk;
    BufferSlab* _slab = nullptr;
    UINT _slabOffset = 0;
    UINT _size = 0;
    bool _dynamic = false;
    bool _index = false;
    // dynamic buffers: CPU-writable memory, replaced by a fresh block on a discarding lock
    ComPtr<ID3D12Resource> _dyn;
    uint8_t* _dynPtr = nullptr;
    std::vector<uint8_t> _shadow; // static buffers: filled on Lock, uploaded on Unlock
};
using VertexBuffer = Buffer;
using IndexBuffer = Buffer;

class VertexDecl : public RefObject
{
  private:
    friend class Device;
    ~VertexDecl() override = default;
    struct Element
    {
        const char* semantic;
        UINT index;
        DXGI_FORMAT format;
        UINT offset;
    };
    std::vector<Element> _elements;
    UINT _id = 0;
};

class VertexShader : public RefObject
{
  private:
    friend class Device;
    ~VertexShader() override = default;
    struct Input
    {
        std::string semantic;
        UINT index;
    };
    std::vector<uint8_t> _code;
    std::vector<Input> _inputs;
    UINT _id = 0;
};

class PixelShader : public RefObject
{
  private:
    friend class Device;
    ~PixelShader() override = default;
    std::vector<uint8_t> _code;
    int8_t _texSlot[16]; // sampler stage -> texture slot, -1 = unused
    UINT _id = 0;
};

struct DeviceInfo
{
    char description[128] = {};
    unsigned vendorId = 0;
    unsigned deviceId = 0;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    uint64_t dedicatedVideoMemory = 0;
};

class Device
{
  public:
    enum
    {
        NumConstRegisters = 256,
        NumStages = 8,
        DepthSlot = 8,        // texture slots above the stages: the scene depth copy
        DepthSlotMS = 9,      // ... when it is multisampled
        LocalShadowSlot = 10, // the local lights' shadow atlas
        NumTextureSlots = 12  // size of a draw's texture table
    };

    // Creates the device and a swap chain for `hwnd`.  Returns nullptr on failure.
    static Device* Create(HWND hwnd, int width, int height, int msaaSamples, bool vsync);
    ~Device();
    // The device while it exists (resources released after it need nothing from it).
    static Device* Live() { return s_live; }

    const DeviceInfo& Info() const { return _info; }
    int MsaaSamples() const { return _msaa; }
    // Highest supported MSAA sample count <= `samples` for the back-buffer formats (0 = none).
    int ClampMsaaSamples(int samples) const;
    bool SupportsFormat(GxFormat format, bool renderTarget) const;

    // Resizes the swap chain and the offscreen back buffer.
    HRESULT Reset(int width, int height, int msaaSamples, bool vsync);
    // `overlay` (optional) draws on the finished picture, just before it is shown: the
    // swap-chain image is bound, single-sampled.
    using OverlayFn = void (*)(ID3D12GraphicsCommandList* list);
    HRESULT Present(OverlayFn overlay = nullptr);

    // For a tool that draws with its own single-sample pipeline on the frame's command
    // list (ImGui): bind the back buffer, let it draw, then call InvalidateBindings.
    // False when the back buffer is multisampled: such a tool can only draw at Present.
    bool BindBackBufferForExternalDraw();
    void InvalidateBindings();
    ID3D12Device* D3D() const { return _dev.Get(); }
    ID3D12CommandQueue* Queue() const { return _queue.Get(); }
    ID3D12GraphicsCommandList* List() const { return _list.Get(); }
    ID3D12DescriptorHeap* TextureHeap() const { return _gpuSrv.Get(); }
    static constexpr DXGI_FORMAT BackBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    static constexpr int FramesInFlight = 3;
    // A slot of the texture heap for such a tool's own textures.
    bool AllocExternalTexture(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu);
    void FreeExternalTexture(D3D12_CPU_DESCRIPTOR_HANDLE cpu);
    // Puts `tex` in such a slot for the tool to draw with (ImGui: the texture id).  `handle`
    // is 0 the first time and is filled in; later calls with it put another texture there.
    bool SetExternalTexture(UINT64* handle, Texture* tex);

    // ---- resources -----------------------------------------------------------
    HRESULT CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, GxFormat format, GxPool pool, Texture** out,
                          HANDLE* shared);
    HRESULT CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, GxPool pool, VertexBuffer** out, HANDLE* shared);
    HRESULT CreateIndexBuffer(UINT length, DWORD usage, GxFormat format, GxPool pool, IndexBuffer** out,
                              HANDLE* shared);
    HRESULT CreateVertexDeclaration(const GxVertexElement* elements, VertexDecl** out);
    HRESULT CreateVertexShader(const void* code, size_t size, VertexShader** out);
    HRESULT CreatePixelShader(const void* code, size_t size, PixelShader** out);
    HRESULT CreateRenderTarget(UINT w, UINT h, GxFormat format, GxMultisample ms, DWORD quality, BOOL lockable,
                               Surface** out, HANDLE* shared);
    HRESULT CreateDepthStencilSurface(UINT w, UINT h, GxFormat format, GxMultisample ms, DWORD quality,
                                      BOOL discard, Surface** out, HANDLE* shared);
    HRESULT CreateOffscreenPlainSurface(UINT w, UINT h, GxFormat format, GxPool pool, Surface** out, HANDLE* shared);

    // ---- state ---------------------------------------------------------------
    HRESULT SetRenderState(GxRenderState state, DWORD value);
    HRESULT SetSamplerState(DWORD stage, GxSamplerState type, DWORD value);
    HRESULT GetSamplerState(DWORD stage, GxSamplerState type, DWORD* value) const;
    HRESULT SetTexture(DWORD stage, Texture* tex);
    HRESULT SetViewport(const GxViewport* vp);
    HRESULT GetViewport(GxViewport* vp) const;
    HRESULT SetVertexShaderConstantF(UINT reg, const float* data, UINT vec4Count);
    HRESULT SetPixelShaderConstantF(UINT reg, const float* data, UINT vec4Count);
    HRESULT SetVertexShader(VertexShader* vs);
    HRESULT SetPixelShader(PixelShader* ps);
    HRESULT SetVertexDeclaration(VertexDecl* decl);
    HRESULT SetStreamSource(UINT stream, VertexBuffer* vb, UINT offset, UINT stride);
    HRESULT SetIndices(IndexBuffer* ib);

    // ---- targets -------------------------------------------------------------
    HRESULT SetRenderTarget(DWORD index, Surface* rt);
    HRESULT GetRenderTarget(DWORD index, Surface** out);
    HRESULT SetDepthStencilSurface(Surface* ds);
    HRESULT GetDepthStencilSurface(Surface** out);
    bool SnapshotDepth(); // copy the depth buffer in use for shaders to read
    void SetLocalShadowTexture(Texture* tex); // bound at LocalShadowSlot until changed (null: none)
    void GetBackDepth(Surface** out)
    {
        _backDepth->AddRef();
        *out = _backDepth;
    }
    bool DepthCopyMultisampled() const { return _depthCopySamples > 1; }
    HRESULT GetBackBuffer(UINT swapChain, UINT index, GxBackBufferType type, Surface** out);
    // Clears the current viewport rectangle of the bound targets, not the whole target.
    HRESULT Clear(DWORD count, const GxRect* rects, DWORD flags, GxColor color, float z, DWORD stencil);
    HRESULT BeginScene() { return S_OK; }
    HRESULT EndScene() { return S_OK; }
    // Whole-surface copy between same-sized surfaces; resolves a multisampled source.
    HRESULT StretchRect(Surface* src, const RECT* srcRect, Surface* dst, const RECT* dstRect, GxTextureFilter f);
    // Copies a render target into a system-memory surface of the same size.
    HRESULT GetRenderTargetData(Surface* src, Surface* dst);

    // ---- draws ---------------------------------------------------------------
    HRESULT DrawPrimitiveUP(GxPrimitiveType type, UINT primitives, const void* data, UINT stride);
    HRESULT DrawIndexedPrimitive(GxPrimitiveType type, INT baseVertex, UINT minIndex, UINT numVertices,
                                 UINT startIndex, UINT primitives);

    // Video memory this process may still take (the system's budget for it less what it uses).
    uint64_t GetAvailableTextureMem() const;
    // Video memory the machine gives this process at most: what its size is judged by.
    uint64_t GetVideoMemoryBudget() const;

    // What went wrong in this layer since the last call, and the messages of the Direct3D
    // debug layer (errors, corruption and warnings).  The layer is on only when the environment variable
    // POSEIDON_D3D12_DEBUG is set and the SDK layers are installed.
    bool DebugLayerActive() const { return _infoQueue != nullptr; }
    void DrainDebugMessages(std::vector<std::string>& out);

    // Debug-event markers (shown by graphics debuggers).
    void BeginEvent(const wchar_t* name);
    void EndEvent();

  private:
    Device() = default;
    bool Init(HWND hwnd, int width, int height, int msaaSamples, bool vsync);
    bool CreateBackBuffer(int width, int height, int msaaSamples);
    void ReleaseBackBuffer();

    // ---- frames --------------------------------------------------------------
    struct UploadPage
    {
        ComPtr<ID3D12Resource> res;
        uint8_t* ptr = nullptr;
        UINT size = 0;
    };
    struct UploadAlloc
    {
        ID3D12Resource* res = nullptr;
        UINT64 offset = 0;
        uint8_t* ptr = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS va = 0;
    };
    struct DynBlock
    {
        ComPtr<ID3D12Resource> res;
        uint8_t* ptr = nullptr;
        UINT size = 0;
    };
    struct Frame
    {
        ComPtr<ID3D12CommandAllocator> alloc;
        UINT64 fence = 0; // the queue has passed this when the frame's work is done
        std::vector<UploadPage> pages;
        size_t page = 0;
        UINT pageUsed = 0;
        UINT64 uploaded = 0;
        std::vector<ComPtr<ID3D12Resource>> dead; // released while this frame was recorded
        std::vector<DynBlock> deadDyn;
        struct SlabRange
        {
            BufferSlab* slab;
            UINT offset, size;
        };
        std::vector<SlabRange> deadRanges; // pieces of blocks given up while this frame was recorded
        UINT textureTables = 0;
    };
    void BeginList();                 // after the list is (re)opened: heaps, root signature, everything dirty
    void SubmitAndWait();             // run what is recorded now and wait for it (mid-frame)
    void WaitFor(UINT64 fenceValue);
    void RecycleFrame(Frame& f);      // its work is done: free what it kept
    UploadAlloc AllocUpload(UINT size, UINT align);
    void Retire(ComPtr<ID3D12Resource>& res);
    DynBlock AcquireDyn(UINT size);
    void RetireDyn(ComPtr<ID3D12Resource>& res, uint8_t* ptr, UINT size);
    void Transition(Tracked& t, D3D12_RESOURCE_STATES state);
    HRESULT CreateTracked(const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
                          Tracked& out);
    void UploadLevel(Texture* tex, UINT level, const void* data, UINT pitch);
    void UploadBuffer(Buffer* buf);

    // ---- descriptors ---------------------------------------------------------
    struct DescriptorPool
    {
        ComPtr<ID3D12DescriptorHeap> heap;
        UINT step = 0, capacity = 0, next = 0;
        std::vector<UINT> freed;
        bool Init(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count);
        UINT Alloc();
        void Free(UINT index);
        D3D12_CPU_DESCRIPTOR_HANDLE At(UINT index) const;
    };
    D3D12_CPU_DESCRIPTOR_HANDLE GpuSrvCpu(UINT index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GpuSrvGpu(UINT index) const;

    // ---- draw-time state -----------------------------------------------------
    bool PrepareDraw(UINT stride, D3D12_GPU_VIRTUAL_ADDRESS vb, UINT vbBytes, D3D12_PRIMITIVE_TOPOLOGY topology);
    void ApplyTargets();
    void ApplyPipeline(D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType);
    void ApplyTextures();
    void ApplySamplers();
    void ApplyConstants();
    bool TargetHasDepth() const;
    static DXGI_FORMAT MapFormat(GxFormat format, bool renderTarget, bool* expand);
    static D3D12_PRIMITIVE_TOPOLOGY MapTopology(GxPrimitiveType type, UINT primitives, UINT* vertexCount);

    friend class Texture;
    friend class Buffer;
    friend class Surface;
    static Device* s_live;

    DeviceInfo _info;
    HWND _hwnd = nullptr;
    ComPtr<ID3D12Device> _dev;
    ComPtr<ID3D12CommandQueue> _queue;
    ComPtr<ID3D12GraphicsCommandList> _list;
    ComPtr<ID3D12Fence> _fence;
    HANDLE _fenceEvent = nullptr;
    UINT64 _fenceValue = 0;
    ComPtr<ID3D12InfoQueue> _infoQueue;
    ComPtr<IDXGISwapChain3> _swap;
    ComPtr<IDXGIAdapter1> _adapter;
    ComPtr<ID3D12RootSignature> _root;
    bool _tearing = false;
    bool _vsync = true;
    int _msaa = 0;
    int _w = 0, _h = 0;

    Frame _frames[FramesInFlight];
    int _frame = 0;
    std::unordered_map<UINT, std::vector<DynBlock>> _dynPool; // free dynamic-buffer blocks by size

    // Static vertex and index buffers are pieces of a few large blocks: making a resource for
    // each cost a third of a millisecond, a couple of thousand times over at every mission start.
    std::vector<BufferSlab*> _slabs;
    bool _slabsCopying = false; // some block is being copied into: ReadySlabs() before drawing
    bool SlabAlloc(UINT size, BufferSlab*& slab, UINT& offset);
    void SlabFree(BufferSlab* slab, UINT offset, UINT size);
    void ReadySlabs();

    enum
    {
        ExternalTextures = 64,     // head of the texture heap, for AllocExternalTexture
        TablesPerFrame = 16384,    // texture tables one frame can use before it has to be submitted
        SamplerTableSize = NumStages,
        SamplerHeapSize = 2048
    };
    DescriptorPool _rtvPool, _dsvPool, _srvPool; // CPU side
    ComPtr<ID3D12DescriptorHeap> _gpuSrv;        // what the shaders read: tables copied from _srvPool
    UINT _gpuSrvStep = 0;
    std::vector<UINT> _externalFree;
    ComPtr<ID3D12DescriptorHeap> _gpuSamplers;
    UINT _samplerStep = 0, _samplerNext = 0;
    std::unordered_map<uint64_t, UINT> _samplerTables; // by the eight stages' states -> first descriptor

    Surface* _back = nullptr;      // offscreen back buffer (persistent object, contents swapped on Reset)
    Surface* _backDepth = nullptr; // its depth-stencil buffer
    Surface* _rt = nullptr;        // current render target (strong)
    Surface* _ds = nullptr;        // current depth-stencil (strong)
    bool _targetsDirty = true;

    GxViewport _viewport = {};
    bool _viewportDirty = true;

    std::array<DWORD, 256> _rs{};
    bool _pipelineDirty = true;
    bool _stencilRefDirty = true;
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> _pipelines;
    ID3D12PipelineState* _pipeline = nullptr; // set on the list
    D3D12_PRIMITIVE_TOPOLOGY_TYPE _pipelineTopology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
    D3D12_PRIMITIVE_TOPOLOGY _topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;

    struct Stage
    {
        DWORD addressU = GXTADDRESS_WRAP, addressV = GXTADDRESS_WRAP;
        DWORD magFilter = GXTEXF_POINT, minFilter = GXTEXF_POINT, mipFilter = GXTEXF_NONE;
        DWORD maxAnisotropy = 1;
        DWORD srgb = 0; // stored, not applied (the engine never turns it on)
        Texture* tex = nullptr; // strong
    };
    Stage _stage[NumStages];
    bool _texturesDirty = true;
    bool _samplersDirty = true;
    UINT _boundTable[NumTextureSlots] = {}; // descriptor numbers of the table on the list
    bool _boundTableValid = false;

    float _vsConst[NumConstRegisters][4] = {};
    float _psConst[NumConstRegisters][4] = {};
    bool _vsConstDirty = true, _psConstDirty = true;

    VertexShader* _vs = nullptr; // weak: shaders outlive their use
    PixelShader* _ps = nullptr;
    VertexDecl* _decl = nullptr;
    UINT _nextDeclId = 1, _nextShaderId = 1;

    Buffer* _streamVB = nullptr; // weak
    UINT _streamOffset = 0, _streamStride = 0;
    Buffer* _indices = nullptr; // weak

    ComPtr<ID3D12Resource> _zeroVB; // feeds vertex inputs the declaration lacks
    Tracked _depthCopy;             // SnapshotDepth
    UINT _depthCopySrv = ~0u;
    UINT _depthCopyW = 0, _depthCopyH = 0, _depthCopySamples = 0;
    Texture* _localShadowTex = nullptr; // SetLocalShadowTexture; the engine owns it
    Tracked _emptyStage;                // opaque black, sampled by stages with no texture
    UINT _emptyStageSrv = ~0u;
    UINT _nullSrv = ~0u, _nullSrvMS = ~0u; // for slots nothing is bound to
    std::vector<std::string> _failures; // for DrainDebugMessages
    UINT _swapRtv = ~0u;                   // the swap-chain image, for the overlay of Present
};
} // namespace Dx12
