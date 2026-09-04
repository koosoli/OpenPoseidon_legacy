#include <Poseidon/Asset/Formats/P3D/P3DModelLoad.hpp>

#include <Poseidon/Asset/Formats/P3D/MLODLoader.hpp>
#include <Poseidon/Asset/Formats/P3D/ODOLLoader.hpp>

#include <exception>

namespace Poseidon::Asset::Formats
{

bool TryLoadP3D(const std::string& path, Poseidon::Model::Model& model, FormatInfo& format, std::string& error)
{
    try
    {
        format = P3DFormatDetector::DetectFormat(path);
        if (!format.isSupported)
        {
            error = format.errorMessage;
            return false;
        }

        if (format.signature == "ODOL")
        {
            model = ODOLLoader::load(path);
        }
        else if (format.signature == "MLOD")
        {
            model = MLODLoader::load(path);
        }
        else
        {
            error = "Unknown P3D signature: " + format.signature;
            return false;
        }
    }
    catch (const std::exception& e)
    {
        error = e.what();
        return false;
    }
    catch (...)
    {
        // Deliberately wider than the upstream version: the readers reach third-party
        // decompressors and stream helpers, and the point of the wrapper is that NOTHING
        // from a malformed file reaches the caller's entry point. A non-std throw would
        // otherwise walk straight past a std::exception-only handler into std::terminate,
        // which is the exact failure this exists to remove.
        error = "unknown error while reading P3D";
        return false;
    }

    error.clear();
    return true;
}

} // namespace Poseidon::Asset::Formats
