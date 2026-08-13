#include <Poseidon/Asset/Probes/AssetInfo.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/Audio/Streaming/WaveStream.hpp>
#include <Poseidon/IO/FileServer.hpp>
#include <Poseidon/IO/FileServerMT.hpp>
#include <Poseidon/World/Model/ModelCache.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/World/Model/ModelFlags.hpp>
#include <Poseidon/World/Model/LodPurpose.hpp>
#include <Poseidon/Graphics/Rendering/Draw/SpecLods.hpp>
#include <Poseidon/Graphics/Rendering/Shape/ShapeShared.hpp>
#include <Poseidon/Graphics/Rendering/Draw/FontData.hpp>
#include <Poseidon/World/Terrain/WrpReader.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/PackFiles.hpp>
#include <Poseidon/Asset/Formats/Common/FormatDetector.hpp>

#include <fstream>
#include <ctime>
#include <sys/stat.h>
#include <algorithm>
#include <filesystem>
#include <Poseidon/Asset/Formats/RTM/RTMReader.hpp>
#include <stdio.h>
#include <cctype>
#include <memory>
#include <set>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>

namespace Poseidon
{
using Asset::Formats::FormatInfo;
using Asset::Formats::P3DFormatDetector;

// LOD resolution to human-readable name.
//
// AST-018: the table now lives with the IR (Model/LodPurpose.hpp). This kept its
// own copy, which is how it came to name twelve sentinels while ResolGeometryOnly
// named thirteen, and how any sentinel neither of them listed printed as a plain
// distance -- "1e+15" reads as a viewing distance, not as an unrecognised LOD.
static std::string ResolutionToName(float resolution)
{
    const Model::LodPurpose purpose = Model::ClassifyLodResolution(resolution);
    if (purpose != Model::LodPurpose::Visual)
        return Model::LodPurposeName(purpose);

    char buf[64];
    snprintf(buf, sizeof(buf), "%.0f", resolution);
    return buf;
}

// Format helpers

std::string FormatTime(long timestamp)
{
    if (timestamp == 0)
        return "-";
    time_t t = static_cast<time_t>(timestamp);
    struct tm* tm = localtime(&t);
    if (!tm)
        return "-";
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
    return buf;
}

std::string FormatSize(long size)
{
    if (size < 1024)
        return std::to_string(size) + " B";
    if (size < 1024 * 1024)
        return std::to_string(size / 1024) + " KB";
    return std::to_string(size / (1024 * 1024)) + " MB";
}

// Texture inspection

TextureInfo InspectTexture(const std::string& path)
{
    TextureInfo info;
    info.path = path;

    PAAInfo paaInfo;
    if (!ReadPAAInfo(path, paaInfo))
        return info;

    info.valid = true;
    info.isPaa = paaInfo.isPaa;
    info.typeName = paaInfo.isPaa ? "PAA" : "PAC";
    info.formatName = paaInfo.formatName ? paaInfo.formatName : "P8";
    info.magic = paaInfo.magic;
    info.width = paaInfo.width;
    info.height = paaInfo.height;
    info.mipmapCount = paaInfo.mipmapCount;
    info.paletteColors = paaInfo.paletteColors;
    info.hasTransparentBlocks = paaInfo.hasTransparentBlocks;
    return info;
}

// Sound inspection

static std::string DetectSoundFormat(const std::string& path)
{
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos)
        return "Unknown";
    std::string ext = path.substr(dot + 1);
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == "wss")
        return "WSS";
    if (ext == "ogg")
        return "OGG";
    if (ext == "wav")
        return "WAV";
    return "Unknown";
}

