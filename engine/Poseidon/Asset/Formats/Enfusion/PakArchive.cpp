#include <Poseidon/Asset/Formats/Enfusion/PakArchive.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <memory>
#include <zlib.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Poseidon::Asset::Formats::Enfusion
{

class PakFile
{
  public:
    PakFile() = default;
    PakFile(const PakFile&) = delete;
    PakFile& operator=(const PakFile&) = delete;
    ~PakFile()
    {
#ifdef _WIN32
        if (_h != INVALID_HANDLE_VALUE)
            CloseHandle(_h);
#else
        if (_fd >= 0)
            close(_fd);
#endif
    }

    static std::shared_ptr<const PakFile> Open(const std::string& path)
    {
        auto file = std::make_shared<PakFile>();
#ifdef _WIN32
        // Same narrow-path (ANSI) interpretation as the std::ifstream it replaces. OVERLAPPED so
        // concurrent positional reads from several threads do not serialise on the file object
        // the way a synchronous handle's reads do; SHARE_DELETE so holding it open for the
        // mount's lifetime never blocks another program from updating the install.
        file->_h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        if (file->_h == INVALID_HANDLE_VALUE)
            return nullptr;
#else
        file->_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (file->_fd < 0)
            return nullptr;
#endif
        return file;
    }

    //! Exactly `size` bytes at absolute `offset`. Thread-safe and position-free.
    bool ReadAt(uint64_t offset, uint8_t* dst, size_t size) const
    {
#ifdef _WIN32
        struct ThreadEvent
        {
            HANDLE h = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            ~ThreadEvent()
            {
                if (h)
                    CloseHandle(h);
            }
        };
        thread_local ThreadEvent event;
        if (!event.h)
            return false;
        while (size > 0)
        {
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, size_t(1) << 30));
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(offset);
            ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
            ov.hEvent = event.h;
            DWORD got = 0;
            if (!ReadFile(_h, dst, chunk, nullptr, &ov) && GetLastError() != ERROR_IO_PENDING)
                return false;
            if (!GetOverlappedResult(_h, &ov, &got, TRUE) || got == 0)
                return false;
            dst += got;
            offset += got;
            size -= got;
        }
        return true;
#else
        while (size > 0)
        {
            const ssize_t got = ::pread(_fd, dst, size, static_cast<off_t>(offset));
            if (got <= 0)
                return false;
            dst += got;
            offset += static_cast<uint64_t>(got);
            size -= static_cast<size_t>(got);
        }
        return true;
#endif
    }

  private:
#ifdef _WIN32
    HANDLE _h = INVALID_HANDLE_VALUE;
#else
    int _fd = -1;
#endif
};

