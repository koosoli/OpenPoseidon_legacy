#include "Renderer.hpp"

#include "Builders.hpp"

#include <PoseidonD3D12/Dx12Device.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

// Embedded bytecode (fxc, see CMakeLists.txt).
#include "psLine.h"
#include "psModel.h"
#include "psTerrain.h"
#include "vsLine.h"
#include "vsTerrain.h"

namespace island
{
namespace
{
struct TerrainVertex
{
    float pos[3];
    float normal[3];
    float uv[2];
};

struct LineVertex
{
    float pos[3];
    uint32_t color; // 0xAARRGGBB
};

float Dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

bool Contains(const std::string& s, const char* what)
{
    return s.find(what) != std::string::npos;
}
} // namespace

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------

void Camera::Basis(float f[3], float r[3], float u[3]) const
{
    const float cp = std::cos(pitch), sp = std::sin(pitch), cy = std::cos(yaw), sy = std::sin(yaw);
    f[0] = sy * cp;
    f[1] = sp;
    f[2] = cy * cp;
    r[0] = cy;
    r[1] = 0;
    r[2] = -sy;
    u[0] = f[1] * r[2] - f[2] * r[1];
    u[1] = f[2] * r[0] - f[0] * r[2];
    u[2] = f[0] * r[1] - f[1] * r[0];
}

void Camera::ViewProj(float aspect, float m[16]) const
{
    float f[3], r[3], u[3];
    Basis(f, r, u);
    const float sy = 1.0f / std::tan(fovY * 0.5f), sx = sy / aspect;
    const float q = farZ / (farZ - nearZ);
    const float row[4][4] = {{sx * r[0], sx * r[1], sx * r[2], -sx * Dot(r, pos)},
                             {sy * u[0], sy * u[1], sy * u[2], -sy * Dot(u, pos)},
                             {q * f[0], q * f[1], q * f[2], -q * Dot(f, pos) - nearZ * q},
                             {f[0], f[1], f[2], -Dot(f, pos)}};
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++)
            m[c * 4 + rr] = row[rr][c];
}

void Camera::Ray(float mouseX, float mouseY, float width, float height, float dir[3]) const
{
    float f[3], r[3], u[3];
    Basis(f, r, u);
    const float t = std::tan(fovY * 0.5f);
    const float nx = (mouseX / width * 2.0f - 1.0f) * t * (width / height);
    const float ny = (1.0f - mouseY / height * 2.0f) * t;
    float d[3] = {f[0] + r[0] * nx + u[0] * ny, f[1] + r[1] * nx + u[1] * ny, f[2] + r[2] * nx + u[2] * ny};
    const float len = std::sqrt(Dot(d, d));
    dir[0] = d[0] / len;
    dir[1] = d[1] / len;
    dir[2] = d[2] / len;
}

bool Camera::Project(const float world[3], float width, float height, float& sx, float& sy) const
{
    float f[3], r[3], u[3];
    Basis(f, r, u);
    const float d[3] = {world[0] - pos[0], world[1] - pos[1], world[2] - pos[2]};
    const float z = Dot(d, f);
    if (z < nearZ)
        return false;
    const float t = std::tan(fovY * 0.5f);
    sx = (Dot(d, r) / (z * t * (width / height)) * 0.5f + 0.5f) * width;
    sy = (0.5f - Dot(d, u) / (z * t) * 0.5f) * height;
    return true;
}

ObjectKind KindOf(const std::string& model)
{
    const std::string m = LowerPath(model);
    const size_t slash = m.find_last_of('\\');
    const std::string file = slash == std::string::npos ? m : m.substr(slash + 1);
    auto starts = [&](const char* p) { return file.rfind(p, 0) == 0; };
    // the stock islands name their models in Czech: str(om) = tree, ker / krovi = bush,
    // asfaltka / silnice / cesta / kos = roads, kr_ = their crossings, plot / pletivo / zed = fences and walls
    if (Contains(m, "\\road\\") || starts("asf") || starts("sil") || starts("ces") || starts("kos") || starts("road") ||
        starts("kr_") || starts("most") || starts("bridge") || starts("runway") || starts("letist"))
        return ObjectKind::Road;
    if (Contains(m, "\\tree\\") || starts("les") || starts("str") || starts("smrk") || starts("jedle") || starts("dub") ||
        starts("buk") || starts("borov") || starts("palm") || starts("oliv") || starts("tree") || starts("jablon") ||
        starts("briza") || starts("topol") || starts("akat") || starts("sakura") || starts("hrusen"))
        return ObjectKind::Tree;
    if (starts("ker") || starts("krovi") || starts("bush") || starts("trs") || Contains(file, "bush") || starts("kaktus"))
        return ObjectKind::Bush;
    if (starts("plot") || starts("pletivo") || starts("zed") || starts("ohrada") || starts("newplot") || starts("jezek") ||
        starts("plutek") || starts("kamen") || starts("skala") || starts("znacka") || starts("barbed") || starts("fence") ||
        starts("wall") || starts("rock") || starts("sign"))
        return ObjectKind::Other;
    return ObjectKind::Building;
}

// ---------------------------------------------------------------------------
// set-up
// ---------------------------------------------------------------------------

bool Renderer::Init(Dx12::Device* device)
{
    _dev = device;
    if (FAILED(_dev->CreateVertexShader(g_vsTerrain, sizeof(g_vsTerrain), &_vsTerrain)) ||
        FAILED(_dev->CreatePixelShader(g_psTerrain, sizeof(g_psTerrain), &_psTerrain)) ||
        FAILED(_dev->CreatePixelShader(g_psModel, sizeof(g_psModel), &_psModel)) ||
        FAILED(_dev->CreateVertexShader(g_vsLine, sizeof(g_vsLine), &_vsLine)) ||
        FAILED(_dev->CreatePixelShader(g_psLine, sizeof(g_psLine), &_psLine)))
        return false;
    const GxVertexElement terrain[] = {{0, 0, GXDECLTYPE_FLOAT3, GXDECLMETHOD_DEFAULT, GXDECLUSAGE_POSITION, 0},
                                       {0, 12, GXDECLTYPE_FLOAT3, GXDECLMETHOD_DEFAULT, GXDECLUSAGE_NORMAL, 0},
                                       {0, 24, GXDECLTYPE_FLOAT2, GXDECLMETHOD_DEFAULT, GXDECLUSAGE_TEXCOORD, 0},
                                       GXDECL_END()};
    const GxVertexElement line[] = {{0, 0, GXDECLTYPE_FLOAT3, GXDECLMETHOD_DEFAULT, GXDECLUSAGE_POSITION, 0},
                                    {0, 12, GXDECLTYPE_COLOR, GXDECLMETHOD_DEFAULT, GXDECLUSAGE_COLOR, 0},
                                    GXDECL_END()};
    if (FAILED(_dev->CreateVertexDeclaration(terrain, &_declTerrain)) ||
        FAILED(_dev->CreateVertexDeclaration(line, &_declLine)))
        return false;

    // the atlas and its smaller copies
    const int levels = 5; // down to 8 pixels a texture
    if (FAILED(_dev->CreateTexture(kTile * kAtlasCols, kTile * kAtlasRows, levels, 0, GXFMT_A8R8G8B8, GXPOOL_MANAGED,
                                   &_atlas, nullptr)))
        return false;
    _atlasMips.resize(levels);
    for (int l = 0; l < levels; l++)
        _atlasMips[l].assign(static_cast<size_t>(kTile * kAtlasCols >> l) * (kTile * kAtlasRows >> l) * 4, 0);
    _dev->SetExternalTexture(&_atlasHandle, _atlas);
    // what a model is drawn with while its texture is not there (yet)
    if (SUCCEEDED(_dev->CreateTexture(1, 1, 1, 0, GXFMT_A8R8G8B8, GXPOOL_MANAGED, &_grey, nullptr)))
    {
        GxLockedRect lr = {};
        if (SUCCEEDED(_grey->LockRect(0, &lr, nullptr, 0)))
        {
            const uint8_t grey[4] = {150, 150, 150, 255};
            std::memcpy(lr.pBits, grey, 4);
            _grey->UnlockRect(0);
        }
    }
    return true;
}

void Renderer::ReleaseChunks()
{
    for (Chunk& c : _chunks)
        if (c.vb)
            c.vb->Release();
    _chunks.clear();
}