SoundInfo InspectSound(const std::string& path)
{
    SoundInfo info;
    info.path = path;
    info.format = DetectSoundFormat(path);

    if (!GFileServer)
        GFileServer = new FileServerST(0);

    Ref<WaveStream> stream = SoundLoadFile(path.c_str());
    if (!stream)
        return info;

    WAVEFORMATEX fmt{};
    stream->GetFormat(fmt);
    info.channels = fmt.nChannels;
    info.sampleRate = fmt.nSamplesPerSec;
    info.bitDepth = fmt.wBitsPerSample;
    info.uncompressedSize = stream->GetUncompressedSize();

    if (fmt.nAvgBytesPerSec > 0)
        info.duration = static_cast<double>(info.uncompressedSize) / fmt.nAvgBytesPerSec;

    // File size on disk
    std::ifstream fileCheck(path, std::ios::binary | std::ios::ate);
    if (fileCheck.is_open())
    {
        info.fileSize = fileCheck.tellg();
        if (info.format == "WSS" && info.fileSize > 0 && info.uncompressedSize > 0)
            info.compressionRatio = static_cast<double>(info.fileSize) / info.uncompressedSize;
    }
    info.valid = true;
    return info;
}

// Model inspection

static std::string FormatRenderHints(uint32_t h)
{
    if (h == 0)
        return "None";
    std::string result;
    auto add = [&](uint32_t bit, const char* name)
    {
        if (h & bit)
        {
            if (!result.empty())
                result += "|";
            result += name;
        }
    };
    // Values match runtime specFlags from types.hpp
    add(0x0001, "GrassTexture");
    add(0x0002, "OnSurface");
    add(0x0004, "IsOnSurface");
    add(0x0008, "NoZBuf");
    add(0x0010, "NoZWrite");
    add(0x0020, "NoShadow");
    add(0x0040, "IsShadow");
    add(0x0080, "ShadowDisabled");
    add(0x0100, "IsAlpha");
    add(0x0200, "IsTransparent");
    add(0x0400, "IsWater");
    add(0x0800, "IsLight");
    add(0x1000, "PointSampling");
    add(0x2000, "NoClamp");
    add(0x4000, "ClampU");
    add(0x8000, "ClampV");
    add(0x10000, "IsAnimated");
    add(0x20000, "IsAlphaOrdered");
    add(0x40000, "NoDropdown");
    add(0x80000, "IsAlphaFog");
    add(0x100000, "FogDisabled");
    add(0x200000, "IsColored");
    add(0x400000, "IsHidden");
    add(0x800000, "BestMipmap");
    add(0x1000000, "DetailTexture");
    add(0x2000000, "SpecularTexture");
    add(0x10000000, "IsHiddenProxy");
    add(0x80000000u, "DisableSun");
    return result.empty() ? "None" : result;
}

ModelInfo InspectModel(const std::string& path)
{
    ModelInfo info;
    info.path = path;

    std::ifstream file(path, std::ios::binary);
    if (!file)
        return info;

    P3DFormatDetector detector;
    FormatInfo fmtInfo = detector.DetectFormat(file);
    file.close();

    info.format = fmtInfo.signature;
    info.version = fmtInfo.version;
    info.isSupported = fmtInfo.isSupported;
    info.errorMessage = fmtInfo.errorMessage;

    // Use modern P3D loaders (x64-safe, supports both ODOL and MLOD)
    ModelCache cache;
    auto model = cache.load(path);
    if (!model)
    {
        // The detector's message, if any, only says the revision is unsupported.
        // The loader's says which constraint this particular file broke, so it is
        // the more specific of the two and wins.
        if (!cache.lastError().empty())
            info.errorMessage = cache.lastError();
        return info;
    }

    info.lodCount = static_cast<int>(model->GetLODCount());
    for (size_t i = 0; i < model->lodLevels.size(); ++i)
    {
        const auto& lod = model->lodLevels[i];
        const auto& mesh = lod.mesh;

        ModelLodInfo lodInfo;
        lodInfo.index = static_cast<int>(i);
        lodInfo.resolution = lod.resolution;
        lodInfo.name = ResolutionToName(lod.resolution);
        lodInfo.points = static_cast<int>(mesh.GetVertexCount());
        lodInfo.faces = static_cast<int>(mesh.GetTriangleCount() + mesh.quads.size());
        lodInfo.textures = static_cast<int>(mesh.GetMaterialCount());
        lodInfo.selections = static_cast<int>(mesh.selections.size());

        for (const auto& mat : mesh.materials)
        {
            if (!mat.texturePath.empty())
                lodInfo.textureNames.push_back(mat.texturePath);
            else if (!mat.name.empty())
                lodInfo.textureNames.push_back(mat.name);
            else
                lodInfo.textureNames.push_back("<default>");
        }

        for (const auto& sel : mesh.selections)
            lodInfo.selectionNames.emplace_back(sel.name, static_cast<int>(sel.vertexIndices.size()));

        for (size_t s = 0; s < mesh.sections.size(); ++s)
        {
            const auto& sec = mesh.sections[s];
            ModelSectionInfo si;
            si.index = static_cast<int>(s);
            si.materialIndex = sec.materialIndex;
            si.startTriangle = sec.startTriangle;
            si.triangleCount = sec.triangleCount;
            si.hints = static_cast<uint32_t>(sec.hints);
            si.hintsStr = FormatRenderHints(si.hints);
            if (sec.materialIndex < mesh.materials.size())
            {
                const auto& mat = mesh.materials[sec.materialIndex];
                si.textureName = mat.texturePath.empty() ? mat.name : mat.texturePath;
            }
            lodInfo.sectionInfos.push_back(std::move(si));
        }

        info.lods.push_back(std::move(lodInfo));
    }

    info.valid = true;
    return info;
}

