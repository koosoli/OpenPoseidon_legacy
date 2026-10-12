#include <map>
#include <PoseidonD3D12/Dx12Device.hpp>

#include <d3d12shader.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace Dx12
{
Device* Device::s_live = nullptr;

namespace
{
constexpr UINT kUploadPage = 4u << 20;         // one block of a frame's upload memory
constexpr UINT kSlabSize = 8u << 20;    // a block static buffers are cut out of
constexpr UINT kSlabLargest = 1u << 20; // a buffer over this gets a resource of its own
constexpr UINT kSlabAlign = 64;
constexpr UINT64 kUploadFlush = 256ull << 20;  // more than this uploaded in one frame: submit and start over
constexpr UINT kNone = ~0u;

// Lock layout: bytes per pixel for the uncompressed formats, 0 for block formats.
UINT BytesPerPixel(GxFormat f)
{
    switch (f)
    {
        case GXFMT_A8R8G8B8:
        case GXFMT_X8R8G8B8:
        case GXFMT_R32F:
            return 4;
        case GXFMT_R5G6B5:
        case GXFMT_A1R5G5B5:
        case GXFMT_A4R4G4B4:
        case GXFMT_A8L8:
            return 2;
        case GXFMT_L8:
            return 1;
        case GXFMT_A16B16G16R16F:
            return 8;
        default:
            return 0;
    }
}

UINT BlockBytes(GxFormat f)
{
    switch (f)
    {
        case GXFMT_DXT1:
            return 8;
        case GXFMT_DXT3:
        case GXFMT_DXT5:
            return 16;
        default:
            return 0;
    }
}

// Row pitch and byte size of one level in the caller's layout.
void LevelLayout(GxFormat f, UINT w, UINT h, UINT& pitch, UINT& bytes)
{
    if (const UINT block = BlockBytes(f))
    {
        const UINT bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4);
        pitch = bw * block;
        bytes = pitch * bh;
        return;
    }
    pitch = w * BytesPerPixel(f);
    bytes = pitch * h;
}

uint64_t HashBytes(const void* data, size_t size)
{
    uint64_t h = 1469598103934665603ull; // FNV-1a
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; i++)
    {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

D3D12_BLEND MapBlend(DWORD b, bool alpha)
{
    switch (b)
    {
        case GXBLEND_ZERO:
            return D3D12_BLEND_ZERO;
        case GXBLEND_ONE:
            return D3D12_BLEND_ONE;
        case GXBLEND_SRCCOLOR:
            return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
        case GXBLEND_INVSRCCOLOR:
            return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
        case GXBLEND_SRCALPHA:
            return D3D12_BLEND_SRC_ALPHA;
        case GXBLEND_INVSRCALPHA:
            return D3D12_BLEND_INV_SRC_ALPHA;
        case GXBLEND_DESTALPHA:
            return D3D12_BLEND_DEST_ALPHA;
        case GXBLEND_INVDESTALPHA:
            return D3D12_BLEND_INV_DEST_ALPHA;
        case GXBLEND_DESTCOLOR:
            return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
        case GXBLEND_INVDESTCOLOR:
            return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
        case GXBLEND_SRCALPHASAT:
            return alpha ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_ALPHA_SAT;
        default:
            return D3D12_BLEND_ONE;
    }
}

D3D12_TEXTURE_ADDRESS_MODE MapAddress(DWORD a)
{
    switch (a)
    {
        case GXTADDRESS_MIRROR:
            return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case GXTADDRESS_CLAMP:
            return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case GXTADDRESS_BORDER:
            return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        case GXTADDRESS_MIRRORONCE:
            return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
        default:
            return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

float DwordToFloat(DWORD d)
{
    float f;
    std::memcpy(&f, &d, sizeof(f));
    return f;
}

UINT AlignUp(UINT v, UINT a)
{
    return (v + a - 1) / a * a;
}

D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = type;
    return hp;
}

D3D12_RESOURCE_DESC BufferDesc(UINT64 bytes)
{
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = std::max<UINT64>(bytes, 1);
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

D3D12_RESOURCE_DESC TextureDesc(UINT w, UINT h, UINT levels, DXGI_FORMAT format, UINT samples,
                                D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = static_cast<UINT16>(levels);
    d.Format = format;
    d.SampleDesc.Count = samples;
    d.Flags = flags;
    return d;
}

// The depth buffers are typeless so that a copy of one can be read by shaders.
constexpr DXGI_FORMAT kDepthResource = DXGI_FORMAT_R24G8_TYPELESS;
constexpr DXGI_FORMAT kDepthView = DXGI_FORMAT_D24_UNORM_S8_UINT;
constexpr DXGI_FORMAT kDepthRead = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
} // namespace

// ---------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------

bool Device::DescriptorPool::Init(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count)
{
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = type;
    hd.NumDescriptors = count;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))))
        return false;
    step = dev->GetDescriptorHandleIncrementSize(type);
    capacity = count;
    return true;
}

UINT Device::DescriptorPool::Alloc()
{
    if (!freed.empty())
    {
        const UINT i = freed.back();
        freed.pop_back();
        return i;
    }
    return next < capacity ? next++ : kNone;
}

void Device::DescriptorPool::Free(UINT index)
{
    if (index != kNone)
        freed.push_back(index);
}

D3D12_CPU_DESCRIPTOR_HANDLE Device::DescriptorPool::At(UINT index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) * step;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE Device::GpuSrvCpu(UINT index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = _gpuSrv->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) * _gpuSrvStep;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE Device::GpuSrvGpu(UINT index) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = _gpuSrv->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(index) * _gpuSrvStep;
    return h;
}

bool Device::AllocExternalTexture(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    if (_externalFree.empty() || !cpu || !gpu)
        return false;
    const UINT i = _externalFree.back();
    _externalFree.pop_back();
    *cpu = GpuSrvCpu(i);
    *gpu = GpuSrvGpu(i);
    return true;
}

void Device::FreeExternalTexture(D3D12_CPU_DESCRIPTOR_HANDLE cpu)
{
    const SIZE_T base = _gpuSrv->GetCPUDescriptorHandleForHeapStart().ptr;
    if (cpu.ptr < base || _gpuSrvStep == 0)
        return;
    const UINT i = static_cast<UINT>((cpu.ptr - base) / _gpuSrvStep);
    if (i < ExternalTextures)
        _externalFree.push_back(i);
}

bool Device::SetExternalTexture(UINT64* handle, Texture* tex)
{
    if (!handle || !tex || !tex->_trk.res)
        return false;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};
    if (*handle == 0)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = {};
        if (!AllocExternalTexture(&cpu, &gpu))
            return false;
        *handle = gpu.ptr;
    }
    else
    {
        const UINT64 base = _gpuSrv->GetGPUDescriptorHandleForHeapStart().ptr;
        cpu = GpuSrvCpu(static_cast<UINT>((*handle - base) / _gpuSrvStep));
    }
    _dev->CreateShaderResourceView(tex->_trk.res.Get(), nullptr, cpu);
    return true;
}

// ---------------------------------------------------------------------------
// Surface
// ---------------------------------------------------------------------------

Surface::~Surface()
{
    if (Device* d = Device::Live())
    {
        d->_rtvPool.Free(_rtv);
        d->_dsvPool.Free(_dsv);
        d->Retire(_own.res);
    }
    if (_owner)
    {
        if (_level < _owner->_levelSurface.size() && _owner->_levelSurface[_level] == this)
            _owner->_levelSurface[_level] = nullptr;
        _owner->Release();
    }
}

HRESULT Surface::GetDesc(GxSurfaceDesc* desc) const
{
    if (!desc)
        return E_POINTER;
    *desc = {};
    desc->Format = _format;
    desc->Type = GXRTYPE_SURFACE;
    desc->Width = _w;
    desc->Height = _h;
    desc->MultiSampleType = _samples > 1 ? static_cast<GxMultisample>(_samples) : GXMULTISAMPLE_NONE;
    return S_OK;
}

HRESULT Surface::LockRect(GxLockedRect* locked, const RECT*, DWORD)
{
    if (!locked || _sysmem.empty())
        return E_FAIL;
    locked->pBits = _sysmem.data();
    locked->Pitch = static_cast<INT>(_sysPitch);
    return S_OK;
}

// ---------------------------------------------------------------------------
// Texture
// ---------------------------------------------------------------------------

Texture::~Texture()
{
    for (Surface* s : _levelSurface)
        if (s)
            s->_owner = nullptr; // cannot happen while a level surface holds a reference; defensive
    if (Device* d = Device::Live())
    {
        d->_srvPool.Free(_srv);
        d->Retire(_trk.res);
        if (d->_localShadowTex == this)
            d->_localShadowTex = nullptr;
        // its descriptor number may be handed to the next texture: the table on the list
        // can no longer be recognised by its numbers
        d->_boundTableValid = false;
        d->_texturesDirty = true;
    }
}

HRESULT Texture::LockRect(UINT level, GxLockedRect* locked, const RECT*, DWORD)
{
    if (!locked || level >= _levels)
        return E_INVALIDARG;
    const UINT w = std::max(1u, _w >> level), h = std::max(1u, _h >> level);
    UINT pitch = 0, bytes = 0;
    LevelLayout(_format, w, h, pitch, bytes);
    if (bytes == 0)
        return E_FAIL;
    if (_lock.size() < _levels)
        _lock.resize(_levels);
    _lock[level].assign(bytes, 0);
    locked->pBits = _lock[level].data();
    locked->Pitch = static_cast<INT>(pitch);
    return S_OK;
}

HRESULT Texture::UnlockRect(UINT level)
{
    if (level >= _lock.size() || _lock[level].empty())
        return E_FAIL;
    Device* device = Device::Live();
    if (!device)
        return E_FAIL;
    const UINT w = std::max(1u, _w >> level), h = std::max(1u, _h >> level);
    UINT pitch = 0, bytes = 0;
    LevelLayout(_format, w, h, pitch, bytes);
    std::vector<uint8_t>& src = _lock[level];

    if (_expand)
    {
        // The device lacks this format: expand to BGRA8.
        std::vector<uint32_t> out(static_cast<size_t>(w) * h);
        const uint8_t* p = src.data();
        for (size_t i = 0, n = out.size(); i < n; i++)
        {
            uint32_t a = 255, r = 0, g = 0, b = 0;
            switch (_format)
            {
                case GXFMT_L8:
                    r = g = b = p[i];
                    break;
                case GXFMT_A8L8:
                    r = g = b = p[i * 2];
                    a = p[i * 2 + 1];
                    break;
                case GXFMT_R5G6B5:
                {
                    const uint16_t v = static_cast<uint16_t>(p[i * 2] | (p[i * 2 + 1] << 8));
                    r = ((v >> 11) & 31) * 255 / 31;
                    g = ((v >> 5) & 63) * 255 / 63;
                    b = (v & 31) * 255 / 31;
                    break;
                }
                case GXFMT_A1R5G5B5:
                {
                    const uint16_t v = static_cast<uint16_t>(p[i * 2] | (p[i * 2 + 1] << 8));
                    a = (v >> 15) ? 255 : 0;
                    r = ((v >> 10) & 31) * 255 / 31;
                    g = ((v >> 5) & 31) * 255 / 31;
                    b = (v & 31) * 255 / 31;
                    break;
                }
                case GXFMT_A4R4G4B4:
                {
                    const uint16_t v = static_cast<uint16_t>(p[i * 2] | (p[i * 2 + 1] << 8));
                    a = ((v >> 12) & 15) * 17;
                    r = ((v >> 8) & 15) * 17;
                    g = ((v >> 4) & 15) * 17;
                    b = (v & 15) * 17;
                    break;
                }
                default:
                    break;
            }
            out[i] = (a << 24) | (r << 16) | (g << 8) | b;
        }
        device->UploadLevel(this, level, out.data(), w * 4);
    }
    else
    {
        device->UploadLevel(this, level, src.data(), pitch);
    }
    std::vector<uint8_t>().swap(src);
    return S_OK;
}

