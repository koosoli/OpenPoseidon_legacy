#include <PoseidonFormats/PoseidonFormats.h>

#include <Poseidon/Foundation/Platform/InitBridge.hpp>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>
#include <Poseidon/IO/ParamFile/InitLibraryElement.hpp>
#include <Poseidon/Graphics/Core/Engine.hpp>
#include <Poseidon/Graphics/Dummy/GraphicsEngineDummy.hpp>
#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/Asset/Formats/P3D/ODOLLoader.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODLoader.hpp>
#include <Poseidon/Asset/Formats/Material/RvMaterialSource.hpp>
#include <Poseidon/Asset/Formats/Common/FormatDetector.hpp>
using Poseidon::Asset::Formats::P3DFormatDetector;
using Poseidon::Asset::Formats::FormatInfo;
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/Core/Application.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <Poseidon/IO/Streams/FileInfo.h>

#include <string>
#include <mutex>
#include <filesystem>
#include <algorithm>

#include <Poseidon/Asset/Formats/RTM/RTMReader.hpp>

#ifdef _WIN32
#include <windows.h>

using Poseidon::GEngine;

using Poseidon::CreateEngineDummy;

#endif

// Thread-safe error string
static thread_local std::string g_lastError;
static bool g_initialized = false;
static std::mutex g_initMutex;

static void SetError(const std::string& msg) { g_lastError = msg; }
static void ClearError() { g_lastError.clear(); }

// Minimal AppFrame for headless usage (no UI, no logging)
class PFAppFrame : public Poseidon::Foundation::AppFrameFunctions {
public:
    void ErrorMessage(Poseidon::Foundation::ErrorMessageLevel, const char*, va_list) override {}
    void ErrorMessage(const char*, va_list) override {}
    void WarningMessage(const char*, va_list) override {}
    void ShowMessage(int, const char*, va_list) override {}
    DWORD TickCount() override { return ::GetTickCount(); }
};
static PFAppFrame g_appFrame;

// Wrapper types
struct PFModel {
    Poseidon::Model::Model model;
};

struct PFImage {
    Poseidon::DecodedImage image;
    std::string formatName;
};

struct PFPboEntry {
    std::string name;
    int32_t length;
    int32_t time;
    int compressedMagic;
    int uncompressedSize;
};

struct PFPbo {
    QFBank bank;
    std::vector<PFPboEntry> entries;
};

// Helpers
static const Poseidon::Model::Mesh* GetMesh(PF_HANDLE h, int lod) {
    if (!h) return nullptr;
    auto* m = static_cast<PFModel*>(h);
    if (lod < 0 || lod >= static_cast<int>(m->model.lodLevels.size())) return nullptr;
    return &m->model.lodLevels[lod].mesh;
}

// ── Library lifecycle ──────────────────────────────────────────────────────

// DllMain: set safe AppFrame immediately to prevent crashes from static
// initializers or early logging calls before pf_init() is called.
#ifdef _WIN32
BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        Poseidon::Foundation::CurrentAppFrameFunctions = &g_appFrame;
    return TRUE;
}
#endif

// Step-by-step init for debugging (call with step 1,2,3,4 in order)
static int g_initStep = 0;
PF_API int pf_init_step(int step) {
    if (step == 1) { Poseidon::Foundation::CurrentAppFrameFunctions = &g_appFrame; g_initStep = 1; return 1; }
    if (step == 2) { SetMemorySystemReady(true); g_initStep = 2; return 1; }
    if (step == 3) { InitLibraryElement(); g_initStep = 3; return 1; }
    if (step == 4)
    {
        Poseidon::GEngine = Poseidon::CreateEngineDummy();
        g_initStep = 4;
        g_initialized = true;
        ClearError();
        return 1;
    }
    return 0;
}

PF_API int pf_init(void) {
    std::lock_guard<std::mutex> lock(g_initMutex);
    if (g_initialized) return 1;
    Poseidon::Foundation::CurrentAppFrameFunctions = &g_appFrame;
    SetMemorySystemReady(true);
    InitLibraryElement();
    Poseidon::GEngine = Poseidon::CreateEngineDummy();
    g_initialized = true;
    ClearError();
    return 1;
}

PF_API void pf_shutdown(void) {
    std::lock_guard<std::mutex> lock(g_initMutex);
    g_initialized = false;
}