void Renderer::Shutdown()
{
    ReleaseChunks();
    ReleaseModels(true);
    if (_grey)
        _grey->Release();
    if (_psModel)
        _psModel->Release();
    _grey = nullptr;
    _psModel = nullptr;
    if (_ib)
        _ib->Release();
    if (_atlas)
        _atlas->Release();
    if (_minimap)
        _minimap->Release();
    if (_declTerrain)
        _declTerrain->Release();
    if (_declLine)
        _declLine->Release();
    if (_vsTerrain)
        _vsTerrain->Release();
    if (_psTerrain)
        _psTerrain->Release();
    if (_vsLine)
        _vsLine->Release();
    if (_psLine)
        _psLine->Release();
    _ib = nullptr;
    _atlas = _minimap = nullptr;
}

// ---------------------------------------------------------------------------
// textures
// ---------------------------------------------------------------------------

void Renderer::AtlasUV(int slot, float uv0[2], float uv1[2]) const
{
    if (slot < 0 || slot >= kMaxTextures)
        slot = 0;
    const float w = static_cast<float>(kTile * kAtlasCols), h = static_cast<float>(kTile * kAtlasRows);
    const int col = slot % kAtlasCols, row = slot / kAtlasCols;
    const float inset = 1.0f; // keeps the neighbours out of the filtered edge
    uv0[0] = (col * kTile + inset) / w;
    uv0[1] = (row * kTile + inset) / h;
    uv1[0] = ((col + 1) * kTile - inset) / w;
    uv1[1] = ((row + 1) * kTile - inset) / h;
}

void Renderer::BuildTile(int slot)
{
    const int col = slot % kAtlasCols, row = slot / kAtlasCols;
    const int aw = kTile * kAtlasCols;
    std::vector<uint8_t> tile(static_cast<size_t>(kTile) * kTile * 4); // BGRA
    const std::string name = _is && slot < static_cast<int>(_is->textures.size()) ? _is->textures[slot] : std::string();
    Image img;
    if (!name.empty() && _game)
        img = _game->LoadTexture(name);
    _missing[slot] = slot != 0 && !name.empty() && !img.Valid(); // slot 0 is the sea whatever it is called
    if (img.Valid() && _filter && slot != 0)
        _filter(img.rgba.data(), static_cast<size_t>(img.width) * img.height);
    if (img.Valid())
    {
        for (int y = 0; y < kTile; y++)
            for (int x = 0; x < kTile; x++)
            {
                // the average of the source pixels this one covers
                const int x0 = x * img.width / kTile, x1 = std::max(x0 + 1, (x + 1) * img.width / kTile);
                const int y0 = y * img.height / kTile, y1 = std::max(y0 + 1, (y + 1) * img.height / kTile);
                unsigned sum[3] = {0, 0, 0}, n = 0;
                for (int sy = y0; sy < y1 && sy < img.height; sy++)
                    for (int sx = x0; sx < x1 && sx < img.width; sx++)
                    {
                        const uint8_t* p = &img.rgba[(static_cast<size_t>(sy) * img.width + sx) * 4];
                        sum[0] += p[0];
                        sum[1] += p[1];
                        sum[2] += p[2];
                        n++;
                    }
                uint8_t* d = &tile[(static_cast<size_t>(y) * kTile + x) * 4];
                if (n)
                {
                    d[0] = static_cast<uint8_t>(sum[2] / n);
                    d[1] = static_cast<uint8_t>(sum[1] / n);
                    d[2] = static_cast<uint8_t>(sum[0] / n);
                }
                d[3] = 255;
            }
    }
    else
    {
        for (int y = 0; y < kTile; y++)
            for (int x = 0; x < kTile; x++)
            {
                uint8_t* d = &tile[(static_cast<size_t>(y) * kTile + x) * 4];
                if (name.empty() || slot == 0)
                {
                    // nothing in this slot (slot 0: the sea bed)
                    const bool dark = ((x / 16) + (y / 16)) & 1;
                    d[0] = slot == 0 ? 120 : (dark ? 60 : 80);
                    d[1] = slot == 0 ? 150 : (dark ? 60 : 80);
                    d[2] = slot == 0 ? 160 : (dark ? 60 : 80);
                }
                else
                {
                    // named but not found: red with a black cross
                    const bool cross = std::abs(x - y) < 4 || std::abs(x - (kTile - 1 - y)) < 4;
                    d[0] = cross ? 0 : 30;
                    d[1] = cross ? 0 : 30;
                    d[2] = cross ? 0 : 200;
                }
                d[3] = 255;
            }
    }
    double avg[3] = {0, 0, 0};
    for (size_t i = 0; i < tile.size(); i += 4)
    {
        avg[0] += tile[i + 2];
        avg[1] += tile[i + 1];
        avg[2] += tile[i];
    }
    for (int c = 0; c < 3; c++)
        _average[slot][c] = static_cast<float>(avg[c] / (kTile * kTile) / 255.0);

    // into every level: each one is half the one before
    int size = kTile;
    for (size_t l = 0; l < _atlasMips.size(); l++)
    {
        const int lw = aw >> l;
        for (int y = 0; y < size; y++)
            std::memcpy(&_atlasMips[l][(static_cast<size_t>(row * size + y) * lw + col * size) * 4],
                        &tile[static_cast<size_t>(y) * size * 4], static_cast<size_t>(size) * 4);
        const int half = size / 2;
        std::vector<uint8_t> next(static_cast<size_t>(half) * half * 4);
        for (int y = 0; y < half; y++)
            for (int x = 0; x < half; x++)
                for (int c = 0; c < 4; c++)
                {
                    const int a = tile[(static_cast<size_t>(y * 2) * size + x * 2) * 4 + c];
                    const int b = tile[(static_cast<size_t>(y * 2) * size + x * 2 + 1) * 4 + c];
                    const int cc = tile[(static_cast<size_t>(y * 2 + 1) * size + x * 2) * 4 + c];
                    const int d = tile[(static_cast<size_t>(y * 2 + 1) * size + x * 2 + 1) * 4 + c];
                    next[(static_cast<size_t>(y) * half + x) * 4 + c] = static_cast<uint8_t>((a + b + cc + d + 2) / 4);
                }
        tile.swap(next);
        size = half;
    }
}

void Renderer::UploadAtlas()
{
    for (size_t l = 0; l < _atlasMips.size(); l++)
    {
        GxLockedRect lr = {};
        if (FAILED(_atlas->LockRect(static_cast<UINT>(l), &lr, nullptr, 0)))
            continue;
        std::memcpy(lr.pBits, _atlasMips[l].data(), _atlasMips[l].size());
        _atlas->UnlockRect(static_cast<UINT>(l));
    }
}

void Renderer::RefreshTextures()
{
    if (!_is || _is->size <= 0)
        return;
    for (int s = 0; s < kMaxTextures; s++)
        BuildTile(s);
    UploadAtlas();
}

void Renderer::SetTextureFilter(std::function<void(uint8_t* rgba, size_t count)> filter)
{
    _filter = std::move(filter);
    RefreshTextures();
}

void Renderer::UpdateTextures(const std::vector<int>& slots)
{
    if (slots.empty() || !_is || _is->size <= 0)
        return;
    for (int s : slots)
        if (s >= 0 && s < kMaxTextures)
            BuildTile(s);
    UploadAtlas();
}

void Renderer::UpdateTexture(int slot)
{
    if (slot < 0 || slot >= kMaxTextures)
        return;
    BuildTile(slot);
    UploadAtlas();
}

// ---------------------------------------------------------------------------
// ground
// ---------------------------------------------------------------------------

