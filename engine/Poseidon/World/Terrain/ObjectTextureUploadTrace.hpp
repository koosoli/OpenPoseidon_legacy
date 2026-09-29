#pragma once

// Optional owner-only first-touch attribution. This records the actual PAA
// uploads reached while one selected model is admitted, including proxies.
// It never selects or prepares a texture. The fixed top-K storage is created
// only for the explicit DayZ diagnostic and cannot grow with world content.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Poseidon::Streaming
{
struct ObjectTextureUploadTrace
{
    static constexpr size_t Limit = 16;
    struct Row
    {
        char name[256]{};
        double readMs = 0;
        size_t bytes = 0;
        bool prepared = false;
        bool uploaded = false;
    };
    std::array<Row, Limit> slowest{};
    size_t count = 0, uploads = 0, preparedUploads = 0, successfulUploads = 0;
    double measuredReadMs = 0;

    void Record(const char* name, double readMs, size_t bytes, bool prepared, bool uploaded)
    {
        ++uploads;
        preparedUploads += prepared;
        successfulUploads += uploaded;
        measuredReadMs += readMs;
        if (!name || readMs < 0) return;
        size_t length = 0;
        while (length < sizeof(Row::name) && name[length]) ++length;
        if (!length || length == sizeof(Row::name)) return;
        size_t slot = count;
        if (count < Limit) ++count;
        else
        {
            slot = 0;
            for (size_t i = 1; i < Limit; ++i)
                if (slowest[i].readMs < slowest[slot].readMs) slot = i;
            if (readMs <= slowest[slot].readMs) return;
        }
        Row& row = slowest[slot];
        std::memcpy(row.name, name, length);
        row.name[length] = '\0';
        row.readMs = readMs;
        row.bytes = bytes;
        row.prepared = prepared;
        row.uploaded = uploaded;
    }
};

inline thread_local ObjectTextureUploadTrace* GObjectTextureUploadTrace = nullptr;
} // namespace Poseidon::Streaming