PF_API const char* pf_version(void) {
    return "1.0.0";
}

PF_API const char* pf_last_error(void) {
    return g_lastError.c_str();
}

PF_API int pf_has_safe_appframe(void) {
    return Poseidon::Foundation::CurrentAppFrameFunctions == &g_appFrame ? 1 : 0;
}

PF_API int pf_check_null_globals(void) {
    return (GApp == nullptr) ? 1 : 0;
}

// ── P3D Model ──────────────────────────────────────────────────────────────

PF_API PF_HANDLE pf_model_load(const char* path) {
    ClearError();
    if (!path) { SetError("null path"); return nullptr; }
    try {
        auto fmt = P3DFormatDetector::DetectFormat(path);
        auto* wrapper = new PFModel();
        if (fmt.signature == "ODOL") {
            wrapper->model = Poseidon::Asset::Formats::ODOLLoader::load(path);
        } else if (fmt.signature == "MLOD") {
            wrapper->model = Poseidon::Asset::Formats::MLODLoader::load(path);
        } else {
            delete wrapper;
            SetError("Unknown P3D format: " + fmt.signature);
            return nullptr;
        }
        return wrapper;
    } catch (const std::exception& e) {
        SetError(e.what());
        return nullptr;
    }
}

PF_API void pf_model_free(PF_HANDLE model) {
    delete static_cast<PFModel*>(model);
}

PF_API const char* pf_model_format(PF_HANDLE model) {
    if (!model) return "";
    return static_cast<PFModel*>(model)->model.sourceFormat.c_str();
}

PF_API int pf_model_lod_count(PF_HANDLE model) {
    if (!model) return 0;
    return static_cast<int>(static_cast<PFModel*>(model)->model.lodLevels.size());
}

PF_API float pf_model_lod_resolution(PF_HANDLE model, int lod) {
    if (!model) return 0.0f;
    auto& levels = static_cast<PFModel*>(model)->model.lodLevels;
    if (lod < 0 || lod >= static_cast<int>(levels.size())) return 0.0f;
    return levels[lod].resolution;
}

PF_API void pf_model_bounding_center(PF_HANDLE model, float* xyz) {
    if (!model || !xyz) return;
    auto& m = static_cast<PFModel*>(model)->model;
    xyz[0] = m.boundingCenter.x;
    xyz[1] = m.boundingCenter.y;
    xyz[2] = m.boundingCenter.z;
}

PF_API int pf_model_autocenter(PF_HANDLE model) {
    if (!model) return 0;
    return static_cast<PFModel*>(model)->model.autoCenterEnabled;
}

// ── Geometry ───────────────────────────────────────────────────────────────

PF_API int pf_lod_vertex_count(PF_HANDLE model, int lod) {
    auto* mesh = GetMesh(model, lod);
    return mesh ? static_cast<int>(mesh->vertices.size()) : 0;
}

PF_API int pf_lod_face_count(PF_HANDLE model, int lod) {
    auto* mesh = GetMesh(model, lod);
    if (!mesh) return 0;
    // Quads become 2 triangles each
    return static_cast<int>(mesh->triangles.size() + mesh->quads.size() * 2);
}

PF_API void pf_lod_get_vertices(PF_HANDLE model, int lod, float* xyz) {
    auto* mesh = GetMesh(model, lod);
    if (!mesh || !xyz) return;
    for (size_t i = 0; i < mesh->vertices.size(); i++) {
        xyz[i * 3 + 0] = mesh->vertices[i].position.x;
        xyz[i * 3 + 1] = mesh->vertices[i].position.y;
        xyz[i * 3 + 2] = mesh->vertices[i].position.z;
    }
}

PF_API void pf_lod_get_normals(PF_HANDLE model, int lod, float* xyz) {
    auto* mesh = GetMesh(model, lod);
    if (!mesh || !xyz) return;
    for (size_t i = 0; i < mesh->vertices.size(); i++) {
        xyz[i * 3 + 0] = mesh->vertices[i].normal.x;
        xyz[i * 3 + 1] = mesh->vertices[i].normal.y;
        xyz[i * 3 + 2] = mesh->vertices[i].normal.z;
    }
}