void Renderer::SetIsland(const Island* is, const GameData* game)
{
    _is = is;
    _game = game;
    ReleaseChunks();
    ReleaseModels(true); // another island, or another game folder: nothing of the old holds
    if (!_is || _is->size <= 0)
        return;
    for (int s = 0; s < kMaxTextures; s++)
        BuildTile(s);
    UploadAtlas();

    _chunkCells = std::min(kChunk, _is->size);
    _chunksPerSide = _is->size / _chunkCells;
    _chunks.resize(static_cast<size_t>(_chunksPerSide) * _chunksPerSide);
    for (int cz = 0; cz < _chunksPerSide; cz++)
        for (int cx = 0; cx < _chunksPerSide; cx++)
        {
            Chunk& c = _chunks[static_cast<size_t>(cz) * _chunksPerSide + cx];
            c.cx = cx;
            c.cz = cz;
            c.cells = _chunkCells;
            c.dirty = true;
        }
    if (!_ib || _ibCells != _chunkCells)
    {
        if (_ib)
            _ib->Release();
        _ib = nullptr;
        _ibCells = _chunkCells;
        const int cells = _chunkCells * _chunkCells;
        if (SUCCEEDED(_dev->CreateIndexBuffer(cells * 6 * 2, 0, GXFMT_INDEX16, GXPOOL_MANAGED, &_ib, nullptr)))
        {
            void* p = nullptr;
            if (SUCCEEDED(_ib->Lock(0, 0, &p, 0)))
            {
                uint16_t* i = static_cast<uint16_t*>(p);
                for (int c = 0; c < cells; c++)
                {
                    const uint16_t v = static_cast<uint16_t>(c * 4);
                    // (x+1,z) (x,z) (x,z+1) and (x+1,z) (x,z+1) (x+1,z+1): the game's split
                    *i++ = v + 1, *i++ = v, *i++ = v + 2;
                    *i++ = v + 1, *i++ = v + 2, *i++ = v + 3;
                }
                _ib->Unlock();
            }
        }
    }

    const int want = std::min(_is->size, 512);
    if (!_minimap || _minimapSize != want)
    {
        if (_minimap)
            _minimap->Release();
        _minimap = nullptr;
        _minimapSize = want;
        if (SUCCEEDED(_dev->CreateTexture(want, want, 1, 0, GXFMT_A8R8G8B8, GXPOOL_MANAGED, &_minimap, nullptr)))
            _dev->SetExternalTexture(&_minimapHandle, _minimap);
    }
}

void Renderer::TerrainChanged(int x0, int z0, int x1, int z1)
{
    if (_objPerSide > 0)
    {
        // what stands there stands on it; a forest block reaches into the cells around its own
        const int ax0 = std::max(0, (x0 - 2) / kObjCells), ax1 = std::min(_objPerSide - 1, (x1 + 2) / kObjCells);
        const int az0 = std::max(0, (z0 - 2) / kObjCells), az1 = std::min(_objPerSide - 1, (z1 + 2) / kObjCells);
        for (int az = az0; az <= az1; az++)
            for (int ax = ax0; ax <= ax1; ax++)
            {
                ObjChunk& c = _objChunks[static_cast<size_t>(az) * _objPerSide + ax];
                if (c.built >= 0)
                    c.dirty = true;
            }
    }
    if (_chunks.empty())
        return;
    // a point shapes the cells around it and the shading two points away
    const int cx0 = std::max(0, (x0 - 2) / _chunkCells), cz0 = std::max(0, (z0 - 2) / _chunkCells);
    const int cx1 = std::min(_chunksPerSide - 1, (x1 + 2) / _chunkCells);
    const int cz1 = std::min(_chunksPerSide - 1, (z1 + 2) / _chunkCells);
    for (int cz = cz0; cz <= cz1; cz++)
        for (int cx = cx0; cx <= cx1; cx++)
            _chunks[static_cast<size_t>(cz) * _chunksPerSide + cx].dirty = true;
}

void Renderer::BuildChunk(Chunk& c)
{
    c.dirty = false;
    const int n = c.cells;
    const UINT bytes = static_cast<UINT>(n * n * 4 * sizeof(TerrainVertex));
    if (!c.vb && FAILED(_dev->CreateVertexBuffer(bytes, 0, 0, GXPOOL_MANAGED, &c.vb, nullptr)))
        return;
    void* p = nullptr;
    if (FAILED(c.vb->Lock(0, 0, &p, 0)))
        return;
    TerrainVertex* v = static_cast<TerrainVertex*>(p);
    c.minY = 1e9f;
    c.maxY = -1e9f;
    const float cell = _is->cell;
    for (int lz = 0; lz < n; lz++)
        for (int lx = 0; lx < n; lx++)
        {
            const int x = c.cx * n + lx, z = c.cz * n + lz;
            float uv0[2], uv1[2];
            AtlasUV(_is->tex[static_cast<size_t>(z) * _is->size + x], uv0, uv1);
            for (int k = 0; k < 4; k++)
            {
                const int dx = k & 1, dz = k >> 1;
                const float y = _is->H(x + dx, z + dz);
                v->pos[0] = (x + dx) * cell;
                v->pos[1] = y;
                v->pos[2] = (z + dz) * cell;
                _is->Normal(x + dx, z + dz, v->normal);
                v->uv[0] = dx ? uv1[0] : uv0[0];
                v->uv[1] = dz ? uv1[1] : uv0[1];
                c.minY = std::min(c.minY, y);
                c.maxY = std::max(c.maxY, y);
                v++;
            }
        }
    c.vb->Unlock();
}

// ---------------------------------------------------------------------------
// the small map
// ---------------------------------------------------------------------------

void Renderer::UpdateMinimap(const ViewOptions& view)
{
    if (!_minimap || !_is || _is->size <= 0)
        return;
    const int n = _minimapSize;
    GxLockedRect lr = {};
    if (FAILED(_minimap->LockRect(0, &lr, nullptr, 0)))
        return;
    const float sun[3] = {std::sin(view.sunYaw) * std::cos(view.sunHeight), std::sin(view.sunHeight),
                          std::cos(view.sunYaw) * std::cos(view.sunHeight)};
    const int step = _is->size / n;
    for (int py = 0; py < n; py++)
    {
        uint8_t* row = static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(py) * lr.Pitch;
        const int z = (n - 1 - py) * step; // north at the top
        for (int px = 0; px < n; px++)
        {
            const int x = px * step;
            const int slot = _is->tex[static_cast<size_t>(z) * _is->size + x];
            float nrm[3];
            _is->Normal(x, z, nrm);
            const float light = 0.55f + 0.6f * std::max(0.0f, Dot(nrm, sun));
            float c[3] = {_average[slot][0] * light, _average[slot][1] * light, _average[slot][2] * light};
            const float h = _is->H(x, z);
            if (h < 0)
            {
                const float k = 0.45f + 0.5f * std::min(1.0f, -h / 30.0f);
                c[0] += (0.06f - c[0]) * k;
                c[1] += (0.2f - c[1]) * k;
                c[2] += (0.33f - c[2]) * k;
            }
            row[px * 4 + 0] = static_cast<uint8_t>(std::min(1.0f, c[2]) * 255);
            row[px * 4 + 1] = static_cast<uint8_t>(std::min(1.0f, c[1]) * 255);
            row[px * 4 + 2] = static_cast<uint8_t>(std::min(1.0f, c[0]) * 255);
            row[px * 4 + 3] = 255;
        }
    }
    _minimap->UnlockRect(0);
}


// ---------------------------------------------------------------------------
// objects as their models
// ---------------------------------------------------------------------------
//
// Islands hold tens of thousands of objects, so they are not drawn one by one: the land is
// cut into areas of 200 m, and the models of an area's objects are put together, already in
// their places, into one buffer with one draw per texture.  Areas are built as the camera
// comes near (in full detail close by, the models' coarsest level further off), rebuilt
// when their objects or the ground under them change, and dropped when it has gone.

namespace
{
uint64_t ObjectHash(const Object& o)
{
    uint64_t h = std::hash<std::string>()(o.model);
    auto mix = [&](uint32_t v) { h = (h ^ v) * 1099511628211ull; };
    for (int i = 0; i < 12; i++)
    {
        uint32_t bits;
        std::memcpy(&bits, &o.m[i], 4);
        mix(bits);
    }
    uint32_t above;
    std::memcpy(&above, &o.above, 4);
    mix(above);
    mix(o.fixed ? 1u : 0u);
    return h;
}

bool PowerOfTwo(int v)
{
    return v > 0 && (v & (v - 1)) == 0;
}

// half the size; colours weighted by how solid they are, so cut-out edges do not go dark
void Halve(std::vector<uint8_t>& rgba, int& w, int& h)
{
    const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
    std::vector<uint8_t> out(static_cast<size_t>(nw) * nh * 4);
    for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++)
        {
            unsigned sum[3] = {0, 0, 0}, plain[3] = {0, 0, 0}, alpha = 0;
            for (int k = 0; k < 4; k++)
            {
                const int sx = std::min(w - 1, x * 2 + (k & 1)), sy = std::min(h - 1, y * 2 + (k >> 1));
                const uint8_t* p = &rgba[(static_cast<size_t>(sy) * w + sx) * 4];
                for (int c = 0; c < 3; c++)
                {
                    sum[c] += p[c] * p[3];
                    plain[c] += p[c];
                }
                alpha += p[3];
            }
            uint8_t* d = &out[(static_cast<size_t>(y) * nw + x) * 4];
            for (int c = 0; c < 3; c++)
                d[c] = static_cast<uint8_t>(alpha ? sum[c] / alpha : plain[c] / 4);
            // a little more solid than the average: thin leaves would vanish with distance
            d[3] = static_cast<uint8_t>(std::min(255u, alpha * 9 / 32));
        }
    rgba.swap(out);
    w = nw;
    h = nh;
}
} // namespace