HRESULT Texture::GetSurfaceLevel(UINT level, Surface** out)
{
    if (!out || level >= _levels)
        return E_INVALIDARG;
    Device* device = Device::Live();
    if (!device)
        return E_FAIL;
    if (_levelSurface.size() < _levels)
        _levelSurface.resize(_levels, nullptr);
    if (Surface* s = _levelSurface[level])
    {
        s->AddRef();
        *out = s;
        return S_OK;
    }
    Surface* s = new Surface;
    s->_trk = &_trk;
    s->_level = level;
    s->_w = std::max(1u, _w >> level);
    s->_h = std::max(1u, _h >> level);
    s->_format = _format;
    s->_dxgi = _dxgi;
    if (_usage & GXUSAGE_RENDERTARGET)
    {
        s->_rtv = device->_rtvPool.Alloc();
        if (s->_rtv == kNone)
        {
            s->Release();
            return E_FAIL;
        }
        D3D12_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = _dxgi;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rd.Texture2D.MipSlice = level;
        device->_dev->CreateRenderTargetView(_trk.res.Get(), &rd, device->_rtvPool.At(s->_rtv));
    }
    s->_owner = this;
    AddRef();
    _levelSurface[level] = s;
    *out = s;
    return S_OK;
}

HRESULT Texture::GetLevelDesc(UINT level, GxSurfaceDesc* desc) const
{
    if (!desc || level >= _levels)
        return E_INVALIDARG;
    *desc = {};
    desc->Format = _format;
    desc->Type = GXRTYPE_SURFACE;
    desc->Usage = _usage;
    desc->Width = std::max(1u, _w >> level);
    desc->Height = std::max(1u, _h >> level);
    return S_OK;
}

// ---------------------------------------------------------------------------
// Buffer
// ---------------------------------------------------------------------------

struct BufferSlab
{
    Tracked trk;
    D3D12_GPU_VIRTUAL_ADDRESS va = 0;
    std::map<UINT, UINT> free; // offset -> length of every free piece, neighbours joined
    UINT freeBytes = 0;
    bool copying = false; // being copied into (COPY_DEST): not readable by a draw yet
};

static UINT SlabRound(UINT size)
{
    return (size + kSlabAlign - 1) & ~(kSlabAlign - 1);
}

bool Device::SlabAlloc(UINT size, BufferSlab*& slab, UINT& offset)
{
    const UINT need = SlabRound(size);
    for (int pass = 0; pass < 2; pass++)
    {
        for (BufferSlab* s : _slabs)
        {
            if (s->freeBytes < need)
                continue;
            for (auto it = s->free.begin(); it != s->free.end(); ++it)
            {
                if (it->second < need)
                    continue;
                const UINT at = it->first, len = it->second;
                s->free.erase(it);
                if (len > need)
                    s->free[at + need] = len - need;
                s->freeBytes -= need;
                slab = s;
                offset = at;
                return true;
            }
        }
        if (pass)
            break;
        // none has room: another block
        BufferSlab* s = new BufferSlab;
        if (FAILED(CreateTracked(BufferDesc(kSlabSize), D3D12_RESOURCE_STATE_COMMON, nullptr, s->trk)))
        {
            delete s;
            return false;
        }
        s->va = s->trk.res->GetGPUVirtualAddress();
        s->free[0] = kSlabSize;
        s->freeBytes = kSlabSize;
        _slabs.push_back(s);
    }
    return false;
}

void Device::SlabFree(BufferSlab* slab, UINT offset, UINT size)
{
    UINT at = offset, len = SlabRound(size);
    slab->freeBytes += len;
    // join with the free pieces on either side
    auto next = slab->free.lower_bound(at);
    if (next != slab->free.begin())
    {
        auto prev = std::prev(next);
        if (prev->first + prev->second == at)
        {
            at = prev->first;
            len += prev->second;
            slab->free.erase(prev);
        }
    }
    if (next != slab->free.end() && at + len == next->first)
    {
        len += next->second;
        slab->free.erase(next);
    }
    slab->free[at] = len;

    // a block with nothing left in it goes back, as long as another empty one stays for the next buffers
    if (slab->freeBytes == kSlabSize)
    {
        int empty = 0;
        for (BufferSlab* s : _slabs)
            empty += s->freeBytes == kSlabSize ? 1 : 0;
        if (empty > 1)
        {
            Retire(slab->trk.res);
            _slabs.erase(std::find(_slabs.begin(), _slabs.end(), slab));
            delete slab;
        }
    }
}

// Blocks that were copied into are made readable again: before anything is drawn from them.
void Device::ReadySlabs()
{
    for (BufferSlab* s : _slabs)
    {
        if (!s->copying)
            continue;
        Transition(s->trk, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER);
        s->copying = false;
    }
    _slabsCopying = false;
}

Buffer::~Buffer()
{
    if (Device* d = Device::Live())
    {
        // its piece of the block may still be read by the frames in flight: free it with this frame
        if (_slab)
            d->_frames[d->_frame].deadRanges.push_back({_slab, _slabOffset, _size});
        d->Retire(_trk.res);
        if (_dyn)
            d->RetireDyn(_dyn, _dynPtr, _size);
        if (d->_streamVB == this)
            d->_streamVB = nullptr;
        if (d->_indices == this)
            d->_indices = nullptr;
    }
}

D3D12_GPU_VIRTUAL_ADDRESS Buffer::Address() const
{
    if (_dynamic)
        return _dyn ? _dyn->GetGPUVirtualAddress() : 0;
    if (_slab)
        return _slab->va + _slabOffset;
    return _trk.res ? _trk.res->GetGPUVirtualAddress() : 0;
}

HRESULT Buffer::Lock(UINT offset, UINT size, void** data, DWORD flags)
{
    if (!data)
        return E_FAIL;
    Device* device = Device::Live();
    if (!device)
        return E_FAIL;
    if (_dynamic)
    {
        // Writing where nothing has been drawn from yet keeps the block; anything else gets
        // a fresh one, since the frames in flight may still read the old.
        if (!(flags & GXLOCK_NOOVERWRITE) || !_dyn)
        {
            if (_dyn)
                device->RetireDyn(_dyn, _dynPtr, _size);
            Device::DynBlock b = device->AcquireDyn(_size);
            _dyn = b.res;
            _dynPtr = b.ptr;
        }
        if (!_dynPtr)
            return E_FAIL;
        *data = _dynPtr + offset;
        return S_OK;
    }
    if (!_trk.res && !_slab)
        return E_FAIL;
    if (_shadow.size() != _size)
        _shadow.assign(_size, 0);
    *data = _shadow.data() + offset;
    return S_OK;
}

HRESULT Buffer::Unlock()
{
    if (_dynamic)
        return S_OK;
    if (!_shadow.empty())
    {
        if (Device* device = Device::Live())
            device->UploadBuffer(this);
        std::vector<uint8_t>().swap(_shadow);
    }
    return S_OK;
}

// ---------------------------------------------------------------------------
// Device: creation
// ---------------------------------------------------------------------------

Device* Device::Create(HWND hwnd, int width, int height, int msaaSamples, bool vsync)
{
    Device* d = new Device;
    if (!d->Init(hwnd, width, height, msaaSamples, vsync))
    {
        delete d;
        return nullptr;
    }
    return d;
}