PF_API void pf_lod_get_uvs(PF_HANDLE model, int lod, float* uv) {
    auto* mesh = GetMesh(model, lod);
    if (!mesh || !uv) return;
    for (size_t i = 0; i < mesh->vertices.size(); i++) {
        uv[i * 2 + 0] = mesh->vertices[i].uv.u;
        uv[i * 2 + 1] = mesh->vertices[i].uv.v;
    }
}

PF_API void pf_lod_get_faces(PF_HANDLE model, int lod, int* indices) {
    auto* mesh = GetMesh(model, lod);
    if (!mesh || !indices) return;
    size_t off = 0;
    for (size_t i = 0; i < mesh->triangles.size(); i++, off += 3) {
        indices[off + 0] = static_cast<int>(mesh->triangles[i].indices[0]);
        indices[off + 1] = static_cast<int>(mesh->triangles[i].indices[1]);
        indices[off + 2] = static_cast<int>(mesh->triangles[i].indices[2]);
    }
    // Split quads into 2 triangles: (0,1,2) and (0,2,3)
    for (size_t i = 0; i < mesh->quads.size(); i++) {
        auto& q = mesh->quads[i].indices;
        indices[off + 0] = static_cast<int>(q[0]);
        indices[off + 1] = static_cast<int>(q[1]);
        indices[off + 2] = static_cast<int>(q[2]);
        off += 3;
        indices[off + 0] = static_cast<int>(q[0]);
        indices[off + 1] = static_cast<int>(q[2]);
        indices[off + 2] = static_cast<int>(q[3]);
        off += 3;
    }
}

PF_API void pf_lod_get_face_materials(PF_HANDLE model, int lod, int* mat_indices) {
    auto* mesh = GetMesh(model, lod);
    if (!mesh || !mat_indices) return;
    size_t off = 0;
    for (size_t i = 0; i < mesh->triangles.size(); i++)
        mat_indices[off++] = static_cast<int>(mesh->triangles[i].materialIndex);
    for (size_t i = 0; i < mesh->quads.size(); i++) {
        int mi = static_cast<int>(mesh->quads[i].materialIndex);
        mat_indices[off++] = mi;
        mat_indices[off++] = mi; // both tris from same quad share material
    }
}

PF_API void pf_lod_get_vertex_flags(PF_HANDLE model, int lod, uint32_t* flags) {
    auto* mesh = GetMesh(model, lod);
    if (!mesh || !flags) return;
    for (size_t i = 0; i < mesh->vertices.size(); i++)
        flags[i] = static_cast<uint32_t>(mesh->vertices[i].flags);
}

PF_API void pf_lod_get_face_flags(PF_HANDLE model, int lod, uint32_t* flags) {
    auto* mesh = GetMesh(model, lod);
    if (!mesh || !flags) return;
    size_t off = 0;
    for (size_t i = 0; i < mesh->triangles.size(); i++)
        flags[off++] = static_cast<uint32_t>(mesh->triangles[i].flags);
    for (size_t i = 0; i < mesh->quads.size(); i++) {
        uint32_t f = static_cast<uint32_t>(mesh->quads[i].flags);
        flags[off++] = f;
        flags[off++] = f; // both tris from same quad share flags
    }
}

// ── Materials ──────────────────────────────────────────────────────────────

PF_API int pf_lod_material_count(PF_HANDLE model, int lod) {
    auto* mesh = GetMesh(model, lod);
    return mesh ? static_cast<int>(mesh->materials.size()) : 0;
}

PF_API const char* pf_lod_material_texture(PF_HANDLE model, int lod, int mat) {
    auto* mesh = GetMesh(model, lod);
    if (!mesh || mat < 0 || mat >= static_cast<int>(mesh->materials.size())) return "";
    return mesh->materials[mat].texturePath.c_str();
}

PF_API const char* pf_lod_material_surface(PF_HANDLE model, int lod, int mat) {
    auto* mesh = GetMesh(model, lod);
    if (!mesh || mat < 0 || mat >= static_cast<int>(mesh->materials.size())) return "";
    return mesh->materials[mat].name.c_str();
}

// ── Named Selections ──────────────────────────────────────────────────────

PF_API int pf_lod_selection_count(PF_HANDLE model, int lod) {
    auto* mesh = GetMesh(model, lod);
    return mesh ? static_cast<int>(mesh->selections.size()) : 0;
}