int Renderer::ModelTexture(const std::string& name)
{
    auto it = _modelTexIndex.find(name);
    if (it != _modelTexIndex.end())
        return it->second;
    ModelTex t;
    t.name = name;
    _modelTex.push_back(t);
    const int index = static_cast<int>(_modelTex.size()) - 1;
    _modelTexIndex[name] = index;
    return index;
}

void Renderer::LoadModelTexture(ModelTex& t)
{
    t.tried = true;
    if (!_game || t.name.empty())
        return;
    Image img = _game->LoadTexture(t.name);
    if (!img.Valid())
        return;
    int w = img.width, h = img.height;
    std::vector<uint8_t> rgba = std::move(img.rgba);
    const bool pow2 = PowerOfTwo(w) && PowerOfTwo(h);
    while (pow2 && (w > 512 || h > 512))
        Halve(rgba, w, h);
    int levels = 1;
    if (pow2)
        for (int a = w, b = h; a > 4 && b > 4; a >>= 1, b >>= 1)
            levels++;
    if (FAILED(_dev->CreateTexture(static_cast<UINT>(w), static_cast<UINT>(h), static_cast<UINT>(levels), 0, GXFMT_A8R8G8B8, GXPOOL_MANAGED,
                                   &t.tex, nullptr)))
    {
        t.tex = nullptr;
        return;
    }
    for (int l = 0; l < levels; l++)
    {
        GxLockedRect lr = {};
        if (SUCCEEDED(t.tex->LockRect(static_cast<UINT>(l), &lr, nullptr, 0)))
        {
            for (int y = 0; y < h; y++)
            {
                uint8_t* d = static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch;
                const uint8_t* s = &rgba[static_cast<size_t>(y) * w * 4];
                for (int x = 0; x < w; x++, d += 4, s += 4)
                {
                    d[0] = s[2];
                    d[1] = s[1];
                    d[2] = s[0];
                    d[3] = s[3];
                }
            }
            t.tex->UnlockRect(static_cast<UINT>(l));
        }
        if (l + 1 < levels)
            Halve(rgba, w, h);
    }
}

void Renderer::ReleaseObjChunk(ObjChunk& c)
{
    if (c.vb)
        c.vb->Release();
    if (c.ib)
        c.ib->Release();
    c.vb = c.ib = nullptr;
    c.batches.clear();
    c.baked.clear();
    c.base.clear();
    c.built = -1;
    c.dirty = false;
    c.triangles = 0;
}

void Renderer::ReleaseModels(bool textures)
{
    for (ObjChunk& c : _objChunks)
        ReleaseObjChunk(c);
    std::fill(_objAsModel.begin(), _objAsModel.end(), static_cast<uint8_t>(0));
    if (textures)
    {
        for (ModelTex& t : _modelTex)
            if (t.tex)
                t.tex->Release();
        _modelTex.clear();
        _modelTexIndex.clear();
        _objChunks.clear();
        _objPerSide = 0;
        _objBinned = false;
    }
}

// Which objects stand in which area; the areas whose objects are no longer what was built
// are marked for building again.
void Renderer::BinObjects()
{
    const int per = (_is->size + kObjCells - 1) / kObjCells;
    if (per != _objPerSide)
    {
        for (ObjChunk& c : _objChunks)
            ReleaseObjChunk(c);
        _objChunks.assign(static_cast<size_t>(per) * per, ObjChunk());
        _objPerSide = per;
    }
    std::vector<uint64_t> was(_objChunks.size());
    for (size_t i = 0; i < _objChunks.size(); i++)
    {
        was[i] = _objChunks[i].hash;
        _objChunks[i].hash = 1469598103934665603ull;
        _objChunks[i].objects.clear();
    }
    const float size = _is->cell * kObjCells;
    for (size_t i = 0; i < _is->objects.size(); i++)
    {
        const Object& o = _is->objects[i];
        const int cx = std::clamp(static_cast<int>(o.X() / size), 0, per - 1), cz = std::clamp(static_cast<int>(o.Z() / size), 0, per - 1);
        ObjChunk& c = _objChunks[static_cast<size_t>(cz) * per + cx];
        c.objects.push_back(static_cast<int>(i));
        c.hash = (c.hash ^ ObjectHash(o)) * 1099511628211ull;
    }
    _objAsModel.assign(_is->objects.size(), 0);
    for (size_t i = 0; i < _objChunks.size(); i++)
    {
        ObjChunk& c = _objChunks[i];
        if (c.built < 0)
            continue;
        if (c.hash != was[i])
            c.dirty = true;
        else if (!c.dirty && c.baked.size() == c.objects.size())
            for (size_t k = 0; k < c.objects.size(); k++)
                _objAsModel[c.objects[k]] = c.baked[k];
    }
    _objRevision = _is->objectsRevision;
    _objBinned = true;
}

