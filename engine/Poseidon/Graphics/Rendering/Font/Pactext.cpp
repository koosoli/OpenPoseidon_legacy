#include <Poseidon/Core/Application.hpp>

#include <atomic>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/Graphics/Rendering/Font/WarmMipDiagnostics.hpp>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <cstring>
#include <Poseidon/Graphics/Rendering/Shape/ObjectAdmitProfile.hpp>
#include <vector>
#include <Poseidon/Core/Global.hpp>
#include <Poseidon/Graphics/Rendering/Font/Pactext.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/Graphics/Rendering/Primitives/Vertex.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Foundation/Containers/StaticArray.hpp>
#include <Poseidon/Foundation/Common/Filenames.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/Streams/ArchiveSourceBinding.hpp>
#include <string.h>
#include <Poseidon/Foundation/Containers/BoolArray.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Memory/FastAlloc.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/Memtype.h>
#include <Poseidon/Foundation/platform.hpp>
#include <string>
#include <unordered_set>
#include <Poseidon/Foundation/Algorithms/Lzo1x.hpp>
#include <Poseidon/Graphics/Rendering/Font/PaaLzoReplayCache.hpp>

namespace Poseidon
{
typedef word Pixel;

namespace
{
render::PaaLzoReplayCache* PaaLzoReplay()
{
    static const bool enabled=[] {
        const char* value=std::getenv("WGR_PAA_LZO_REPLAY_CACHE");
        return value && std::strcmp(value,"1")==0;
    }();
    if(!enabled) return nullptr;
    static render::PaaLzoReplayCache cache;
    return &cache;
}
struct PaaLzoReplayReport
{
    ~PaaLzoReplayReport() noexcept
    try {
        auto* cache=PaaLzoReplay(); if(!cache) return;
        // Optional aggregate only, bounded for the entire process. No source log
        // or clock on the ordinary disabled route.
        static std::mutex logMutex;
        static uint64_t lastHits=0,lastMisses=0;
        static unsigned rows=0;
        std::lock_guard<std::mutex> lock(logMutex);
        if(rows>128) return;
        const auto stats=cache->Snapshot();
        if(rows && stats.hits==lastHits && stats.misses-lastMisses<32) return;
        lastHits=stats.hits; lastMisses=stats.misses;
        if(rows==128) { ++rows; LOG_INFO(Graphics,"PAA LZO replay stats truncated: limit=128"); }
        if(rows>=128) return;
        ++rows;
        LOG_INFO(Graphics,"PAA LZO replay: hits={} misses={} hitOutputBytes={} storeBytes={} peakStoreBytes={} entries={} evictions={} oversize={} admissionFailures={} budgetBytes=33554432 maxEntries=128",
            stats.hits,stats.misses,stats.hitOutputBytes,stats.storeBytes,stats.peakStoreBytes,stats.entries,stats.evictions,stats.oversize,stats.admissionFailures);
    } catch (...) { /* Optional diagnostic cannot change texture-read success. */ }
};
using WarmMipClock = std::chrono::steady_clock;
struct WarmMipChainTiming;
thread_local WarmMipChainTiming* GCurrentWarmMipChainTiming = nullptr;
// At most128 slow rows before capture plus128 after the FIRST successful owner arm.
// No timer, decode, cache or texture admission behavior depends on this log budget.
std::atomic<unsigned> GWarmMipChainRows{0};
bool GWarmMipCaptureReset = false; // accessed only by the joined owner reset function
bool WarmMipChainProfileRequested()
{
    static const bool enabled = [] {
        const char* value = std::getenv("WGR_OBJECT_STREAM_WARM_PROFILE");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
bool WarmMipChainTimingEnabled()
{
    return WarmMipChainProfileRequested() && render::GObjectAdmitProfile.active;
}
class WarmMipPhase
{
    double* _bucket;
    WarmMipClock::time_point _began;
public:
    explicit WarmMipPhase(double* bucket) : _bucket(bucket),
        _began(bucket ? WarmMipClock::now() : WarmMipClock::time_point{}) {}
    ~WarmMipPhase() { Stop(); }
    void Stop()
    {
        if (_bucket)
        {
            *_bucket += std::chrono::duration<double, std::milli>(WarmMipClock::now() - _began).count();
            _bucket = nullptr;
        }
    }
};
struct WarmMipChainTiming
{
    bool enabled, success = false;
    const char* source;
    int requestedMips, selectedMips = 0;
    uint64_t dxtStoredBytes = 0, dxtOutputBytes = 0;
    int lzoMips = 0, rawDxtMips = 0, borrowedLzoMips = 0;
    double openMs = 0.0, loopMs = 0.0, packedAllocCopyMs = 0.0, lzoMs = 0.0, rawReadMs = 0.0;
    WarmMipClock::time_point began;
    WarmMipChainTiming* previous;
    WarmMipChainTiming(const char* name, int count)
        : enabled(WarmMipChainTimingEnabled()), source(name), requestedMips(count),
          began(enabled ? WarmMipClock::now() : WarmMipClock::time_point{}),
          previous(enabled ? GCurrentWarmMipChainTiming : nullptr)
    {
        if (enabled) GCurrentWarmMipChainTiming = this;
    }
    ~WarmMipChainTiming()
    {
        if (!enabled) return;
        GCurrentWarmMipChainTiming = previous; // never lend the stack scope beyond this call
        const double totalMs = std::chrono::duration<double, std::milli>(WarmMipClock::now() - began).count();
        if (totalMs < 2.0) return;
        unsigned slot = GWarmMipChainRows.load(std::memory_order_relaxed);
        while (slot < 129 && !GWarmMipChainRows.compare_exchange_weak(slot, slot + 1, std::memory_order_relaxed)) {}
        if (slot == 128)
            LOG_INFO(Graphics, "Warm mip chain timings truncated: limit=128 thresholdMs=2");
        if (slot >= 128) return;
        LOG_INFO(Graphics,
            "Warm mip chain timing: source={} success={} requestedMips={} selectedMips={} "
            "lzoMips={} borrowedLzoMips={} rawDxtMips={} dxtStoredBytes={} dxtOutputBytes={} openOwnerMs={:.3f} "
            "loopOwnerMs={:.3f} packedAllocCopyMs={:.3f} lzoMs={:.3f} rawReadMs={:.3f} totalOwnerMs={:.3f}",
            source, success, requestedMips, selectedMips, lzoMips, borrowedLzoMips, rawDxtMips, dxtStoredBytes,
            dxtOutputBytes, openMs, loopMs, packedAllocCopyMs, lzoMs, rawReadMs, totalMs);
    }
};
} // namespace


WarmMipDiagnosticCaptureReset ResetWarmMipChainDiagnosticsForCapture()
{
    if (!WarmMipChainProfileRequested()) return WarmMipDiagnosticCaptureReset::Disabled;
    if (!Foundation::IsMainThread() || GCurrentWarmMipChainTiming || GWarmMipCaptureReset)
        return WarmMipDiagnosticCaptureReset::Refused;
    GWarmMipCaptureReset = true;
    GWarmMipChainRows.store(0, std::memory_order_relaxed);
    return WarmMipDiagnosticCaptureReset::Accepted;
}

inline Pixel Conv888To555(int rgb)
{
    int r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = (rgb >> 0) & 0xff;
    r >>= 3, g >>= 3, b >>= 3;
    return Pixel((r << 10) | (g << 5) | (b << 0));
}

inline Pixel Conv888To565(int rgb)
{
    int r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = (rgb >> 0) & 0xff;
    r >>= 3, g >>= 2, b >>= 3;
    return Pixel((r << 11) | (g << 5) | (b << 0));
}

struct DXTBlock64
{
    // see "Compressed Texture Formats" in DX Docs
    word c0, c1;     // color data
    word tex0, tex1; // texel data
};

#ifdef _MSC_VER
#define TAGG 'TAGG'
#define AVGC 'AVGC'
#define FLAG 'FLAG'
#define OFFS 'OFFS'
#else
#define TAGG ((int)'G' + ((int)'G' << 8) + ((int)'A' << 16) + ((int)'T' << 24))
#define AVGC ((int)'C' + ((int)'G' << 8) + ((int)'V' << 16) + ((int)'A' << 24))
#define FLAG ((int)'G' + ((int)'A' << 8) + ((int)'L' << 16) + ((int)'F' << 24))
#define OFFS ((int)'S' + ((int)'F' << 8) + ((int)'F' << 16) + ((int)'O' << 24))
#endif

// load texture map from PAC file

int PacPalette::Skip(QIStream& in)
{
    // each texture file starts with palette definition
    // return<0 - error
    // return==0 - texture loaded successfully
    int error = -1;

    for (;;)
    {
        // skip any taggs
        DWORD magic = fgetil(in);
        if (magic != TAGG)
        {
            in.seekg(-4, QIOS::cur);
            break;
        }
        // some tagg - load it
        DWORD tagg = fgetil(in);
        (void)tagg;
        int size = fgetil(in);
        PoseidonAssert(size >= 0);
        in.seekg(size, QIOS::cur);
        if (in.eof() || in.fail())
        {
            goto Error;
        }
    }

    enum
    {
        psize = 256
    };

    // get palette  size
    long C;
    C = fgetiw(in);
    if (in.eof() || in.fail())
    {
        goto Error;
    }

    if (C < 0 || C > psize)
    {
        error = 1;
        goto Error;
    } // mip-map list terminator

    in.seekg(3 * C, QIOS::cur);
    if (in.eof() || in.fail())
    {
        goto Error;
    }

    error = 0;

Error:
    return error;
}

PacPalette::PacPalette()
    : _nColors(0), _palette(nullptr), _transparentColor(-1), _averageColor(HBlack), _averageColor32(0xff000000),
      _isTransparent(false), _isAlpha(false)
{
}

PacPalette::~PacPalette()
{
    if (_palette)
    {
        delete[] _palette;
    }
}

enum PicFlags
{
    PicFlagAlpha = 1,
    PicFlagTransparent = 2,
};

int PacPalette::Load(QIStream& in, int* startOffsets, int maxStartOffsets)
{
    int error = -1;
    // each texture file starts with palette definition
    // return<0 - error
    // return==0 - texture loaded successfully

    _averageColor32 = PackedColor(0); // default - transparent
    for (;;)
    {
        // skip/load any taggs
        DWORD magic = fgetil(in);
        if (magic != TAGG)
        {
            in.seekg(-4, QIOS::cur);
            break;
        }
        // some tagg - load it
        DWORD tagg = fgetil(in);
        int size = fgetil(in);
        if (in.eof() || in.fail())
        {
            goto Error;
        }
        PoseidonAssert(size >= 0);
        switch (tagg)
        {
            case AVGC:
                PoseidonAssert(size == sizeof(_averageColor32));
                _averageColor32 = PackedColor(fgetil(in));
                break;
            case FLAG:
            {
                int flags = fgetil(in);
                if (flags & PicFlagAlpha)
                {
                    _isAlpha = true;
                }
                if (flags & PicFlagTransparent)
                {
                    _isTransparent = true;
                }
            }
            break;
            case OFFS:
            {
                int nOffs = size / sizeof(int);
                for (int i = 0; i < nOffs; i++)
                {
                    int data = fgetil(in);
                    if (i < maxStartOffsets)
                    {
                        startOffsets[i] = data;
                    }
                }
            }
            break;
            default:
                in.seekg(size, QIOS::cur);
                break;
        }
    }
    _averageColor = Color((long)_averageColor32);
    // load PAC file
    enum
    {
        psize = 256
    };
    int i;

    // get palette  size
    long C;
    C = fgetiw(in);

    if (C < 0 || C > psize)
    {
        error = 1;
        goto Error;
    } // mip-map list terminator

    _nColors = C;

    if (!_palette && _nColors > 0)
    {
        _palette = new DWORD[_nColors]; // packed 24-bit color (palette)
    }

    _transparentColor = -1;
    if (_palette || _nColors == 0)
    {
        for (i = 0; i < C; i++)
        {
            _palette[i] = fgeti24(in);
        }

        if (in.eof() || in.fail())
        {
            goto Error;
        }

        // check if texture contains transparent color
        for (i = 0; i < _nColors; i++)
        {
            if (_palette[i] == TRANSPARENT1_RGB)
            {
                _transparentColor = i;
            }
            if (_palette[i] == TRANSPARENT2_RGB)
            {
                _transparentColor = i;
            }
        }

        error = 0;

        if (_transparentColor >= 0)
        {
            // convert transparent color to average texture color
            DWORD avg = _averageColor32;
            avg &= 0xffffff; // transparent
            _palette[_transparentColor] = avg;
            _isTransparent = true;
        }
    }

    if (_averageColor32 > 0)
    {
        // some value loaded - it should be correct
        if ((_averageColor32 & 0xff000000) == 0xff000000)
        {
            // it is not transparent, but is assumed to be
            _isTransparent = false;
        }
        else
        {
            // it should be alpha or transparent
            if (!_isAlpha)
            {
                _isTransparent = true;
            }
        }
    }

Error:
    if (error < 0)
    {
        if (_palette)
        {
            delete[] _palette, _palette = nullptr;
        }
        _nColors = 0;
    }
    return error;
}

inline int ConvColorTo888(ColorVal color)
{
    int r = toIntFloor(color.R() * 255);
    int g = toIntFloor(color.G() * 255);
    int b = toIntFloor(color.B() * 255);
    return (r << 16) | (g << 8) | (b << 0);
}

#define PAA_4444 0x4444
#define PAA_8080 0x8080
#define PAA_1555 0x1555
#define PAA_8888 0x8888
#define PAA_DXT1 0xff01
#define PAA_DXT2 0xff02
#define PAA_DXT3 0xff03
#define PAA_DXT4 0xff04
#define PAA_DXT5 0xff05

PacFormat PacFormatFromDesc(int desc, bool& alpha)
{
    PacFormat format = PacFormatN;
    alpha = false;
    if (desc == PAA_8080)
    {
        format = PacAI88, alpha = true;
    }
    else if (desc == PAA_4444)
    {
        format = PacARGB4444, alpha = true;
    }
    else if (desc == PAA_1555)
    {
        format = PacARGB1555;
    }
    else if (desc == PAA_8888)
    {
        format = PacARGB8888, alpha = true;
    }
    else if (desc == PAA_DXT1)
    {
        format = PacDXT1;
    }
    else if (desc == PAA_DXT2)
    {
        format = PacDXT2;
    }
    else if (desc == PAA_DXT3)
    {
        format = PacDXT3;
    }
    else if (desc == PAA_DXT4)
    {
        format = PacDXT4;
    }
    else if (desc == PAA_DXT5)
    {
        format = PacDXT5;
    }
    return format;
}

PacLevelMem::PacLevelMem()
    : _w(0), _h(0), _pitch(0), _sFormat(PacP8), _dFormat(PacARGB1555), _start(-1) // invalid offset
{
}

// general LZW decompression routine

#define N 4096

#define N 4096      // textbuffer length
#define F 18        // max. match len
#define THRESHOLD 2 // min. match len

static int DecodeLZW(QIStream& in, char* dst, long lensb, int byteW, int pitch, Pixel* resPal = nullptr,
                     const PacPalette* pal = nullptr)
{
    if (lensb <= 0)
    {
        return 0;
    }

    char text_buf[N + F - 1];
    int i, j, r, c, csum = 0, csr;
    int flags;
    int lineCnt = byteW;
    int lineAlign = pitch - byteW;
    int pSize = (resPal ? sizeof(Pixel) : sizeof(char));
    memset(text_buf, ' ', N - F);
    r = N - F;
    flags = 0;
    while (lensb > 0)
    {
        if (((flags >>= 1) & 256) == 0)
        {
            c = in.get();
            flags = c | 0xff00;
        }
        if (in.fail() || in.eof())
        {
            Fail("LZW: stream read failed");
            return -1;
        }
        if (flags & 1)
        {
            c = in.get();
            if (in.fail() || in.eof())
            {
                Fail("LZW: stream read failed");
                return -1;
            }
            csum += (signed char)c;
            // save pixel
            if (!resPal)
            {
                *dst = c;
            }
            else
            {
                *(word*)dst = resPal[c];
            }
            dst += pSize;
            lensb--;
            lineCnt -= pSize;
            if (lineCnt == 0)
            {
                dst += lineAlign, lineCnt = byteW;
            }
            // continue decompression
            text_buf[r] = (signed char)c;
            r++;
            r &= (N - 1);
        }
        else
        {
            i = in.get();
            j = in.get();
            if (in.fail() || in.eof())
            {
                Fail("LZW: stream read failed");
                return -1;
            }
            i |= (j & 0xf0) << 4;
            j &= 0x0f;
            j += THRESHOLD;
            // Stop at lensb: a back-reference match copies up to F+THRESHOLD bytes
            // in one step, but only `lensb` output bytes were requested (and the
            // destination is sized for exactly that). A crafted stream whose final
            // match runs past the end would otherwise overflow the output buffer.
            for (i = r - i, j += i; i <= j && lensb > 0; i++)
            {
                c = (byte)text_buf[i & (N - 1)];
                csum += (signed char)c;
                // save pixel
                if (!resPal)
                {
                    *dst = c;
                }
                else
                {
                    *(word*)dst = resPal[c];
                }
                dst += pSize;
                lensb--;
                lineCnt -= pSize;
                if (lineCnt == 0)
                {
                    dst += lineAlign, lineCnt = byteW;
                }
                // continue decompression
                text_buf[r] = (signed char)c;
                r++;
                r &= (N - 1);
            }
        }
    }
    in.read((char*)&csr, sizeof(csr));
    if (in.fail() || in.eof())
    {
        Fail("LZW: end of stream");
        return -1;
    }
    if (csr != csum)
    {
        Fail("Checksum");
        return -1;
    }
    return 0;
}

#define MAGIC_W_LZW 1234
#define MAGIC_H_LZW 8765

int PacLevelMem::LoadPacP8(QIStream& in, void* mem, const PacPalette* pal) const
{
    // return<0 - error
    // return>0 - no more mip-map levels available
    // return==0 - texture loaded successfully

    // load PAC file
    // get image size
    {
        long w = fgetiw(in);
        long h = fgetiw(in);
        if (w == 0 && h == 0)
        {
            return 1;
        } // mip-map list terminator

        if (w == MAGIC_W_LZW && h == MAGIC_H_LZW)
        {
            w = fgetiw(in);
            h = fgetiw(in);

            long dSize = fgeti24(in);
            (void)dSize;

            if (w > 4096 || w < 2)
            {
                Fail("Size out of range");
                return -1;
            }

            PoseidonAssert(_w == w);
            PoseidonAssert(_h == h);

            if (DecodeLZW(in, (char*)mem, _w * _h, _w, _pitch) < 0)
            {
                Fail("LZW Decode error.");
                return -1;
            }
        }
        else
        {
            //  get compressed data size
            long dSize = fgeti24(in);
            (void)dSize;

            long W = w * h;

            if (w > 4096 || w < 2)
            {
                Fail("Size out of range");
                return -1;
            }

            PoseidonAssert(_w == w);
            PoseidonAssert(_h == h);

            int lineCnt = _w;
            int lineAlign = _pitch - _w;

            // resulting surface is pallete - simple copy
            byte* dst = (byte*)mem;
            while (W > 0)
            {
                int c = in.get();
                if (c & 0x80)
                {
                    int v = in.get();
                    c &= 0x7f;
                    c++;
                    W -= c;
                    if (lineAlign == 0)
                    {
                        while (--c >= 0)
                        {
                            *dst++ = (byte)v;
                        }
                    }
                    else
                    {
                        while (--c >= 0)
                        {
                            *dst++ = (byte)v;
                            if (--lineCnt == 0)
                            {
                                dst += lineAlign, lineCnt = w;
                            }
                        }
                    }
                }
                else
                {
                    c++;
                    W -= c;
                    while (--c >= 0)
                    {
                        int v = in.get();
                        *dst++ = (byte)v;
                        if (--lineCnt == 0)
                        {
                            dst += lineAlign, lineCnt = w;
                        }
                    }
                }
            }
        }
        if (in.fail() || in.eof())
        {
            Fail("Stream Fail error.");
            return -1;
        }

        return 0;
    }
}

int PacLevelMem::LoadPacARGB1555(QIStream& in, void* mem, const PacPalette* pal) const
{
    // picture is saved in Palette 8 format
    // load as raw ARGB 1555 data
    // return<0 - error
    // return>0 - no more mip-map levels available
    // return==0 - texture loaded successfully

    // load PAC file

    // resulting surface is 16b-rgb
    // prepare a palette for conversion
    word resPal[256];
    PoseidonAssert(pal->_nColors <= 256);
    int i;
    for (i = 0; i < pal->_nColors; i++)
    {
        word rgb = Conv888To555(pal->_palette[i]);
        if (i != pal->_transparentColor)
        {
            rgb |= 0x8000;
        }
        resPal[i] = rgb;
    }

    // get image size
    {
        long w = fgetiw(in);
        long h = fgetiw(in);
        if (w == 0 && h == 0)
        {
            return 1;
        } // mip-map list terminator

        if (w == MAGIC_W_LZW && h == MAGIC_H_LZW)
        {
            w = fgetiw(in);
            h = fgetiw(in);

            long dSize = fgeti24(in);
            (void)dSize;

            if (w > 4096 || w < 2)
            {
                Fail("Size out of range");
                return -1;
            }

            PoseidonAssert(_w == w);
            PoseidonAssert(_h == h);

            // skip actual image data
            if (DecodeLZW(in, (char*)mem, _w * _h, _w * 2, _pitch, resPal, pal) < 0)
            {
                Fail("LZW Decode error");
                return -1;
            }
        }
        else
        {
            // get compressed data size
            long dSize = fgeti24(in);
            (void)dSize;

            long W = w * h;

            if (w > 4096 || w < 2)
            {
                Fail("Size out of range");
                return -1;
            }

            PoseidonAssert(_w == w);
            PoseidonAssert(_h == h);

            int lineCnt = w;
            int lineAlign = _pitch - w * 2;

            word* B = (word*)mem;
            while (W > 0)
            {
                int c = in.get();
                if (c & 0x80)
                {
                    int v = in.get();
                    c &= 0x7f;
                    c++;
                    W -= c;
                    word vv;
                    vv = resPal[v];
                    if (lineAlign == 0)
                    {
                        while (--c >= 0)
                        {
                            *B++ = vv;
                        }
                    }
                    else
                    {
                        while (--c >= 0)
                        {
                            *B++ = vv;
                            if (--lineCnt == 0)
                            {
                                B = (word*)((byte*)B + lineAlign), lineCnt = w;
                            }
                        }
                    }
                }
                else
                {
                    c++;
                    W -= c;
                    while (--c >= 0)
                    {
                        int v = in.get();
                        word vv;
                        vv = resPal[v];
                        *B++ = vv;
                        if (--lineCnt == 0)
                        {
                            B = (word*)((byte*)B + lineAlign), lineCnt = w;
                        }
                    }
                }
            }
        }

        if (in.fail() || in.eof())
        {
            Fail("Texture load failed.");
            return -1;
        }

        return 0;
    }
}

int PacLevelMem::LoadPacRGB565(QIStream& in, void* mem, const PacPalette* pal) const
{
    // picture is saved in Palette 8 format
    // load as raw RGB 565 data
    // return<0 - error
    // return>0 - no more mip-map levels available
    // return==0 - texture loaded successfully

    // load PAC file

    // resulting surface is 16b-rgb
    // prepare a palette for conversion
    word resPal[256];
    PoseidonAssert(pal->_nColors <= 256);
    PoseidonAssert(pal->_transparentColor < 0);
    int i;
    for (i = 0; i < pal->_nColors; i++)
    {
        word rgb = Conv888To565(pal->_palette[i]);
        resPal[i] = rgb;
    }

    // get image size
    {
        long w = fgetiw(in);
        long h = fgetiw(in);
        if (w == 0 && h == 0)
        {
            return 1;
        } // mip-map list terminator

        if (w == MAGIC_W_LZW && h == MAGIC_H_LZW)
        {
            w = fgetiw(in);
            h = fgetiw(in);

            long dSize = fgeti24(in);
            (void)dSize;

            if (w > 4096 || w < 2)
            {
                Fail("Size out of range");
                return -1;
            }

            PoseidonAssert(_w == w);
            PoseidonAssert(_h == h);

            // skip actual image data
            if (DecodeLZW(in, (char*)mem, _w * _h, _w * 2, _pitch, resPal, pal) < 0)
            {
                Fail("LZW Decode error");
                return -1;
            }
        }
        else
        {
            // get compressed data size
            long dSize = fgeti24(in);
            (void)dSize;

            long W = w * h;

            if (w > 4096 || w < 2)
            {
                Fail("Size out of range");
                return -1;
            }

            PoseidonAssert(_w == w);
            PoseidonAssert(_h == h);

            int lineCnt = w;
            int lineAlign = _pitch - w * 2;

            word* B = (word*)mem;
            while (W > 0)
            {
                int c = in.get();
                if (c & 0x80)
                {
                    int v = in.get();
                    c &= 0x7f;
                    c++;
                    W -= c;
                    word vv;
                    vv = resPal[v];
                    if (lineAlign == 0)
                    {
                        while (--c >= 0)
                        {
                            *B++ = vv;
                        }
                    }
                    else
                    {
                        while (--c >= 0)
                        {
                            *B++ = vv;
                            if (--lineCnt == 0)
                            {
                                B = (word*)((byte*)B + lineAlign), lineCnt = w;
                            }
                        }
                    }
                }
                else
                {
                    c++;
                    W -= c;
                    while (--c >= 0)
                    {
                        int v = in.get();
                        word vv;
                        vv = resPal[v];
                        *B++ = vv;
                        if (--lineCnt == 0)
                        {
                            B = (word*)((byte*)B + lineAlign), lineCnt = w;
                        }
                    }
                }
            }
        }

        if (in.fail() || in.eof())
        {
            Fail("Texture load failed.");
            return -1;
        }

        return 0;
    }
}

int PacLevelMem::LoadPaaBin16(QIStream& in, void* mem, const PacPalette* pal) const
{
    // return<0 - error
    // return>0 - no more mip-map levels available
    // return==0 - texture loaded successfully

    PoseidonAssert(pal->_nColors == 0);

    // load PAA file
    // get image size
    long w = fgetiw(in);
    long h = fgetiw(in);
    if (w == 0 && h == 0)
    {
        return 1;
    } // mip-map list terminator

    // get compressed data size
    long dSize = fgeti24(in);
    (void)dSize;

    if (w > 4096 || w < 2)
    {
        Fail("Size out of range");
        return -1;
    }

    PoseidonAssert(_w == w);
    PoseidonAssert(_h == h);

    if (_dFormat != PacARGB8888)
    {
        // picture is saved in 4444 LZW format
        if (DecodeLZW(in, (char*)mem, _w * _h * 2, _w * 2, _pitch) < 0)
        {
            Fail("LZW Decode error");
            return -1;
        }

        if (_sFormat == PacAI88 && _dFormat == PacARGB4444)
        {
            int n = _w * _h;
            word* data = reinterpret_cast<word*>(mem);
            PoseidonAssert(data);
            while (--n >= 0)
            {
                word s = *data;
                word a = (s & 0xf000);
                word i = (s & 0x00f0);
                word d = a | (i << 4) | i | (i >> 4);
                *data++ = d;
            }
        }
        else
        {
            DoAssert(ENUM_CAST(PacFormat, _sFormat) == ENUM_CAST(PacFormat, _dFormat));
        }
    }
    else
    {
        PoseidonAssert(_sFormat == PacAI88);
        // conversion from 88 to 8888
        // allocate temporary 88 surface
        AUTO_STATIC_ARRAY(char, temp, 256 * 256 * 2);
        temp.Resize(_w * _h * 2);
        if (DecodeLZW(in, temp.Data(), _w * _h * 2, _w * 2, _w * 2) < 0)
        {
            Fail("LZW Decode error");
            return -1;
        }
        // convert from temp to mem
        int n = _w * _h;
        DWORD* data = reinterpret_cast<DWORD*>(mem);
        const word* sdata = reinterpret_cast<const word*>(temp.Data());
        PoseidonAssert(data);
        while (--n >= 0)
        {
            DWORD s = *sdata++;
            DWORD i = (s & 0xff);
            DWORD d = (s << 16) | (i << 8) | i;
            *data++ = d;
        }
    }

    return 0;
}

// note: int. division very slow - precalc. table instead

#define DIV_3(x) ((x) / 3)

static char Div3[128];

static struct Div3Init
{
    Div3Init()
    {
        for (int i = 64; i < 128; i++)
        {
            Div3[i] = (i - 64) / 3;
        }
        for (int i = 0; i < 64; i++)
        {
            Div3[i] = -(-(i - 64) / 3);
        }
    }
} dummy;

inline int Convert565To1555(int x)
{
    return 0x8000 | (x & 0x1f) | ((x >> 1) & 0x7fe0);
}

void PacLevelMem::DecompressDXT1(void* dst, const void* src, int w, int h)
{
    const DXTBlock64* s = (const DXTBlock64*)src;
    word* line = (word*)dst;
    for (int y = 0; y < h; y += 4)
    {
        word* base = line;
        for (int x = 0; x < w; x += 4, base += 4)
        {
            word color[4];
            // decompress current block
            int c0 = s->c0;
            int c1 = s->c1;
            if (c0 > c1)
            {
                // 4 color block
                int rc0 = (c0 >> 11) & 0x1f;
                int gc0 = (c0 >> 5) & 0x3f;
                int bc0 = (c0) & 0x1f;
                int rc1 = (c1 >> 11) & 0x1f;
                int gc1 = (c1 >> 5) & 0x3f;
                int bc1 = (c1) & 0x1f;
                // c0-c1 is positive
                int rd3 = Div3[rc1 - rc0 + 64];
                int gd3 = Div3[gc1 - gc0 + 64];
                int bd3 = Div3[bc1 - bc0 + 64];
                // color[2] = c0 + (c1-c0)/3; // (2 * c0 + c1) / 3;
                // color[3] = c1 - (c1-c0)/3; // (c0 + 2 * c1) / 3;
                //  convert 565 to 1555
                //  verify c2, c3 components in range
                int rc2 = rc0 + rd3;
                int gc2 = gc0 + gd3;
                int bc2 = bc0 + bd3;

                int rc3 = rc1 - rd3;
                int gc3 = gc1 - gd3;
                int bc3 = bc1 - bd3;
                int c2 = (rc2 << 11) | (gc2 << 5) | bc2;
                int c3 = (rc3 << 11) | (gc3 << 5) | bc3;
                color[0] = Convert565To1555(c0);
                color[1] = Convert565To1555(c1);
                color[2] = Convert565To1555(c2);
                color[3] = Convert565To1555(c3);
            }
            else if (c0 == c1)
            {
                // mono-color transparent block
                word c01555 = Convert565To1555(c0);
                color[0] = c01555;
                color[1] = c01555;
                color[2] = c01555;
                color[3] = c01555 & 0x7fff;
            }
            else
            {
                int rc0 = (c0 >> 11) & 0x1f;
                int gc0 = (c0 >> 5) & 0x3f;
                int bc0 = (c0) & 0x1f;
                int rc1 = (c1 >> 11) & 0x1f;
                int gc1 = (c1 >> 5) & 0x3f;
                int bc1 = (c1) & 0x1f;

                int rd2 = (rc1 - rc0) >> 1;
                int gd2 = (gc1 - gc0) >> 1;
                int bd2 = (bc1 - bc0) >> 1;

                int c2 = ((rc0 + rd2) << 11) | ((gc0 + gd2) << 5) | (bc0 + bd2);

                color[0] = Convert565To1555(c0);
                color[1] = Convert565To1555(c1);
                color[2] = Convert565To1555(c2);
                color[3] = color[2] & 0x7fff;
                // 3 color + transparency block
            }

            // get dibits
            int w0 = s->tex0;
            int w1 = s->tex1;

            word* tbase = base;
            *tbase++ = color[(w0) & 3];      // 1:0 Texel[0][0]
            *tbase++ = color[(w0 >> 2) & 3]; // 3:2 Texel[0][1]
            *tbase++ = color[(w0 >> 4) & 3]; // 5:4 Texel[0][2]
            *tbase = color[(w0 >> 6) & 3];   // 7:6 Texel[0][3]
            tbase += w - 3;

            *tbase++ = color[(w0 >> 8) & 3];  // 9:8 Texel[1][0]
            *tbase++ = color[(w0 >> 10) & 3]; // 11:10 Texel[1][1]
            *tbase++ = color[(w0 >> 12) & 3]; // 13:12 Texel[1][2]
            *tbase = color[(w0 >> 14) & 3];   // 15:14 Texel[1][3]
            tbase += w - 3;

            *tbase++ = color[(w1) & 3];      // 1:0 Texel[2][0]
            *tbase++ = color[(w1 >> 2) & 3]; // 3:2 Texel[2][1]
            *tbase++ = color[(w1 >> 4) & 3]; // 5:4 Texel[2][2]
            *tbase = color[(w1 >> 6) & 3];   // 7:6 Texel[2][3]
            tbase += w - 3;

            *tbase++ = color[(w1 >> 8) & 3];  // 9:8 Texel[3][0]
            *tbase++ = color[(w1 >> 10) & 3]; // 11:10 Texel[3][1]
            *tbase++ = color[(w1 >> 12) & 3]; // 13:12 Texel[3][2]
            *tbase = color[(w1 >> 14) & 3];   // 15:14 Texel[3][3]

            s++; // move to next block
        }
        line += 4 * w; // skip all 4 decompressed lines
    }
}

} // namespace Poseidon
#include <Poseidon/Foundation/Containers/StaticArray.hpp>
namespace Poseidon
{

int PacLevelMem::LoadPaaDXT(QIStream& in, void* mem, const PacPalette* pal) const
{
    // return<0 - error
    // return>0 - no more mip-map levels available
    // return==0 - texture loaded successfully

    if (in.fail())
    {
        Fail("Invalid compressed mip destination or stream");
        return -1;
    }
    PoseidonAssert(pal->_nColors == 0);

    // load PAA file
    // get image size
    long w = fgetiw(in);
    long h = fgetiw(in);
    if (in.fail() || in.eof()) { Fail("Incomplete compressed mip header"); return -1; }
    if (w == 0 && h == 0)
    {
        return 1;
    } // mip-map list terminator
    if (!mem) { Fail("Invalid compressed mip destination"); return -1; }

    // Bit 15 of the stored width marks an LZO-compressed level; the real width is
    // the low bits. Reading it raw produced widths like 34816 (0x8000 | 2048) and
    // 33280 (0x8000 | 512), which the range check below then rejected -- so every
    // compressed mip failed to load and its texture came out white. Arma 3 uses
    // this heavily; the modern PAA decoder already handles it (AST-013), and this
    // legacy path did not.
    const bool lzoCompressed = (w & 0x8000) != 0;
    w &= 0x7fff;

    // get compressed data size
    long dSize = fgeti24(in);

    if (w > 4096 || w < 2)
    {
        // Say which size: "out of range" alone cannot distinguish a texture above
        // the reader's ceiling from a mip tail below its floor.
        char detail[64];
        snprintf(detail, sizeof(detail), "%ldx%ld", w, h);
        static std::unordered_set<std::string> loggedSizes;
        if (loggedSizes.insert(detail).second)
            LOG_WARN(Graphics, "Pactext: level size {} out of range (accepts 2..4096)", detail);
        Fail("Size out of range");
        return -1;
    }

    // This fresh header must describe the destination metadata used by the
    // caller. In particular LZO's output bound must not use changed dimensions.
    // Keep legacy positive thin/tall heights rather than adding a new ceiling.
    if (in.fail() || in.eof() || h <= 0 || _w != w || _h != h)
    {
        Fail("Compressed mip dimensions do not match destination");
        return -1;
    }
    const size_t blockBytes = ENUM_CAST(PacFormat, _sFormat) == ENUM_CAST(PacFormat, PacDXT1) ? 8u : 16u;
    const size_t blocksW = (static_cast<size_t>(w) + 3) / 4;
    const size_t blocksH = (static_cast<size_t>(h) + 3) / 4;
    if (blocksH > std::numeric_limits<size_t>::max() / blocksW ||
        blocksW * blocksH > std::numeric_limits<size_t>::max() / blockBytes)
    {
        Fail("Compressed mip size overflow");
        return -1;
    }
    const size_t sourceCapacity = blocksW * blocksH * blockBytes;
    if (lzoCompressed && ENUM_CAST(PacFormat, _dFormat) != ENUM_CAST(PacFormat, _sFormat))
    {
        // The LZO path emits BC blocks, not converted pixels.
        Fail("Unsupported compressed mip conversion");
        return -1;
    }
    if (lzoCompressed)
    {
        // Block-compressed payload size from the level's own dimensions: DXT1
        // stores 8 bytes per 4x4 block, the rest 16. This is the decompressed
        // size the level must produce; anything else means the payload was not
        // what its header claimed.
        const size_t expected = sourceCapacity;

        WarmMipChainTiming* timing = GCurrentWarmMipChainTiming;
        if (timing)
        {
            ++timing->lzoMips;
            timing->dxtStoredBytes += static_cast<uint64_t>(dSize);
            timing->dxtOutputBytes += expected;
        }
        WarmMipPhase packedPhase(timing ? &timing->packedAllocCopyMs : nullptr);
        const char* borrowed = nullptr;
        size_t got = 0;
        const auto decode = [&](const char* bytes) {
            packedPhase.Stop();
            WarmMipPhase lzoPhase(timing ? &timing->lzoMs : nullptr);
            auto* replay=PaaLzoReplay();
            if(replay) {
                try {
                    if(replay->TryCopy(reinterpret_cast<const uint8_t*>(bytes),static_cast<size_t>(dSize),expected,static_cast<uint8_t*>(mem))) return expected;
                } catch (...) { /* Optional cache failure never replaces the decoder. */ }
            }
            const size_t result=Foundation::Lzo1x::Decompress(reinterpret_cast<const uint8_t*>(bytes),
                static_cast<size_t>(dSize), static_cast<uint8_t*>(mem), expected);
            if(replay && result==expected) {
                try { replay->StoreSuccessful(reinterpret_cast<const uint8_t*>(bytes),static_cast<size_t>(dSize),expected,static_cast<const uint8_t*>(mem),result); }
                catch (...) { /* Successful original decode remains successful. */ }
            }
            return result;
        };
        if (in.TryReadDisjointSpan(static_cast<int>(dSize), mem, expected, borrowed))
        {
            if (timing) ++timing->borrowedLzoMips;
            if (in.fail())
            {
                Fail("Compressed Read error");
                return -1;
            }
            // Backing data remains owned by this stream throughout the immediate
            // decoder call. Disjoint output cannot overwrite compressed input.
            got = decode(borrowed);
        }
        else
        {
            // Retain original copying/error semantics for aliases, truncation,
            // empty payloads or unsupported buffer/cursor invariants.
            AUTO_STATIC_ARRAY(char, packed, 256 * 256);
            packed.Resize(dSize);
            in.read(packed.Data(), dSize);
            if (in.fail())
            {
                Fail("Compressed Read error");
                return -1;
            }
            got = decode(packed.Data());
        }
        if (got != expected)
        {
            Fail("LZO level did not decompress to its declared size");
            return -1;
        }
        return 0;
    }

    PoseidonAssert(_w == w);
    PoseidonAssert(_h == h);

    // picture is saved in raw compressed data format

    if (ENUM_CAST(PacFormat, _dFormat) == ENUM_CAST(PacFormat, _sFormat))
    {
        // Short legacy raw tails intentionally leave the remainder untouched.
        // A larger declaration cannot fit the authoritative BC block layout.
        if (static_cast<size_t>(dSize) > sourceCapacity)
        {
            Fail("Compressed mip payload exceeds destination layout");
            return -1;
        }
        WarmMipChainTiming* timing = GCurrentWarmMipChainTiming;
        if (timing)
        {
            ++timing->rawDxtMips;
            timing->dxtStoredBytes += static_cast<uint64_t>(dSize);
            timing->dxtOutputBytes += static_cast<uint64_t>(dSize);
        }
        WarmMipPhase rawPhase(timing ? &timing->rawReadMs : nullptr);
        in.read(mem, dSize);
        rawPhase.Stop();
        if (in.fail())
        {
            Fail("Compressed Read error");
            return -1;
        }
    }
    else
    {
        if (ENUM_CAST(PacFormat, _sFormat) != ENUM_CAST(PacFormat, PacDXT1) ||
            ENUM_CAST(PacFormat, _dFormat) != ENUM_CAST(PacFormat, PacARGB1555) ||
            (w % 4) || (h % 4) || static_cast<size_t>(dSize) != sourceCapacity)
        {
            Fail("Unsupported or incomplete DXT1 pixel conversion");
            return -1;
        }
        // load to temporary buffer and decompress
        AUTO_STATIC_ARRAY(char, temp, 256 * 256);
        temp.Resize(dSize);
        in.read(temp.Data(), dSize);
        if (in.fail()) { Fail("Compressed Read error"); return -1; }
        // decompress from temp to mem
        DecompressDXT1(mem, temp.Data(), _w, _h);
    }

    return 0;
}

int PacLevelMem::LoadPac(QIStream& in, void* mem, const PacPalette* pal) const
{
    int ret = -1;
    switch (ENUM_CAST(PacFormat, _sFormat))
    {
        case PacDXT1:
        case PacDXT2:
        case PacDXT3:
        case PacDXT4:
        case PacDXT5:
            ret = LoadPaaDXT(in, mem, pal);
            break;
        case PacP8:
            switch (ENUM_CAST(PacFormat, _dFormat))
            {
                case PacARGB1555:
                    ret = LoadPacARGB1555(in, mem, pal);
                    break;
                case PacRGB565:
                    ret = LoadPacRGB565(in, mem, pal);
                    break;
                default:
                    LOG_ERROR(Graphics, "Bad destination format for P8 source {}", (int)ENUM_CAST(PacFormat, _sFormat));
                    ret = -1;
                    break;
            }
            break;
        default:
            LOG_ERROR(Graphics, "Bad source texture format {}", (int)ENUM_CAST(PacFormat, _sFormat));
            ret = -1;
            break;
    }
    return ret;
}
int PacLevelMem::LoadPaa(QIStream& in, void* mem, const PacPalette* pal) const
{
    int ret = -1;
    switch (ENUM_CAST(PacFormat, _sFormat))
    {
        case PacARGB1555:
        case PacARGB4444:
        case PacAI88:
            ret = LoadPaaBin16(in, mem, pal);
            break;
        case PacARGB8888:
        {
            long w = fgetiw(in);
            long h = fgetiw(in);
            if (w == 0 && h == 0)
            {
                ret = 1; // mipmap terminator
                break;
            }
            long dSize = fgeti24(in);
            PoseidonAssert(_w == w);
            PoseidonAssert(_h == h);
            if (_dFormat == PacARGB8888)
            {
                in.read(static_cast<char*>(mem), dSize);
                ret = in.fail() ? -1 : 0;
            }
            else
            {
                std::vector<char> tmp(dSize);
                in.read(tmp.data(), dSize);
                if (in.fail())
                {
                    ret = -1;
                    break;
                }
                int srcPitch = _w * 4;
                char* dst = static_cast<char*>(mem);
                for (int y = 0; y < _h; y++)
                    memcpy(dst + y * _pitch, tmp.data() + y * srcPitch, srcPitch);
                ret = 0;
            }
            break;
        }
        case PacDXT1:
        case PacDXT2:
        case PacDXT3:
        case PacDXT4:
        case PacDXT5:
            ret = LoadPaaDXT(in, mem, pal);
            break;
        default:
            Fail("Bad texture format.");
            ret = -1;
            break;
    }
    return ret;
}

void PacLevelMem::Interpolate(void* data, void* withData, const PacLevelMem& with, float factor)
{
    if (ENUM_CAST(PacFormat, _dFormat) != ENUM_CAST(PacFormat, with._dFormat))
    {
        Fail("Interpolated surface format does not match.");
        return;
    }
    if (_dFormat == PacARGB1555)
    {
        PoseidonAssert(_w == with._w);
        PoseidonAssert(_pitch == with._pitch);
        PoseidonAssert(_h == with._h);
        const word* sData = reinterpret_cast<const word*>(withData);
        word* dData = reinterpret_cast<word*>(data);
        int x, y;
        int coef = toIntFloor(factor * 256);
        if (coef > 255)
        {
            coef = 255;
        }
        if (coef < 0)
        {
            coef = 0;
        }
        int dCoef = 255 - coef;
        for (y = 0; y < _h; y++)
        {
            for (x = 0; x < _w; x++)
            {
                int offset = y * (_pitch / 2) + x;
                word s = sData[offset];
                word& d = dData[offset];
                word dd = d;
                int r = (((s >> 10) & 0x1f) * coef + ((dd >> 10) & 0x1f) * dCoef + 128) >> 8;
                int g = (((s >> 5) & 0x1f) * coef + ((dd >> 5) & 0x1f) * dCoef + 128) >> 8;
                int b = (((s >> 0) & 0x1f) * coef + ((dd >> 0) & 0x1f) * dCoef + 128) >> 8;
                // assume alpha opaque
                d = 0x8000 | (r << 10) | (g << 5) | b;
            }
        }
    }
    else if (_dFormat == PacRGB565)
    {
        PoseidonAssert(_w == with._w);
        PoseidonAssert(_pitch == with._pitch);
        PoseidonAssert(_h == with._h);
        const word* sData = reinterpret_cast<const word*>(withData);
        word* dData = reinterpret_cast<word*>(data);
        int x, y;
        int coef = toIntFloor(factor * 256);
        if (coef > 255)
        {
            coef = 255;
        }
        if (coef < 0)
        {
            coef = 0;
        }
        int dCoef = 255 - coef;
        for (y = 0; y < _h; y++)
        {
            for (x = 0; x < _w; x++)
            {
                int offset = y * (_pitch / 2) + x;
                word s = sData[offset];
                word& d = dData[offset];
                word dd = d;
                int r = (((s >> 11) & 0x1f) * coef + ((dd >> 11) & 0x1f) * dCoef + 128) >> 8;
                int g = (((s >> 5) & 0x3f) * coef + ((dd >> 5) & 0x3f) * dCoef + 128) >> 8;
                int b = (((s >> 0) & 0x1f) * coef + ((dd >> 0) & 0x1f) * dCoef + 128) >> 8;
                // assume alpha opaque
                d = (r << 11) | (g << 5) | b;
            }
        }
    }
    else
    {
        Fail("Unsupported interpolation format.");
    }
}

void PacLevelMem::SetDestFormat(PacFormat dFormat, int align)
{
    _dFormat = dFormat;
    // mipmap already initialized
    PoseidonAssert(_w > 0 && _h > 0);

    switch (ENUM_CAST(PacFormat, _dFormat))
    {
        case PacP8:
            _pitch = ((_w + align - 1) & ~(align - 1));
            break;
        case PacARGB4444:
        case PacARGB1555:
        case PacAI88:
        case PacRGB565:
            _pitch = ((_w * 2 + align - 1) & ~(align - 1));
            break;
        case PacARGB8888:
            _pitch = ((_w * 4 + align - 1) & ~(align - 1));
            break;
        // Block-compressed pitch is bytes per row of 4x4 blocks: DXT1 stores 8
        // bytes per block (half a byte per pixel), every other DXT stores 16
        // (one byte per pixel).
        case PacDXT1:
        // RFG-047: BC4 is BC1's block size with one channel -- 8 bytes per 4x4 block,
        // i.e. half a byte per texel, so the same row pitch.
        case PacBC4:
            _pitch = _w / 2;
            break;
        // DXT3 and DXT4 set their pitch and then fell through into the failure
        // case below -- they could never load. DXT5 was not handled at all, so
        // it landed on `default` and failed too. Measured on the local Arma 3
        // rock set, 36 of 114 textures are DXT5, so a third of the model
        // textures in a single addon were unloadable.
        //
        // DXT2 moves here from the DXT1 group for the same reason: it is DXT3
        // with premultiplied alpha and stores 16 bytes per block, so half the
        // pitch was wrong. No local corpus contains a DXT2 texture, so nothing
        // can have depended on the old value -- at half pitch it would have
        // decoded to garbage, not to something correct.
        case PacDXT2:
        case PacDXT3:
        case PacDXT4:
        case PacDXT5:
        // RFG-047: BC5 (two channels) and BC7 (RGBA) are 16 bytes per 4x4 block, the
        // same one byte per texel as BC2/BC3.
        case PacBC5:
        case PacBC7:
            _pitch = _w;
            break;
        default:
            Fail("Texture format");
            break;
    }
}

int PacLevelMem::Init(QIStream& in, PacFormat sFormat)
{
    // return<0 - error
    // return>0 - no more mip-map levels available
    // return==0 - texture loaded successfully

    // if data is already loaded, skip it

    int error = -1;

    _sFormat = sFormat;

    // get size and palette  size

    if (_w > 0 && _h > 0)
    {
        // mipmap already initialized
        // no need to read and seek - offset based file
        return 0;
    }

    {
        int startOffset = in.tellg(); // remember start offset

        int w = fgetiw(in); // get texture dimensions
        int h = fgetiw(in);
        if (w == 0 && h == 0)
        {
            error = 1;
            goto Error;
        } // mip-map list terminator

        if (_start >= 0)
        {
            DoAssert(_start == startOffset);
        }
        _start = startOffset;
        // some textures are saved using lzw
        if (w == MAGIC_W_LZW && h == MAGIC_H_LZW)
        {
            w = fgetiw(in);
            h = fgetiw(in);
        }

        // Bit 15 of the stored width marks a compressed mip (AST-013). Arma 2/3
        // textures set it routinely: every one of 3,283 sampled from the Arma 3
        // archives does. Unmasked, a 2048-wide mip reads as 0x8800 = 34816 and
        // falls into the size check below, which then blames the dimension for
        // what is actually an unsupported compression -- and sends anyone reading
        // the log looking for a corrupt file that is perfectly valid.
        //
        // This function only reads headers and sizes the level -- it skips the
        // payload rather than decoding it -- so the flag is masked off and the
        // level is accepted. Decompression happens where the block data is
        // actually consumed (PAADecoder's DXT path, via Foundation::Lzo1x).
        if (w & 0x8000)
            w &= 0x7FFF;

        // get compressed data size
        int dSize = fgeti24(in);

        if (w > 4096 || w < 2 || h > 4096 || h < 2)
        {
            LOG_ERROR(Graphics, "Extreme texture size ({}x{})", w, h);
            goto Error; // check extreme size
        }

        _w = w, _h = h;

        // skip actual image data
        in.seekg(dSize, QIOS::cur);

        error = 0;
        // no ramp - ramp is attached by bank
    }
Error:
    if (error < 0)
    {
        Fail("Texture init failed.");
        _w = 0;
        _h = 0;
    }
    return error;
}

void PacLevelMem::SeekLevel(QIStream& in) const
{
    if (_start >= 0)
    {
        in.seekg(_start, QIOS::beg);
        return;
    }
    Fail("Start point now known");
}

PackedColor PacLevelMem::GetPixel(void* data, float u, float v) const
{
    int iU = toIntFloor(u * _w);
    int iV = toIntFloor(v * _h);
    if (iU < 0)
    {
        iU = 0;
    }
    if (iV < 0)
    {
        iV = 0;
    }
    if (iU > _w - 1)
    {
        iU = _w - 1;
    }
    if (iV > _h - 1)
    {
        iV = _h - 1;
    }
    return GetPixelInt(data, iU, iV);
}

inline PackedColor PackedColor565(int rgb)
{
    int r = (rgb >> 11) & 0x1f;
    int g = (rgb >> 5) & 0x3f;
    int b = (rgb >> 0) & 0x1f;
    const int scale5 = 255 / 31;
    const int scale6 = 255 / 63;
    return PackedColor(r * scale5, g * scale6, b * scale5, 255);
}

inline int Convert5To8(int x)
{
    // return toIntFloor(x*(255.0/31)); // exact but not fast
    return (x * 8424) >> 10; // much faster
}

inline int Convert6To8(int x)
{
    // return toIntFloor(x*(255.0/31)); // exact but not fast
    return (x * 8290) >> 11; // much faster
}

inline int Convert565To8888(int x)
{
    int r = Convert5To8((x >> 11) & 0x1f);
    int g = Convert6To8((x >> 5) & 0x3f);
    int b = Convert5To8((x) & 0x1f);
    return 0xff000000 | (r << 16) | (g << 8) | b;
}

PackedColor PacLevelMem::GetPixelInt(void* data, int u, int v) const
{
    switch (ENUM_CAST(PacFormat, _dFormat))
    {
        case PacDXT1:
        {
            // calc. 64-b block address
            int x = u >> 2, y = v >> 2;
            int bPitch = _w >> 2;
            DXTBlock64* s = ((DXTBlock64*)data) + y * bPitch + x;
            // pitch of one 64b blocks line (2 actual lines) size
            // get color 0 and 1

            int xb = u & 3, yb = v & 3;

            DWORD color[4];
            int c0 = s->c0;
            int c1 = s->c1;
            if (c0 > c1)
            {
                // 4 color block
                int rc0 = (c0 >> 11) & 0x1f;
                int gc0 = (c0 >> 5) & 0x3f;
                int bc0 = (c0) & 0x1f;
                int rc1 = (c1 >> 11) & 0x1f;
                int gc1 = (c1 >> 5) & 0x3f;
                int bc1 = (c1) & 0x1f;
                // c0-c1 is positive
                int rd3 = Div3[rc1 - rc0 + 64];
                int gd3 = Div3[gc1 - gc0 + 64];
                int bd3 = Div3[bc1 - bc0 + 64];
                // color[2] = c0 + (c1-c0)/3; // (2 * c0 + c1) / 3;
                // color[3] = c1 - (c1-c0)/3; // (c0 + 2 * c1) / 3;
                //  convert 565 to 1555
                //  verify c2, c3 components in range
                int rc2 = rc0 + rd3;
                int gc2 = gc0 + gd3;
                int bc2 = bc0 + bd3;

                int rc3 = rc1 - rd3;
                int gc3 = gc1 - gd3;
                int bc3 = bc1 - bd3;
                int c2 = (rc2 << 11) | (gc2 << 5) | bc2;
                int c3 = (rc3 << 11) | (gc3 << 5) | bc3;

                color[0] = Convert565To8888(c0);
                color[1] = Convert565To8888(c1);
                color[2] = Convert565To8888(c2);
                color[3] = Convert565To8888(c3);
            }
            else if (c0 == c1)
            {
                // mono-color transparent block
                DWORD c08888 = Convert565To8888(c0);
                color[0] = c08888;
                color[1] = c08888;
                color[2] = c08888;
                color[3] = c08888 & 0x00ffffff;
            }
            else
            {
                int rc0 = (c0 >> 11) & 0x1f;
                int gc0 = (c0 >> 5) & 0x3f;
                int bc0 = (c0) & 0x1f;
                int rc1 = (c1 >> 11) & 0x1f;
                int gc1 = (c1 >> 5) & 0x3f;
                int bc1 = (c1) & 0x1f;

                int rd2 = (rc1 - rc0) >> 1;
                int gd2 = (gc1 - gc0) >> 1;
                int bd2 = (bc1 - bc0) >> 1;

                int c2 = ((rc0 + rd2) << 11) | ((gc0 + gd2) << 5) | (bc0 + bd2);

                color[0] = Convert565To8888(c0);
                color[1] = Convert565To8888(c1);
                color[2] = Convert565To8888(c2);
                color[3] = color[2] & 0x00ffffff;
                // 3 color + transparency block
            }

            int w0 = s->tex0;
            int w1 = s->tex1;

            int b = (yb << 2) | xb;
            switch (b)
            {
                case 0:
                    return (PackedColor)color[(w0) & 3]; // 1:0 Texel[0][0]
                case 1:
                    return (PackedColor)color[(w0 >> 2) & 3]; // 3:2 Texel[0][1]
                case 2:
                    return (PackedColor)color[(w0 >> 4) & 3]; // 5:4 Texel[0][2]
                case 3:
                    return (PackedColor)color[(w0 >> 6) & 3]; // 7:6 Texel[0][3]

                case 4:
                    return (PackedColor)color[(w0 >> 8) & 3]; // 9:8 Texel[1][0]
                case 5:
                    return (PackedColor)color[(w0 >> 10) & 3]; // 11:10 Texel[1][1]
                case 6:
                    return (PackedColor)color[(w0 >> 12) & 3]; // 13:12 Texel[1][2]
                case 7:
                    return (PackedColor)color[(w0 >> 14) & 3]; // 15:14 Texel[1][3]

                case 8:
                    return (PackedColor)color[(w1) & 3]; // 1:0 Texel[2][0]
                case 9:
                    return (PackedColor)color[(w1 >> 2) & 3]; // 3:2 Texel[2][1]
                case 10:
                    return (PackedColor)color[(w1 >> 4) & 3]; // 5:4 Texel[2][2]
                case 11:
                    return (PackedColor)color[(w1 >> 6) & 3]; // 7:6 Texel[2][3]

                case 12:
                    return (PackedColor)color[(w1 >> 8) & 3]; // 9:8 Texel[3][0]
                case 13:
                    return (PackedColor)color[(w1 >> 10) & 3]; // 11:10 Texel[3][1]
                case 14:
                    return (PackedColor)color[(w1 >> 12) & 3]; // 13:12 Texel[3][2]
                case 15:
                    return (PackedColor)color[(w1 >> 14) & 3]; // 15:14 Texel[3][3]
            }
        }
        case PacP8:
        {
            Fail("Unsupported format palette 8");
            return PackedColor(0);
        }
        case PacRGB565:
        {
            int rgb = ((Pixel*)data)[v * _w + u];
            return PackedColor565(rgb);
        }
        case PacARGB1555:
        {
            int rgb = ((Pixel*)data)[v * _w + u];
            int r = (rgb >> 10) & 0x1f;
            int g = (rgb >> 5) & 0x1f;
            int b = (rgb >> 0) & 0x1f;
            int a = ((rgb >> 15) & 1) * 255;
            const int scale5 = 255 / 31;
            return PackedColor(r * scale5, g * scale5, b * scale5, a);
        }
        case PacARGB4444:
        {
            int rgb = ((Pixel*)data)[v * _w + u];
            int a = (rgb >> 12) & 0xf;
            int r = (rgb >> 8) & 0xf;
            int g = (rgb >> 4) & 0xf;
            int b = (rgb >> 0) & 0xf;
            const int scale4 = 255 / 15;
            return PackedColor(r * scale4, g * scale4, b * scale4, a * scale4);
        }
        case PacAI88:
        {
            int rgb = ((Pixel*)data)[v * _w + u];
            int i = (rgb >> 0) & 0xff;
            int a = (rgb >> 8) & 0xff;
            return PackedColor(i, i, i, a);
        }
        default:
        {
            Fail("Bad texture format.");
            return PackedColor(0, 0, 0, 0);
        }
    }
}

// general memory management

SystemHeap::SystemHeap() : _alocated(nullptr), _size(0) {}

void SystemHeap::Release()
{
    MemoryHeap::Release();
    if (_alocated)
    {
        delete[] _alocated, _alocated = nullptr;
    }
    _size = 0;
}
void SystemHeap::Init(int size)
{
    // we must remember alocated memory because we need to free it
    // and it may be aligned by MemoryHeap::Init
    _alocated = new byte[size];
    _size = size;
    if (_alocated)
    {
        MemoryHeap::Init(_alocated, _size, 8);
        PoseidonAssert((char*)Memory() + Size() <= (char*)_alocated + _size);
    }
}

template <>
DEFINE_FAST_ALLOCATOR(MemoryItem)

#if DO_FILLS
// for easier debugging performs fills on alloc and free
MemoryItem* MemoryHeap::Alloc(int size)
{
    MemoryItem* mem = Heap<byte*, int>::Alloc(size);
    if (mem)
    {
        int* buf = (int*)mem->Memory();
        for (int i = 0; i < size / 4; i++)
            buf[i] = NEW_FILL;
    }
    return mem;
}
void MemoryHeap::Free(MemoryItem* mem)
{
    PoseidonAssert(mem);
    int* buf = (int*)mem->Memory();
    int size = mem->Size() / 4;
    for (int i = 0; i < size; i++)
        buf[i] = DEL_FILL;
    Heap<byte*, int>::Free(mem);
}

#endif

bool SystemHeap::Check() const
{
    return MemoryHeap::Check();
}

SystemHeap::~SystemHeap()
{
    Release();
}

class TextureSourcePac : public ITextureSource
{
    PacPalette _pal;
    int _mipmaps;      //!< source file mipmap count
    PacFormat _format; //!< source file pixel format
    bool _isPaa; // Suffix-based existing PAA source-proof admission.
    bool _usesPaaLevelReader; // Independent format-marker decoding decision.
    RStringB _name;
    std::shared_ptr<const ArchiveSourceBinding> _archiveSourceBinding;
    BankReadMemberIdentity _archiveMemberIdentity{};
    bool _hasArchiveMemberIdentity = false;