PF_API const char* pf_lod_selection_name(PF_HANDLE model, int lod, int sel) {
    auto* mesh = GetMesh(model, lod);
    if (!mesh || sel < 0 || sel >= static_cast<int>(mesh->selections.size())) return "";
    return mesh->selections[sel].name.c_str();
}

PF_API int pf_lod_selection_vertex_count(PF_HANDLE model, int lod, int sel) {
    auto* mesh = GetMesh(model, lod);
    if (!mesh || sel < 0 || sel >= static_cast<int>(mesh->selections.size())) return 0;
    return static_cast<int>(mesh->selections[sel].vertexIndices.size());
}

PF_API void pf_lod_selection_get_vertices(PF_HANDLE model, int lod, int sel,
                                           int* indices, float* weights) {
    auto* mesh = GetMesh(model, lod);
    if (!mesh || sel < 0 || sel >= static_cast<int>(mesh->selections.size())) return;
    auto& s = mesh->selections[sel];
    if (indices) {
        for (size_t i = 0; i < s.vertexIndices.size(); i++)
            indices[i] = static_cast<int>(s.vertexIndices[i]);
    }
    if (weights) {
        for (size_t i = 0; i < s.vertexIndices.size(); i++)
            weights[i] = (i < s.vertexWeights.size()) ? s.vertexWeights[i] / 255.0f : 1.0f;
    }
}

// ── Proxies ────────────────────────────────────────────────────────────────

PF_API int pf_lod_proxy_count(PF_HANDLE model, int lod) {
    auto* mesh = GetMesh(model, lod);
    return mesh ? static_cast<int>(mesh->proxies.size()) : 0;
}

PF_API const char* pf_lod_proxy_path(PF_HANDLE model, int lod, int prx) {
    auto* mesh = GetMesh(model, lod);
    if (!mesh || prx < 0 || prx >= static_cast<int>(mesh->proxies.size())) return "";
    return mesh->proxies[prx].name.c_str();
}

PF_API void pf_lod_proxy_transform(PF_HANDLE model, int lod, int prx, float* mat4x4) {
    auto* mesh = GetMesh(model, lod);
    if (!mesh || !mat4x4 || prx < 0 || prx >= static_cast<int>(mesh->proxies.size())) return;
    auto& t = mesh->proxies[prx].transform;
    // Convert 4x3 → 4x4 column-major (Blender convention)
    // Row 0: right vector
    mat4x4[0]  = t.m[0][0]; mat4x4[1]  = t.m[1][0]; mat4x4[2]  = t.m[2][0]; mat4x4[3]  = 0.0f;
    // Row 1: up vector
    mat4x4[4]  = t.m[0][1]; mat4x4[5]  = t.m[1][1]; mat4x4[6]  = t.m[2][1]; mat4x4[7]  = 0.0f;
    // Row 2: forward vector
    mat4x4[8]  = t.m[0][2]; mat4x4[9]  = t.m[1][2]; mat4x4[10] = t.m[2][2]; mat4x4[11] = 0.0f;
    // Row 3: translation
    mat4x4[12] = t.m[0][3]; mat4x4[13] = t.m[1][3]; mat4x4[14] = t.m[2][3]; mat4x4[15] = 1.0f;
}

// ── Named Properties ──────────────────────────────────────────────────────

PF_API int pf_lod_property_count(PF_HANDLE model, int lod) {
    auto* mesh = GetMesh(model, lod);
    return mesh ? static_cast<int>(mesh->properties.size()) : 0;
}

PF_API const char* pf_lod_property_name(PF_HANDLE model, int lod, int prop) {
    auto* mesh = GetMesh(model, lod);
    if (!mesh || prop < 0 || prop >= static_cast<int>(mesh->properties.size())) return "";
    return mesh->properties[prop].name.c_str();
}

PF_API const char* pf_lod_property_value(PF_HANDLE model, int lod, int prop) {
    auto* mesh = GetMesh(model, lod);
    if (!mesh || prop < 0 || prop >= static_cast<int>(mesh->properties.size())) return "";
    return mesh->properties[prop].value.c_str();
}

// ── PAA/PAC Image ──────────────────────────────────────────────────────────

