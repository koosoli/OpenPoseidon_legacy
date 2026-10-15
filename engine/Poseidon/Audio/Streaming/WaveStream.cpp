#include <Poseidon/Foundation/Common/Win.h>

#include <Poseidon/Audio/Streaming/WaveStream.hpp>
#include <Poseidon/Audio/Core/Format/WaveFile.hpp>
#include <Poseidon/Audio/Streaming/WaveStreamPlain.hpp>
#include <Poseidon/Audio/Streaming/WaveStreamOGG.hpp>
#include <Poseidon/Audio/Core/Format/WaveBuffer.hpp>
#include <Poseidon/Game/Compat/Arma1Compat.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <string.h>
#include <string>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Foundation/platform.hpp>

namespace Poseidon
{

using Poseidon::FileBufferMemory;

int WaveStream::GetDataClipped(void* data, int offset, int size) const
{
    if (size < 0)
    {
        Fail("GetDataClipped: Negative size");
        return 0;
    }
    // clip offset..size to valid range
    int offsetDone = 0;
    if (offset < 0)
    {
        int zeroData = -offset;
        saturateMin(zeroData, size);
        memset(data, 0, zeroData);
        offset += zeroData;
        size -= zeroData;
        offsetDone += zeroData;

        data = (char*)data + zeroData;
    }

    if (offset >= 0)
    {
        int total = GetUncompressedSize();
        int rest = total - offset;
        if (rest > 0)
        {
            int unpack = size;
            saturateMin(unpack, rest);
            // verify valid range
            if (offset < 0 || unpack < 0 || offset + unpack > total)
            {
                RptF("Sound: out of range: offset %d, unpack %d, total %d", offset, unpack, total);
                RptF("  size %d, offsetDone %d", size, offsetDone);
            }
            else
            {
                int dataDone = GetData(data, offset, unpack);
                offsetDone += dataDone;
                size -= dataDone;

                data = (char*)data + dataDone;
            }
        }
    }

    if (size > 0)
    {
        memset(data, 0, size);
    }

    return offsetDone;
}

int WaveStream::GetDataLooped(void* data, int offset, int size) const
{
    if (size < 0)
    {
        Fail("GetDataLooped: Negative size");
        return 0;
    }
    // clip offset..size to valid range
    int offsetDone = 0;
    if (offset < 0)
    {
        int zeroData = -offset;
        saturateMin(zeroData, size);
        memset(data, 0, zeroData);
        offset += zeroData;
        size -= zeroData;
        offsetDone += zeroData;

        data = (char*)data + zeroData;
    }

    if (offset >= 0)
    {
        int total = GetUncompressedSize();
        if (offset >= total)
        {
            offset = offset % total;
        }
        int rest = total - offset;
        if (rest > 0)
        {
            int unpack = size;
            saturateMin(unpack, rest);
            // verify valid range
            if (offset < 0 || unpack < 0 || offset + unpack > total)
            {
                RptF("Sound: out of range: offset %d, unpack %d, total %d", offset, unpack, total);
                RptF("  size %d, offsetDone %d", size, offsetDone);
            }
            else
            {
                int dataDone = GetData(data, offset, unpack);
                offsetDone += dataDone;
                size -= dataDone;

                data = (char*)data + dataDone;
            }
        }
        if (size > 0)
        {
            // end reached, but we need some more data
            int sizeStep = size;
            saturateMin(sizeStep, total);
            int dataDone = GetData(data, 0, sizeStep);
            offsetDone += dataDone;
            size -= dataDone;

            data = (char*)data + dataDone;
        }
    }

    if (size > 0)
    {
        memset(data, 0, size);
    }

    return offsetDone;
}

// Sound loading function - delegates to specific loaders based on extension

WaveStream* SoundLoadFile(const char* name)
{
    const char* ext = strrchr(name, '.');
    if (!ext)
    {
        return WSSLoadFile(name);
    }
    if (!stricmp(ext, ".wav"))
    {
        return WaveLoadFile(name);
    }
    if (!stricmp(ext, ".ogg"))
    {
        // OGGLoadFile is defined in WaveStreamOGG.cpp.
        extern WaveStream* OGGLoadFile(const char* name);
        return OGGLoadFile(name);
    }
    WaveStream* wss = WSSLoadFile(name);
    // Arma 1 configs and missions name their sounds .wss while the files ship as .ogg
    // (mp04cleansweep's CfgSounds: sound\m04r01.wss, the archive: Sound\M04r01.ogg); Arma 1
    // plays the .ogg. Only with --arma1/--arma2, and only when the .wss itself is missing.
    if (!wss && !stricmp(ext, ".wss") && Arma1Compat::Active())
    {
        std::string ogg(name, static_cast<size_t>(ext - name));
        ogg += ".ogg";
        if (QIFStreamB::FileExist(ogg.c_str()))
        {
            extern WaveStream* OGGLoadFile(const char* name);
            return OGGLoadFile(ogg.c_str());
        }
    }
    return wss;
}

static QIFStream MakeStreamFromMemory(const void* data, size_t size)
{
    auto* buf = new FileBufferMemory(static_cast<int>(size));
    memcpy(buf->GetWritableData(), data, size);
    QIFStream stream;
    stream.open(Ref<IFileBuffer>(buf));
    return stream;
}

static WaveStream* WaveLoadMemoryStream(QIFStream& file)
{
    WaveFile input;
    input.Open(file);
    if (input.GetError())
        return nullptr;
    if (input.Format().wFormatTag != WAVE_FORMAT_PCM)
        return nullptr;
    input.StartDataRead();
    if (input.GetError())
        return nullptr;
    IFileBuffer* fBuffer = input.GetBuffer();
    WaveBuffer buffer(fBuffer, input.Format(), input.DataOffset(), input.DataSize(), 0);
    return new WaveStreamPlain(buffer);
}

static WaveStream* WSSLoadMemoryStream(QIFStream& in)
{
    if (in.fail() || in.eof())
        return nullptr;

#ifdef _MSC_VER
    const int WSSMagic = '0SSW';
#else
    const int WSSMagic = StrToInt("WSS0");
#endif
    struct WSSHeader
    {
        int magic;
        char deltaPack;
        char resvd[3];
    };

    WSSHeader wHeader;
    WAVEFORMATEX header;
    in.read(reinterpret_cast<char*>(&wHeader), sizeof(wHeader));
    if (wHeader.magic != WSSMagic)
    {
        in.seekg(-static_cast<int>(sizeof(wHeader)), QIOS::cur);
        wHeader.deltaPack = 0;
    }
    int deltaPack = wHeader.deltaPack;
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (in.fail() || in.eof())
        return nullptr;
    header.cbSize = 0;
    IFileBuffer* fBuffer = in.GetBuffer();
    if (in.fail() || in.eof() || in.rest() <= 0)
        return nullptr;
    WaveBuffer buffer(fBuffer, header, in.tellg(), in.rest(), deltaPack);
    return new WaveStreamPlain(buffer);
}

WaveStream* SoundLoadMemory(const void* data, size_t size, const char* ext)
{
    if (!data || size == 0)
        return nullptr;

    QIFStream stream = MakeStreamFromMemory(data, size);

    if (ext && !stricmp(ext, ".wav"))
        return WaveLoadMemoryStream(stream);
    if (ext && !stricmp(ext, ".ogg"))
    {
        auto* ret = new WaveStreamOGG(stream);
        if (ret->GetError())
        {
            delete ret;
            return nullptr;
        }
        return ret;
    }
    return WSSLoadMemoryStream(stream);
}

} // namespace Poseidon