namespace
{

std::string LowerCopy(const std::string& text)
{
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace

void PakArchive::Close()
{
    _path.clear();
    _error.clear();
    _file.reset();
    _entries.clear();
    _directories = 0;
    _closure = {};
}

bool PakArchive::Open(const std::string& path)
{
    Close();

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        _error = "cannot open " + path;
        return false;
    }
    const auto end = file.tellg();
    if (end <= 0)
    {
        _error = "empty file " + path;
        return false;
    }
    const auto size = static_cast<size_t>(end);

    // Only the header and the two chunk headers are needed to locate the
    // directory; the DATA blob is up to 2 GB per archive and must not be read.
    file.seekg(0);
    std::vector<uint8_t> header(12);
    file.read(reinterpret_cast<char*>(header.data()), 12);
    if (ReadBe32(header.data()) != FourCC("FORM").value)
    {
        _error = path + ": not a FORM container";
        return false;
    }
    const uint32_t declared = ReadBe32(header.data() + 4);
    _closure.formSizeCloses = (static_cast<size_t>(declared) + 8 == size);
    const FourCC formType(ReadBe32(header.data() + 8));
    if (formType != FourCC("PAC1"))
    {
        _error = path + ": form type is '" + formType.ToString() + "', not 'PAC1'";
        return false;
    }

    size_t dataStart = 0;
    size_t dataSize = 0;
    size_t fileChunkStart = 0;
    size_t fileChunkSize = 0;

    size_t at = 12;
    uint8_t chunkHeader[8];
    while (at + 8 <= size)
    {
        file.seekg(static_cast<std::streamoff>(at));
        file.read(reinterpret_cast<char*>(chunkHeader), 8);
        if (!file)
        {
            _error = path + ": truncated chunk header at " + std::to_string(at);
            return false;
        }
        const FourCC tag(ReadBe32(chunkHeader));
        const size_t chunkSize = ReadBe32(chunkHeader + 4);
        const size_t payload = at + 8;
        if (payload + chunkSize > size)
        {
            _error = path + ": chunk '" + tag.ToString() + "' runs past the end";
            return false;
        }
        if (tag == FourCC("DATA"))
        {
            dataStart = payload;
            dataSize = chunkSize;
        }
        else if (tag == FourCC("FILE"))
        {
            fileChunkStart = payload;
            fileChunkSize = chunkSize;
        }
        at = payload + chunkSize;
    }
    _closure.chunkWalkCloses = (at == size);

    if (fileChunkSize == 0)
    {
        _error = path + ": no FILE directory chunk";
        return false;
    }

    std::vector<uint8_t> directory(fileChunkSize);
    file.seekg(static_cast<std::streamoff>(fileChunkStart));
    file.read(reinterpret_cast<char*>(directory.data()), static_cast<std::streamsize>(fileChunkSize));
    if (!file)
    {
        _error = path + ": could not read the FILE directory";
        return false;
    }

    _path = path;
    if (!ParseDirectory(directory, dataStart, dataSize))
    {
        const std::string message = _error;
        Close();
        _error = message;
        return false;
    }
    // Null on refusal: reads then fall back to reopening the archive per member.
    _file = PakFile::Open(path);
    return true;
}

bool PakArchive::ParseDirectory(const std::vector<uint8_t>& blob, size_t dataStart, size_t dataSize)
{
    const size_t total = blob.size();
    size_t at = 0;

    // The tree is a pre-order walk from a single nameless root. An explicit stack
    // rather than recursion: the directory is authored data, and a hand-made file
    // with a deep chain must not be able to overflow the stack.
    struct Frame
    {
        std::string prefix;
        uint32_t remaining;
    };
    std::vector<Frame> stack;

    auto readEntry = [&](std::string& name, uint8_t& kind) -> bool
    {
        if (at + 2 > total)
            return false;
        kind = blob[at];
        const uint8_t nameLength = blob[at + 1];
        at += 2;
        if (at + nameLength > total)
            return false;
        name.assign(reinterpret_cast<const char*>(blob.data()) + at, nameLength);
        at += nameLength;
        return true;
    };

    // Root.
    {
        std::string name;
        uint8_t kind = 0;
        if (!readEntry(name, kind) || kind != 0 || at + 4 > total)
        {
            _error = _path + ": directory does not begin with a root node";
            return false;
        }
        _directories++;
        const uint32_t children = ReadLe32(blob.data(), at);
        at += 4;
        stack.push_back({name.empty() ? std::string() : name + "/", children});
    }

    while (!stack.empty())
    {
        if (stack.back().remaining == 0)
        {
            stack.pop_back();
            continue;
        }
        stack.back().remaining--;

        std::string name;
        uint8_t kind = 0;
        if (!readEntry(name, kind))
        {
            _error = _path + ": directory truncated at " + std::to_string(at);
            return false;
        }
        if (kind == 0)
        {
            if (at + 4 > total)
            {
                _error = _path + ": directory node has no child count";
                return false;
            }
            _directories++;
            const uint32_t children = ReadLe32(blob.data(), at);
            at += 4;
            stack.push_back({stack.back().prefix + name + "/", children});
        }
        else if (kind == 1)
        {
            if (at + 24 > total)
            {
                _error = _path + ": file record truncated at " + std::to_string(at);
                return false;
            }
            PakEntry entry;
            entry.path = stack.back().prefix + name;
            entry.offset = ReadLe32(blob.data(), at);
            entry.storedSize = ReadLe32(blob.data(), at + 4);
            entry.realSize = ReadLe32(blob.data(), at + 8);
            const uint32_t reserved0 = ReadLe32(blob.data(), at + 12);
            const uint16_t reserved1 = ReadLe16(blob.data(), at + 16);
            entry.flags = blob[at + 18];
            entry.method = blob[at + 19];
            entry.stamp = ReadLe32(blob.data(), at + 20);
            at += 24;
            // Checked rather than assumed: these are zero across all 222,566
            // entries of the local corpus, and a non-zero one would mean the
            // record is wider than 24 bytes somewhere.
            if (reserved0 != 0 || reserved1 != 0)
                _closure.nonZeroReserved++;
            _entries.push_back(std::move(entry));
        }
        else
        {
            char message[96];
            std::snprintf(message, sizeof(message), ": unknown directory entry kind 0x%02x at %zu",
                          static_cast<unsigned>(kind), at - 2 - name.size());
            _error = _path + message;
            return false;
        }
    }
    _closure.directoryCloses = (at == total);

    // Does the payload tile DATA exactly? This is the closure the other three do
    // not imply: a field read at the wrong width still leaves the tree consumable
    // and the chunk walk intact, but the extents stop meeting.
    std::vector<const PakEntry*> ordered;
    ordered.reserve(_entries.size());
    for (const PakEntry& entry : _entries)
        ordered.push_back(&entry);
    std::sort(ordered.begin(), ordered.end(), [](const PakEntry* a, const PakEntry* b) { return a->offset < b->offset; });

    size_t cursor = dataStart;
    for (const PakEntry* entry : ordered)
    {
        if (entry->offset > cursor)
        {
            _closure.gapBytes += static_cast<size_t>(entry->offset) - cursor;
            cursor = static_cast<size_t>(entry->offset);
        }
        else if (entry->offset < cursor)
        {
            _closure.overlaps++;
        }
        cursor = std::max(cursor, static_cast<size_t>(entry->offset) + entry->storedSize);
    }
    _closure.dataTiles = (_entries.empty() || (cursor == dataStart + dataSize && _closure.gapBytes == 0 &&
                                               _closure.overlaps == 0));
    return true;
}

const PakEntry* PakArchive::Find(const std::string& path) const
{
    const std::string needle = LowerCopy(path);
    for (const PakEntry& entry : _entries)
        if (LowerCopy(entry.path) == needle)
            return &entry;
    return nullptr;
}

bool PakArchive::Read(const PakEntry& entry, std::vector<uint8_t>& out) const
{
    std::string error;
    const bool ok = PakReadRequest{_path, entry, _file}.Read(out, error);
    if (!ok) const_cast<PakArchive*>(this)->_error = std::move(error);
    return ok;
}

bool PakReadRequest::Read(std::vector<uint8_t>& out, std::string& error) const
{
    out.clear();
    error.clear();
    std::vector<uint8_t> stored(entry.storedSize);
    if (!(file && file->ReadAt(entry.offset, stored.data(), stored.size())))
    {
        std::ifstream reopened(archivePath, std::ios::binary);
        if (!reopened)
        {
            error = "cannot reopen " + archivePath;
            return false;
        }
        reopened.seekg(static_cast<std::streamoff>(entry.offset));
        reopened.read(reinterpret_cast<char*>(stored.data()), static_cast<std::streamsize>(entry.storedSize));
        if (!reopened)
        {
            error = entry.path + ": could not read " + std::to_string(entry.storedSize) + " stored bytes";
            return false;
        }
    }

    if (!entry.compressed())
    {
        if (entry.storedSize != entry.realSize)
        {
            error = entry.path + ": stored uncompressed but sizes differ";
            return false;
        }
        out = std::move(stored);
        return true;
    }
    if (entry.method != 6)
    {
        error =
            entry.path + ": unknown compression method " + std::to_string(entry.method);
        return false;
    }

    out.resize(entry.realSize);
    uLongf produced = entry.realSize;
    const int status = uncompress(out.data(), &produced, stored.data(), stored.size());
    if (status != Z_OK)
    {
        out.clear();
        error = entry.path + ": zlib failed with " + std::to_string(status);
        return false;
    }
    // A short decode that is accepted becomes a corrupt render much later, at a
    // point where nothing points back here.
    if (produced != entry.realSize)
    {
        out.clear();
        error = entry.path + ": decoded " + std::to_string(produced) + " of " +
                                                std::to_string(entry.realSize) + " declared bytes";
        return false;
    }
    return true;
}

bool PakArchive::Read(const std::string& path, std::vector<uint8_t>& out) const
{
    const PakEntry* entry = Find(path);
    if (!entry)
    {
        const_cast<PakArchive*>(this)->_error = path + ": not in " + _path;
        return false;
    }
    return Read(*entry, out);
}

} // namespace Poseidon::Asset::Formats::Enfusion
