#include <Poseidon/Foundation/Common/Win.h>
#include <Poseidon/Core/Application.hpp>

#include <Poseidon/Audio/Core/Format/WaveFile.hpp>
#include <Poseidon/Audio/Streaming/WaveStream.hpp>
#include <Poseidon/Audio/Streaming/WaveStreamPlain.hpp>
#include <Poseidon/Audio/Core/Format/WaveBuffer.hpp>

#include <Poseidon/Core/Global.hpp>
#include <Poseidon/IO/FileServer.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>

namespace Poseidon
{


// Abort the current read on failure (the file arg is ignored).
#define exc(file) \
    {             \
        return;   \
    }

// RIFF FOURCC macros
#define RIFF_FOURCC_C(ch0, ch1, ch2, ch3) \
    ((DWORD)(BYTE)(ch0) | ((DWORD)(BYTE)(ch1) << 8) | ((DWORD)(BYTE)(ch2) << 16) | ((DWORD)(BYTE)(ch3) << 24))

#define RIFF_FOURCC RIFF_FOURCC_C('R', 'I', 'F', 'F')
#define RIFF_WAVE RIFF_FOURCC_C('W', 'A', 'V', 'E')
#define RIFF_fmt RIFF_FOURCC_C('f', 'm', 't', ' ')
#define RIFF_data RIFF_FOURCC_C('d', 'a', 't', 'a')
#define RIFF_fact RIFF_FOURCC_C('f', 'a', 'c', 't')

struct RiffChunk
{
    DWORD id;
    DWORD len;
};

WaveFile::WaveFile()
{
    _format = nullptr;
}

void WaveFile::New(const QIFStream& file)
{
    error = true;
    _samples = 0;

    _file = file;
    _file.seekg(0, QIOS::beg);

    if (_file.fail())
        exc(file);

    // check file header

    RiffChunk chunk;
    _file.read(&chunk, sizeof(chunk));
    if (_file.fail() || chunk.id != RIFF_FOURCC)
        exc(filename);

    DWORD fformat;
    _file.read(&fformat, sizeof(fformat));
    if (_file.fail() || fformat != RIFF_WAVE)
        exc(filename);

    // search for the 'fmt ' chunk
    for (;;)
    {
        _file.read(&chunk, sizeof(chunk));
        if (_file.fail())
            exc(filename);
        if (chunk.id != RIFF_fmt)
        {
            // skip chunk -- a huge/overflowing chunk.len can leave the cursor at or
            // before its prior position (the seek clamps or fails), which would loop
            // the chunk search forever; require forward progress.
            const int before = _file.tellg();
            _file.seekg(chunk.len, QIOS::cur);
            if (_file.fail() || _file.tellg() <= before)
                exc(filename);
            continue;
        }
        // check if it is valid
        if (chunk.len < sizeof(WAVEFORMATEX))
        {
            if (chunk.len < sizeof(WAVEFORMAT))
            {
                LOG_ERROR(Audio, "Too short fmt chunk in PCM wave file");
                exc(filename);
            }
            else
            {
                // try to read old PCM format
                WAVEFORMAT fmt;
                _file.read(&fmt, sizeof(fmt));
                if (_file.fail())
                    exc(filename);
                _file.seekg(chunk.len - sizeof(fmt), QIOS::cur); // skip rest of chunk
                CreateFormat(sizeof(WAVEFORMATEX));
                _format->cbSize = 0;
                if (fmt.wFormatTag != WAVE_FORMAT_PCM)
                {
                    LOG_ERROR(Audio, "Old fmt chunk in PCM wave file");
                    exc(filename);
                }
                _format->wFormatTag = fmt.wFormatTag;
                _format->nAvgBytesPerSec = fmt.nAvgBytesPerSec;
                _format->nBlockAlign = fmt.nBlockAlign;
                _format->nChannels = fmt.nChannels;
                _format->nSamplesPerSec = fmt.nSamplesPerSec;
                _format->wBitsPerSample = fmt.nChannels ? (fmt.nBlockAlign / fmt.nChannels * 8) : 0;
            }
        }
        else
        {
            // read format -- chunk.len is an attacker-controlled DWORD; reject a fmt
            // chunk far larger than any real format before new char[chunk.len] (a value
            // above INT_MAX becomes a negative int and a near-2^64 allocation).
            if (chunk.len > 65536)
            {
                LOG_ERROR(Audio, "Oversized fmt chunk in wave file");
                exc(filename);
            }
            CreateFormat(chunk.len);
            _file.read(_format, chunk.len);
            if (_file.fail())
                exc(filename);

            if (_format->wFormatTag == WAVE_FORMAT_PCM)
            {
                if (chunk.len != sizeof(WAVEFORMATEX))
                {
                    LOG_ERROR(Audio, "Extra data in fmt chunk (PCM wave)");
                }
                _format->cbSize = 0;
            }
        }
        break;
    }

    error = false;
}

// Must run before Read: scans forward from just after the format to the 'data'
// chunk to descend into, skipping leading chunks such as 'fact'.
void WaveFile::StartDataRead()
{
    if (error)
    {
        return;
    }

    RiffChunk chunk;
    int lastPos = -1;
    for (;;)
    {
        // A huge/overflowing chunk length can leave the cursor at or before its prior
        // position (the seeks below clamp or fail), looping the search forever; bail
        // if an iteration made no forward progress.
        const int pos = _file.tellg();
        if (pos <= lastPos)
        {
            error = true;
            break;
        }
        lastPos = pos;
        _file.read(&chunk, sizeof(chunk));
        if (_file.fail())
        {
            error = true;
            break;
        }
        if (chunk.id == RIFF_fact)
        {
            DWORD fact;
            _file.read(&fact, sizeof(fact));
            _file.seekg(chunk.len - sizeof(fact), QIOS::cur);
            _samples = fact;
        }
        else if (chunk.id == RIFF_data)
        {
            _dataSize = chunk.len;
            _dataRest = chunk.len;
            _dataOffset = _file.tellg();
            break;
        }
        else
        {
            // skip chunk
            _file.seekg(chunk.len, QIOS::cur);
            continue;
        }
    }
}

// Reads from the data chunk; StartDataRead must have descended into it first.
UINT WaveFile::Read(UINT cbRead, char* Dest)
{
    if (error)
    {
        return 0;
    }

    UINT cbDataIn = cbRead;
    if (cbDataIn > _dataRest)
    {
        cbDataIn = _dataRest;
    }

    _dataRest -= cbDataIn;

    _file.read(Dest, cbDataIn);

    if (_file.fail())
    {
        error = true;
        return 0;
    }

    return cbDataIn;
}

void WaveFile::CreateFormat(int size)
{
    FreeFormat();
    _format = (WAVEFORMATEX*)new char[size];
}

void WaveFile::FreeFormat()
{
    if (_format)
    {
        delete[] (char*)_format;
        _format = nullptr;
    }
}

void WaveFile::Delete()
{
    FreeFormat();
    // no need to close the file
}

WaveStream* WaveLoadFile(const char* filename)
{
    QIFStreamB file;
    file.AutoOpen(filename);
    WaveFile input;
    input.Open(file);
    if (input.GetError())
    {
        return nullptr;
    }

    // check file format

    if (input.Format().wFormatTag == WAVE_FORMAT_PCM)
    {
        input.StartDataRead();
        if (input.GetError())
        {
            return nullptr;
        }

        IFileBuffer* fBuffer = input.GetBuffer();
        // return part of file buffer
        WaveBuffer buffer(fBuffer, input.Format(), input.DataOffset(), input.DataSize(), 0);
        return new WaveStreamPlain(buffer);
    }
    else
    {
        // Only PCM is supported; the ACM codec path that once sat behind a compile-time
        // switch had no class behind it and was never buildable.
        LOG_ERROR(Audio, "non-PCM format {}", input.Format().wFormatTag);
        return nullptr;
    }
}

} // namespace Poseidon
