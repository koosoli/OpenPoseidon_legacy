#pragma once

#include <Poseidon/IO/Streams/QStream.hpp>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace Poseidon::Asset::Formats
{

// Arrays >= this size use LZSS compression in all BIS binary formats.
// 1024 matches the original game engine threshold.
#ifndef BIS_COMPRESSION_THRESHOLD
    #define BIS_COMPRESSION_THRESHOLD 1024
#endif
constexpr size_t COMPRESSION_THRESHOLD = BIS_COMPRESSION_THRESHOLD;

class BinaryReader
{
  public:
    explicit BinaryReader(QIStream& stream) : stream_(stream) {}

    int  tell() const { return stream_.tellg(); }
    void seek(int pos) { stream_.seekg(pos, QIOS::beg); }
    void seekRelative(int offset) { stream_.seekg(offset, QIOS::cur); }
    bool fail() const { return stream_.fail(); }

    // Bytes left in the stream. Used to reject a wire count that cannot possibly be
    // backed by the remaining input before it drives a huge allocation.
    int remaining() const { return stream_.rest(); }

    template<typename T>
    typename std::enable_if<std::is_arithmetic<T>::value, T>::type
    read()
    {
        if constexpr (std::is_same_v<T, bool>)
        {
            // Read into a byte and normalize: a raw wire byte landing in a bool's
            // storage is an out-of-range bool, and any later load of it is UB.
            unsigned char raw = 0;
            stream_.read(&raw, sizeof(raw));
            if (stream_.fail())
                throw std::runtime_error("Failed to read primitive type");
            return raw != 0;
        }
        else
        {
            T value;
            stream_.read(&value, sizeof(T));
            if (stream_.fail())
                throw std::runtime_error("Failed to read primitive type");
            return value;
        }
    }

    template<typename T>
    typename std::enable_if<std::is_arithmetic<T>::value, void>::type
    read(T& value)
    {
        value = read<T>();
    }

    std::string readAsciiz()
    {
        std::string result;
        char c;
        while (true)
        {
            stream_.read(&c, 1);
            if (stream_.fail() || c == '\0')
                break;
            result.push_back(c);
            if (result.size() > 1024 * 1024)
                throw std::runtime_error("String too long (no null terminator)");
        }
        return result;
    }

    std::string readString()
    {
        int32_t length = read<int32_t>();
        if (length < 0 || length > 1024 * 1024)
            throw std::runtime_error("Invalid string length");
        if (length == 0)
            return std::string();
        std::string result(length, '\0');
        stream_.read(&result[0], length);
        if (stream_.fail())
            throw std::runtime_error("Failed to read string data");
        return result;
    }

    template<typename T>
    typename std::enable_if<std::is_trivially_copyable<T>::value, std::vector<T>>::type
    readArray()
    {
        uint32_t count = read<uint32_t>();
        if (count == 0)
            return std::vector<T>();
        // An uncompressed array of `count` elements must fit the remaining input;
        // reject a count that would drive a huge allocation from a short stream.
        if (static_cast<uint64_t>(count) * sizeof(T) > static_cast<uint64_t>(remaining()))
            throw std::runtime_error("Array count exceeds remaining input");
        std::vector<T> result(count);
        stream_.read(result.data(), count * sizeof(T));
        if (stream_.fail())
            throw std::runtime_error("Failed to read array data");
        return result;
    }

    template<typename T>
    typename std::enable_if<std::is_trivially_copyable<T>::value, void>::type
    readArray(std::vector<T>& result)
    {
        result = readArray<T>();
    }

    template<typename T>
    typename std::enable_if<std::is_trivially_copyable<T>::value, std::vector<T>>::type
    readCompressedArray()
    {
        uint32_t count = read<uint32_t>();
        if (count == 0)
            return std::vector<T>();
        // Cap the decompressed size: the count is attacker-controlled and (unlike the
        // uncompressed path) can exceed the remaining input, so an unbounded value
        // would drive a huge allocation before any data is read. A P3D array above
        // this is malformed.
        if (static_cast<uint64_t>(count) * sizeof(T) > (256ull * 1024 * 1024))
            throw std::runtime_error("Compressed array count too large");
        std::vector<T> result(count);
        size_t sizeInBytes = count * sizeof(T);
        if (sizeInBytes >= COMPRESSION_THRESHOLD)
        {
            if (!decompressLZSS(result.data(), sizeInBytes))
                throw std::runtime_error("LZSS decompression failed");
        }
        else
        {
            stream_.read(result.data(), sizeInBytes);
            if (stream_.fail())
                throw std::runtime_error("Failed to read array data");
        }
        return result;
    }

    template<typename T>
    typename std::enable_if<std::is_trivially_copyable<T>::value, void>::type
    readCompressedArray(std::vector<T>& result)
    {
        result = readCompressedArray<T>();
    }

    void readBytes(void* buffer, size_t count)
    {
        stream_.read(buffer, static_cast<int>(count));
        if (stream_.fail())
            throw std::runtime_error("Failed to read raw bytes");
    }

    // Decompress exactly `count` bytes of an LZSS payload that carries no leading
    // element count. readCompressedArray cannot serve here because it reads its own
    // count first; the pre-LZO OPRW bulk arrays derive their size from the grid
    // dimensions instead, so the payload starts at the very first flag byte.
    //
    // The stream is left just past the payload's trailing checksum, which the
    // decoder consumes and verifies -- that check is the only thing that says an
    // LZSS payload was the size the caller claimed.
    void readCompressedBytes(void* buffer, size_t count)
    {
        if (count == 0)
            return;
        if (!decompressLZSS(buffer, count))
            throw std::runtime_error("LZSS decompression failed");
    }

  private:
    QIStream& stream_;

    bool decompressLZSS(void* dst, size_t size);
};

template<typename T>
T read(BinaryReader& reader)
{
    return reader.read<T>();
}

template<typename T>
void read(BinaryReader& reader, T& value)
{
    reader.read(value);
}

template<typename T>
std::vector<T> readArray(BinaryReader& reader)
{
    return reader.readArray<T>();
}

template<typename T>
void readArray(BinaryReader& reader, std::vector<T>& array)
{
    reader.readArray(array);
}

template<typename T>
std::vector<T> readCompressedArray(BinaryReader& reader)
{
    return reader.readCompressedArray<T>();
}

template<typename T>
void readCompressedArray(BinaryReader& reader, std::vector<T>& array)
{
    reader.readCompressedArray(array);
}

inline std::string readAsciiz(BinaryReader& reader)
{
    return reader.readAsciiz();
}

inline std::string readString(BinaryReader& reader)
{
    return reader.readString();
}

} // namespace Poseidon::Asset::Formats
