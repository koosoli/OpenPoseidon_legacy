#pragma once

// Original, narrow reader for Bohemia Interactive's binary `\0raP` config
// container.  It produces a neutral syntax tree so configuration consumers can
// interpret only the parts of the grammar they own (RVMAT is the first one).

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace Poseidon::Asset::Config
{
struct RapValue
{
    uint8_t type = 0;
    std::string text;
    float number = 0.0f;
    std::vector<RapValue> array;
};

struct RapMember
{
    uint8_t type = 0;
    std::string name;
    RapValue value;
    std::vector<RapMember> body;
};

class ArmaRapReader
{
    const std::vector<uint8_t>& bytes;
    size_t cursor = 0;

    void need(size_t n) const
    {
        if (cursor + n > bytes.size())
            throw std::runtime_error("truncated Arma raP config");
    }

    uint8_t byte()
    {
        need(1);
        return bytes[cursor++];
    }

    uint32_t u32()
    {
        need(4);
        const uint32_t result = uint32_t(bytes[cursor]) | (uint32_t(bytes[cursor + 1]) << 8) |
                                (uint32_t(bytes[cursor + 2]) << 16) | (uint32_t(bytes[cursor + 3]) << 24);
        cursor += 4;
        return result;
    }

    std::string string()
    {
        const size_t start = cursor;
        while (byte() != 0)
        {
        }
        return std::string(reinterpret_cast<const char*>(bytes.data() + start), cursor - start - 1);
    }

    uint32_t count()
    {
        uint32_t result = 0;
        unsigned shift = 0;
        while (true)
        {
            const uint8_t part = byte();
            result |= uint32_t(part & 0x7f) << shift;
            if ((part & 0x80) == 0)
                return result;
            shift += 7;
            if (shift > 28)
                throw std::runtime_error("invalid Arma raP count");
        }
    }

    RapValue value()
    {
        RapValue result;
        result.type = byte();
        if (result.type == 0 || result.type == 4)
            result.text = string();
        else if (result.type == 1)
        {
            const uint32_t bits = u32();
            std::memcpy(&result.number, &bits, sizeof(bits));
        }
        else if (result.type == 2)
            result.number = static_cast<float>(static_cast<int32_t>(u32()));
        else if (result.type == 3)
        {
            const uint32_t size = count();
            result.array.reserve(size);
            for (uint32_t i = 0; i < size; ++i)
                result.array.push_back(value());
        }
        else
            throw std::runtime_error("unsupported Arma raP value type");
        return result;
    }

    std::vector<RapMember> members()
    {
        // Each class body starts with its NUL-terminated parent reference. The
        // root has one too; it is binary metadata in retail files rather than a
        // printable base-class name, so a neutral reader intentionally ignores it.
        (void)string();
        const uint32_t size = count();
        if (size > 4096)
            throw std::runtime_error("invalid Arma raP member count");

        std::vector<RapMember> result;
        result.reserve(size);
        for (uint32_t i = 0; i < size; ++i)
        {
            RapMember member;
            member.type = byte();
            if (member.type == 0)
            {
                member.name = string();
                const uint32_t offset = u32();
                if (offset >= bytes.size())
                    throw std::runtime_error("invalid Arma raP class offset");
                const size_t resume = cursor;
                cursor = offset;
                member.body = members();
                cursor = resume;
            }
            else if (member.type == 1)
            {
                member.value.type = byte();
                member.name = string();
                if (member.value.type == 0 || member.value.type == 4)
                    member.value.text = string();
                else if (member.value.type == 1)
                {
                    const uint32_t bits = u32();
                    std::memcpy(&member.value.number, &bits, sizeof(bits));
                }
                else if (member.value.type == 2)
                    member.value.number = static_cast<float>(static_cast<int32_t>(u32()));
                else
                    throw std::runtime_error("unsupported Arma raP literal value type");
            }
            else if (member.type == 2 || member.type == 5)
            {
                member.name = string();
                member.value.type = 3;
                const uint32_t size = count();
                member.value.array.reserve(size);
                for (uint32_t j = 0; j < size; ++j)
                    member.value.array.push_back(value());
            }
            else if (member.type == 3 || member.type == 4)
                member.name = string();
            else
                throw std::runtime_error("unsupported Arma raP member type");
            result.push_back(std::move(member));
        }
        return result;
    }

  public:
    explicit ArmaRapReader(const std::vector<uint8_t>& source) : bytes(source) {}

    std::vector<RapMember> root()
    {
        if (bytes.size() < 13 || bytes[0] != 0 || bytes[1] != 'r' || bytes[2] != 'a' || bytes[3] != 'P')
            throw std::runtime_error("not an Arma raP config");
        // Bytes 0x0c..0x0f are the absolute enum-table offset.  The root
        // ClassBody begins at 0x10 with its inherited-class ASCIIZ field.
        cursor = 16;
        return members();
    }
};
} // namespace Poseidon::Asset::Config
