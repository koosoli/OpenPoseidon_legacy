#include <Poseidon/Foundation/platform.hpp>

#undef _findfirst
#undef _findnext
#undef _findclose

#include <Poseidon/IO/Filesystem/Utf8Paths.hpp>

#include <windows.h>

#include <cstring>
#include <string>

namespace Poseidon
{
namespace
{
struct Utf8FindHandle
{
    HANDLE find = INVALID_HANDLE_VALUE;
};

// Never return a truncated filename: callers would open a different path.
bool FillEntry(const WIN32_FIND_DATAW& data, _finddata_t* info)
{
    const std::string name = WidePathToUtf8(data.cFileName);
    if (name.size() >= sizeof(info->name)) return false;
    std::memcpy(info->name, name.c_str(), name.size() + 1);
    info->attrib = data.dwFileAttributes & (_A_RDONLY | _A_HIDDEN | _A_SYSTEM | _A_SUBDIR | _A_ARCH);
    info->size = data.nFileSizeLow;
    const auto unixTime = [](FILETIME value) {
        ULARGE_INTEGER ticks;
        ticks.LowPart = value.dwLowDateTime;
        ticks.HighPart = value.dwHighDateTime;
        return static_cast<__time64_t>(ticks.QuadPart / 10000000ULL) - 11644473600LL;
    };
    info->time_create = unixTime(data.ftCreationTime);
    info->time_access = unixTime(data.ftLastAccessTime);
    info->time_write = unixTime(data.ftLastWriteTime);
    return true;
}
} // namespace

intptr_t Utf8FindFirst(const char* pattern, _finddata_t* info)
{
    if (pattern == nullptr || info == nullptr)
    {
        return -1;
    }

    const std::wstring wide = Utf8PathToWide(pattern);
    if (wide.empty())
    {
        return -1;
    }

    WIN32_FIND_DATAW data;
    const HANDLE find = ::FindFirstFileW(wide.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
    {
        return -1;
    }

    auto* handle = new Utf8FindHandle;
    handle->find = find;
    while (!FillEntry(data, info))
    {
        if (!::FindNextFileW(find, &data)) { ::FindClose(find); delete handle; return -1; }
    }
    return reinterpret_cast<intptr_t>(handle);
}

int Utf8FindNext(intptr_t handle, _finddata_t* info)
{
    if (handle == -1 || info == nullptr)
    {
        return -1;
    }

    auto* find = reinterpret_cast<Utf8FindHandle*>(handle);
    WIN32_FIND_DATAW data;
    while (::FindNextFileW(find->find, &data))
        if (FillEntry(data, info)) return 0;
    return -1;
}

int Utf8FindClose(intptr_t handle)
{
    if (handle == -1)
    {
        return -1;
    }

    auto* find = reinterpret_cast<Utf8FindHandle*>(handle);
    if (find->find != INVALID_HANDLE_VALUE)
    {
        ::FindClose(find->find);
    }
    delete find;
    return 0;
}

} // namespace Poseidon