bool Device::Init(HWND hwnd, int width, int height, int msaaSamples, bool vsync)
{
    _hwnd = hwnd;
    _vsync = vsync;

    char wantDebug[8] = {};
    const bool debug =
        GetEnvironmentVariableA("POSEIDON_D3D12_DEBUG", wantDebug, sizeof(wantDebug)) > 0 && wantDebug[0] != '0';
    if (debug)
    {
        ComPtr<ID3D12Debug> layer;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&layer))))
            layer->EnableDebugLayer();
    }

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        return false;
    for (UINT i = 0;; i++)
    {
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 ad = {};
        if (FAILED(a->GetDesc1(&ad)) || (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            continue;
        if (SUCCEEDED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&_dev))))
        {
            _adapter = a;
            WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, _info.description, sizeof(_info.description) - 1,
                                nullptr, nullptr);
            _info.vendorId = ad.VendorId;
            _info.deviceId = ad.DeviceId;
            _info.dedicatedVideoMemory = ad.DedicatedVideoMemory;
            break;
        }
    }
    if (!_dev)
        return false;
    {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                            D3D_FEATURE_LEVEL_11_0};
        D3D12_FEATURE_DATA_FEATURE_LEVELS fl = {};
        fl.NumFeatureLevels = ARRAYSIZE(levels);
        fl.pFeatureLevelsRequested = levels;
        if (SUCCEEDED(_dev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl))))
            _info.featureLevel = fl.MaxSupportedFeatureLevel;
    }
    if (SUCCEEDED(_dev.As(&_infoQueue)) && _infoQueue)
    {
        D3D12_MESSAGE_SEVERITY keep[] = {D3D12_MESSAGE_SEVERITY_CORRUPTION, D3D12_MESSAGE_SEVERITY_ERROR,
                                         D3D12_MESSAGE_SEVERITY_WARNING};
        // the engine clears with whatever colour the scene wants
        D3D12_MESSAGE_ID drop[] = {D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
                                   D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE};
        D3D12_INFO_QUEUE_FILTER filter = {};
        filter.AllowList.NumSeverities = ARRAYSIZE(keep);
        filter.AllowList.pSeverityList = keep;
        filter.DenyList.NumIDs = ARRAYSIZE(drop);
        filter.DenyList.pIDList = drop;
        _infoQueue->PushStorageFilter(&filter);
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&_queue))))
        return false;
    for (Frame& f : _frames)
        if (FAILED(_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc))))
            return false;
    if (FAILED(_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _frames[0].alloc.Get(), nullptr,
                                       IID_PPV_ARGS(&_list))) ||
        FAILED(_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence))))
        return false;
    _fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!_fenceEvent)
        return false;

    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory.As(&factory5)))
    {
        BOOL allow = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
            _tearing = allow != FALSE;
    }

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = static_cast<UINT>(std::max(1, width));
    sd.Height = static_cast<UINT>(std::max(1, height));
    sd.Format = BackBufferFormat;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = FramesInFlight;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = _tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ComPtr<IDXGISwapChain1> swap1;
    if (FAILED(factory->CreateSwapChainForHwnd(_queue.Get(), hwnd, &sd, nullptr, nullptr, &swap1)) ||
        FAILED(swap1.As(&_swap)))
        return false;
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

    // Descriptors: views are made in CPU-side pools; the shaders read tables copied from them.
    if (!_rtvPool.Init(_dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1024) ||
        !_dsvPool.Init(_dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 256) ||
        !_srvPool.Init(_dev.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 65536))
        return false;
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = ExternalTextures + FramesInFlight * TablesPerFrame * NumTextureSlots;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&_gpuSrv))))
            return false;
        _gpuSrvStep = _dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (UINT i = ExternalTextures; i-- > 0;)
            _externalFree.push_back(i);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        hd.NumDescriptors = SamplerHeapSize;
        if (FAILED(_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&_gpuSamplers))))
            return false;
        _samplerStep = _dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    }

    // One root signature for every pipeline: the two register blocks, a draw's textures, its samplers.
    {
        D3D12_DESCRIPTOR_RANGE textures = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, NumTextureSlots, 0, 0, 0};
        D3D12_DESCRIPTOR_RANGE samplers = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, SamplerTableSize, 0, 0, 0};
        D3D12_ROOT_PARAMETER p[4] = {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[0].Descriptor.ShaderRegister = 0;
        p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[1].Descriptor.ShaderRegister = 0;
        p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[2].DescriptorTable.NumDescriptorRanges = 1;
        p[2].DescriptorTable.pDescriptorRanges = &textures;
        p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[3].DescriptorTable.NumDescriptorRanges = 1;
        p[3].DescriptorTable.pDescriptorRanges = &samplers;
        p[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd = {};
        rd.NumParameters = ARRAYSIZE(p);
        rd.pParameters = p;
        rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
            FAILED(_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&_root))))
            return false;
    }
    s_live = this;
    BeginList();

    // One zeroed vector for vertex inputs a declaration does not provide (the same for every vertex).
    {
        const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
        const D3D12_RESOURCE_DESC bd = BufferDesc(16);
        if (FAILED(_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr, IID_PPV_ARGS(&_zeroVB))))
            return false;
        void* p = nullptr;
        const D3D12_RANGE none = {0, 0};
        if (FAILED(_zeroVB->Map(0, &none, &p)))
            return false;
        std::memset(p, 0, 16);
        _zeroVB->Unmap(0, nullptr);
    }

    // Opaque black for texture stages with nothing bound, and empty views for unused slots.
    {
        const D3D12_RESOURCE_DESC td = TextureDesc(1, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, 1, D3D12_RESOURCE_FLAG_NONE);
        if (FAILED(CreateTracked(td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, _emptyStage)))
            return false;
        const uint32_t black = 0xFF000000u;
        const UploadAlloc a = AllocUpload(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (!a.ptr)
            return false;
        std::memcpy(a.ptr, &black, 4);
        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = _emptyStage.res.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = a.res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = a.offset;
        src.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
        _list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(_emptyStage, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        _emptyStageSrv = _srvPool.Alloc();
        _dev->CreateShaderResourceView(_emptyStage.res.Get(), nullptr, _srvPool.At(_emptyStageSrv));

        D3D12_SHADER_RESOURCE_VIEW_DESC nd = {};
        nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nd.Texture2D.MipLevels = 1;
        _nullSrv = _srvPool.Alloc();
        _dev->CreateShaderResourceView(nullptr, &nd, _srvPool.At(_nullSrv));
        nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        _nullSrvMS = _srvPool.Alloc();
        _dev->CreateShaderResourceView(nullptr, &nd, _srvPool.At(_nullSrvMS));
        _depthCopySrv = _srvPool.Alloc();
    }

    // Initial render states the drawing code relies on.
    _rs.fill(0);
    _rs[GXRS_ZENABLE] = GXZB_TRUE;
    _rs[GXRS_ZWRITEENABLE] = TRUE;
    _rs[GXRS_ZFUNC] = GXCMP_LESSEQUAL;
    _rs[GXRS_CULLMODE] = GXCULL_CCW;
    _rs[GXRS_SRCBLEND] = GXBLEND_ONE;
    _rs[GXRS_DESTBLEND] = GXBLEND_ZERO;
    _rs[GXRS_COLORWRITEENABLE] = 0xF;
    _rs[GXRS_STENCILFUNC] = GXCMP_ALWAYS;
    _rs[GXRS_STENCILFAIL] = GXSTENCILOP_KEEP;
    _rs[GXRS_STENCILZFAIL] = GXSTENCILOP_KEEP;
    _rs[GXRS_STENCILPASS] = GXSTENCILOP_KEEP;
    _rs[GXRS_STENCILMASK] = 0xFFFFFFFF;
    _rs[GXRS_STENCILWRITEMASK] = 0xFFFFFFFF;
    _rs[GXRS_MULTISAMPLEANTIALIAS] = TRUE;

    _back = new Surface;
    _backDepth = new Surface;
    _back->_trk = &_back->_own;
    _backDepth->_trk = &_backDepth->_own;
    _back->_rtv = _rtvPool.Alloc();
    _backDepth->_dsv = _dsvPool.Alloc();
    if (!CreateBackBuffer(width, height, msaaSamples))
        return false;
    _rt = _back;
    _rt->AddRef();
    _ds = _backDepth;
    _ds->AddRef();
    _viewport = {0, 0, static_cast<DWORD>(_w), static_cast<DWORD>(_h), 0.0f, 1.0f};
    return true;
}

Device::~Device()
{
    if (_queue && _fence && _list)
    {
        // let the GPU finish what was submitted; what is still being recorded is dropped
        _list->Close();
        const UINT64 v = ++_fenceValue;
        if (SUCCEEDED(_queue->Signal(_fence.Get(), v)))
            WaitFor(v);
    }
    for (Stage& s : _stage)
        if (s.tex)
        {
            s.tex->Release();
            s.tex = nullptr;
        }
    if (_rt)
        _rt->Release();
    if (_ds)
        _ds->Release();
    if (_back)
        _back->Release();
    if (_backDepth)
        _backDepth->Release();
    if (s_live == this)
        s_live = nullptr;
    // buffers that outlive the device find no device and leave their pieces alone
    for (BufferSlab* s : _slabs)
        delete s;
    _slabs.clear();
    if (_fenceEvent)
        CloseHandle(_fenceEvent);
}

int Device::ClampMsaaSamples(int samples) const
{
    for (int s = samples; s >= 2; s /= 2)
    {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS c = {}, d = {};
        c.Format = BackBufferFormat;
        c.SampleCount = static_cast<UINT>(s);
        d.Format = kDepthView;
        d.SampleCount = static_cast<UINT>(s);
        if (SUCCEEDED(_dev->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &c, sizeof(c))) &&
            c.NumQualityLevels > 0 &&
            SUCCEEDED(_dev->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &d, sizeof(d))) &&
            d.NumQualityLevels > 0)
            return s;
    }
    return 0;
}

bool Device::SupportsFormat(GxFormat format, bool renderTarget) const
{
    bool expand = false;
    const DXGI_FORMAT f = MapFormat(format, renderTarget, &expand);
    if (f == DXGI_FORMAT_UNKNOWN)
        return false;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {};
    fs.Format = f;
    if (FAILED(_dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))))
        return false;
    const UINT need = D3D12_FORMAT_SUPPORT1_TEXTURE2D | (renderTarget ? D3D12_FORMAT_SUPPORT1_RENDER_TARGET : 0);
    return (fs.Support1 & need) == need;
}

HRESULT Device::CreateTracked(const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES state,
                              const D3D12_CLEAR_VALUE* clear, Tracked& out)
{
    const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    out.res.Reset();
    const HRESULT hr =
        _dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state, clear, IID_PPV_ARGS(&out.res));
    out.state = state;
    return hr;
}