PF_API PF_HANDLE pf_image_load(const char* path) {
    ClearError();
    if (!path) { SetError("null path"); return nullptr; }
    try {
        Poseidon::PAAInfo info;
        if (!Poseidon::ReadPAAInfo(path, info)) {
            SetError("Not a PAA/PAC file");
            return nullptr;
        }
        auto decoded = Poseidon::DecodePAAFile(path);
        if (!decoded.valid()) {
            SetError("Failed to decode image");
            return nullptr;
        }
        auto* wrapper = new PFImage();
        wrapper->image = std::move(decoded);
        wrapper->formatName = info.formatName ? info.formatName : "unknown";
        return wrapper;
    } catch (const std::exception& e) {
        SetError(e.what());
        return nullptr;
    }
}

PF_API void pf_image_free(PF_HANDLE img) {
    delete static_cast<PFImage*>(img);
}

PF_API int pf_image_width(PF_HANDLE img) {
    if (!img) return 0;
    return static_cast<PFImage*>(img)->image.width;
}

PF_API int pf_image_height(PF_HANDLE img) {
    if (!img) return 0;
    return static_cast<PFImage*>(img)->image.height;
}

PF_API const char* pf_image_format(PF_HANDLE img) {
    if (!img) return "";
    return static_cast<PFImage*>(img)->formatName.c_str();
}

PF_API void pf_image_get_rgba(PF_HANDLE img, uint8_t* buffer) {
    if (!img || !buffer) return;
    auto& image = static_cast<PFImage*>(img)->image;
    memcpy(buffer, image.rgba.data(), image.rgba.size());
}

// ── PBO Archive ────────────────────────────────────────────────────────────

static void PboCollectEntry(const FileInfoO& fi, const FileBankType*, void* ctx) {
    auto* entries = static_cast<std::vector<PFPboEntry>*>(ctx);
    PFPboEntry e;
    e.name = (const char*)fi.name;
    e.length = fi.length;
    e.time = fi.time;
    e.compressedMagic = fi.compressedMagic;
    e.uncompressedSize = fi.uncompressedSize;
    entries->push_back(e);
}

// Strip .pbo extension (QFBank::open appends it)
static std::string StripPboExt(const std::string& path) {
    if (path.size() >= 4) {
        auto ext = path.substr(path.size() - 4);
        for (auto& c : ext) c = static_cast<char>(tolower(c));
        if (ext == ".pbo") return path.substr(0, path.size() - 4);
    }
    return path;
}

PF_API PF_HANDLE pf_pbo_open(const char* path) {
    ClearError();
    if (!path) { SetError("null path"); return nullptr; }
    try {
        auto* pbo = new PFPbo();
        std::string bankName = StripPboExt(path);
        if (!pbo->bank.open(RString(bankName.c_str()))) {
            SetError("Failed to open PBO");
            delete pbo;
            return nullptr;
        }
        pbo->bank.Lock();
        if (pbo->bank.error()) {
            SetError("PBO read error");
            pbo->bank.Unlock();
            delete pbo;
            return nullptr;
        }
        pbo->bank.ForEach(PboCollectEntry, &pbo->entries);
        return pbo;
    } catch (const std::exception& e) {
        SetError(e.what());
        return nullptr;
    }
}

PF_API void pf_pbo_close(PF_HANDLE pbo) {
    if (!pbo) return;
    auto* p = static_cast<PFPbo*>(pbo);
    p->bank.Unlock();
    delete p;
}

PF_API int pf_pbo_entry_count(PF_HANDLE pbo) {
    if (!pbo) return 0;
    return static_cast<int>(static_cast<PFPbo*>(pbo)->entries.size());
}

PF_API const char* pf_pbo_entry_name(PF_HANDLE pbo, int idx) {
    if (!pbo) return "";
    auto& entries = static_cast<PFPbo*>(pbo)->entries;
    if (idx < 0 || idx >= static_cast<int>(entries.size())) return "";
    return entries[idx].name.c_str();
}

PF_API int pf_pbo_entry_size(PF_HANDLE pbo, int idx) {
    if (!pbo) return 0;
    auto& entries = static_cast<PFPbo*>(pbo)->entries;
    if (idx < 0 || idx >= static_cast<int>(entries.size())) return 0;
    auto& e = entries[idx];
    return (e.compressedMagic == 0x43707273 /*CompMagic*/) ? e.uncompressedSize : e.length;
}