// The models of an area's objects, in their places, as one buffer.  Returns the number of
// corners made (what building costs).
int Renderer::BuildObjChunk(ObjChunk& c, int lod)
{
    struct Group
    {
        int texture = 0;
        bool surface = false;
        std::vector<TerrainVertex> v;
        std::vector<uint16_t> i;
    };
    std::vector<Group> groups;
    std::map<int, int> open; // texture (and whether on the ground) -> the group taking its triangles now
    auto groupFor = [&](int texture, bool surface, size_t corners) -> Group*
    {
        if (corners > 65535)
            return nullptr; // more than one draw can hold
        const int key = texture * 2 + (surface ? 1 : 0);
        auto it = open.find(key);
        if (it == open.end() || groups[it->second].v.size() + corners > 65535)
        {
            groups.emplace_back();
            groups.back().texture = texture;
            groups.back().surface = surface;
            open[key] = static_cast<int>(groups.size()) - 1;
            return &groups.back();
        }
        return &groups[it->second];
    };

    c.baked.assign(c.objects.size(), 0);
    c.base.assign(c.objects.size(), 0.0f);
    c.builds++;
    c.minY = 1e9f;
    c.maxY = -1e9f;
    const float lift = 0.02f; // what lies on the ground, just clear of it
    for (size_t k = 0; k < c.objects.size(); k++)
    {
        const Object& o = _is->objects[c.objects[k]];
        const ModelMesh& mesh = _game->Mesh(o.model);
        if (!mesh.valid)
            continue;
        const ModelLod& level = (lod == 1 && !mesh.coarse.Empty()) ? mesh.coarse : mesh.fine;
        const float oy = _is->ObjectY(o);
        // where the model's own origin has come to: the ground there is what its heights are measured from
        const float* bc = mesh.centre;
        const float rx = o.m[9] - (o.m[0] * bc[0] + o.m[3] * bc[1] + o.m[6] * bc[2]);
        const float rz = o.m[11] - (o.m[2] * bc[0] + o.m[5] * bc[1] + o.m[8] * bc[2]);
        const float refGround = _is->SurfaceY(rx, rz);
        const bool onLand = !o.fixed;
        float low = 1e9f;
        auto place = [&](const ModelLod::Vertex& s) -> TerrainVertex
        {
            TerrainVertex v;
            v.pos[0] = o.m[9] + o.m[0] * s.pos[0] + o.m[3] * s.pos[1] + o.m[6] * s.pos[2];
            v.pos[1] = oy + o.m[1] * s.pos[0] + o.m[4] * s.pos[1] + o.m[7] * s.pos[2];
            v.pos[2] = o.m[11] + o.m[2] * s.pos[0] + o.m[5] * s.pos[1] + o.m[8] * s.pos[2];
            if (onLand && s.land == 1)
                v.pos[1] = _is->SurfaceY(v.pos[0], v.pos[2]) + lift;
            else if (onLand && s.land == 2)
                v.pos[1] = _is->SurfaceY(v.pos[0], v.pos[2]) + (v.pos[1] - refGround);
            v.normal[0] = o.m[0] * s.normal[0] + o.m[3] * s.normal[1] + o.m[6] * s.normal[2];
            v.normal[1] = o.m[1] * s.normal[0] + o.m[4] * s.normal[1] + o.m[7] * s.normal[2];
            v.normal[2] = o.m[2] * s.normal[0] + o.m[5] * s.normal[1] + o.m[8] * s.normal[2];
            v.uv[0] = s.uv[0];
            v.uv[1] = s.uv[1];
            low = std::min(low, v.pos[1]);
            c.minY = std::min(c.minY, v.pos[1]);
            c.maxY = std::max(c.maxY, v.pos[1]);
            return v;
        };
        for (const ModelLod::Part& part : level.parts)
        {
            const int texture = ModelTexture(part.texture);
            if (mesh.onSurface && onLand)
            {
                // A road lies on the ground.  The ground is flat only within each half of a cell,
                // so its triangles are cut along the cells' edges and diagonals, as the game cuts them.
                struct P
                {
                    float x, z, u, v;
                };
                typedef std::vector<P> Poly;
                // the two sides of the line a x + b z = c
                auto cut = [](const Poly& in, float a, float b, float cc, Poly& below, Poly& above)
                {
                    below.clear();
                    above.clear();
                    for (size_t e = 0; e < in.size(); e++)
                    {
                        const P& p = in[e];
                        const P& q = in[(e + 1) % in.size()];
                        const float dp = a * p.x + b * p.z - cc, dq = a * q.x + b * q.z - cc;
                        if (dp <= 0)
                            below.push_back(p);
                        if (dp >= 0)
                            above.push_back(p);
                        if ((dp < 0 && dq > 0) || (dp > 0 && dq < 0))
                        {
                            const float k = dp / (dp - dq);
                            const P m = {p.x + (q.x - p.x) * k, p.z + (q.z - p.z) * k, p.u + (q.u - p.u) * k, p.v + (q.v - p.v) * k};
                            below.push_back(m);
                            above.push_back(m);
                        }
                    }
                };
                const float cell = _is->cell;
                std::vector<Poly> polys, next;
                Poly lo, hi;
                for (size_t t = 0; t + 2 < part.indices.size(); t += 3)
                {
                    polys.assign(1, Poly());
                    float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
                    for (int e = 0; e < 3; e++)
                    {
                        const TerrainVertex w = place(part.vertices[part.indices[t + e]]);
                        polys[0].push_back({w.pos[0], w.pos[2], w.uv[0], w.uv[1]});
                        x0 = std::min(x0, w.pos[0]), x1 = std::max(x1, w.pos[0]);
                        z0 = std::min(z0, w.pos[2]), z1 = std::max(z1, w.pos[2]);
                    }
                    if (x1 - x0 > 400.0f || z1 - z0 > 400.0f)
                        continue; // not a road piece, whatever it says
                    for (int axis = 0; axis < 2; axis++)
                    {
                        const float from = axis ? z0 : x0, to = axis ? z1 : x1;
                        for (int k = static_cast<int>(std::floor(from / cell)) + 1; k * cell < to; k++)
                        {
                            next.clear();
                            for (const Poly& p : polys)
                            {
                                cut(p, axis ? 0.0f : 1.0f, axis ? 1.0f : 0.0f, k * cell, lo, hi);
                                if (lo.size() >= 3)
                                    next.push_back(lo);
                                if (hi.size() >= 3)
                                    next.push_back(hi);
                            }
                            polys.swap(next);
                        }
                    }
                    for (const Poly& whole : polys)
                    {
                        // the cell it is in, and that cell's diagonal: from (x + 1, z) to (x, z + 1)
                        float mx = 0, mz = 0;
                        for (const P& p : whole)
                        {
                            mx += p.x;
                            mz += p.z;
                        }
                        const float cx = std::floor(mx / whole.size() / cell), cz = std::floor(mz / whole.size() / cell);
                        cut(whole, 1.0f, 1.0f, (cx + cz + 1.0f) * cell, lo, hi);
                        for (const Poly* half : {&lo, &hi})
                        {
                            if (half->size() < 3)
                                continue;
                            Group* g = groupFor(texture, true, half->size());
                            const size_t base = g->v.size();
                            // where the middle of it is, the ground is this half of the cell
                            float hx = 0, hz = 0;
                            for (const P& p : *half)
                            {
                                hx += p.x;
                                hz += p.z;
                            }
                            hx /= half->size();
                            hz /= half->size();
                            const float gy = _is->SurfaceY(hx, hz);
                            const float gx = (_is->SurfaceY(hx + 0.25f, hz) - _is->SurfaceY(hx - 0.25f, hz)) * 2.0f;
                            const float gz = (_is->SurfaceY(hx, hz + 0.25f) - _is->SurfaceY(hx, hz - 0.25f)) * 2.0f;
                            const float nl = 1.0f / std::sqrt(gx * gx + 1.0f + gz * gz);
                            for (const P& p : *half)
                            {
                                TerrainVertex v;
                                v.pos[0] = p.x;
                                v.pos[2] = p.z;
                                // on the plane of that half, wherever the corner itself falls
                                v.pos[1] = gy + gx * (p.x - hx) + gz * (p.z - hz) + lift;
                                v.normal[0] = -gx * nl;
                                v.normal[1] = nl;
                                v.normal[2] = -gz * nl;
                                v.uv[0] = p.u;
                                v.uv[1] = p.v;
                                c.minY = std::min(c.minY, v.pos[1]);
                                c.maxY = std::max(c.maxY, v.pos[1]);
                                g->v.push_back(v);
                            }
                            for (size_t e = 2; e < half->size(); e++)
                            {
                                g->i.push_back(static_cast<uint16_t>(base));
                                g->i.push_back(static_cast<uint16_t>(base + e - 1));
                                g->i.push_back(static_cast<uint16_t>(base + e));
                            }
                        }
                    }
                }
                continue;
            }
            Group* g = groupFor(texture, false, part.vertices.size());
            if (!g)
                continue;
            const size_t base = g->v.size();
            for (const ModelLod::Vertex& s : part.vertices)
                g->v.push_back(place(s));
            for (uint32_t index : part.indices)
                g->i.push_back(static_cast<uint16_t>(base + index));
        }
        c.baked[k] = 1;
        c.base[k] = low;
    }

    size_t corners = 0, indices = 0;
    for (const Group& g : groups)
    {
        corners += g.v.size();
        indices += g.i.size();
    }
    if (c.vb)
        c.vb->Release();
    if (c.ib)
        c.ib->Release();
    c.vb = c.ib = nullptr;
    c.batches.clear();
    c.triangles = static_cast<int>(indices / 3);
    c.built = lod;
    c.dirty = false;
    for (size_t k = 0; k < c.objects.size(); k++)
        if (c.objects[k] < static_cast<int>(_objAsModel.size()))
            _objAsModel[c.objects[k]] = c.baked[k];
    if (!corners || !indices)
        return 0;
    if (FAILED(_dev->CreateVertexBuffer(static_cast<UINT>(corners * sizeof(TerrainVertex)), 0, 0, GXPOOL_MANAGED, &c.vb, nullptr)) ||
        FAILED(_dev->CreateIndexBuffer(static_cast<UINT>(indices * 2), 0, GXFMT_INDEX16, GXPOOL_MANAGED, &c.ib, nullptr)))
    {
        ReleaseObjChunk(c);
        return 0;
    }
    void* pv = nullptr;
    void* pi = nullptr;
    if (FAILED(c.vb->Lock(0, 0, &pv, 0)) || FAILED(c.ib->Lock(0, 0, &pi, 0)))
    {
        ReleaseObjChunk(c);
        return 0;
    }
    size_t atV = 0, atI = 0;
    for (const Group& g : groups)
    {
        if (g.v.empty() || g.i.empty())
            continue;
        std::memcpy(static_cast<TerrainVertex*>(pv) + atV, g.v.data(), g.v.size() * sizeof(TerrainVertex));
        std::memcpy(static_cast<uint16_t*>(pi) + atI, g.i.data(), g.i.size() * 2);
        ModelBatch b;
        b.texture = g.texture;
        b.surface = g.surface;
        b.baseVertex = static_cast<int>(atV);
        b.vertices = static_cast<unsigned>(g.v.size());
        b.startIndex = static_cast<unsigned>(atI);
        b.triangles = static_cast<unsigned>(g.i.size() / 3);
        c.batches.push_back(b);
        atV += g.v.size();
        atI += g.i.size();
    }
    c.vb->Unlock();
    c.ib->Unlock();
    return static_cast<int>(corners);
}

