
#include <Poseidon/World/Model/ModelCache.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/Asset/Formats/Common/FormatDetector.hpp>
#include <utility>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <Poseidon/Asset/Formats/P3D/ODOLLoader.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODLoader.hpp>
#include <Poseidon/Asset/Addon/VirtualPath.hpp>
#include <Poseidon/IO/FileServer.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <cstring>

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

std::shared_ptr<Poseidon::Model::Model> ModelCache::load(const std::string& filePath)
{
    _stats.totalLoads++;

    std::string key = normalizePath(filePath);

    auto it = _cache.find(key);
    if (it != _cache.end())
    {
        _stats.cacheHits++;
        return it->second;
    }

    _stats.cacheMisses++;

    auto model = loadFromFile(filePath);
    if (!model)
    {
        _stats.loadFailures++;
        return nullptr;
    }

    _cache[key] = model;
    _stats.cachedModels = _cache.size();
    return model;
}

std::shared_ptr<Poseidon::Model::Model> ModelCache::loadFromFile(const std::string& filePath)
{
    _lastError.clear();

    std::ifstream file(filePath, std::ios::binary);
    if (!file)
    {
        // Fallback to PBO archives
        if (GFileServer)
        {
            return loadFromFileServer(filePath);
        }
        _lastError = "cannot open file";
        return nullptr;
    }

    FormatInfo info = P3DFormatDetector::DetectFormat(file);
    if (!info.isSupported)
    {
        _lastError = info.errorMessage.empty() ? "unsupported format" : info.errorMessage;
        return nullptr;
    }

    auto model = std::make_shared<Poseidon::Model::Model>();

    try
    {
        if (info.signature == "ODOL")
        {
            *model = Poseidon::Asset::Formats::ODOLLoader::load(filePath);
        }
        else if (info.signature == "MLOD")
        {
            *model = Poseidon::Asset::Formats::MLODLoader::load(filePath);
        }
        else
        {
            _lastError = "unrecognised P3D signature '" + info.signature + "'";
            return nullptr;
        }
    }
    // Kept separate so a recognised-but-unimplemented revision is not reported as
    // a malformed file: the two call for different follow-up.
    catch (const Poseidon::Asset::Formats::P3D::UnsupportedOdolRevision& error)
    {
        _lastError = error.what();
        return nullptr;
    }
    catch (const std::exception& error)
    {
        _lastError = error.what();
        return nullptr;
    }
    catch (...)
    {
        _lastError = "unknown exception while reading the model";
        return nullptr;
    }

    if (!model->compile() || !model->isCompiled())
    {
        _lastError = "model read but failed to compile";
        return nullptr;
    }

    return model;
}

std::shared_ptr<Poseidon::Model::Model> ModelCache::loadFromFileServer(const std::string& filePath)
{
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

    auto model = std::make_shared<Poseidon::Model::Model>();
    try
    {
        if (signature == "ODOL")
            *model = Poseidon::Asset::Formats::ODOLLoader::loadFromBuffer(data, dataSize, filePath);
        else if (signature == "MLOD")
            *model = Poseidon::Asset::Formats::MLODLoader::loadFromBuffer(data, dataSize, filePath);
        else
            return nullptr;
    }
    catch (...)
    {
        return nullptr;
    }

    if (!model->compile() || !model->isCompiled())
        return nullptr;

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