bool Device::CreateBackBuffer(int width, int height, int msaaSamples)
{
    _w = std::max(1, width);
    _h = std::max(1, height);
    _msaa = ClampMsaaSamples(msaaSamples);
    const UINT samples = _msaa > 1 ? static_cast<UINT>(_msaa) : 1;

    D3D12_CLEAR_VALUE cv = {};
    cv.Format = BackBufferFormat;
    cv.Color[3] = 1.0f;
    if (FAILED(CreateTracked(TextureDesc(static_cast<UINT>(_w), static_cast<UINT>(_h), 1, BackBufferFormat, samples,
                                         D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                             D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, _back->_own)))
        return false;
    _dev->CreateRenderTargetView(_back->_own.res.Get(), nullptr, _rtvPool.At(_back->_rtv));
    _back->_w = static_cast<UINT>(_w);
    _back->_h = static_cast<UINT>(_h);
    _back->_samples = samples;
    _back->_format = GXFMT_X8R8G8B8;
    _back->_dxgi = BackBufferFormat;

    D3D12_CLEAR_VALUE dv = {};
    dv.Format = kDepthView;
    dv.DepthStencil.Depth = 1.0f;
    if (FAILED(CreateTracked(TextureDesc(static_cast<UINT>(_w), static_cast<UINT>(_h), 1, kDepthResource, samples,
                                         D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL),
                             D3D12_RESOURCE_STATE_DEPTH_WRITE, &dv, _backDepth->_own)))
        return false;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsd = {};
    dsd.Format = kDepthView;
    dsd.ViewDimension = samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
    _dev->CreateDepthStencilView(_backDepth->_own.res.Get(), &dsd, _dsvPool.At(_backDepth->_dsv));
    _backDepth->_w = static_cast<UINT>(_w);
    _backDepth->_h = static_cast<UINT>(_h);
    _backDepth->_samples = samples;
    _backDepth->_format = GXFMT_D24S8;
    _backDepth->_dxgi = kDepthView;
    _targetsDirty = true;
    return true;
}

void Device::ReleaseBackBuffer()
{
    // only with the GPU idle (Reset): nothing uses them any more
    _back->_own.res.Reset();
    _backDepth->_own.res.Reset();
}

HRESULT Device::Reset(int width, int height, int msaaSamples, bool vsync)
{
    _vsync = vsync;
    SubmitAndWait();
    ReleaseBackBuffer();
    const HRESULT hr = _swap->ResizeBuffers(0, static_cast<UINT>(std::max(1, width)),
                                            static_cast<UINT>(std::max(1, height)), DXGI_FORMAT_UNKNOWN,
                                            _tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(hr))
        return hr;
    if (!CreateBackBuffer(width, height, msaaSamples))
        return E_FAIL;
    // Targets go back to the back buffer.
    SetRenderTarget(0, _back);
    SetDepthStencilSurface(_backDepth);
    _viewport = {0, 0, static_cast<DWORD>(_w), static_cast<DWORD>(_h), 0.0f, 1.0f};
    _viewportDirty = true;
    InvalidateBindings();
    return S_OK;
}

// ---------------------------------------------------------------------------
// Device: frames
// ---------------------------------------------------------------------------

void Device::BeginList()
{
    ID3D12DescriptorHeap* heaps[2] = {_gpuSrv.Get(), _gpuSamplers.Get()};
    _list->SetDescriptorHeaps(2, heaps);
    _list->SetGraphicsRootSignature(_root.Get());
    _targetsDirty = _viewportDirty = true;
    _pipelineDirty = _stencilRefDirty = true;
    _pipeline = nullptr;
    _topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    _texturesDirty = _samplersDirty = true;
    _boundTableValid = false;
    _vsConstDirty = _psConstDirty = true;
}

void Device::InvalidateBindings()
{
    BeginList();
}

void Device::WaitFor(UINT64 fenceValue)
{
    if (_fence->GetCompletedValue() >= fenceValue)
        return;
    if (SUCCEEDED(_fence->SetEventOnCompletion(fenceValue, _fenceEvent)))
        WaitForSingleObject(_fenceEvent, INFINITE);
}

void Device::RecycleFrame(Frame& f)
{
    f.dead.clear();
    for (const Frame::SlabRange& r : f.deadRanges)
        SlabFree(r.slab, r.offset, r.size);
    f.deadRanges.clear();
    for (DynBlock& b : f.deadDyn)
        _dynPool[b.size].push_back(std::move(b));
    f.deadDyn.clear();
    // odd-sized blocks were for one big upload; keep a reasonable number of the regular ones
    f.pages.erase(std::remove_if(f.pages.begin(), f.pages.end(), [](const UploadPage& p) { return p.size != kUploadPage; }),
                  f.pages.end());
    if (f.pages.size() > 32)
        f.pages.resize(32);
    f.page = 0;
    f.pageUsed = 0;
    f.uploaded = 0;
    f.textureTables = 0;
}

void Device::SubmitAndWait()
{
    _list->Close();
    ID3D12CommandList* lists[] = {_list.Get()};
    _queue->ExecuteCommandLists(1, lists);
    const UINT64 v = ++_fenceValue;
    _queue->Signal(_fence.Get(), v);
    WaitFor(v);
    // everything submitted so far is done: every frame can let go of what it kept
    for (Frame& f : _frames)
        RecycleFrame(f);
    Frame& cur = _frames[_frame];
    cur.alloc->Reset();
    _list->Reset(cur.alloc.Get(), nullptr);
    BeginList();
}

HRESULT Device::Present(OverlayFn overlay)
{
    HRESULT hr = E_FAIL;
    {
        ComPtr<ID3D12Resource> target;
        if (FAILED(_swap->GetBuffer(_swap->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&target))))
            return E_FAIL;
        Tracked swapBuffer;
        swapBuffer.res = target;
        swapBuffer.state = D3D12_RESOURCE_STATE_PRESENT;
        Tracked& back = *_back->_trk;
        if (_back->_samples > 1)
        {
            Transition(back, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            Transition(swapBuffer, D3D12_RESOURCE_STATE_RESOLVE_DEST);
            _list->ResolveSubresource(target.Get(), 0, back.res.Get(), 0, BackBufferFormat);
        }
        else
        {
            Transition(back, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(swapBuffer, D3D12_RESOURCE_STATE_COPY_DEST);
            _list->CopyResource(target.Get(), back.res.Get());
        }
        if (overlay)
        {
            if (_swapRtv == kNone)
                _swapRtv = _rtvPool.Alloc();
            if (_swapRtv != kNone)
            {
                Transition(swapBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
                const D3D12_CPU_DESCRIPTOR_HANDLE rtv = _rtvPool.At(_swapRtv);
                _dev->CreateRenderTargetView(target.Get(), nullptr, rtv);
                _list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                const D3D12_VIEWPORT vp = {0, 0, static_cast<float>(_w), static_cast<float>(_h), 0.0f, 1.0f};
                _list->RSSetViewports(1, &vp);
                const D3D12_RECT sc = {0, 0, _w, _h};
                _list->RSSetScissorRects(1, &sc);
                overlay(_list.Get());
            }
        }
        Transition(swapBuffer, D3D12_RESOURCE_STATE_PRESENT);
        _list->Close();
        ID3D12CommandList* lists[] = {_list.Get()};
        _queue->ExecuteCommandLists(1, lists);
    }
    const UINT sync = _vsync ? 1 : 0;
    hr = _swap->Present(sync, (!_vsync && _tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0);

    const UINT64 v = ++_fenceValue;
    _queue->Signal(_fence.Get(), v);
    _frames[_frame].fence = v;
    _frame = (_frame + 1) % FramesInFlight;
    Frame& next = _frames[_frame];
    WaitFor(next.fence); // its work of three frames ago
    RecycleFrame(next);
    next.alloc->Reset();
    _list->Reset(next.alloc.Get(), nullptr);
    BeginList();
    return hr;
}

bool Device::BindBackBufferForExternalDraw()
{
    if (_back->_samples > 1)
        return false;
    Transition(*_back->_trk, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = _rtvPool.At(_back->_rtv);
    _list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT vp = {0, 0, static_cast<float>(_w), static_cast<float>(_h), 0.0f, 1.0f};
    _list->RSSetViewports(1, &vp);
    const D3D12_RECT sc = {0, 0, _w, _h};
    _list->RSSetScissorRects(1, &sc);
    _targetsDirty = true;
    return true;
}

Device::UploadAlloc Device::AllocUpload(UINT size, UINT align)
{
    Frame& f = _frames[_frame];
    for (;;)
    {
        if (f.page < f.pages.size())
        {
            UploadPage& p = f.pages[f.page];
            const UINT off = AlignUp(f.pageUsed, align);
            if (off + size <= p.size)
            {
                f.pageUsed = off + size;
                f.uploaded += size;
                UploadAlloc a;
                a.res = p.res.Get();
                a.offset = off;
                a.ptr = p.ptr + off;
                a.va = p.res->GetGPUVirtualAddress() + off;
                return a;
            }
            f.page++;
            f.pageUsed = 0;
            continue;
        }
        UploadPage p;
        p.size = std::max(kUploadPage, size);
        const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
        const D3D12_RESOURCE_DESC bd = BufferDesc(p.size);
        void* ptr = nullptr;
        const D3D12_RANGE none = {0, 0};
        if (FAILED(_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr, IID_PPV_ARGS(&p.res))) ||
            FAILED(p.res->Map(0, &none, &ptr)))
            return {};
        p.ptr = static_cast<uint8_t*>(ptr);
        f.pages.push_back(std::move(p));
        f.page = f.pages.size() - 1;
        f.pageUsed = 0;
    }
}

void Device::Retire(ComPtr<ID3D12Resource>& res)
{
    if (res)
        _frames[_frame].dead.push_back(std::move(res));
    res.Reset();
}

Device::DynBlock Device::AcquireDyn(UINT size)
{
    std::vector<DynBlock>& pool = _dynPool[size];
    if (!pool.empty())
    {
        DynBlock b = std::move(pool.back());
        pool.pop_back();
        return b;
    }
    DynBlock b;
    b.size = size;
    const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC bd = BufferDesc(size);
    void* ptr = nullptr;
    const D3D12_RANGE none = {0, 0};
    if (FAILED(_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&b.res))) ||
        FAILED(b.res->Map(0, &none, &ptr)))
        return {};
    b.ptr = static_cast<uint8_t*>(ptr);
    return b;
}

void Device::RetireDyn(ComPtr<ID3D12Resource>& res, uint8_t* ptr, UINT size)
{
    if (!res)
        return;
    DynBlock b;
    b.res = std::move(res);
    b.ptr = ptr;
    b.size = size;
    _frames[_frame].deadDyn.push_back(std::move(b));
    res.Reset();
}

void Device::Transition(Tracked& t, D3D12_RESOURCE_STATES state)
{
    if (!t.res || t.state == state)
        return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t.res.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = t.state;
    b.Transition.StateAfter = state;
    _list->ResourceBarrier(1, &b);
    t.state = state;
}

void Device::UploadLevel(Texture* tex, UINT level, const void* data, UINT pitch)
{
    if (!tex->_trk.res || !data)
        return;
    if (_frames[_frame].uploaded > kUploadFlush) // a loading screen: do not pile the whole world up
        SubmitAndWait();
    const D3D12_RESOURCE_DESC desc = tex->_trk.res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT rows = 0;
    UINT64 rowBytes = 0, total = 0;
    _dev->GetCopyableFootprints(&desc, level, 1, 0, &fp, &rows, &rowBytes, &total);
    const UploadAlloc a = AllocUpload(static_cast<UINT>(total), D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (!a.ptr)
        return;
    const size_t copy = static_cast<size_t>(std::min<UINT64>(pitch, rowBytes));
    for (UINT y = 0; y < rows; y++)
        std::memcpy(a.ptr + static_cast<size_t>(y) * fp.Footprint.RowPitch,
                    static_cast<const uint8_t*>(data) + static_cast<size_t>(y) * pitch, copy);
    fp.Offset = a.offset;
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = tex->_trk.res.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = level;
    src.pResource = a.res;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    Transition(tex->_trk, D3D12_RESOURCE_STATE_COPY_DEST);
    _list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(tex->_trk, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void Device::UploadBuffer(Buffer* buf)
{
    if ((!buf->_trk.res && !buf->_slab) || buf->_shadow.empty())
        return;
    if (_frames[_frame].uploaded > kUploadFlush)
        SubmitAndWait();
    const UploadAlloc a = AllocUpload(buf->_size, 16);
    if (!a.ptr)
        return;
    std::memcpy(a.ptr, buf->_shadow.data(), buf->_size);
    if (buf->_slab)
    {
        // The whole block changes state, not the piece: it stays open for copying while buffers
        // are being filled one after another and is made readable once, before the next draw.
        Transition(buf->_slab->trk, D3D12_RESOURCE_STATE_COPY_DEST);
        buf->_slab->copying = true;
        _slabsCopying = true;
        _list->CopyBufferRegion(buf->_slab->trk.res.Get(), buf->_slabOffset, a.res, a.offset, buf->_size);
        return;
    }
    Transition(buf->_trk, D3D12_RESOURCE_STATE_COPY_DEST);
    _list->CopyBufferRegion(buf->_trk.res.Get(), 0, a.res, a.offset, buf->_size);
    Transition(buf->_trk,
               buf->_index ? D3D12_RESOURCE_STATE_INDEX_BUFFER : D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
}

// ---------------------------------------------------------------------------
// Device: resources
// ---------------------------------------------------------------------------

DXGI_FORMAT Device::MapFormat(GxFormat format, bool renderTarget, bool* expand)
{
    *expand = false;
    switch (format)
    {
        case GXFMT_A8R8G8B8:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case GXFMT_X8R8G8B8:
            return renderTarget ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_B8G8R8X8_UNORM;
        case GXFMT_R5G6B5:
            return DXGI_FORMAT_B5G6R5_UNORM;
        case GXFMT_A1R5G5B5:
            return DXGI_FORMAT_B5G5R5A1_UNORM;
        case GXFMT_A4R4G4B4:
            return DXGI_FORMAT_B4G4R4A4_UNORM;
        case GXFMT_DXT1:
            return DXGI_FORMAT_BC1_UNORM;
        case GXFMT_DXT3:
            return DXGI_FORMAT_BC2_UNORM;
        case GXFMT_DXT5:
            return DXGI_FORMAT_BC3_UNORM;
        case GXFMT_R32F:
            return DXGI_FORMAT_R32_FLOAT;
        case GXFMT_A16B16G16R16F:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case GXFMT_L8:
        case GXFMT_A8L8:
            *expand = true; // no luminance formats
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case GXFMT_D24S8:
        case GXFMT_D24X8:
            return kDepthView;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

HRESULT Device::CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, GxFormat format, GxPool, Texture** out,
                              HANDLE*)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    const bool rt = (usage & GXUSAGE_RENDERTARGET) != 0;
    bool expand = false;
    DXGI_FORMAT dxgi = MapFormat(format, rt, &expand);
    if (dxgi == DXGI_FORMAT_UNKNOWN)
        return E_INVALIDARG;

    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {};
    fs.Format = dxgi;
    const bool known = SUCCEEDED(_dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs)));
    // The 16-bit packed formats are optional: expand when missing.
    if (!expand && (format == GXFMT_R5G6B5 || format == GXFMT_A1R5G5B5 || format == GXFMT_A4R4G4B4) &&
        (!known || !(fs.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D)))
    {
        expand = true;
        dxgi = DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    else if (rt && (!known || !(fs.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET)))
    {
        return E_FAIL;
    }

    if (levels == 0)
    {
        levels = 1;
        for (UINT m = std::max(w, h); m > 1; m >>= 1)
            levels++;
    }

    Texture* t = new Texture;
    t->_w = w;
    t->_h = h;
    t->_levels = levels;
    t->_usage = usage;
    t->_format = format;
    t->_dxgi = dxgi;
    t->_expand = expand;
    D3D12_CLEAR_VALUE cv = {};
    cv.Format = dxgi;
    HRESULT hr = CreateTracked(
        TextureDesc(w, h, levels, dxgi, 1, rt ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, rt ? &cv : nullptr, t->_trk);
    if (SUCCEEDED(hr))
    {
        t->_srv = _srvPool.Alloc();
        if (t->_srv == kNone)
            hr = E_OUTOFMEMORY;
        else
            _dev->CreateShaderResourceView(t->_trk.res.Get(), nullptr, _srvPool.At(t->_srv));
    }
    if (FAILED(hr))
    {
        t->Release();
        return hr;
    }
    *out = t;
    return S_OK;
}

HRESULT Device::CreateVertexBuffer(UINT length, DWORD usage, DWORD, GxPool, VertexBuffer** out, HANDLE*)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    Buffer* b = new Buffer;
    b->_size = length;
    b->_dynamic = (usage & GXUSAGE_DYNAMIC) != 0;
    HRESULT hr = S_OK;
    if (b->_dynamic)
    {
        DynBlock blk = AcquireDyn(length);
        b->_dyn = blk.res;
        b->_dynPtr = blk.ptr;
        hr = b->_dyn ? S_OK : E_OUTOFMEMORY;
    }
    else if (length > 0 && length <= kSlabLargest)
    {
        hr = SlabAlloc(length, b->_slab, b->_slabOffset) ? S_OK : E_OUTOFMEMORY;
    }
    else
    {
        hr = CreateTracked(BufferDesc(length), D3D12_RESOURCE_STATE_COMMON, nullptr, b->_trk);
    }
    if (FAILED(hr))
    {
        b->Release();
        return hr;
    }
    *out = b;
    return S_OK;
}

HRESULT Device::CreateIndexBuffer(UINT length, DWORD usage, GxFormat format, GxPool pool, IndexBuffer** out,
                                  HANDLE* shared)
{
    (void)format;
    const HRESULT hr = CreateVertexBuffer(length, usage, 0, pool, out, shared);
    if (SUCCEEDED(hr))
        (*out)->_index = true;
    return hr;
}

HRESULT Device::CreateVertexDeclaration(const GxVertexElement* elements, VertexDecl** out)
{
    if (!elements || !out)
        return E_POINTER;
    VertexDecl* d = new VertexDecl;
    d->_id = _nextDeclId++;
    for (const GxVertexElement* e = elements; e->Stream != 0xFF; e++)
    {
        VertexDecl::Element el = {};
        switch (e->Usage)
        {
            case GXDECLUSAGE_POSITION:
                el.semantic = "POSITION";
                break;
            case GXDECLUSAGE_NORMAL:
                el.semantic = "NORMAL";
                break;
            case GXDECLUSAGE_COLOR:
                el.semantic = "COLOR";
                break;
            default:
                el.semantic = "TEXCOORD";
                break;
        }
        switch (e->Type)
        {
            case GXDECLTYPE_FLOAT1:
                el.format = DXGI_FORMAT_R32_FLOAT;
                break;
            case GXDECLTYPE_FLOAT2:
                el.format = DXGI_FORMAT_R32G32_FLOAT;
                break;
            case GXDECLTYPE_FLOAT3:
                el.format = DXGI_FORMAT_R32G32B32_FLOAT;
                break;
            case GXDECLTYPE_FLOAT4:
                el.format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                break;
            case GXDECLTYPE_COLOR:
                el.format = DXGI_FORMAT_B8G8R8A8_UNORM; // ARGB in memory order B,G,R,A
                break;
            default:
                d->Release();
                return E_INVALIDARG;
        }
        el.index = e->UsageIndex;
        el.offset = e->Offset;
        d->_elements.push_back(el);
    }
    *out = d;
    return S_OK;
}

HRESULT Device::CreateVertexShader(const void* code, size_t size, VertexShader** out)
{
    if (!code || !out)
        return E_POINTER;
    ComPtr<ID3D12ShaderReflection> refl;
    const HRESULT hr = D3DReflect(code, size, IID_PPV_ARGS(&refl));
    if (FAILED(hr))
        return hr;
    VertexShader* s = new VertexShader;
    s->_id = _nextShaderId++;
    s->_code.assign(static_cast<const uint8_t*>(code), static_cast<const uint8_t*>(code) + size);
    D3D12_SHADER_DESC sd = {};
    refl->GetDesc(&sd);
    for (UINT i = 0; i < sd.InputParameters; i++)
    {
        D3D12_SIGNATURE_PARAMETER_DESC pd = {};
        if (SUCCEEDED(refl->GetInputParameterDesc(i, &pd)) && pd.SystemValueType == D3D_NAME_UNDEFINED)
            s->_inputs.push_back({pd.SemanticName, pd.SemanticIndex});
    }
    *out = s;
    return S_OK;
}

HRESULT Device::CreatePixelShader(const void* code, size_t size, PixelShader** out)
{
    if (!code || !out)
        return E_POINTER;
    ComPtr<ID3D12ShaderReflection> refl;
    const HRESULT hr = D3DReflect(code, size, IID_PPV_ARGS(&refl));
    if (FAILED(hr))
        return hr;
    PixelShader* s = new PixelShader;
    s->_id = _nextShaderId++;
    s->_code.assign(static_cast<const uint8_t*>(code), static_cast<const uint8_t*>(code) + size);
    for (int8_t& t : s->_texSlot)
        t = -1;
    // fxc keeps the sampler registers of legacy sampler2D declarations but packs
    // the textures: pair them up by name.
    D3D12_SHADER_DESC sd = {};
    refl->GetDesc(&sd);
    for (UINT i = 0; i < sd.BoundResources; i++)
    {
        D3D12_SHADER_INPUT_BIND_DESC smp = {};
        if (FAILED(refl->GetResourceBindingDesc(i, &smp)) || smp.Type != D3D_SIT_SAMPLER || smp.BindPoint >= 16)
            continue;
        for (UINT j = 0; j < sd.BoundResources; j++)
        {
            D3D12_SHADER_INPUT_BIND_DESC tex = {};
            if (SUCCEEDED(refl->GetResourceBindingDesc(j, &tex)) && tex.Type == D3D_SIT_TEXTURE &&
                std::strcmp(tex.Name, smp.Name) == 0)
            {
                s->_texSlot[smp.BindPoint] = static_cast<int8_t>(tex.BindPoint);
                break;
            }
        }
    }
    *out = s;
    return S_OK;
}

HRESULT Device::CreateRenderTarget(UINT w, UINT h, GxFormat format, GxMultisample ms, DWORD, BOOL, Surface** out,
                                   HANDLE*)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    bool expand = false;
    const DXGI_FORMAT dxgi = MapFormat(format, true, &expand);
    if (dxgi == DXGI_FORMAT_UNKNOWN || expand)
        return E_INVALIDARG;
    const UINT samples = ms > 1 ? static_cast<UINT>(ms) : 1;
    Surface* s = new Surface;
    s->_trk = &s->_own;
    D3D12_CLEAR_VALUE cv = {};
    cv.Format = dxgi;
    HRESULT hr = CreateTracked(TextureDesc(w, h, 1, dxgi, samples, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                               D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, s->_own);
    if (SUCCEEDED(hr))
    {
        s->_rtv = _rtvPool.Alloc();
        if (s->_rtv == kNone)
            hr = E_OUTOFMEMORY;
        else
            _dev->CreateRenderTargetView(s->_own.res.Get(), nullptr, _rtvPool.At(s->_rtv));
    }
    if (FAILED(hr))
    {
        s->Release();
        return hr;
    }
    s->_w = w;
    s->_h = h;
    s->_samples = samples;
    s->_format = format;
    s->_dxgi = dxgi;
    *out = s;
    return S_OK;
}

HRESULT Device::CreateDepthStencilSurface(UINT w, UINT h, GxFormat format, GxMultisample ms, DWORD, BOOL,
                                          Surface** out, HANDLE*)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    const UINT samples = ms > 1 ? static_cast<UINT>(ms) : 1;
    Surface* s = new Surface;
    s->_trk = &s->_own;
    D3D12_CLEAR_VALUE dv = {};
    dv.Format = kDepthView;
    dv.DepthStencil.Depth = 1.0f;
    HRESULT hr = CreateTracked(TextureDesc(w, h, 1, kDepthResource, samples, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL),
                               D3D12_RESOURCE_STATE_DEPTH_WRITE, &dv, s->_own);
    if (SUCCEEDED(hr))
    {
        s->_dsv = _dsvPool.Alloc();
        if (s->_dsv == kNone)
        {
            hr = E_OUTOFMEMORY;
        }
        else
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC dsd = {};
            dsd.Format = kDepthView;
            dsd.ViewDimension = samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
            _dev->CreateDepthStencilView(s->_own.res.Get(), &dsd, _dsvPool.At(s->_dsv));
        }
    }
    if (FAILED(hr))
    {
        s->Release();
        return hr;
    }
    s->_w = w;
    s->_h = h;
    s->_samples = samples;
    s->_format = format;
    s->_dxgi = kDepthResource;
    *out = s;
    return S_OK;
}

HRESULT Device::CreateOffscreenPlainSurface(UINT w, UINT h, GxFormat format, GxPool, Surface** out, HANDLE*)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    const UINT bpp = BytesPerPixel(format);
    if (bpp == 0)
        return E_INVALIDARG;
    Surface* s = new Surface;
    s->_w = w;
    s->_h = h;
    s->_format = format;
    s->_sysPitch = w * bpp;
    s->_sysmem.assign(static_cast<size_t>(s->_sysPitch) * h, 0);
    *out = s;
    return S_OK;
}

// ---------------------------------------------------------------------------
// Device: state
// ---------------------------------------------------------------------------

HRESULT Device::SetRenderState(GxRenderState state, DWORD value)
{
    const unsigned i = static_cast<unsigned>(state);
    if (i >= _rs.size())
        return S_OK;
    if (_rs[i] == value)
        return S_OK;
    _rs[i] = value;
    switch (state)
    {
        case GXRS_ALPHABLENDENABLE:
        case GXRS_SRCBLEND:
        case GXRS_DESTBLEND:
        case GXRS_COLORWRITEENABLE:
        case GXRS_ZENABLE:
        case GXRS_ZWRITEENABLE:
        case GXRS_ZFUNC:
        case GXRS_STENCILENABLE:
        case GXRS_STENCILFUNC:
        case GXRS_STENCILFAIL:
        case GXRS_STENCILZFAIL:
        case GXRS_STENCILPASS:
        case GXRS_STENCILMASK:
        case GXRS_STENCILWRITEMASK:
        case GXRS_CULLMODE:
        case GXRS_DEPTHBIAS:
        case GXRS_SLOPESCALEDEPTHBIAS:
        case GXRS_MULTISAMPLEANTIALIAS:
            _pipelineDirty = true;
            break;
        case GXRS_STENCILREF:
            _stencilRefDirty = true;
            break;
        default: // fixed-function leftovers (fog, lighting, clipping, alpha test, sRGB write) and the
                 // scissor switch (no rectangle is ever set): nothing to do
            break;
    }
    return S_OK;
}

HRESULT Device::SetSamplerState(DWORD stage, GxSamplerState type, DWORD value)
{
    if (stage >= NumStages)
        return S_OK;
    Stage& s = _stage[stage];
    DWORD* slot = nullptr;
    switch (type)
    {
        case GXSAMP_ADDRESSU:
            slot = &s.addressU;
            break;
        case GXSAMP_ADDRESSV:
            slot = &s.addressV;
            break;
        case GXSAMP_MAGFILTER:
            slot = &s.magFilter;
            break;
        case GXSAMP_MINFILTER:
            slot = &s.minFilter;
            break;
        case GXSAMP_MIPFILTER:
            slot = &s.mipFilter;
            break;
        case GXSAMP_MAXANISOTROPY:
            slot = &s.maxAnisotropy;
            break;
        case GXSAMP_SRGBTEXTURE:
            s.srgb = value;
            return S_OK;
        default:
            return S_OK;
    }
    if (*slot != value)
    {
        *slot = value;
        _samplersDirty = true;
    }
    return S_OK;
}

HRESULT Device::GetSamplerState(DWORD stage, GxSamplerState type, DWORD* value) const
{
    if (!value || stage >= NumStages)
        return E_INVALIDARG;
    const Stage& s = _stage[stage];
    switch (type)
    {
        case GXSAMP_ADDRESSU:
            *value = s.addressU;
            break;
        case GXSAMP_ADDRESSV:
            *value = s.addressV;
            break;
        case GXSAMP_MAGFILTER:
            *value = s.magFilter;
            break;
        case GXSAMP_MINFILTER:
            *value = s.minFilter;
            break;
        case GXSAMP_MIPFILTER:
            *value = s.mipFilter;
            break;
        case GXSAMP_MAXANISOTROPY:
            *value = s.maxAnisotropy;
            break;
        case GXSAMP_SRGBTEXTURE:
            *value = s.srgb;
            break;
        default:
            *value = 0;
            break;
    }
    return S_OK;
}

HRESULT Device::SetTexture(DWORD stage, Texture* tex)
{
    if (stage >= NumStages)
        return S_OK;
    Stage& s = _stage[stage];
    if (s.tex == tex)
        return S_OK;
    if (tex)
        tex->AddRef();
    if (s.tex)
        s.tex->Release();
    s.tex = tex;
    _texturesDirty = true;
    return S_OK;
}

HRESULT Device::SetViewport(const GxViewport* vp)
{
    if (!vp)
        return E_POINTER;
    _viewport = *vp;
    _viewportDirty = true;
    return S_OK;
}

HRESULT Device::GetViewport(GxViewport* vp) const
{
    if (!vp)
        return E_POINTER;
    *vp = _viewport;
    return S_OK;
}

HRESULT Device::SetVertexShaderConstantF(UINT reg, const float* data, UINT count)
{
    if (!data || reg + count > NumConstRegisters)
        return E_INVALIDARG;
    std::memcpy(_vsConst[reg], data, static_cast<size_t>(count) * 16);
    _vsConstDirty = true;
    return S_OK;
}

HRESULT Device::SetPixelShaderConstantF(UINT reg, const float* data, UINT count)
{
    if (!data || reg + count > NumConstRegisters)
        return E_INVALIDARG;
    std::memcpy(_psConst[reg], data, static_cast<size_t>(count) * 16);
    _psConstDirty = true;
    return S_OK;
}

HRESULT Device::SetVertexShader(VertexShader* vs)
{
    if (_vs != vs)
    {
        _vs = vs;
        _pipelineDirty = true;
    }
    return S_OK;
}

HRESULT Device::SetPixelShader(PixelShader* ps)
{
    if (_ps != ps)
    {
        _ps = ps;
        _pipelineDirty = true;
        _texturesDirty = true; // texture slots depend on the shader
    }
    return S_OK;
}

HRESULT Device::SetVertexDeclaration(VertexDecl* decl)
{
    if (_decl != decl)
    {
        _decl = decl;
        _pipelineDirty = true;
    }
    return S_OK;
}

HRESULT Device::SetStreamSource(UINT stream, VertexBuffer* vb, UINT offset, UINT stride)
{
    if (stream != 0)
        return E_INVALIDARG;
    _streamVB = vb;
    _streamOffset = offset;
    _streamStride = stride;
    return S_OK;
}

HRESULT Device::SetIndices(IndexBuffer* ib)
{
    _indices = ib;
    return S_OK;
}

// ---------------------------------------------------------------------------
// Device: targets
// ---------------------------------------------------------------------------

HRESULT Device::SetRenderTarget(DWORD index, Surface* rt)
{
    if (index != 0 || !rt || rt->_rtv == kNone)
        return E_INVALIDARG;
    if (_rt != rt)
    {
        rt->AddRef();
        if (_rt)
            _rt->Release();
        _rt = rt;
    }
    _targetsDirty = true;
    // The viewport becomes the whole target.
    _viewport = {0, 0, rt->_w, rt->_h, 0.0f, 1.0f};
    _viewportDirty = true;
    return S_OK;
}

HRESULT Device::GetRenderTarget(DWORD index, Surface** out)
{
    if (index != 0 || !out)
        return E_INVALIDARG;
    *out = _rt;
    if (_rt)
        _rt->AddRef();
    return _rt ? S_OK : E_FAIL;
}

HRESULT Device::SetDepthStencilSurface(Surface* ds)
{
    if (_ds != ds)
    {
        if (ds)
            ds->AddRef();
        if (_ds)
            _ds->Release();
        _ds = ds;
    }
    _targetsDirty = true;
    return S_OK;
}

HRESULT Device::GetDepthStencilSurface(Surface** out)
{
    if (!out)
        return E_POINTER;
    *out = _ds;
    if (_ds)
        _ds->AddRef();
    return _ds ? S_OK : E_FAIL;
}

HRESULT Device::GetBackBuffer(UINT, UINT, GxBackBufferType, Surface** out)
{
    if (!out)
        return E_POINTER;
    _back->AddRef();
    *out = _back;
    return S_OK;
}

void Device::SetLocalShadowTexture(Texture* tex)
{
    // goes into the texture table of the next draw; never while it is the render target
    _localShadowTex = tex;
    _texturesDirty = true;
}

// A copy of the depth buffer in use, for shaders to read (soft particles, ambient occlusion,
// light culling): the buffer itself cannot be sampled while it is being written.  Read at
// slot DepthSlot (single-sampled) or DepthSlotMS (multisampled); the other slot is empty.
bool Device::SnapshotDepth()
{
    if (!_ds || !_ds->_trk || !_ds->_trk->res || _ds->_dsv == kNone)
        return false;
    const UINT samples = _ds->_samples > 1 ? _ds->_samples : 1;
    if (!_depthCopy.res || _depthCopyW != _ds->_w || _depthCopyH != _ds->_h || _depthCopySamples != samples)
    {
        Retire(_depthCopy.res);
        // a multisampled texture has to be a target of some kind
        if (FAILED(CreateTracked(TextureDesc(_ds->_w, _ds->_h, 1, kDepthResource, samples,
                                             D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL),
                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, _depthCopy)))
        {
            _depthCopy.res.Reset();
            _depthCopySamples = 0;
            _texturesDirty = true;
            return false;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = kDepthRead;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension = samples > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
        if (samples <= 1)
            sd.Texture2D.MipLevels = 1;
        _dev->CreateShaderResourceView(_depthCopy.res.Get(), &sd, _srvPool.At(_depthCopySrv));
        _depthCopyW = _ds->_w;
        _depthCopyH = _ds->_h;
        _depthCopySamples = samples;
    }
    Transition(*_ds->_trk, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(_depthCopy, D3D12_RESOURCE_STATE_COPY_DEST);
    _list->CopyResource(_depthCopy.res.Get(), _ds->_trk->res.Get());
    Transition(_depthCopy, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    _targetsDirty = true; // the depth buffer goes back to being written
    _texturesDirty = true;
    _boundTableValid = false; // same view, new contents under a new copy
    return true;
}

bool Device::TargetHasDepth() const
{
    return _ds && _rt && _ds->_dsv != kNone && _ds->_trk && _ds->_trk->res && _ds->_w == _rt->_w &&
           _ds->_h == _rt->_h && _ds->_samples == _rt->_samples;
}

void Device::ApplyTargets()
{
    if (_targetsDirty)
    {
        Transition(*_rt->_trk, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = _rtvPool.At(_rt->_rtv);
        // Depth must match the target's size and sample count; callers may leave a stale one bound.
        if (TargetHasDepth())
        {
            Transition(*_ds->_trk, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            const D3D12_CPU_DESCRIPTOR_HANDLE dsv = _dsvPool.At(_ds->_dsv);
            _list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        }
        else
        {
            _list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        }
        _targetsDirty = false;
        _viewportDirty = true;
        _pipelineDirty = true; // target formats are part of the pipeline
        _texturesDirty = true; // a texture cannot be sampled while it is the render target
    }
    if (_viewportDirty)
    {
        D3D12_VIEWPORT vp;
        vp.TopLeftX = static_cast<float>(_viewport.X);
        vp.TopLeftY = static_cast<float>(_viewport.Y);
        vp.Width = static_cast<float>(_viewport.Width);
        vp.Height = static_cast<float>(_viewport.Height);
        vp.MinDepth = _viewport.MinZ;
        vp.MaxDepth = _viewport.MaxZ;
        _list->RSSetViewports(1, &vp);
        const D3D12_RECT whole = {0, 0, static_cast<LONG>(_rt->_w), static_cast<LONG>(_rt->_h)};
        _list->RSSetScissorRects(1, &whole);
        _viewportDirty = false;
    }
}

HRESULT Device::Clear(DWORD count, const GxRect* rects, DWORD flags, GxColor color, float z, DWORD stencil)
{
    if (!_rt)
        return E_FAIL;
    ApplyTargets();
    const float rgba[4] = {((color >> 16) & 0xFF) / 255.0f, ((color >> 8) & 0xFF) / 255.0f, (color & 0xFF) / 255.0f,
                           ((color >> 24) & 0xFF) / 255.0f};
    const bool haveDepth = TargetHasDepth();
    const D3D12_CLEAR_FLAGS dsFlags =
        static_cast<D3D12_CLEAR_FLAGS>(((flags & GXCLEAR_ZBUFFER) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                                       ((flags & GXCLEAR_STENCIL) ? D3D12_CLEAR_FLAG_STENCIL : 0));
    auto clear = [&](const D3D12_RECT* r)
    {
        if (flags & GXCLEAR_TARGET)
            _list->ClearRenderTargetView(_rtvPool.At(_rt->_rtv), rgba, r ? 1 : 0, r);
        if (haveDepth && dsFlags)
            _list->ClearDepthStencilView(_dsvPool.At(_ds->_dsv), dsFlags, z, static_cast<UINT8>(stencil), r ? 1 : 0, r);
    };
    auto clip = [&](LONG x1, LONG y1, LONG x2, LONG y2, D3D12_RECT& r)
    {
        r.left = std::max<LONG>(x1, 0);
        r.top = std::max<LONG>(y1, 0);
        r.right = std::min<LONG>(x2, static_cast<LONG>(_rt->_w));
        r.bottom = std::min<LONG>(y2, static_cast<LONG>(_rt->_h));
        return r.right > r.left && r.bottom > r.top;
    };
    if (count > 0 && rects)
    {
        for (DWORD i = 0; i < count; i++)
        {
            D3D12_RECT r;
            if (clip(rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2, r))
                clear(&r);
        }
        return S_OK;
    }
    const bool whole = _viewport.X == 0 && _viewport.Y == 0 && _viewport.Width >= _rt->_w && _viewport.Height >= _rt->_h;
    if (whole)
    {
        clear(nullptr);
        return S_OK;
    }
    D3D12_RECT r;
    if (clip(static_cast<LONG>(_viewport.X), static_cast<LONG>(_viewport.Y),
             static_cast<LONG>(_viewport.X + _viewport.Width), static_cast<LONG>(_viewport.Y + _viewport.Height), r))
        clear(&r);
    return S_OK;
}

HRESULT Device::StretchRect(Surface* src, const RECT*, Surface* dst, const RECT*, GxTextureFilter)
{
    if (!src || !dst || !src->_trk || !dst->_trk || !src->_trk->res || !dst->_trk->res || src->_w != dst->_w ||
        src->_h != dst->_h || src->_trk == dst->_trk)
        return E_INVALIDARG;
    Tracked& s = *src->_trk;
    Tracked& d = *dst->_trk;
    if (src->_samples > 1 && dst->_samples <= 1)
    {
        Transition(s, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        Transition(d, D3D12_RESOURCE_STATE_RESOLVE_DEST);
        _list->ResolveSubresource(d.res.Get(), dst->_level, s.res.Get(), src->_level, dst->_dxgi);
    }
    else
    {
        Transition(s, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(d, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dl = {}, sl = {};
        dl.pResource = d.res.Get();
        dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dl.SubresourceIndex = dst->_level;
        sl.pResource = s.res.Get();
        sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        sl.SubresourceIndex = src->_level;
        _list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    }
    // whichever of them is a target or a texture of the next draw is moved back then
    _targetsDirty = true;
    _texturesDirty = true;
    _boundTableValid = false;
    return S_OK;
}

HRESULT Device::GetRenderTargetData(Surface* src, Surface* dst)
{
    if (!src || !dst || !src->_trk || !src->_trk->res || dst->_sysmem.empty() || src->_w != dst->_w ||
        src->_h != dst->_h || src->_samples > 1)
        return E_INVALIDARG;
    const D3D12_RESOURCE_DESC desc = src->_trk->res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT rows = 0;
    UINT64 rowBytes = 0, total = 0;
    _dev->GetCopyableFootprints(&desc, src->_level, 1, 0, &fp, &rows, &rowBytes, &total);
    ComPtr<ID3D12Resource> readback;
    const D3D12_HEAP_PROPERTIES hp = HeapProps(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC bd = BufferDesc(total);
    HRESULT hr = _dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&readback));
    if (FAILED(hr))
        return hr;
    Transition(*src->_trk, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dl = {}, sl = {};
    dl.pResource = readback.Get();
    dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dl.PlacedFootprint = fp;
    sl.pResource = src->_trk->res.Get();
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sl.SubresourceIndex = src->_level;
    _list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    SubmitAndWait();

    void* p = nullptr;
    const D3D12_RANGE all = {0, static_cast<SIZE_T>(total)};
    hr = readback->Map(0, &all, &p);
    if (FAILED(hr))
        return hr;
    const UINT rowCopy = std::min<UINT>(dst->_sysPitch, static_cast<UINT>(rowBytes));
    for (UINT y = 0; y < dst->_h && y < rows; y++)
        std::memcpy(dst->_sysmem.data() + static_cast<size_t>(y) * dst->_sysPitch,
                    static_cast<const uint8_t*>(p) + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch,
                    rowCopy);
    const D3D12_RANGE none = {0, 0};
    readback->Unmap(0, &none);
    return S_OK;
}

// ---------------------------------------------------------------------------
// Device: draw-time state
// ---------------------------------------------------------------------------

void Device::ApplyPipeline(D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType)
{
    if (!_pipelineDirty && topologyType == _pipelineTopology && _pipeline)
        return;
    _pipelineDirty = false;
    _pipelineTopology = topologyType;
    const bool depth = TargetHasDepth();

    // everything a pipeline state object is made from
    struct Key
    {
        UINT vs, ps, decl;
        UINT rtFormat, samples;
        INT depthBias;
        float slopeBias;
        UINT8 blend, srcBlend, dstBlend, writeMask;
        UINT8 depthTarget, zEnable, zWrite, zFunc;
        UINT8 stencil, stencilRead, stencilWrite, stencilFunc;
        UINT8 stencilFail, stencilZFail, stencilPass, cull;
        UINT8 msaa, topology, pad0, pad1;
    } k;
    std::memset(&k, 0, sizeof(k));
    k.vs = _vs->_id;
    k.ps = _ps->_id;
    k.decl = _decl->_id;
    k.rtFormat = static_cast<UINT>(_rt->_dxgi);
    k.samples = _rt->_samples > 1 ? _rt->_samples : 1;
    // The state is in depth-buffer units (0..1); Direct3D counts 24-bit depth steps.
    k.depthBias = static_cast<INT>(std::lround(DwordToFloat(_rs[GXRS_DEPTHBIAS]) * 16777216.0f));
    k.slopeBias = DwordToFloat(_rs[GXRS_SLOPESCALEDEPTHBIAS]);
    k.blend = _rs[GXRS_ALPHABLENDENABLE] ? 1 : 0;
    k.srcBlend = static_cast<UINT8>(_rs[GXRS_SRCBLEND]);
    k.dstBlend = static_cast<UINT8>(_rs[GXRS_DESTBLEND]);
    k.writeMask = static_cast<UINT8>(_rs[GXRS_COLORWRITEENABLE] & 0xF);
    k.depthTarget = depth ? 1 : 0;
    if (depth)
    {
        const DWORD zf = _rs[GXRS_ZFUNC], sf = _rs[GXRS_STENCILFUNC];
        auto op = [](DWORD o) { return static_cast<UINT8>(o >= 1 && o <= 8 ? o : GXSTENCILOP_KEEP); };
        k.zEnable = _rs[GXRS_ZENABLE] != GXZB_FALSE ? 1 : 0;
        k.zWrite = _rs[GXRS_ZWRITEENABLE] ? 1 : 0;
        k.zFunc = static_cast<UINT8>(zf >= 1 && zf <= 8 ? zf : GXCMP_LESSEQUAL); // same numbering
        k.stencil = _rs[GXRS_STENCILENABLE] ? 1 : 0;
        k.stencilRead = static_cast<UINT8>(_rs[GXRS_STENCILMASK]);
        k.stencilWrite = static_cast<UINT8>(_rs[GXRS_STENCILWRITEMASK]);
        k.stencilFunc = static_cast<UINT8>(sf >= 1 && sf <= 8 ? sf : GXCMP_ALWAYS);
        k.stencilFail = op(_rs[GXRS_STENCILFAIL]);
        k.stencilZFail = op(_rs[GXRS_STENCILZFAIL]);
        k.stencilPass = op(_rs[GXRS_STENCILPASS]);
    }
    k.cull = static_cast<UINT8>(_rs[GXRS_CULLMODE]);
    k.msaa = _rs[GXRS_MULTISAMPLEANTIALIAS] ? 1 : 0;
    k.topology = static_cast<UINT8>(topologyType);

    const uint64_t hash = HashBytes(&k, sizeof(k));
    auto it = _pipelines.find(hash);
    if (it == _pipelines.end())
    {
        // One element per shader input: from the declaration when it has it, else
        // the zero vector (missing elements read as zero).
        std::vector<D3D12_INPUT_ELEMENT_DESC> elems;
        for (const VertexShader::Input& in : _vs->_inputs)
        {
            D3D12_INPUT_ELEMENT_DESC e = {};
            e.SemanticName = in.semantic.c_str();
            e.SemanticIndex = in.index;
            e.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            bool found = false;
            for (const VertexDecl::Element& d : _decl->_elements)
            {
                if (d.index == in.index && _stricmp(d.semantic, in.semantic.c_str()) == 0)
                {
                    e.Format = d.format;
                    e.InputSlot = 0;
                    e.AlignedByteOffset = d.offset;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                // one value for the whole draw
                e.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                e.InputSlot = 1;
                e.AlignedByteOffset = 0;
                e.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
                e.InstanceDataStepRate = 1;
            }
            elems.push_back(e);
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = _root.Get();
        pd.VS = {_vs->_code.data(), _vs->_code.size()};
        pd.PS = {_ps->_code.data(), _ps->_code.size()};
        D3D12_RENDER_TARGET_BLEND_DESC& rt = pd.BlendState.RenderTarget[0];
        rt.BlendEnable = k.blend ? TRUE : FALSE;
        rt.SrcBlend = MapBlend(k.srcBlend, false);
        rt.DestBlend = MapBlend(k.dstBlend, false);
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = MapBlend(k.srcBlend, true);
        rt.DestBlendAlpha = MapBlend(k.dstBlend, true);
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = k.writeMask;
        pd.SampleMask = 0xFFFFFFFF;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        // Clockwise triangles face the viewer (D3D default), so culling the
        // counter-clockwise ones (GXCULL_CCW) is back-face culling.
        pd.RasterizerState.CullMode =
            k.cull == GXCULL_CW ? D3D12_CULL_MODE_FRONT : (k.cull == GXCULL_CCW ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE);
        pd.RasterizerState.FrontCounterClockwise = FALSE;
        pd.RasterizerState.DepthBias = k.depthBias;
        pd.RasterizerState.SlopeScaledDepthBias = k.slopeBias;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.RasterizerState.MultisampleEnable = k.msaa ? TRUE : FALSE;
        pd.DepthStencilState.DepthEnable = k.zEnable ? TRUE : FALSE;
        pd.DepthStencilState.DepthWriteMask = k.zWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        pd.DepthStencilState.DepthFunc =
            static_cast<D3D12_COMPARISON_FUNC>(k.zFunc ? k.zFunc : static_cast<UINT8>(GXCMP_ALWAYS));
        pd.DepthStencilState.StencilEnable = k.stencil ? TRUE : FALSE;
        pd.DepthStencilState.StencilReadMask = k.stencilRead;
        pd.DepthStencilState.StencilWriteMask = k.stencilWrite;
        D3D12_DEPTH_STENCILOP_DESC face = {};
        face.StencilFunc = static_cast<D3D12_COMPARISON_FUNC>(k.stencilFunc ? k.stencilFunc : static_cast<UINT8>(GXCMP_ALWAYS));
        face.StencilFailOp = static_cast<D3D12_STENCIL_OP>(k.stencilFail ? k.stencilFail : static_cast<UINT8>(GXSTENCILOP_KEEP));
        face.StencilDepthFailOp =
            static_cast<D3D12_STENCIL_OP>(k.stencilZFail ? k.stencilZFail : static_cast<UINT8>(GXSTENCILOP_KEEP));
        face.StencilPassOp = static_cast<D3D12_STENCIL_OP>(k.stencilPass ? k.stencilPass : static_cast<UINT8>(GXSTENCILOP_KEEP));
        pd.DepthStencilState.FrontFace = face;
        pd.DepthStencilState.BackFace = face;
        pd.InputLayout = {elems.data(), static_cast<UINT>(elems.size())};
        pd.PrimitiveTopologyType = topologyType;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = _rt->_dxgi;
        pd.DSVFormat = depth ? kDepthView : DXGI_FORMAT_UNKNOWN;
        pd.SampleDesc.Count = k.samples;
        ComPtr<ID3D12PipelineState> pso;
        const HRESULT hr = _dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) // kept as null: the draws that want it are skipped, and it is said once
        {
            char msg[160];
            snprintf(msg, sizeof(msg), "pipeline state not created (0x%08X): vertex shader %u, pixel shader %u, layout %u",
                     static_cast<unsigned>(hr), k.vs, k.ps, k.decl);
            _failures.emplace_back(msg);
        }
        it = _pipelines.emplace(hash, pso).first;
    }
    ID3D12PipelineState* pso = it->second.Get();
    if (pso && pso != _pipeline)
        _list->SetPipelineState(pso);
    _pipeline = pso;
}

void Device::ApplySamplers()
{
    if (!_samplersDirty)
        return;
    _samplersDirty = false;
    D3D12_SAMPLER_DESC sd[SamplerTableSize];
    std::memset(sd, 0, sizeof(sd));
    for (UINT i = 0; i < NumStages; i++)
    {
        const Stage& s = _stage[i];
        const bool aniso = s.minFilter == GXTEXF_ANISOTROPIC || s.magFilter == GXTEXF_ANISOTROPIC;
        const bool minLin = s.minFilter >= GXTEXF_LINEAR, magLin = s.magFilter >= GXTEXF_LINEAR;
        const bool mipLin = s.mipFilter >= GXTEXF_LINEAR;
        if (aniso)
            sd[i].Filter = D3D12_FILTER_ANISOTROPIC;
        else
            sd[i].Filter = static_cast<D3D12_FILTER>((minLin ? 0x10 : 0) | (magLin ? 0x4 : 0) | (mipLin ? 0x1 : 0));
        sd[i].AddressU = MapAddress(s.addressU);
        sd[i].AddressV = MapAddress(s.addressV);
        sd[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sd[i].MaxAnisotropy = aniso ? std::min<UINT>(std::max<UINT>(s.maxAnisotropy, 1), 16) : 1;
        sd[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sd[i].MinLOD = 0.0f;
        sd[i].MaxLOD = s.mipFilter == GXTEXF_NONE ? 0.0f : D3D12_FLOAT32_MAX; // "no mip filter" = top level only
    }
    const uint64_t key = HashBytes(sd, sizeof(sd));
    auto it = _samplerTables.find(key);
    if (it == _samplerTables.end())
    {
        const UINT base = _samplerNext;
        _samplerNext += SamplerTableSize;
        D3D12_CPU_DESCRIPTOR_HANDLE h = _gpuSamplers->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(base) * _samplerStep;
        for (UINT i = 0; i < SamplerTableSize; i++, h.ptr += _samplerStep)
            _dev->CreateSampler(&sd[i], h);
        it = _samplerTables.emplace(key, base).first;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE g = _gpuSamplers->GetGPUDescriptorHandleForHeapStart();
    g.ptr += static_cast<UINT64>(it->second) * _samplerStep;
    _list->SetGraphicsRootDescriptorTable(3, g);
}

void Device::ApplyTextures()
{
    if (!_texturesDirty && _boundTableValid)
        return;
    _texturesDirty = false;
    ID3D12Resource* target = _rt && _rt->_trk ? _rt->_trk->res.Get() : nullptr;

    UINT want[NumTextureSlots];
    for (UINT i = 0; i < NumTextureSlots; i++)
        // An empty stage samples as opaque black, which the drawing code relies on (the
        // projected shadows draw untextured and multiply by the sampled alpha).
        want[i] = i < NumStages ? _emptyStageSrv : _nullSrv;
    want[DepthSlotMS] = _nullSrvMS;
    for (UINT i = 0; i < NumStages; i++)
    {
        const int slot = _ps->_texSlot[i];
        if (slot < 0 || slot >= NumTextureSlots)
            continue;
        Texture* t = _stage[i].tex;
        // Never sample the texture that is being rendered to.
        if (!t || !t->_trk.res || t->_trk.res.Get() == target)
        {
            want[slot] = _emptyStageSrv;
            continue;
        }
        Transition(t->_trk, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        want[slot] = t->_srv;
    }
    if (_depthCopy.res)
    {
        Transition(_depthCopy, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        want[_depthCopySamples > 1 ? DepthSlotMS : DepthSlot] = _depthCopySrv;
    }
    if (_localShadowTex && _localShadowTex->_trk.res && _localShadowTex->_trk.res.Get() != target)
    {
        Transition(_localShadowTex->_trk, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        want[LocalShadowSlot] = _localShadowTex->_srv;
    }
    if (_boundTableValid && std::memcmp(want, _boundTable, sizeof(want)) == 0)
        return;

    Frame& f = _frames[_frame];
    const UINT base = ExternalTextures + (static_cast<UINT>(_frame) * TablesPerFrame + f.textureTables) * NumTextureSlots;
    f.textureTables++;
    for (UINT i = 0; i < NumTextureSlots; i++)
        _dev->CopyDescriptorsSimple(1, GpuSrvCpu(base + i), _srvPool.At(want[i]), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    _list->SetGraphicsRootDescriptorTable(2, GpuSrvGpu(base));
    std::memcpy(_boundTable, want, sizeof(want));
    _boundTableValid = true;
}

void Device::ApplyConstants()
{
    if (_vsConstDirty)
    {
        const UploadAlloc a = AllocUpload(sizeof(_vsConst), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        if (a.ptr)
        {
            std::memcpy(a.ptr, _vsConst, sizeof(_vsConst));
            _list->SetGraphicsRootConstantBufferView(0, a.va);
            _vsConstDirty = false;
        }
    }
    if (_psConstDirty)
    {
        const UploadAlloc a = AllocUpload(sizeof(_psConst), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        if (a.ptr)
        {
            std::memcpy(a.ptr, _psConst, sizeof(_psConst));
            _list->SetGraphicsRootConstantBufferView(1, a.va);
            _psConstDirty = false;
        }
    }
}

bool Device::PrepareDraw(UINT stride, D3D12_GPU_VIRTUAL_ADDRESS vb, UINT vbBytes, D3D12_PRIMITIVE_TOPOLOGY topology)
{
    if (!_vs || !_ps || !_decl || !_rt || !vb)
        return false;
    if (_slabsCopying)
        ReadySlabs();
    // room for this draw's tables; if not, run what there is and start over
    if (_frames[_frame].textureTables >= TablesPerFrame)
        SubmitAndWait();
    if (_samplerNext + SamplerTableSize > SamplerHeapSize)
    {
        SubmitAndWait();
        _samplerTables.clear();
        _samplerNext = 0;
    }

    D3D12_PRIMITIVE_TOPOLOGY_TYPE type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    if (topology == D3D_PRIMITIVE_TOPOLOGY_LINELIST || topology == D3D_PRIMITIVE_TOPOLOGY_LINESTRIP)
        type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    else if (topology == D3D_PRIMITIVE_TOPOLOGY_POINTLIST)
        type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;

    ApplyTargets();
    ApplyPipeline(type);
    if (!_pipeline)
        return false;
    if (_stencilRefDirty)
    {
        _stencilRefDirty = false;
        _list->OMSetStencilRef(_rs[GXRS_STENCILREF] & 0xFF);
    }
    ApplyTextures();
    ApplySamplers();
    ApplyConstants();
    const D3D12_VERTEX_BUFFER_VIEW views[2] = {{vb, vbBytes, stride}, {_zeroVB->GetGPUVirtualAddress(), 16, 16}};
    _list->IASetVertexBuffers(0, 2, views);
    if (topology != _topology)
    {
        _topology = topology;
        _list->IASetPrimitiveTopology(topology);
    }
    return true;
}

D3D12_PRIMITIVE_TOPOLOGY Device::MapTopology(GxPrimitiveType type, UINT primitives, UINT* vertexCount)
{
    switch (type)
    {
        case GXPT_TRIANGLESTRIP:
            *vertexCount = primitives + 2;
            return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case GXPT_LINELIST:
            *vertexCount = primitives * 2;
            return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        case GXPT_LINESTRIP:
            *vertexCount = primitives + 1;
            return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case GXPT_POINTLIST:
            *vertexCount = primitives;
            return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        default:
            *vertexCount = primitives * 3;
            return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

HRESULT Device::DrawPrimitiveUP(GxPrimitiveType type, UINT primitives, const void* data, UINT stride)
{
    if (!data || primitives == 0 || stride == 0)
        return E_INVALIDARG;
    UINT count = 0;
    const D3D12_PRIMITIVE_TOPOLOGY topo = MapTopology(type, primitives, &count);
    const UINT bytes = count * stride;
    // PrepareDraw may submit the frame so far and with it rewind the upload memory: first
    if (_frames[_frame].textureTables >= TablesPerFrame || _samplerNext + SamplerTableSize > SamplerHeapSize)
    {
        SubmitAndWait();
        _samplerTables.clear();
        _samplerNext = 0;
    }
    const UploadAlloc a = AllocUpload(bytes, 16);
    if (!a.ptr)
        return E_FAIL;
    std::memcpy(a.ptr, data, bytes);
    if (!PrepareDraw(stride, a.va, bytes, topo))
        return E_FAIL;
    _list->DrawInstanced(count, 1, 0, 0);
    return S_OK;
}

HRESULT Device::DrawIndexedPrimitive(GxPrimitiveType type, INT baseVertex, UINT, UINT, UINT startIndex,
                                     UINT primitives)
{
    if (!_streamVB || !_indices || primitives == 0)
        return E_FAIL;
    UINT count = 0;
    const D3D12_PRIMITIVE_TOPOLOGY topo = MapTopology(type, primitives, &count);
    const D3D12_GPU_VIRTUAL_ADDRESS vb = _streamVB->Address();
    if (!vb || _streamOffset >= _streamVB->_size)
        return E_FAIL;
    if (!PrepareDraw(_streamStride, vb + _streamOffset, _streamVB->_size - _streamOffset, topo))
        return E_FAIL;
    const D3D12_GPU_VIRTUAL_ADDRESS ib = _indices->Address();
    if (!ib)
        return E_FAIL;
    // every time: another buffer of another size may sit at a freed one's address
    const D3D12_INDEX_BUFFER_VIEW view = {ib, _indices->_size, DXGI_FORMAT_R16_UINT};
    _list->IASetIndexBuffer(&view);
    _list->DrawIndexedInstanced(count, 1, startIndex, baseVertex, 0);
    return S_OK;
}

uint64_t Device::GetAvailableTextureMem() const
{
    uint64_t avail = _info.dedicatedVideoMemory;
    ComPtr<IDXGIAdapter3> a3;
    if (_adapter && SUCCEEDED(_adapter.As(&a3)))
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO vm = {};
        if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm)) && vm.Budget > 0)
            avail = vm.Budget > vm.CurrentUsage ? vm.Budget - vm.CurrentUsage : 0;
    }
    return avail;
}

uint64_t Device::GetVideoMemoryBudget() const
{
    uint64_t total = _info.dedicatedVideoMemory;
    ComPtr<IDXGIAdapter3> a3;
    if (_adapter && SUCCEEDED(_adapter.As(&a3)))
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO vm = {};
        // graphics that share the main memory report little of their own: the budget is what counts
        if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm)) && vm.Budget > total)
            total = vm.Budget;
    }
    return total;
}

void Device::DrainDebugMessages(std::vector<std::string>& out)
{
    for (std::string& f : _failures)
        out.push_back(std::move(f));
    _failures.clear();
    if (!_infoQueue)
        return;
    const UINT64 n = _infoQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; i++)
    {
        SIZE_T len = 0;
        if (FAILED(_infoQueue->GetMessage(i, nullptr, &len)) || len == 0)
            continue;
        std::vector<uint8_t> buf(len);
        D3D12_MESSAGE* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (SUCCEEDED(_infoQueue->GetMessage(i, msg, &len)) && msg->pDescription)
            out.emplace_back(msg->pDescription, msg->DescriptionByteLength ? msg->DescriptionByteLength - 1 : 0);
    }
    _infoQueue->ClearStoredMessages();
}

void Device::BeginEvent(const wchar_t* name)
{
    if (name && _list)
        _list->BeginEvent(0, name, static_cast<UINT>((std::wcslen(name) + 1) * sizeof(wchar_t)));
}

void Device::EndEvent()
{
    if (_list)
        _list->EndEvent();
}
} // namespace Dx12