Renderer::AreaInfo Renderer::AreaAt(int index) const
{
    AreaInfo a;
    if (index < 0 || index >= static_cast<int>(_objChunks.size()))
        return a;
    const ObjChunk& c = _objChunks[index];
    a.valid = true;
    a.dirty = c.dirty;
    a.built = c.built;
    a.triangles = c.triangles;
    a.objects = static_cast<int>(c.objects.size());
    a.builds = c.builds;
    a.minY = c.minY;
    a.maxY = c.maxY;
    return a;
}

Renderer::AreaInfo Renderer::Area(float x, float z) const
{
    if (!_is || _objPerSide <= 0)
        return AreaInfo();
    const float size = _is->cell * kObjCells;
    const int cx = std::clamp(static_cast<int>(x / size), 0, _objPerSide - 1), cz = std::clamp(static_cast<int>(z / size), 0, _objPerSide - 1);
    return AreaAt(cz * _objPerSide + cx);
}

bool Renderer::BakedBase(int object, float& y) const
{
    if (!_is || _objPerSide <= 0 || object < 0 || object >= static_cast<int>(_is->objects.size()))
        return false;
    const Object& o = _is->objects[object];
    const float size = _is->cell * kObjCells;
    const int cx = std::clamp(static_cast<int>(o.X() / size), 0, _objPerSide - 1), cz = std::clamp(static_cast<int>(o.Z() / size), 0, _objPerSide - 1);
    const ObjChunk& c = _objChunks[static_cast<size_t>(cz) * _objPerSide + cx];
    if (c.built < 0 || c.dirty || c.base.size() != c.objects.size())
        return false;
    for (size_t k = 0; k < c.objects.size(); k++)
        if (c.objects[k] == object && c.baked[k])
        {
            y = c.base[k];
            return true;
        }
    return false;
}

void Renderer::DrawModels(const Camera& cam, const ViewOptions& view, const float fwd[3], const float vp[16])
{
    typedef std::chrono::steady_clock Clock;
    auto since = [](Clock::time_point from) { return std::chrono::duration<float, std::milli>(Clock::now() - from).count(); };
    {
        // the running totals stay; what is counted per frame starts again
        ModelStats fresh;
        fresh.builds = _modelStats.builds;
        fresh.texturesRead = _modelStats.texturesRead;
        fresh.buildMs = _modelStats.buildMs;
        fresh.textureMs = _modelStats.textureMs;
        _modelStats = fresh;
    }
    const bool wanted = view.objects && view.models && _game && !_game->Folder().empty();
    if (!wanted)
    {
        if (_modelsShown)
            ReleaseModels(false);
        _modelsShown = false;
        return;
    }
    _modelsShown = true;
    if (!_objBinned || _objRevision != _is->objectsRevision || _objPerSide != (_is->size + kObjCells - 1) / kObjCells)
        BinObjects();

    // ---- which areas are wanted, and how: the nearest are built first, a few to the frame
    struct Want
    {
        ObjChunk* chunk;
        float distance;
        int lod;
    };
    static std::vector<Want> wants;
    static std::vector<ObjChunk*> shown;
    wants.clear();
    shown.clear();
    const float size = _is->cell * kObjCells;
    const float range = std::max(size, view.modelRange), keep = range * 1.25f + size;
    const float fine = std::min(view.modelFine, range);
    for (int cz = 0; cz < _objPerSide; cz++)
    {
        const float z = (cz + 0.5f) * size - cam.pos[2];
        if (std::fabs(z) > keep + size)
        {
            // too far north or south, the whole row: only what was built needs a look
            for (int cx = 0; cx < _objPerSide; cx++)
            {
                ObjChunk& c = _objChunks[static_cast<size_t>(cz) * _objPerSide + cx];
                if (c.built >= 0)
                    ReleaseObjChunk(c);
            }
            continue;
        }
        for (int cx = 0; cx < _objPerSide; cx++)
        {
            ObjChunk& c = _objChunks[static_cast<size_t>(cz) * _objPerSide + cx];
            if (c.built < 0 && c.objects.empty())
                continue;
            const float x = (cx + 0.5f) * size - cam.pos[0];
            const float ground = c.built >= 0 && c.maxY >= c.minY ? (c.minY + c.maxY) * 0.5f
                                                                 : _is->SurfaceY((cx + 0.5f) * size, (cz + 0.5f) * size);
            const float y = ground - cam.pos[1];
            const float d = std::sqrt(x * x + y * y + z * z);
            if (d > keep)
            {
                if (c.built >= 0)
                    ReleaseObjChunk(c);
                continue;
            }
            // full detail near; it stays a little longer than it takes to come, so it does not flicker
            const int lod = d < fine || (c.built == 0 && d < fine * 1.25f) ? 0 : 1;
            if (d <= range || c.built >= 0)
            {
                if (c.built != lod || c.dirty)
                    wants.push_back({&c, d, lod});
                if (c.built >= 0)
                    shown.push_back(&c);
            }
        }
    }
    std::sort(wants.begin(), wants.end(), [](const Want& a, const Want& b) { return a.distance < b.distance; });
    // a few milliseconds of building to the frame, the nearest first; always one, so it gets done
    const Clock::time_point buildFrom = Clock::now();
    bool builtOne = false;
    for (const Want& w : wants)
    {
        if (builtOne && since(buildFrom) > 6.0f)
        {
            _modelStats.waiting++;
            continue;
        }
        const bool was = w.chunk->built >= 0;
        BuildObjChunk(*w.chunk, w.lod);
        builtOne = true;
        _modelStats.builds++;
        if (!was)
            shown.push_back(w.chunk);
    }
    if (builtOne)
        _modelStats.buildMs += since(buildFrom);

    // ---- draw
    _dev->SetVertexShader(_vsTerrain);
    _dev->SetPixelShader(_psModel);
    _dev->SetVertexDeclaration(_declTerrain);
    _dev->SetVertexShaderConstantF(0, vp, 4);
    const float sun[4] = {std::sin(view.sunYaw) * std::cos(view.sunHeight), std::sin(view.sunHeight),
                          std::cos(view.sunYaw) * std::cos(view.sunHeight), 0.5f};
    _dev->SetPixelShaderConstantF(0, sun, 1);
    _dev->SetSamplerState(0, GXSAMP_ADDRESSU, GXTADDRESS_WRAP);
    _dev->SetSamplerState(0, GXSAMP_ADDRESSV, GXTADDRESS_WRAP);
    // textures are read as they are first drawn with: a few milliseconds' worth to the frame
    const Clock::time_point readFrom = Clock::now();
    bool readOne = false;
    auto dword = [](float f)
    {
        DWORD d;
        std::memcpy(&d, &f, 4);
        return d;
    };
    for (int pass = 0; pass < 2; pass++)
    for (ObjChunk* c : shown)
    {
        if (!c->vb || !c->ib || c->batches.empty())
            continue;
        if (pass == 0)
            _modelStats.areas++;
        bool bound = false;
        for (const ModelBatch& b : c->batches)
        {
            if (b.surface != (pass == 1))
                continue;
            if (!bound)
            {
                if (pass == 1)
                {
                    // roads lie in the ground's own planes: a nudge towards the eye keeps them on top
                    _dev->SetRenderState(GXRS_DEPTHBIAS, dword(-0.00001f));
                    _dev->SetRenderState(GXRS_SLOPESCALEDEPTHBIAS, dword(-1.0f));
                }
                _dev->SetStreamSource(0, c->vb, 0, sizeof(TerrainVertex));
                _dev->SetIndices(c->ib);
                bound = true;
            }
            ModelTex& t = _modelTex[b.texture];
            if (!t.tried && (!readOne || since(readFrom) < 4.0f))
            {
                LoadModelTexture(t);
                readOne = true;
                _modelStats.texturesRead++;
            }
            if (!t.tried)
                _modelStats.waiting++;
            _dev->SetTexture(0, t.tex ? t.tex : _grey);
            _dev->DrawIndexedPrimitive(GXPT_TRIANGLELIST, b.baseVertex, 0, b.vertices, b.startIndex, b.triangles);
            _modelStats.draws++;
            _modelStats.triangles += static_cast<int>(b.triangles);
        }
    }
    _dev->SetRenderState(GXRS_DEPTHBIAS, dword(0.0f));
    _dev->SetRenderState(GXRS_SLOPESCALEDEPTHBIAS, dword(0.0f));
    if (readOne)
        _modelStats.textureMs += since(readFrom);
    for (const ModelTex& t : _modelTex)
        _modelStats.textures += t.tex ? 1 : 0;
    (void)fwd;
}