  public:
    TextureSourcePac();
    ~TextureSourcePac() override;

    bool Init(const char* name, PacLevelMem* mips, int maxMips) override;

    int GetMipmapCount() const override;
    PacFormat GetFormat() const override;
    bool GetMipmapData(void* mem, const PacLevelMem& mip, int level) const override;
    bool GetMipmapChain(const MipmapRead* reads, int count) const override;

    PackedColor GetAverageColor() const override { return _pal.AverageColor32(); }

    bool IsAlpha() const override { return _pal._isAlpha; }
    bool IsTransparent() const override { return _pal._isTransparent; }
    void ForceAlpha() override { _pal._isAlpha = true; }
    std::shared_ptr<const ArchiveSourceBinding> GetArchiveSourceBinding() const override
    { return _archiveSourceBinding; }
    bool CopyArchiveMemberIdentity(BankReadMemberIdentity& out) const override
    {
        if (!_hasArchiveMemberIdentity) return false;
        out = _archiveMemberIdentity;
        return true;
    }
    bool ReleaseArchiveSourceBinding() override
    {
        if (!Foundation::IsMainThread() || !_archiveSourceBinding) return false;
        _archiveSourceBinding.reset(); // Only this owner; cache/worker/store aliases remain charged.
        return true;
    }
};

//! all routines required to create pac/paa texture
class TextureSourcePacFactory : public ITextureSourceFactory
{
  public:
    bool Check(const char* name) override;
    void PreInit(const char* name) override;
    ITextureSource* Create(const char* name, PacLevelMem* mips, int maxMips) override;
};

} // namespace Poseidon
#include <Poseidon/IO/FileServer.hpp>
namespace Poseidon
{

TextureSourcePac::TextureSourcePac()
{
    _mipmaps = 0;
    _isPaa = false;
    _usesPaaLevelReader = false;
}

TextureSourcePac::~TextureSourcePac() = default;

static PacFormat BasicFormat(const char* name)
{
    const char* ext = strrchr(name, '.');
    if (ext && !strcmpi(ext, ".paa"))
    {
        return PacARGB4444;
    }
    else
    {
        return PacP8;
    }
}

#define MIN_MIP_SIZE 4 // guarantee QWORD alignment (4 16-bit pixels are enough)

bool TextureSourcePac::Init(const char* name, PacLevelMem* mips, int maxMips)
{
    _archiveSourceBinding.reset();
    _hasArchiveMemberIdentity = false;
    _name = name;
    QIFStream in;
    GFileServer->Open(in, name);
    // This guard owns only the exact already-open Init buffer. Failure completion
    // drops its bootstrap proof; successful parsing first retains the same binding.
    struct InitProofCompletion
    {
        Ref<IFileBuffer> buffer;
        std::shared_ptr<const ArchiveSourceBinding> initialized;
        ~InitProofCompletion() { if (buffer) buffer->CompleteArchiveSourceInit(initialized); }
    } completion;
    if (ArchiveSourceBinding::CacheHandoffEnabled() && Foundation::IsMainThread())
        completion.buffer = in.GetBuffer();
    if (in.fail())
    {
        return false;
    }
    if (in.rest() == 0)
    {
        return false;
    }

    // .paa should start with format marker
    int desc = fgetiw(in);
    bool alpha = false;
    PacFormat format = BasicFormat(name);

    _isPaa = format == PacARGB4444;
    _usesPaaLevelReader = _isPaa;

    PacFormat nFormat = PacFormatFromDesc(desc, alpha);
    if (nFormat != PacFormatN)
    {
        format = nFormat;
        _usesPaaLevelReader = true;
    }
    else
    {
        in.seekg(-2, QIOS::cur);
    }

    _format = format;

    const int maxOffsets = 16;
    int offsets[maxOffsets];
    for (int i = 0; i < maxOffsets; i++)
    {
        offsets[i] = -1;
    }

    if (_pal.Load(in, offsets, maxOffsets))
    {
        return false;
    }
    // get number of mipmaps
    // initialize them

    int i;
    for (i = 0; i < maxMips; i++)
    {
        if (offsets[0] >= 0 && offsets[i] < 0)
        {
            break; // last known mipmap read
        }

        PacLevelMem& mip = mips[i];

        mip.SetStart(offsets[i]);

        if (i > 0 && offsets[i] >= 0)
        {
            // _w and _h size is known
            mip._w = mips[0]._w >> i;
            mip._h = mips[0]._h >> i;
        }

        int ret = mips[i].Init(in, format);

        if (ret < 0)
        {
            return false;
        }
        if (ret > 0)
        {
            break; // last mip-map read
        }
        if (mip._w <= MIN_MIP_SIZE)
        {
            break;
        }
        if (mip._h <= MIN_MIP_SIZE)
        {
            break;
        }
    }
    _mipmaps = i;

    // The identity belongs to this exact successfully parsed Init buffer. A
    // cache hit may preserve an older birth identity, never a fresh path claim.
    static const bool retainMemberIdentity = [] {
        const char* value = std::getenv("WGR_OBJECT_STREAM_RAP_BUILDING_PILOT");
        const char* multi = std::getenv("WGR_OBJECT_STREAM_RAP_BUILDING_MULTISTAGE");
        return (value && std::strcmp(value, "1") == 0) ||
            (multi && std::strcmp(multi, "1") == 0);
    }();
    if ((retainMemberIdentity || TenementPhysicalPaaScope::Active()) && _isPaa && _mipmaps > 0 && in.GetBuffer())
        _hasArchiveMemberIdentity = in.GetBuffer()->CopyArchiveMemberIdentity(_archiveMemberIdentity);

    // The shared binding contains metadata/lease only, not the Init read buffer.
    // Old cache entries remain unsupported, even when a fresh bank name resolves.
    static const bool retainArchiveBinding = [] {
        const char* value = std::getenv("WGR_OBJECT_STREAM_WARM_TEXTURES");
        const char* cold = std::getenv("WGR_OBJECT_STREAM_COLD_TEXTURE_HANDOFF");
        return (value && std::strcmp(value, "1") == 0) || (cold && std::strcmp(cold, "1") == 0);
    }();
    // The exact retail PAC purpose retains only the binding from this original
    // successfully parsed Init buffer. It does not recapture by filename or
    // broaden the usual PAA rule for any other PAC.
    const bool retailPac = !_isPaa && ArchiveSourceBinding::RetailPacReadScope::MatchesLogicalName(name);
    if (((retainArchiveBinding && _isPaa) || retailPac) && _mipmaps > 0 && in.GetBuffer())
    {
        _archiveSourceBinding = in.GetBuffer()->GetArchiveSourceBinding();
        if (_archiveSourceBinding) ArchiveSourceBinding::NoteInitRetained();
        if (completion.buffer) completion.initialized = _archiveSourceBinding;
    }

    return true;
}

int TextureSourcePac::GetMipmapCount() const
{
    return _mipmaps;
}

PacFormat TextureSourcePac::GetFormat() const
{
    return _format;
}

//! Every VFS open this file makes to read mip data, from either entry point below.
//! Relaxed: this is a diagnostic tally, never a synchronisation point, and texture upload
//! runs on more than one thread.
static std::atomic<uint64_t> GTextureMipOpens{0};

uint64_t TextureMipOpenCount()
{
    return GTextureMipOpens.load(std::memory_order_relaxed);
}

//! The default batched read: one call per level, which is what every caller did before
//! GetMipmapChain existed. A source that opens nothing per level (DDS holds the decoded
//! file, JPEG the decoded pixels) has nothing to gain from overriding it.
bool ITextureSource::GetMipmapChain(const MipmapRead* reads, int count) const
{
    if (!reads)
    {
        return count <= 0;
    }
    for (int i = 0; i < count; i++)
    {
        if (!reads[i].mip || !GetMipmapData(reads[i].mem, *reads[i].mip, reads[i].level))
        {
            return false;
        }
    }
    return true;
}

bool TextureSourcePac::GetMipmapData(void* mem, const PacLevelMem& mip, int level) const
{
    const MipmapRead read{mem, &mip, level};
    return GetMipmapChain(&read, 1);
}

//! One open for the whole chain.
//!
//! PacLevelMem::SeekLevel is an absolute seek to that level's recorded start offset, so
//! the levels are independent of each other and of the stream's current position — which
//! is what makes reusing one stream across them legal, and what made re-opening per level
//! pure waste. On a settled world this path is cold; during the streaming transient it runs
//! once per texture, and it used to run once per texture per LEVEL.
bool TextureSourcePac::GetMipmapChain(const MipmapRead* reads, int count) const
{
    if (!reads || count <= 0)
    {
        return count <= 0;
    }

    PaaLzoReplayReport replayReport;
    WarmMipChainTiming timing(static_cast<const char*>(_name), count);
    WarmMipPhase openPhase(timing.enabled ? &timing.openMs : nullptr);
    QIFStream in;
    GFileServer->Open(in, _name);
    openPhase.Stop();
    GTextureMipOpens.fetch_add(1, std::memory_order_relaxed);
    if (in.fail())
    {
        return false;
    }

    WarmMipPhase loopPhase(timing.enabled ? &timing.loopMs : nullptr);
    for (int i = 0; i < count; i++)
    {
        if (!reads[i].mip)
        {
            return false;
        }
        if (timing.enabled) ++timing.selectedMips;
        const PacLevelMem& mip = *reads[i].mip;
        mip.SeekLevel(in);
        const int ret = _usesPaaLevelReader ? mip.LoadPaa(in, reads[i].mem, &_pal) : mip.LoadPac(in, reads[i].mem, &_pal);
        if (ret != 0)
        {
            return false;
        }
    }
    if (timing.enabled) timing.success = true;
    return true;
}

ITextureSource* CreateTextureSourcePac(const char* name, PacLevelMem* mips, int maxMips)
{
    TextureSourcePac* source = new TextureSourcePac;
    bool ret = source->Init(name, mips, maxMips);
    if (!ret)
    {
        delete source;
        return nullptr;
    }
    return source;
}

} // namespace Poseidon
#include <Poseidon/Graphics/Textures/DdsImport.hpp>
#include <Poseidon/Graphics/Textures/JpgImport.hpp>
namespace Poseidon
{

ITextureSource* CreateTextureSourceJPEG(const char* name, PacLevelMem* mips, int maxMips)
{
#if defined _WIN32
    TextureSourceJPEG* source = new TextureSourceJPEG;
    if (!source->Init(name, mips, maxMips))
    {
        delete source;
        return nullptr;
    }
    return source;
#else
    return nullptr;
#endif
}

bool TextureSourcePacFactory::Check(const char* name)
{
    //! pre-init data source
    return QIFStreamB::FileExist(name);
}
void TextureSourcePacFactory::PreInit(const char* name)
{
    GFileServer->Request(name, 2);
}

ITextureSource* TextureSourcePacFactory::Create(const char* name, PacLevelMem* mips, int maxMips)
{
    TextureSourcePac* source = new TextureSourcePac;
    if (!source->Init(name, mips, maxMips))
    {
        delete source;
        return nullptr;
    }
    return source;
}

static TextureSourcePacFactory STextureSourcePacFactory;

TextureSourcePacFactory* GTextureSourcePacFactory = &STextureSourcePacFactory;

ITextureSourceFactory* SelectTextureSourceFactory(const char* name)
{
    if (!name || !name[0])
    {
        return nullptr;
    }
    const char* filename = GetFilenameExt(name);
    // No filename component (name ends in a path separator, e.g. an empty
    // optional equipment icon "dtaext\equip\") is "no texture", not an error.
    // Return null silently — logging here trips strict mode on a benign empty
    // gear slot the original engine simply skipped.
    if (!filename[0])
    {
        return nullptr;
    }
    // RFG-081: `enfc|R,G,B` is a flat colour with no file behind it (DdsImport.cpp,
    // RFG-039), so it has no extension either, and GetFileExt read its last decimal
    // fraction as one: "Unrecognized texture type '.1880': 'enfc|0.1680,0.1850,0.1880'".
    // Three materials per Everon load lost their colour that way, silently textureless.
    // The DDS source is the one that understands the name; hand it over before the
    // extension is looked at. The other `enf*|` names carry a real `.edds` at the end
    // and were never affected.
    if (strncmp(name, "enfc|", 5) == 0)
    {
        return GTextureSourceDDSFactory;
    }
    const char* ext = GetFileExt(filename);
    if (!strcmpi(ext, ".pac") || !strcmpi(ext, ".paa"))
    {
        return GTextureSourcePacFactory;
    }
    // .jpg, .png, .tga, .bmp all decode via stb_image; the JPEG factory
    // is already wired to stbi_load_from_memory and tolerates any of
    // them. Registering .png and .tga here is what makes the loose-
    // textures fallback usable — without it the resolver swaps the
    // .paa/.jpg name to a .png sibling and this selector rejects the
    // result as "Unrecognized texture type '.png'".
    if (!strcmpi(ext, ".jpg") || !strcmpi(ext, ".jpeg") || !strcmpi(ext, ".png") || !strcmpi(ext, ".tga") ||
        !strcmpi(ext, ".bmp"))
    {
        return GTextureSourceJPEGFactory;
    }
    // .dds and Enfusion's .edds share one source; it picks the container by the
    // 'ENF1' marker rather than by the extension, because DayZ names them
    // interchangeably. Without these two, every DayZ water texture -- normal map,
    // caustic, environment, flow map -- reached the resolver and was rejected here
    // as "Unrecognized texture type '.edds'" (DZ-002).
    if (!strcmpi(ext, ".dds") || !strcmpi(ext, ".edds"))
    {
        return GTextureSourceDDSFactory;
    }
    LOG_ERROR(Graphics, "Unrecognized texture type '{}': '{}'", ext, name);
    return nullptr;
}
} // namespace Poseidon
