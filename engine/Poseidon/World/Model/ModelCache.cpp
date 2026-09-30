
#include <Poseidon/World/Model/ModelCache.hpp>
#include <cctype>
#include <cstring>
#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/Asset/Formats/Common/FormatDetector.hpp>
#include <utility>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Asset/Formats/P3D/ODOLLoader.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODLoader.hpp>
#include <Poseidon/Asset/Addon/VirtualPath.hpp>
#include <Poseidon/IO/FileServer.hpp>
#include <Poseidon/IO/Streams/ModelCompressedSourceBirth.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/World/Model/ModelDerivedCache.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <cstring>
#include <vector>
#include <limits>

namespace Poseidon
{
using namespace Asset::Formats;

std::string ModelCache::normalizePath(const std::string& path) const
{
    // AST-015: one canonical form, not a platform-dependent one.
    //
    // This previously chose the separator by #ifdef _WIN32, so `ca\air\x.p3d` and
    // `ca/air/x.p3d` produced two cache keys on Linux and one on Windows -- the
    // same model cached twice, or not, depending on the host. A Real Virtuality
    // virtual path is platform-independent identity, so it normalises the same way
    // everywhere.
    return Asset::VirtualPath::Parse(path).canonical();
}

std::shared_ptr<Poseidon::Model::Model> ModelCache::load(
    const std::string& filePath,
    std::shared_ptr<const ModelCompressedSourceBirth>* sourceBirth)
{
    if (sourceBirth) sourceBirth->reset();
    _stats.totalLoads++;

    std::string key = normalizePath(filePath);

    auto it = _cache.find(key);
    if (it != _cache.end())
    {
        _stats.cacheHits++;
        return it->second;
    }

    _stats.cacheMisses++;

    auto model = loadFromFile(filePath, sourceBirth);
    if (!model)
    {
        _stats.loadFailures++;
        return nullptr;
    }

    _cache[key] = model;
    _stats.cachedModels = _cache.size();
    return model;
}

namespace
{

bool SameSha256(const std::string& hex, const std::array<uint8_t, 32>& raw)
{
    if (hex.size() != raw.size() * 2) return false;
    constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < raw.size(); ++i)
        if (hex[i * 2] != digits[raw[i] >> 4] || hex[i * 2 + 1] != digits[raw[i] & 15])
            return false;
    return true;
}

// AST-004 -- the one seam where the derived data cache sits.
//
// Both entry points below reach the same place: source bytes in hand, a virtual
// path, and a loader that has not run. That is where a cache belongs, because it
// is the last point at which the expensive step can still be skipped and the
// first at which the key's every input is known.
//
// `contentSha` is the SHA-256 of the exact bytes handed to the parser or cache
// hit. An EMPTY sha means "the
// key cannot be completed", and the only safe response to that is to derive
// without touching the cache: a key missing an input is exactly the wrong-data
// hit this design exists to prevent.
std::shared_ptr<Poseidon::Model::Model> DeriveOrHit(const char* data, int size, const std::string& sourcePath,
                                                    const std::string& signature, const std::string& contentSha,
                                                    std::string& lastError, bool bypassCache = false)
{
    ModelDerivedCache&            cache   = ModelDerivedCache::Instance();
    const bool                    useCache = !bypassCache && cache.Enabled() && !contentSha.empty();
    Asset::Cache::DerivedAssetKey key;
    if (useCache)
    {
        key       = ModelDerivedCache::MakeKey(sourcePath, contentSha);
        auto  hit = std::make_shared<Poseidon::Model::Model>();
        if (cache.Load(key, *hit))
        {
            return hit;
        }
    }

    auto model = std::make_shared<Poseidon::Model::Model>();
    try
    {
        if (signature == "ODOL")
        {
            *model = Poseidon::Asset::Formats::ODOLLoader::loadFromBuffer(data, size, sourcePath);
        }
        else if (signature == "MLOD")
        {
            *model = Poseidon::Asset::Formats::MLODLoader::loadFromBuffer(data, size, sourcePath);
        }
        else
        {
            lastError = "unrecognised P3D signature '" + signature + "'";
            return nullptr;
        }
    }
    // Kept separate so a recognised-but-unimplemented revision is not reported as
    // a malformed file: the two call for different follow-up.
    catch (const Poseidon::Asset::Formats::P3D::UnsupportedOdolRevision& error)
    {
        lastError = error.what();
        return nullptr;
    }
    catch (const std::exception& error)
    {
        lastError = error.what();
        return nullptr;
    }
    catch (...)
    {
        lastError = "unknown exception while reading the model";
        return nullptr;
    }

    if (!model->compile() || !model->isCompiled())
    {
        lastError = "model read but failed to compile";
        return nullptr;
    }

    // Only a model that passed compile() is stored, so a cache hit is never a
    // shortcut past a validity check the cold path applies.
    if (useCache)
    {
        cache.Store(key, *model);
    }
    return model;
}

} // namespace