PF_API int pf_pbo_extract(PF_HANDLE pbo, int idx, uint8_t* buffer, int buffer_size) {
    ClearError();
    if (!pbo || !buffer) return 0;
    auto* p = static_cast<PFPbo*>(pbo);
    if (idx < 0 || idx >= static_cast<int>(p->entries.size())) return 0;
    try {
        Ref<IFileBuffer> data = p->bank.Read(p->entries[idx].name.c_str());
        if (!data) { SetError("Failed to read entry"); return 0; }
        int size = static_cast<int>(data->GetSize());
        if (size > buffer_size) size = buffer_size;
        if (size > 0) memcpy(buffer, data->GetData(), size);
        return size;
    } catch (const std::exception& e) {
        SetError(e.what());
        return 0;
    }
}

// ── VFS (Virtual File System) ──────────────────────────────────────────────

static bool g_vfsMounted = false;

PF_API int pf_vfs_mount(const char* game_dir) {
    ClearError();
    if (!game_dir) { SetError("null path"); return 0; }
    try {
        GFileBanks.Clear();
        int count = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 game_dir, std::filesystem::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".pbo") continue;
            std::string dir = entry.path().parent_path().string() + "\\";
            std::string stem = entry.path().stem().string();
            std::transform(stem.begin(), stem.end(), stem.begin(), ::tolower);
            GFileBanks.Load(RString(dir.c_str()), RString(""), RString(stem.c_str()), true);
            count++;
        }
        GUseFileBanks = count > 0;
        g_vfsMounted = GUseFileBanks;
        return count;
    } catch (const std::exception& e) {
        SetError(e.what());
        return 0;
    }
}

PF_API void pf_vfs_unmount(void) {
    QIFStreamB::ClearBanks();
    GUseFileBanks = false;
    g_vfsMounted = false;
}

PF_API int pf_vfs_mounted(void) {
    return g_vfsMounted ? 1 : 0;
}

PF_API int pf_vfs_read(const char* vfs_path, uint8_t* buffer, int buffer_size) {
    ClearError();
    if (!vfs_path || !g_vfsMounted) { SetError("VFS not mounted or null path"); return 0; }
    try {
        std::string epath(vfs_path);
        std::replace(epath.begin(), epath.end(), '/', '\\');
        const char* lookup = epath.c_str();
        if (*lookup == '\\') lookup++;

        QFBank* bank = QIFStreamB::AutoBank(lookup);
        if (!bank) { SetError("File not found in VFS: " + epath); return 0; }
        const char* rel = lookup + bank->GetPrefix().GetLength();
        bank->Lock();
        Ref<IFileBuffer> data = bank->Read(rel);
        bank->Unlock();
        if (!data || data->GetSize() == 0) { SetError("Failed to read from PBO: " + epath); return 0; }
        int size = static_cast<int>(data->GetSize());
        if (!buffer) return size;  // query size only
        if (size > buffer_size) size = buffer_size;
        memcpy(buffer, data->GetData(), size);
        return size;
    } catch (const std::exception& e) {
        SetError(e.what());
        return 0;
    }
}

PF_API PF_HANDLE pf_vfs_image_load(const char* vfs_path) {
    ClearError();
    if (!vfs_path || !g_vfsMounted) { SetError("VFS not mounted or null path"); return nullptr; }
    try {
        std::string epath(vfs_path);
        std::replace(epath.begin(), epath.end(), '/', '\\');
        // Strip leading backslash for AutoBank prefix matching
        const char* lookup = epath.c_str();
        if (*lookup == '\\') lookup++;

        QFBank* bank = QIFStreamB::AutoBank(lookup);
        if (!bank) { SetError("Texture not found in VFS: " + epath); return nullptr; }
        const char* rel = lookup + bank->GetPrefix().GetLength();
        bank->Lock();
        Ref<IFileBuffer> data = bank->Read(rel);
        bank->Unlock();
        if (!data || data->GetSize() == 0) { SetError("Failed to read from PBO: " + epath); return nullptr; }

        bool isPaa = true;
        auto dot = epath.rfind('.');
        if (dot != std::string::npos) {
            auto ext = epath.substr(dot);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            isPaa = (ext == ".paa");
        }
        auto decoded = Poseidon::DecodePAABuffer(data->GetData(), data->GetSize(), isPaa);
        if (!decoded.valid()) { SetError("Failed to decode texture"); return nullptr; }

        auto* wrapper = new PFImage();
        wrapper->image = std::move(decoded);
        wrapper->formatName = isPaa ? "PAA" : "PAC";
        return wrapper;
    } catch (const std::exception& e) {
        SetError(e.what());
        return nullptr;
    }
}

