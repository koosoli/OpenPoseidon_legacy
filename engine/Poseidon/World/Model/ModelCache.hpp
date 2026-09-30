#pragma once

#include <Poseidon/World/Model/Model.hpp>
#include <string>
#include <map>
#include <memory>

namespace Poseidon {

class ModelCompressedSourceBirth;

namespace Model {
    struct Model;
}

class ModelCache {
public:
    ModelCache() = default;
    ~ModelCache() = default;

    ModelCache(const ModelCache&) = delete;
    ModelCache& operator=(const ModelCache&) = delete;

    // Optional receipt is published only for a selected cold FileServer buffer
    // that carried its own one-shot compressed archive birth through a fresh
    // parser run. Cache hits, loose files and ordinary model loads leave it empty.
    std::shared_ptr<Model::Model> load(
        const std::string& filePath,
        std::shared_ptr<const ModelCompressedSourceBirth>* sourceBirth = nullptr);

    // Read, parse and compile ONE LOOSE FILE (std::ifstream on `filePath` as given), and
    // nothing else: no ModelCache instance, no GFileServer, no PBO banks, no engine globals.
    //
    // This is the piece of `load` that is safe to call from a thread that is not the main
    // thread, and it is split out for exactly that reason (Landscape's asynchronous streamed
    // object admission, ObjectStreamPrepare.cpp). Everything it touches is std:: or the
    // header-only P3D readers (ODOLLoader / MLODLoader / P3DFormatDetector / Model::compile),
    // which hold no static state; the global heap is MemHeapLocked and spdlog is thread-safe.
    // GFileServer (FileServerST) and QFBank are NOT on that list -- FileCache keeps an unlocked
    // MRU array -- so a file that only exists inside a PBO is refused here (`*opened == false`)
    // and the caller must fall back to `load` on the main thread.
    //
    // Returns null on any failure; `*opened` says whether the file could be opened at all
    // (false = not a loose file, i.e. try the file server), `*error` names the parse failure.
    static std::shared_ptr<Model::Model> LoadLooseFile(const std::string& filePath, std::string* error,
                                                       bool* opened);

    // Pure parser entry for an owned worker read. Does not access the VFS or
    // mutable derived-cache statistics; sourcePath retains material resolution.
    static std::shared_ptr<Model::Model> LoadOwnedBytes(const char* data, size_t size,
                                                       const std::string& sourcePath, std::string& error);

    /// RFG-097: a loader for names no file or PBO answers -- a natively mounted Reforger
    /// `.xob`, built through the same converter the world loader uses. Installed by the
    /// native world loader while its archives are mounted; null otherwise. Consulted by
    /// `loadFromFile` only after the loose-file and PBO paths have both declined.
    using ExternalLoader = std::shared_ptr<Model::Model> (*)(const std::string& path, std::string& why);
    static void SetExternalLoader(ExternalLoader loader);
    static ExternalLoader GetExternalLoader();

    bool isLoaded(const std::string& filePath) const;

    std::shared_ptr<Model::Model> get(const std::string& filePath) const;

    bool unload(const std::string& filePath);

    void clear();

    size_t size() const { return _cache.size(); }

    struct Stats {
        size_t totalLoads = 0;
        size_t cacheHits = 0;
        size_t cacheMisses = 0;
        size_t loadFailures = 0;
        size_t cachedModels = 0;
    };

    Stats getStats() const { return _stats; }
    void resetStats() { _stats = Stats{0, 0, 0, 0, _cache.size()}; }

    // Why the most recent load returned null. The readers throw messages that name
    // the exact constraint a file broke -- the revision, the field, the boundary it
    // missed -- and loadFromFile used to discard all of it in a catch(...), so every
    // failure reached the caller as an indistinguishable nullptr. Keeping it is what
    // makes an unsupported revision, a malformed file and a missing file three
    // different reports instead of one.
    const std::string& lastError() const { return _lastError; }

private:
    std::string normalizePath(const std::string& path) const;

    std::shared_ptr<Model::Model> loadFromFile(
        const std::string& filePath,
        std::shared_ptr<const ModelCompressedSourceBirth>* sourceBirth);

    std::shared_ptr<Model::Model> loadFromFileServer(
        const std::string& filePath,
        std::shared_ptr<const ModelCompressedSourceBirth>* sourceBirth);

    std::map<std::string, std::shared_ptr<Model::Model>> _cache;
    mutable Stats _stats;
    std::string _lastError;
};

} // namespace Poseidon