std::shared_ptr<Poseidon::Model::Model> ModelCache::LoadOwnedBytes(
    const char* data, size_t size, const std::string& sourcePath, std::string& error)
{
    error.clear();
    if (!data || size < 8 || size > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
        error = "invalid model byte span";
        return nullptr;
    }
    return DeriveOrHit(data, static_cast<int>(size), sourcePath, std::string(data, 4), {}, error);
}

std::shared_ptr<Poseidon::Model::Model> ModelCache::LoadLooseFile(const std::string& filePath, std::string* error,
                                                                 bool* opened)
{
    std::string localError;
    std::string& lastError = error ? *error : localError;
    lastError.clear();

    std::ifstream file(filePath, std::ios::binary);
    if (!file)
    {
        if (opened)
            *opened = false;
        lastError = "cannot open file";
        return nullptr;
    }
    if (opened)
        *opened = true;

    FormatInfo info = P3DFormatDetector::DetectFormat(file);
    if (!info.isSupported)
    {
        lastError = info.errorMessage.empty() ? "unsupported format" : info.errorMessage;
        return nullptr;
    }

    // Read once, here. Both loaders' `load(path)` overloads slurped the whole file
    // themselves, so this is the same bytes by the same route -- it is hoisted so
    // the cache key can be computed from them without a second read.
    std::vector<char> bytes;
    {
        std::ifstream in(filePath, std::ios::binary | std::ios::ate);
        if (!in)
        {
            lastError = "cannot open file";
            return nullptr;
        }
        const std::streamoff length = in.tellg();
        if (length <= 0)
        {
            lastError = "empty file";
            return nullptr;
        }
        bytes.resize(static_cast<size_t>(length));
        in.seekg(0);
        if (!in.read(bytes.data(), length))
        {
            lastError = "short read";
            return nullptr;
        }
    }

    // A loose file is not a bank member, so AST-003's memo does not apply and the
    // bytes are hashed directly. Same value, no memo -- and the hash is only paid
    // when the cache is on.
    const std::string sha = ModelDerivedCache::Instance().Enabled()
                                ? ModelDerivedCache::HashBytes(bytes.data(), bytes.size())
                                : std::string();

    return DeriveOrHit(bytes.data(), static_cast<int>(bytes.size()), filePath, info.signature, sha, lastError);
}

namespace
{
ModelCache::ExternalLoader g_externalLoader = nullptr;

bool EndsWithNoCase(const std::string& s, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    if (s.size() < n)
        return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != suffix[i])
            return false;
    return true;
}
} // namespace

void ModelCache::SetExternalLoader(ExternalLoader loader) { g_externalLoader = loader; }
ModelCache::ExternalLoader ModelCache::GetExternalLoader() { return g_externalLoader; }