// ── VFS File Enumeration ───────────────────────────────────────────────────

struct PFVfsList { std::vector<std::string> paths; };

PF_API PF_HANDLE pf_vfs_find(const char* extension) {
    ClearError();
    if (!g_vfsMounted) { SetError("VFS not mounted"); return nullptr; }
    try {
        auto* list = new PFVfsList();
        std::string ext(extension ? extension : "");
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        for (int i = 0; i < GFileBanks.Size(); i++) {
            QFBank& bank = GFileBanks[i];
            bank.Lock();
            std::string prefix((const char*)bank.GetPrefix());
            struct Ctx { PFVfsList* list; std::string* ext; std::string* prefix; };
            Ctx ctx{list, &ext, &prefix};
            bank.ForEach([](const FileInfoO& fi, const FileBankType*, void* c) {
                auto* x = static_cast<Ctx*>(c);
                std::string name((const char*)fi.name);
                std::string lower = name;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                if (x->ext->empty() || (lower.size() >= x->ext->size() &&
                    lower.substr(lower.size() - x->ext->size()) == *x->ext))
                    x->list->paths.push_back(*x->prefix + name);
            }, &ctx);
            bank.Unlock();
        }
        std::sort(list->paths.begin(), list->paths.end());
        return list;
    } catch (const std::exception& e) {
        SetError(e.what());
        return nullptr;
    }
}

PF_API int pf_vfs_find_count(PF_HANDLE list) {
    if (!list) return 0;
    return static_cast<int>(static_cast<PFVfsList*>(list)->paths.size());
}

PF_API const char* pf_vfs_find_path(PF_HANDLE list, int idx) {
    if (!list) return "";
    auto& paths = static_cast<PFVfsList*>(list)->paths;
    if (idx < 0 || idx >= static_cast<int>(paths.size())) return "";
    return paths[idx].c_str();
}

PF_API void pf_vfs_find_free(PF_HANDLE list) {
    delete static_cast<PFVfsList*>(list);
}

// ── RTM Animation ──────────────────────────────────────────────────────────

struct PFRtm { Poseidon::Asset::Formats::RTMAnimation anim; };

PF_API PF_HANDLE pf_rtm_load(const char* path) {
    ClearError();
    if (!path) { SetError("NULL path"); return nullptr; }
    auto rtm = new PFRtm();
    if (!Poseidon::Asset::Formats::readRTMFromFile(path, rtm->anim)) {
        delete rtm;
        SetError(std::string("Failed to load RTM: ") + path);
        return nullptr;
    }
    return rtm;
}

PF_API PF_HANDLE pf_rtm_load_from_memory(const uint8_t* data, int size) {
    ClearError();
    if (!data || size <= 0) { SetError("Invalid RTM data"); return nullptr; }
    auto rtm = new PFRtm();
    if (!Poseidon::Asset::Formats::readRTMFromMemory(data, size, rtm->anim)) {
        delete rtm;
        SetError("Failed to parse RTM from memory");
        return nullptr;
    }
    return rtm;
}

PF_API void pf_rtm_free(PF_HANDLE rtm) {
    delete static_cast<PFRtm*>(rtm);
}

PF_API int pf_rtm_phase_count(PF_HANDLE rtm) {
    if (!rtm) return 0;
    return static_cast<PFRtm*>(rtm)->anim.phaseCount();
}

PF_API int pf_rtm_bone_count(PF_HANDLE rtm) {
    if (!rtm) return 0;
    return static_cast<PFRtm*>(rtm)->anim.boneCount();
}

PF_API void pf_rtm_step(PF_HANDLE rtm, float* xyz) {
    if (!rtm || !xyz) return;
    auto& a = static_cast<PFRtm*>(rtm)->anim;
    xyz[0] = a.stepX; xyz[1] = a.stepY; xyz[2] = a.stepZ;
}

PF_API const char* pf_rtm_bone_name(PF_HANDLE rtm, int bone) {
    if (!rtm) return "";
    auto& a = static_cast<PFRtm*>(rtm)->anim;
    if (bone < 0 || bone >= a.boneCount()) return "";
    return a.boneNames[bone].c_str();
}