// Terrain inspection

TerrainInfo InspectTerrain(const std::string& path)
{
    TerrainInfo info;
    info.path = path;

    WrpReader reader;
    if (!reader.Load(path.c_str()))
        return info;

    info.formatName = reader.GetFormatName();
    info.gridX = reader.GetGridX();
    info.gridZ = reader.GetGridZ();
    info.terrainX = reader.GetTerrainX();
    info.terrainZ = reader.GetTerrainZ();
    info.heightmapSize = reader.GetHeightmapSize();
    info.minHeight = reader.GetMinHeight();
    info.maxHeight = reader.GetMaxHeight();
    info.textureCount = reader.GetTextureCount();
    info.objectCount = reader.GetObjectCount();
    info.objectNameCount = reader.GetObjectNameCount();

    for (int i = 0; i < reader.GetTextureCount(); i++)
        info.textureNames.push_back(reader.GetTextureName(i).Data());

    info.valid = true;
    return info;
}

// PBO inspection

static void CollectPboEntries(const FileInfoO& fi, const FileBankType*, void* ctx)
{
    auto* entries = static_cast<std::vector<PboFileEntry>*>(ctx);
    PboFileEntry entry;
    entry.name = fi.name.Data();
    entry.length = fi.length;
    entry.time = fi.time;
    entry.compressed = (fi.compressedMagic == CompMagic);
    entry.uncompressedSize = fi.uncompressedSize;
    entries->push_back(entry);
}

static std::string StripPboExtension(const std::string& path)
{
    if (path.size() >= 4)
    {
        std::string ext = path.substr(path.size() - 4);
        for (auto& c : ext)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (ext == ".pbo")
            return path.substr(0, path.size() - 4);
    }
    return path;
}

PboInfo InspectPbo(const std::string& path)
{
    PboInfo info;
    info.path = path;

    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return info;

    std::string bankName = StripPboExtension(path);
    QFBank bank;
    if (!bank.open(RString(bankName.c_str())))
        return info;
    bank.Lock();
    if (bank.error())
    {
        bank.Unlock();
        return info;
    }

    // The archive's own declared root, from the header-extension block -- not
    // QFBank::GetPrefix(), which is the mount point the *caller* assigned and is
    // empty for a bank merely opened for inspection.
    info.prefix = static_cast<const char*>(bank.GetProperty(RString("prefix")));
    bank.ForEach(CollectPboEntries, &info.entries);

    for (const auto& e : info.entries)
    {
        long displaySize = e.compressed ? e.uncompressedSize : e.length;
        info.totalSize += displaySize;
        info.totalStored += e.length;
    }

    bank.Unlock();
    info.valid = true;
    return info;
}

// Font inspection