std::shared_ptr<Poseidon::Model::Model> ModelCache::loadFromFile(
    const std::string& filePath,
    std::shared_ptr<const ModelCompressedSourceBirth>* sourceBirth)
{
    // Same sequence as before the split: loose file first, PBO banks only when the loose
    // open itself fails (a loose file that fails to PARSE is a failure, not a fallback).
    bool opened = false;
    auto model = LoadLooseFile(filePath, &_lastError, &opened);
    if (!opened)
    {
        // RFG-097: a native `.xob` name is answered by the world loader's converter, not
        // by a file; the PBO route would only fail slower.
        if (g_externalLoader != nullptr && EndsWithNoCase(filePath, ".xob"))
        {
            std::string why;
            auto external = g_externalLoader(filePath, why);
            if (!external)
                _lastError = why.empty() ? "external loader declined" : why;
            return external;
        }
        // Fallback to PBO archives
        if (GFileServer)
        {
            return loadFromFileServer(filePath, sourceBirth);
        }
        _lastError = "cannot open file";
        return nullptr;
    }
    return model;
}

std::shared_ptr<Poseidon::Model::Model> ModelCache::loadFromFileServer(
    const std::string& filePath,
    std::shared_ptr<const ModelCompressedSourceBirth>* sourceBirth)
{
    // A fresh owner scope stamps the bank's actual compressed read. Returning a
    // pre-existing FileCache buffer cannot claim its earlier token for parsing.
    ModelCompressedSourceBirth::ReadScope coldRead(sourceBirth ? filePath.c_str() : nullptr);
    QIFStream f;
    GFileServer->Open(f, filePath.c_str());
    if (f.fail())
        return nullptr;

    const char* data = f.act();
    int dataSize = f.rest();
    if (dataSize < 8)
        return nullptr;

    char sig[5] = {};
    memcpy(sig, data, 4);
    std::string signature(sig);

    std::shared_ptr<const ModelCompressedSourceBirth> selectedBirth;
    if (sourceBirth && coldRead.Active() &&
        ModelCompressedSourceBirth::ReadScope::MatchesLogicalName(filePath.c_str()) &&
        signature == "ODOL" && f.GetBuffer())
        selectedBirth = f.GetBuffer()->TakeCompressedModelSourceBirthForParse();

    // FileServer selected these bytes. AutoBank/GetContentHash may instead read
    // a different mount (or return its earlier memo) for the same virtual path;
    // that hash must never key a derivation from this stream's buffer.
    const std::string sha = (ModelDerivedCache::Instance().Enabled() || selectedBirth)
        ? ModelDerivedCache::HashBytes(data, static_cast<size_t>(dataSize)) : std::string();
    if (selectedBirth && (!selectedBirth->Valid() ||
        selectedBirth->logicalName != filePath ||
        selectedBirth->decodedBytes != static_cast<uint32_t>(dataSize) ||
        !SameSha256(sha, selectedBirth->decodedSha256)))
        selectedBirth.reset();

    // A source birth means this exact owned buffer must execute the canonical
    // parser and compiler. A derived-cache hit carries no parser-executed birth.
    auto model = DeriveOrHit(data, dataSize, filePath, signature, sha, _lastError,
                             bool(selectedBirth));
    if (model && selectedBirth && sourceBirth)
    {
        *sourceBirth = std::move(selectedBirth);
        LOG_INFO(Core, "Retail cold model parser birth: source={} bytes={} scope=exact-owned-buffer",
                 filePath, dataSize);
    }
    return model;
}

bool ModelCache::isLoaded(const std::string& filePath) const
{
    std::string key = normalizePath(filePath);
    return _cache.find(key) != _cache.end();
}

std::shared_ptr<Poseidon::Model::Model> ModelCache::get(const std::string& filePath) const
{
    std::string key = normalizePath(filePath);
    auto it = _cache.find(key);
    return (it != _cache.end()) ? it->second : nullptr;
}

bool ModelCache::unload(const std::string& filePath)
{
    std::string key = normalizePath(filePath);
    auto it = _cache.find(key);
    if (it != _cache.end())
    {
        _cache.erase(it);
        _stats.cachedModels = _cache.size();
        return true;
    }
    return false;
}

void ModelCache::clear()
{
    _cache.clear();
    _stats.cachedModels = 0;
}

} // namespace Poseidon
