#include <Poseidon/Asset/Formats/Common/FormatDetector.hpp>
#include <Poseidon/Asset/Formats/P3D/OdolRevision.hpp>
#include <cstring>
#include <fstream>

namespace Poseidon::Asset::Formats
{

FormatInfo P3DFormatDetector::DetectFormat(const std::string& filePath)
{
    std::ifstream stream(filePath, std::ios::binary);
    if (!stream)
    {
        FormatInfo info;
        info.isSupported = false;
        info.errorMessage = "Cannot open file: " + filePath;
        info.extension = GetFileExtension(filePath);
        return info;
    }
    return DetectFormat(stream, GetFileExtension(filePath));
}

FormatInfo P3DFormatDetector::DetectFormat(std::istream& stream, const std::string& extension)
{
    FormatInfo info;
    info.extension = extension;

    std::streampos startPos = stream.tellg();

    char signature[5] = {0};
    stream.read(signature, 4);
    if (stream.gcount() < 4)
    {
        info.isSupported = false;
        info.errorMessage = "File too small (< 4 bytes)";
        stream.seekg(startPos);
        return info;
    }
    info.signature = std::string(signature, 4);

    uint32_t version = 0;
    stream.read(reinterpret_cast<char*>(&version), 4);
    if (stream.fail())
    {
        info.isSupported = false;
        info.errorMessage = "Cannot read version field";
        stream.seekg(startPos);
        return info;
    }
    info.version = version;

    stream.seekg(startPos);

    if (info.signature == "MLOD")
    {
        info.isSupported = IsSupportedMLODVersion(version);
        if (!info.isSupported)
            info.errorMessage = "Unsupported MLOD version: " + info.GetVersionString() + " (only 1.0 supported)";
    }
    else if (info.signature == "ODOL")
    {
        info.isSupported = IsSupportedODOLVersion(version);
        if (!info.isSupported)
            info.errorMessage = "Unsupported ODOL version: " + std::to_string(version) + " (" +
                                std::string(P3D::DescribeOdolRevision(version).generation) + ")";
    }
    else
    {
        info.isSupported = false;
        bool hasPrintable = false;
        for (int i = 0; i < 4; ++i)
        {
            if (signature[i] >= 32 && signature[i] <= 126)
            {
                hasPrintable = true;
                break;
            }
        }
        info.errorMessage = hasPrintable ? "Unknown format signature: '" + info.signature + "'"
                                         : "Not a valid P3D file (invalid signature)";
    }

    return info;
}

bool P3DFormatDetector::IsSupportedMLODVersion(uint32_t version)
{
    uint8_t major = (version >> 8) & 0xFF;
    uint8_t minor = version & 0xFF;
    // MLOD 1.0 (0x0100) and 1.1 (0x0101) — both versions present in OFP/CWA files
    return (major == 1 && (minor == 0 || minor == 1));
}

bool P3DFormatDetector::IsSupportedODOLVersion(uint32_t version)
{
    // Deferred to the revision table rather than restated here. This was a
    // hardcoded {7, 8}, and because ModelCache consults the detector *before*
    // handing the file to ODOLLoader, it silently gated revision 73 out of the
    // runtime path -- AST-012B's reader was reachable from its own tests and
    // from nothing else. A second copy of "which revisions exist" is exactly the
    // drift AST-012A introduced DescribeOdolRevision to prevent.
    //
    // v8 has no entry in that table and is kept explicitly: it is accepted by
    // the v7 reader, and dropping it here would be a silent narrowing.
    if (version == 8)
        return true;
    const auto revision = P3D::DescribeOdolRevision(version);
    return revision.support == P3D::OdolSupport::Parsed || revision.support == P3D::OdolSupport::NarrowSubset;
}

std::string P3DFormatDetector::GetFileExtension(const std::string& path)
{
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos && dot < path.length() - 1)
        return path.substr(dot + 1);
    return "";
}

} // namespace Poseidon::Asset::Formats