FontInfo InspectFont(const std::string& path)
{
    FontInfo info;
    info.path = path;

    // Open FXY file directly (no GFileServer needed)
    QIFStream stream;
    stream.open(path.c_str());
    if (stream.rest() <= 0)
        return info;

    // Extract base name from path (without .fxy extension)
    std::filesystem::path p(path);
    std::string baseName = p.stem().string();

    FXYData data = ParseFXY(stream, baseName.c_str());
    if (!data.valid())
        return info;

    info.name = data.name;
    info.glyphCount = data.nChars;
    info.textureSetCount = static_cast<int>(data.textureSetNums.size());
    info.maxHeight = data.maxHeight;
    info.maxWidth = data.maxWidth;

    // texture names derived from set numbers and FXY stem
    for (int setNum : data.textureSetNums)
    {
        char texName[256];
        snprintf(texName, sizeof(texName), "%s-%02d.paa", baseName.c_str(), setNum);
        info.textureNames.emplace_back(texName);
    }

    // char range from non-empty (w/h > 0) glyphs
    info.charCodeMin = -1;
    info.charCodeMax = -1;
    for (size_t i = 0; i < data.glyphs.size(); i++)
    {
        const auto& g = data.glyphs[i];
        if (g.w > 0 || g.h > 0)
        {
            if (info.charCodeMin < 0)
                info.charCodeMin = static_cast<int>(i);
            info.charCodeMax = static_cast<int>(i);
        }
    }

    // Check which textures exist on disk (relative to FXY file directory)
    std::string dir = p.parent_path().string();
    if (!dir.empty())
        dir += "/";
    for (const auto& texName : info.textureNames)
    {
        std::string texPath = dir + texName;
        info.texturesExist.push_back(std::filesystem::exists(texPath));
    }

    info.valid = true;
    return info;
}

// Animation inspection

AnimationInfo InspectAnimation(const std::string& path)
{
    AnimationInfo info;
    info.path = path;

    Asset::Formats::RTMAnimation anim;
    if (!Asset::Formats::readRTMFromFile(path.c_str(), anim))
        return info;

    info.boneCount = anim.boneCount();
    info.phaseCount = anim.phaseCount();
    info.format = (anim.version == Asset::Formats::RTMVersion::V101) ? "RTM v1.01" : "RTM v1.00";
    if (anim.version == Asset::Formats::RTMVersion::V101)
    {
        info.stepX = anim.stepX;
        info.stepY = anim.stepY;
        info.stepZ = anim.stepZ;
    }
    info.valid = true;
    return info;
}

// Config inspection (raP binary detection)

ConfigInfo InspectConfig(const std::string& path)
{
    ConfigInfo info;
    info.path = path;

    try
    {
        info.fileSize = static_cast<int64_t>(std::filesystem::file_size(path));
    }
    catch (...)
    {
        return info;
    }

    std::ifstream f(path, std::ios::binary);
    if (!f)
        return info;

    // raP magic: "\0raP" (4 bytes) followed by version uint32
    char magic[4] = {};
    f.read(magic, 4);
    if (f.gcount() == 4 && magic[0] == '\0' && magic[1] == 'r' && magic[2] == 'a' && magic[3] == 'P')
    {
        info.isBinarized = true;
        uint32_t ver = 0;
        f.read(reinterpret_cast<char*>(&ver), 4);
        info.version = static_cast<int>(ver);
    }

    info.valid = true;
    return info;
}

// Generic file inspection

AssetFileInfo InspectFile(const std::string& path)
{
    AssetFileInfo info;
    info.path = path;

    try
    {
        std::filesystem::path p(path);
        info.name = p.filename().string();
        info.extension = p.extension().string();

        if (std::filesystem::exists(p))
        {
            info.size = static_cast<int64_t>(std::filesystem::file_size(p));
            struct stat st;
            if (stat(path.c_str(), &st) == 0)
                info.modifiedTime = st.st_mtime;
            info.valid = true;
        }
    }
    catch (...)
    {
    }

    return info;
}

} // namespace Poseidon