PF_API float pf_rtm_phase_time(PF_HANDLE rtm, int phase) {
    if (!rtm) return 0.0f;
    auto& a = static_cast<PFRtm*>(rtm)->anim;
    if (phase < 0 || phase >= a.phaseCount()) return 0.0f;
    return a.phases[phase].time;
}

PF_API void pf_rtm_bone_matrix(PF_HANDLE rtm, int phase, int bone, float* mat4x4) {
    if (!rtm || !mat4x4) return;
    auto& a = static_cast<PFRtm*>(rtm)->anim;
    if (phase < 0 || phase >= a.phaseCount()) return;
    if (bone < 0 || bone >= a.boneCount()) return;
    // Input: 12 floats [aside(3) up(3) dir(3) pos(3)]
    // Output: column-major 4x4 — col0=aside, col1=up, col2=dir, col3=pos
    // Engine Get(i,j)=(&aside)[j][i] means columns ARE the basis vectors
    const float* m = a.phases[phase].transforms[bone].m;
    mat4x4[0]  = m[0];  mat4x4[1]  = m[1];  mat4x4[2]  = m[2];  mat4x4[3]  = 0.0f;
    mat4x4[4]  = m[3];  mat4x4[5]  = m[4];  mat4x4[6]  = m[5];  mat4x4[7]  = 0.0f;
    mat4x4[8]  = m[6];  mat4x4[9]  = m[7];  mat4x4[10] = m[8];  mat4x4[11] = 0.0f;
    mat4x4[12] = m[9];  mat4x4[13] = m[10]; mat4x4[14] = m[11]; mat4x4[15] = 1.0f;
}

// --- RVMAT material source (AST-014) ---------------------------------------

namespace
{
struct PFMaterial
{
    Poseidon::Asset::Material::RvMaterialSource source;
    std::string                                 scratch;
};
} // namespace

PF_API PF_HANDLE pf_material_load(const char* path)
{
    ClearError();
    if (!path) { SetError("null path"); return nullptr; }
    try
    {
        auto* wrapper = new PFMaterial();
        wrapper->source = Poseidon::Asset::Material::ParseRvMaterialFile(path);
        return wrapper;
    }
    catch (const std::exception& e) { SetError(e.what()); return nullptr; }
}

PF_API void pf_material_free(PF_HANDLE mat) { delete static_cast<PFMaterial*>(mat); }

static PFMaterial* AsMaterial(PF_HANDLE mat) { return static_cast<PFMaterial*>(mat); }

PF_API const char* pf_material_pixel_shader(PF_HANDLE mat)
{
    auto* m = AsMaterial(mat);
    return m ? m->source.pixelShaderId.c_str() : "";
}
PF_API int pf_material_stage_count(PF_HANDLE mat)
{
    auto* m = AsMaterial(mat);
    return m ? static_cast<int>(m->source.stages.size()) : 0;
}
PF_API int pf_material_stage_index(PF_HANDLE mat, int i)
{
    auto* m = AsMaterial(mat);
    if (!m || i < 0 || i >= static_cast<int>(m->source.stages.size())) return -1;
    return m->source.stages[i].index;
}
PF_API const char* pf_material_stage_texture(PF_HANDLE mat, int i)
{
    auto* m = AsMaterial(mat);
    if (!m || i < 0 || i >= static_cast<int>(m->source.stages.size())) return "";
    m->scratch = m->source.stages[i].texture.raw;
    return m->scratch.c_str();
}
PF_API const char* pf_material_stage_uvsource(PF_HANDLE mat, int i)
{
    auto* m = AsMaterial(mat);
    if (!m || i < 0 || i >= static_cast<int>(m->source.stages.size())) return "";
    return m->source.stages[i].uvSource.c_str();
}
PF_API int pf_material_stage_is_procedural(PF_HANDLE mat, int i)
{
    auto* m = AsMaterial(mat);
    if (!m || i < 0 || i >= static_cast<int>(m->source.stages.size())) return 0;
    return m->source.stages[i].texture.isProcedural ? 1 : 0;
}
PF_API int pf_material_texgen_count(PF_HANDLE mat)
{
    auto* m = AsMaterial(mat);
    return m ? static_cast<int>(m->source.texGens.size()) : 0;
}
PF_API int pf_material_other_class_count(PF_HANDLE mat)
{
    auto* m = AsMaterial(mat);
    return m ? static_cast<int>(m->source.otherClasses.size()) : 0;
}