// ---------------------------------------------------------------------------
// drawing
// ---------------------------------------------------------------------------

void Renderer::Draw(const Camera& cam, const ViewOptions& view, const BrushView& brush, int width, int height,
                    int selectedObject, const std::vector<OverlayLine>* overlay)
{
    _chunksDrawn = 0;
    if (!_is || _is->size <= 0 || width <= 0 || height <= 0)
        return;
    float vp[16];
    cam.ViewProj(static_cast<float>(width) / height, vp);
    float fwd[3], right[3], up[3];
    cam.Basis(fwd, right, up);

    _dev->SetRenderState(GXRS_ZENABLE, GXZB_TRUE);
    _dev->SetRenderState(GXRS_ZWRITEENABLE, TRUE);
    _dev->SetRenderState(GXRS_ZFUNC, GXCMP_LESSEQUAL);
    _dev->SetRenderState(GXRS_CULLMODE, GXCULL_NONE);
    _dev->SetRenderState(GXRS_ALPHABLENDENABLE, FALSE);
    _dev->SetRenderState(GXRS_STENCILENABLE, FALSE);
    _dev->SetRenderState(GXRS_COLORWRITEENABLE, 0xF);

    // ---- the ground
    _dev->SetVertexShader(_vsTerrain);
    _dev->SetPixelShader(_psTerrain);
    _dev->SetVertexDeclaration(_declTerrain);
    _dev->SetVertexShaderConstantF(0, vp, 4);
    const float sun[4] = {std::sin(view.sunYaw) * std::cos(view.sunHeight), std::sin(view.sunHeight),
                          std::cos(view.sunYaw) * std::cos(view.sunHeight), 0.42f};
    const float mode[4] = {static_cast<float>(view.mode), view.grid ? 1.0f : 0.0f, view.contour, _is->cell};
    const float br[4] = {brush.x, brush.z, brush.radius, brush.shown ? 1.0f : 0.0f};
    const float range[4] = {_is->MinHeight(), _is->MaxHeight(), 0, 0};
    const float tint[4] = {brush.r, brush.g, brush.b, 0.85f};
    _dev->SetPixelShaderConstantF(0, sun, 1);
    _dev->SetPixelShaderConstantF(1, mode, 1);
    _dev->SetPixelShaderConstantF(2, br, 1);
    _dev->SetPixelShaderConstantF(3, range, 1);
    _dev->SetPixelShaderConstantF(4, tint, 1);
    _dev->SetTexture(0, _atlas);
    _dev->SetSamplerState(0, GXSAMP_MINFILTER, GXTEXF_LINEAR);
    _dev->SetSamplerState(0, GXSAMP_MAGFILTER, GXTEXF_LINEAR);
    _dev->SetSamplerState(0, GXSAMP_MIPFILTER, GXTEXF_LINEAR);
    _dev->SetSamplerState(0, GXSAMP_ADDRESSU, GXTADDRESS_CLAMP);
    _dev->SetSamplerState(0, GXSAMP_ADDRESSV, GXTADDRESS_CLAMP);
    _dev->SetIndices(_ib);
    const float chunkSize = _chunkCells * _is->cell;
    for (Chunk& c : _chunks)
    {
        // behind the camera or out of sight: not drawn (and not rebuilt until it is seen)
        const float centre[3] = {(c.cx + 0.5f) * chunkSize - cam.pos[0], (c.minY + c.maxY) * 0.5f - cam.pos[1],
                                 (c.cz + 0.5f) * chunkSize - cam.pos[2]};
        const float radius = chunkSize * 0.75f + (c.maxY - c.minY) * 0.5f;
        if (!c.dirty && Dot(centre, fwd) < -radius)
            continue;
        if (c.dirty)
            BuildChunk(c);
        if (!c.vb || !_ib)
            continue;
        _dev->SetStreamSource(0, c.vb, 0, sizeof(TerrainVertex));
        _dev->DrawIndexedPrimitive(GXPT_TRIANGLELIST, 0, 0, c.cells * c.cells * 4, 0, c.cells * c.cells * 2);
        _chunksDrawn++;
    }

    // ---- what stands on it
    DrawModels(cam, view, fwd, vp);

    // ---- lines and flat shapes
    _dev->SetVertexShader(_vsLine);
    _dev->SetPixelShader(_psLine);
    _dev->SetVertexDeclaration(_declLine);
    _dev->SetVertexShaderConstantF(0, vp, 4);
    static std::vector<LineVertex> lines;
    auto flush = [&](GxPrimitiveType type, int perPrim)
    {
        const size_t maxVerts = 60000;
        for (size_t i = 0; i < lines.size(); i += maxVerts)
        {
            const size_t n = std::min(maxVerts, lines.size() - i);
            if (n >= static_cast<size_t>(perPrim))
                _dev->DrawPrimitiveUP(type, static_cast<UINT>(n / perPrim), lines.data() + i, sizeof(LineVertex));
        }
        lines.clear();
    };
    auto add = [&](float x0, float y0, float z0, float x1, float y1, float z1, uint32_t color)
    {
        lines.push_back({{x0, y0, z0}, color});
        lines.push_back({{x1, y1, z1}, color});
    };

    if (view.objects)
    {
        const float range2 = view.objectRange * view.objectRange;
        for (size_t i = 0; i < _is->objects.size(); i++)
        {
            const Object& o = _is->objects[i];
            const float dx = o.X() - cam.pos[0], dz = o.Z() - cam.pos[2];
            if (dx * dx + dz * dz > range2 || dx * fwd[0] + dz * fwd[2] < -100.0f)
                continue;
            // its model is there to see: no marker, unless it is the one in hand
            if (i < _objAsModel.size() && _objAsModel[i] && static_cast<int>(i) != selectedObject)
                continue;
            const float y = _is->ObjectY(o);
            uint32_t color = 0xFFD0D0D0;
            float tall = 4.0f, foot = 2.0f;
            switch (KindOf(o.model))
            {
                case ObjectKind::Building:
                    color = 0xFFF0A030;
                    tall = 7.0f;
                    foot = 5.0f;
                    break;
                case ObjectKind::Tree:
                    color = 0xFF30B040;
                    tall = 9.0f;
                    if (IsForestBlock(o.model))
                    {
                        // a block of forest: the cell it fills (two half blocks to the cell: one square for both)
                        const std::string name = LowerPath(o.model);
                        const bool corner = name.find("troj") != std::string::npos;
                        if (name.find("_t2.") == std::string::npos)
                        {
                            const float x0 = std::floor(o.X() / _is->cell) * _is->cell, z0 = std::floor(o.Z() / _is->cell) * _is->cell;
                            const float in = corner ? 4.0f : 0.5f; // a corner block's square a little smaller, to tell them apart
                            const float cx[4] = {x0 + in, x0 + _is->cell - in, x0 + _is->cell - in, x0 + in};
                            const float cz[4] = {z0 + in, z0 + in, z0 + _is->cell - in, z0 + _is->cell - in};
                            const uint32_t edge = static_cast<int>(i) == selectedObject ? 0xFFFFFFFF : corner ? 0xFF80D050 : 0xFF209030;
                            for (int k = 0; k < 4; k++)
                            {
                                const int n = (k + 1) & 3;
                                add(cx[k], std::max(0.0f, _is->SurfaceY(cx[k], cz[k])) + 0.5f, cz[k], cx[n],
                                    std::max(0.0f, _is->SurfaceY(cx[n], cz[n])) + 0.5f, cz[n], edge);
                            }
                        }
                        tall = 22.0f;
                    }
                    break;
                case ObjectKind::Bush:
                    color = 0xFF90D060;
                    tall = 2.0f;
                    foot = 1.0f;
                    break;
                case ObjectKind::Road:
                {
                    color = 0xFFB0B0B8;
                    tall = 0.4f;
                    foot = 6.0f;
                    // a road piece or a bridge section: its roadway, as the model has it
                    const ModelShape* shape = _game ? &_game->Model(o.model) : nullptr;
                    if (shape && shape->road)
                    {
                        const float* pts[4] = {shape->lb, shape->pb, shape->pe, shape->le};
                        float c[4][3];
                        for (int k = 0; k < 4; k++)
                        {
                            c[k][0] = o.m[9] + o.m[0] * pts[k][0] + o.m[6] * pts[k][2];
                            c[k][2] = o.m[11] + o.m[2] * pts[k][0] + o.m[8] * pts[k][2];
                            c[k][1] = o.fixed ? y + pts[k][1] : std::max(0.0f, _is->SurfaceY(c[k][0], c[k][2])) + 0.3f;
                        }
                        const uint32_t edge = o.fixed ? 0xFFF0C060 : static_cast<int>(i) == selectedObject ? 0xFFFFFFFF : 0xFFC8C8D0;
                        for (int k = 0; k < 4; k++)
                        {
                            const float* a = c[k];
                            const float* b = c[(k + 1) & 3];
                            // a road lies on the ground: its long edges follow it
                            const float len = std::sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[2] - a[2]) * (b[2] - a[2]));
                            const int parts = o.fixed ? 1 : std::clamp(static_cast<int>(len / 6.0f), 1, 8);
                            float px = a[0], py = a[1], pz = a[2];
                            for (int s = 1; s <= parts; s++)
                            {
                                const float t = static_cast<float>(s) / parts;
                                const float qx = a[0] + (b[0] - a[0]) * t, qz = a[2] + (b[2] - a[2]) * t;
                                const float qy = s == parts ? b[1] : std::max(0.0f, _is->SurfaceY(qx, qz)) + 0.3f;
                                add(px, py, pz, qx, qy, qz, edge);
                                px = qx;
                                py = qy;
                                pz = qz;
                            }
                        }
                        if (o.fixed)
                        {
                            // what it stands on: a leg from each end of the deck to the ground
                            for (int k = 0; k < 4; k += 2)
                            {
                                const float mx = (c[k][0] + c[k + 1][0]) * 0.5f, mz = (c[k][2] + c[k + 1][2]) * 0.5f;
                                add(mx, c[k][1], mz, mx, std::min(c[k][1], _is->SurfaceY(mx, mz)), mz, edge);
                            }
                        }
                        continue;
                    }
                    break;
                }
                default:
                    break;
            }
            add(o.X(), y, o.Z(), o.X(), y + tall, o.Z(), color);
            // a foot along the way it faces, and across
            const float fx = o.m[6] * foot, fz = o.m[8] * foot, sx = o.m[0] * foot * 0.5f, sz = o.m[2] * foot * 0.5f;
            add(o.X() - fx, y + 0.3f, o.Z() - fz, o.X() + fx, y + 0.3f, o.Z() + fz, color);
            add(o.X() - sx, y + 0.3f, o.Z() - sz, o.X() + sx, y + 0.3f, o.Z() + sz, color);
        }
        flush(GXPT_LINELIST, 2);
    }

    if (view.sea)
    {
        _dev->SetRenderState(GXRS_ALPHABLENDENABLE, TRUE);
        _dev->SetRenderState(GXRS_SRCBLEND, GXBLEND_SRCALPHA);
        _dev->SetRenderState(GXRS_DESTBLEND, GXBLEND_INVSRCALPHA);
        _dev->SetRenderState(GXRS_ZWRITEENABLE, FALSE);
        const float a = -30000.0f, b = _is->WorldSize() + 30000.0f;
        const uint32_t sea = 0x8C1E5078;
        lines.push_back({{a, 0, a}, sea});
        lines.push_back({{b, 0, a}, sea});
        lines.push_back({{a, 0, b}, sea});
        lines.push_back({{b, 0, a}, sea});
        lines.push_back({{a, 0, b}, sea});
        lines.push_back({{b, 0, b}, sea});
        flush(GXPT_TRIANGLELIST, 3);
        _dev->SetRenderState(GXRS_ALPHABLENDENABLE, FALSE);
        _dev->SetRenderState(GXRS_ZWRITEENABLE, TRUE);
    }

    // ---- on top of everything: the island's edge, the ramp line, the selection
    _dev->SetRenderState(GXRS_ZENABLE, GXZB_FALSE);
    {
        const float s = _is->WorldSize();
        const uint32_t edge = 0xFFFFE060;
        const int steps = 64;
        for (int i = 0; i < steps; i++)
        {
            const float t0 = s * i / steps, t1 = s * (i + 1) / steps;
            auto y = [&](float x, float z) { return std::max(0.0f, _is->SurfaceY(x, z)) + 1.0f; };
            add(t0, y(t0, 0), 0, t1, y(t1, 0), 0, edge);
            add(t0, y(t0, s), s, t1, y(t1, s), s, edge);
            add(0, y(0, t0), t0, 0, y(0, t1), t1, edge);
            add(s, y(s, t0), t0, s, y(s, t1), t1, edge);
        }
    }
    if (brush.line)
    {
        const int steps = 48;
        for (int i = 0; i < steps; i++)
        {
            const float t0 = static_cast<float>(i) / steps, t1 = static_cast<float>(i + 1) / steps;
            const float ax = brush.lx0 + (brush.lx1 - brush.lx0) * t0, az = brush.lz0 + (brush.lz1 - brush.lz0) * t0;
            const float bx = brush.lx0 + (brush.lx1 - brush.lx0) * t1, bz = brush.lz0 + (brush.lz1 - brush.lz0) * t1;
            add(ax, _is->SurfaceY(ax, az) + 0.5f, az, bx, _is->SurfaceY(bx, bz) + 0.5f, bz, 0xFFFFFFFF);
        }
    }
    if (selectedObject >= 0 && selectedObject < static_cast<int>(_is->objects.size()))
    {
        const Object& o = _is->objects[selectedObject];
        const float y = _is->ObjectY(o), r = 6.0f, h = 12.0f;
        const uint32_t sel = 0xFFFFFF40;
        const ModelShape* shape = _game ? &_game->Model(o.model) : nullptr;
        if (shape && shape->valid && shape->bmax[0] > shape->bmin[0])
        {
            // the box of its model, turned as it is
            float c[8][3];
            for (int k = 0; k < 8; k++)
            {
                const float lx = (k & 1) ? shape->bmax[0] : shape->bmin[0], ly = (k & 2) ? shape->bmax[1] : shape->bmin[1];
                const float lz = (k & 4) ? shape->bmax[2] : shape->bmin[2];
                c[k][0] = o.m[9] + o.m[0] * lx + o.m[3] * ly + o.m[6] * lz;
                c[k][1] = y + o.m[1] * lx + o.m[4] * ly + o.m[7] * lz;
                c[k][2] = o.m[11] + o.m[2] * lx + o.m[5] * ly + o.m[8] * lz;
            }
            for (int k = 0; k < 8; k++)
                for (int bit = 1; bit < 8; bit <<= 1)
                    if (!(k & bit))
                        add(c[k][0], c[k][1], c[k][2], c[k | bit][0], c[k | bit][1], c[k | bit][2], sel);
        }
        else
        {
            const float cx[4] = {-r, r, r, -r}, cz[4] = {-r, -r, r, r};
            for (int k = 0; k < 4; k++)
            {
                const int n = (k + 1) & 3;
                const float x0 = o.X() + cx[k], z0 = o.Z() + cz[k], x1 = o.X() + cx[n], z1 = o.Z() + cz[n];
                add(x0, y, z0, x1, y, z1, sel);
                add(x0, y + h, z0, x1, y + h, z1, sel);
                add(x0, y, z0, x0, y + h, z0, sel);
            }
        }
        add(o.X(), y + 1.0f, o.Z(), o.X() + o.m[6] * 15.0f, y + 1.0f, o.Z() + o.m[8] * 15.0f, 0xFFFF4040);
    }
    if (overlay)
        for (const OverlayLine& l : *overlay)
            add(l.a[0], l.a[1], l.a[2], l.b[0], l.b[1], l.b[2], l.color);
    flush(GXPT_LINELIST, 2);
    _dev->SetRenderState(GXRS_ZENABLE, GXZB_TRUE);
}
} // namespace island
