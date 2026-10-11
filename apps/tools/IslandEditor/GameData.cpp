#include "GameData.hpp"

#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>

#include <Poseidon/Asset/Formats/P3D/MLODLoader.hpp>
#include <Poseidon/Asset/Formats/P3D/ODOLLoader.hpp>
#include <Poseidon/Asset/Probes/AssetInfo.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/Graphics/Textures/DDSConverter.hpp>
#include <Poseidon/Graphics/Textures/Image.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <Poseidon/World/Terrain/WrpReader.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace island
{
namespace fs = std::filesystem;

namespace
{
bool EndsWith(const std::string& s, const char* ext)
{
    const size_t n = std::strlen(ext);
    return s.size() >= n && s.compare(s.size() - n, n, ext) == 0;
}
} // namespace

bool GameData::Mount(const std::string& folder)
{
    using namespace Poseidon;
    _folder = folder;
    _archives = 0;
    _textures.clear();
    _models.clear();
    GFileBanks.Clear();
    GUseFileBanks = false;

    std::error_code ec;
    for (const char* sub : {"DTA", "AddOns", "Res\\DTA", "Res\\AddOns"})
    {
        const fs::path dir = fs::path(folder) / sub;
        if (!fs::is_directory(dir, ec))
            continue;
        for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
        {
            if (!e.is_regular_file(ec) || LowerPath(e.path().extension().string()) != ".pbo")
                continue;
            const std::string stem = LowerPath(e.path().stem().string());
            const std::string parent = e.path().parent_path().string() + "\\";
            GFileBanks.Load(RString(parent.c_str()), RString(""), RString(stem.c_str()), true);
            _archives++;
            try
            {
                const PboInfo info = InspectPbo(e.path().string());
                if (!info.valid)
                    continue;
                for (const PboFileEntry& entry : info.entries)
                {
                    const std::string name = LowerPath(stem + "\\" + entry.name);
                    if (EndsWith(name, ".p3d"))
                        _models.push_back(name);
                    else if (EndsWith(name, ".paa") || EndsWith(name, ".pac"))
                        _textures.push_back(name);
                }
            }
            catch (...)
            {
            }
        }
    }
    GUseFileBanks = _archives > 0;
    auto tidy = [](std::vector<std::string>& v)
    {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    };
    tidy(_textures);
    tidy(_models);

    // texture sets: folders with two-letter textures
    _sets.clear();
    for (const std::string& name : _textures)
    {
        const size_t slash = name.find_last_of('\\'), dot = name.find_last_of('.');
        if (slash == std::string::npos || dot == std::string::npos || dot < slash)
            continue;
        const std::string folder = name.substr(0, slash + 1), stem = name.substr(slash + 1, dot - slash - 1);
        if (stem.size() != 2 && stem.size() != 8)
            continue;
        TextureSet* set = nullptr;
        for (TextureSet& s : _sets)
            if (s.folder == folder)
                set = &s;
        if (!set)
        {
            _sets.emplace_back();
            set = &_sets.back();
            set->folder = folder;
        }
        if (stem.size() == 2)
        {
            bool have = false;
            for (const TextureSet::Plain& p : set->plains)
                have = have || p.code == stem;
            if (!have)
            {
                TextureSet::Plain p;
                p.code = stem;
                set->plains.push_back(p);
            }
        }
        else
        {
            set->transitions++;
        }
    }
    // a set is a few plain textures and the transitions between them
    _sets.erase(std::remove_if(_sets.begin(), _sets.end(),
                               [](const TextureSet& s) { return s.plains.size() < 3 || s.transitions < 8; }),
                _sets.end());
    return _archives > 0;
}

int GameData::SetOf(const Island& is) const
{
    if (_sets.empty() || is.size <= 0)
        return -1;
    std::vector<int> perSlot(kMaxTextures, -1);
    for (int i = 0; i < kMaxTextures && i < static_cast<int>(is.textures.size()); i++)
    {
        const std::string n = LowerPath(is.textures[i]);
        for (size_t s = 0; s < _sets.size(); s++)
            if (n.rfind(_sets[s].folder, 0) == 0)
                perSlot[i] = static_cast<int>(s);
    }
    std::vector<int> count(_sets.size(), 0);
    for (uint16_t t : is.tex)
        if (t < kMaxTextures && perSlot[t] >= 0)
            count[perSlot[t]]++;
    int best = -1, bestCount = 0;
    for (size_t s = 0; s < count.size(); s++)
        if (count[s] > bestCount)
        {
            bestCount = count[s];
            best = static_cast<int>(s);
        }
    return best;
}

void GameData::LoadSetColours(TextureSet& set) const
{
    if (set.coloursKnown)
        return;
    set.coloursKnown = true;
    for (TextureSet::Plain& p : set.plains)
    {
        Image img = LoadTexture(set.folder + p.code + ".paa");
        if (!img.Valid())
            img = LoadTexture(set.folder + p.code + ".pac");
        if (!img.Valid())
            continue;
        double sum[3] = {0, 0, 0};
        const size_t n = static_cast<size_t>(img.width) * img.height;
        for (size_t i = 0; i < n; i++)
            for (int c = 0; c < 3; c++)
                sum[c] += img.rgba[i * 4 + c];
        for (int c = 0; c < 3; c++)
            p.rgb[c] = static_cast<float>(sum[c] / n / 255.0);
    }
}

bool GameData::LearnRoles(const Island& is, const TextureSet& set, AutoTextureRules& rules, std::string* report)
{
    struct Use
    {
        std::string code;
        int count = 0, forest = 0;
        double height = 0, slope = 0;
    };
    std::vector<Use> uses(set.plains.size());
    for (size_t i = 0; i < set.plains.size(); i++)
        uses[i].code = set.plains[i].code;
    // which slot is which plain texture of the set
    std::vector<int> slotUse(kMaxTextures, -1);
    for (int s = 1; s < kMaxTextures && s < static_cast<int>(is.textures.size()); s++)
    {
        const std::string n = LowerPath(is.textures[s]);
        if (n.rfind(set.folder, 0) != 0)
            continue;
        const size_t dot = n.find_last_of('.');
        const std::string stem = n.substr(set.folder.size(), dot == std::string::npos ? std::string::npos : dot - set.folder.size());
        for (size_t i = 0; i < uses.size(); i++)
            if (uses[i].code == stem)
                slotUse[s] = static_cast<int>(i);
    }
    std::vector<uint8_t> forestCell(static_cast<size_t>(is.size) * is.size, 0);
    for (const Object& o : is.objects)
    {
        const std::string m = LowerPath(o.model);
        const size_t slash = m.find_last_of('\\');
        if (m.compare(slash == std::string::npos ? 0 : slash + 1, 3, "les") != 0)
            continue;
        const int cx = static_cast<int>(o.X() / is.cell), cz = static_cast<int>(o.Z() / is.cell);
        if (is.InRange(cx, cz))
            forestCell[static_cast<size_t>(cz) * is.size + cx] = 1;
    }
    int total = 0;
    for (int z = 0; z < is.size; z++)
        for (int x = 0; x < is.size; x++)
        {
            const int u = slotUse[is.tex[static_cast<size_t>(z) * is.size + x]];
            if (u < 0)
                continue;
            float n[3];
            is.Normal(x, z, n);
            uses[u].count++;
            uses[u].forest += forestCell[static_cast<size_t>(z) * is.size + x];
            uses[u].height += (is.H(x, z) + is.H(x + 1, z) + is.H(x, z + 1) + is.H(x + 1, z + 1)) * 0.25;
            uses[u].slope += std::acos(std::min(1.0f, std::max(-1.0f, n[1]))) * 57.29578;
            total++;
        }
    if (report)
    {
        for (const Use& u : uses)
            if (u.count)
            {
                char line[160];
                snprintf(line, sizeof(line), "  %s%s: %d cells, mean height %.0f m, mean slope %.1f deg, %.0f%% under forest\n",
                         set.folder.c_str(), u.code.c_str(), u.count, u.height / u.count, u.slope / u.count, 100.0 * u.forest / u.count);
                *report += line;
            }
    }
    // enough of the set in use to say anything?
    std::vector<Use*> live;
    for (Use& u : uses)
        if (u.count >= std::max(8, total / 400))
            live.push_back(&u);
    if (total < 200 || live.size() < 3)
        return false;
    for (Use* u : live)
    {
        u->height /= u->count;
        u->slope /= u->count;
    }
    auto take = [&](auto better, auto allowed) -> std::string
    {
        Use* best = nullptr;
        for (Use* u : live)
            if (allowed(*u) && (!best || better(*u, *best)))
                best = u;
        if (!best)
            return std::string();
        live.erase(std::find(live.begin(), live.end(), best));
        return best->code;
    };
    auto any = [](const Use&) { return true; };
    rules.folder = set.folder;
    rules.beach = rules.shore = rules.grass = rules.grassAlt = rules.hill = rules.rock = rules.peak = rules.forest = std::string();
    rules.forest = take([](const Use& a, const Use& b) { return a.forest * b.count > b.forest * a.count; },
                        [](const Use& u) { return u.forest * 4 > u.count; });
    rules.beach = take([](const Use& a, const Use& b) { return a.height < b.height; }, [](const Use& u) { return u.height < 12.0; });
    rules.shore = take([](const Use& a, const Use& b) { return a.height < b.height; }, [](const Use& u) { return u.height < 20.0; });
    rules.grass = take([](const Use& a, const Use& b) { return a.count > b.count; }, any);
    rules.rock = take([](const Use& a, const Use& b) { return a.slope > b.slope; }, any);
    rules.grassAlt = take([](const Use& a, const Use& b) { return a.count > b.count; }, any);
    rules.peak = take([](const Use& a, const Use& b) { return a.height > b.height; }, any);
    rules.hill = take([](const Use& a, const Use& b) { return a.height > b.height; }, any);
    if (rules.grassAlt.empty())
        rules.grassAlt = rules.grass;
    if (rules.hill.empty())
        rules.hill = rules.grassAlt;
    if (rules.rock.empty())
        rules.rock = rules.hill;
    if (rules.peak.empty())
        rules.peak = rules.rock;
    return !rules.grass.empty();
}

int GameData::RolesFor(const Island& is, TextureSet& set, AutoTextureRules& rules)
{
    if (LearnRoles(is, set, rules))
        return 0;
    if (set.stockState == 0)
    {
        set.stockState = 2;
        std::string name = set.folder;
        while (!name.empty() && name.back() == '\\')
            name.pop_back();
        const fs::path file = fs::path(_folder) / "Worlds" / (name + ".wrp");
        std::error_code ec;
        Island stock;
        std::string error;
        if (!_folder.empty() && fs::exists(file, ec) && LoadIsland(file.string(), stock, error) &&
            LearnRoles(stock, set, set.stock))
            set.stockState = 1;
    }
    auto roles = [](AutoTextureRules& to, const AutoTextureRules& from)
    {
        to.folder = from.folder;
        to.beach = from.beach;
        to.shore = from.shore;
        to.grass = from.grass;
        to.grassAlt = from.grassAlt;
        to.hill = from.hill;
        to.rock = from.rock;
        to.peak = from.peak;
        to.forest = from.forest;
    };
    if (set.stockState == 1)
    {
        roles(rules, set.stock);
        return 1;
    }
    LoadSetColours(set);
    GuessRoles(set, rules);
    return 2;
}

void GameData::GuessRoles(const TextureSet& set, AutoTextureRules& rules)
{
    rules.folder = set.folder;
    rules.beach = rules.shore = rules.grass = rules.grassAlt = rules.hill = rules.rock = rules.peak = rules.forest = std::string();
    if (set.plains.empty())
        return;
    struct Look
    {
        const TextureSet::Plain* p;
        float lum, sat, green, warm;
    };
    std::vector<Look> all;
    for (const TextureSet::Plain& p : set.plains)
    {
        Look l;
        l.p = &p;
        l.lum = p.rgb[0] * 0.3f + p.rgb[1] * 0.59f + p.rgb[2] * 0.11f;
        l.sat = std::max(p.rgb[0], std::max(p.rgb[1], p.rgb[2])) - std::min(p.rgb[0], std::min(p.rgb[1], p.rgb[2]));
        l.green = (p.rgb[1] - p.rgb[2]) + (p.rgb[1] - p.rgb[0]) * 2.0f; // olive counts, yellow does not
        l.warm = p.rgb[0] - p.rgb[2];
        all.push_back(l);
    }
    std::vector<const TextureSet::Plain*> used;
    auto pick = [&](auto score, bool fresh) -> std::string
    {
        const Look* best = nullptr;
        float bestScore = -1e9f;
        for (const Look& l : all)
        {
            if (fresh && std::find(used.begin(), used.end(), l.p) != used.end())
                continue;
            const float s = score(l);
            if (s > bestScore)
            {
                bestScore = s;
                best = &l;
            }
        }
        if (!best)
            return std::string();
        used.push_back(best->p);
        return best->p->code;
    };
    rules.grass = pick([](const Look& l) { return l.green * 3.0f + l.lum * 0.4f; }, true);
    rules.beach = pick([](const Look& l) { return l.lum * 1.2f + l.warm * 1.5f - std::fabs(l.green) * 1.5f; }, true);
    rules.rock = pick([](const Look& l) { return -l.sat * 5.0f - std::fabs(l.lum - 0.42f); }, true);
    rules.forest = pick([](const Look& l) { return l.green * 2.0f - l.lum * 2.0f; }, true);
    rules.grassAlt = pick([](const Look& l) { return l.green * 3.0f + l.lum * 0.4f; }, true);
    rules.hill = pick([](const Look& l) { return l.warm * 1.5f - std::fabs(l.lum - 0.36f) * 2.0f - std::fabs(l.green); }, true);
    rules.peak = pick([](const Look& l) { return -l.sat * 4.0f + l.lum * 0.8f; }, true);
    if (rules.grassAlt.empty())
        rules.grassAlt = rules.grass;
    if (rules.hill.empty())
        rules.hill = rules.grassAlt;
    if (rules.peak.empty())
        rules.peak = rules.rock;
    if (rules.forest.empty())
        rules.forest = rules.grass;
}

void GameData::AddLooseFolder(const std::string& folder)
{
    if (!folder.empty() && std::find(_loose.begin(), _loose.end(), folder) == _loose.end())
        _loose.push_back(folder);
}

bool GameData::ReadFile(const std::string& name, std::vector<char>& bytes) const
{
    using namespace Poseidon;
    bytes.clear();
    std::string n = LowerPath(name);
    while (!n.empty() && n[0] == '\\')
        n.erase(0, 1);
    if (n.empty())
        return false;
    auto loose = [&](const fs::path& file)
    {
        std::ifstream f(file, std::ios::binary | std::ios::ate);
        if (!f)
            return false;
        bytes.resize(static_cast<size_t>(f.tellg()));
        f.seekg(0);
        f.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return !bytes.empty();
    };
    for (const std::string& dir : _loose)
        if (loose(fs::path(dir) / n))
            return true;
    if (GUseFileBanks)
    {
        QIFStreamB f;
        f.AutoOpen(n.c_str());
        if (!f.fail() && f.rest() > 0)
        {
            bytes.resize(static_cast<size_t>(f.rest()));
            f.read(bytes.data(), static_cast<int>(bytes.size()));
            return true;
        }
    }
    return !_folder.empty() && loose(fs::path(_folder) / n);
}

Image GameData::LoadTexture(const std::string& name) const
{
    using namespace Poseidon;
    Image out;
    std::vector<char> bytes;
    if (!ReadFile(name, bytes))
        return out;
    try
    {
        if (bytes.size() > 4 && std::memcmp(bytes.data(), "DDS ", 4) == 0)
        {
            // addons of the newer kind bring their textures as DDS
            const DDSFile dds = DDSConverter::ReadDDSBuffer(bytes.data(), bytes.size());
            if (dds.valid() && !dds.mipmaps[0].data.empty())
            {
                out.width = dds.mipmaps[0].width;
                out.height = dds.mipmaps[0].height;
                out.rgba.resize(static_cast<size_t>(out.width) * out.height * 4);
                if (!ConvertPixels(dds.mipmaps[0].data.data(), out.rgba.data(), out.width, out.height, dds.format, PixelFormat::RGBA8888))
                    out = Image();
            }
            return out;
        }
        DecodedImage img = DecodePAABuffer(bytes.data(), bytes.size(), EndsWith(LowerPath(name), ".paa"));
        if (img.valid())
        {
            out.width = img.width;
            out.height = img.height;
            out.rgba = std::move(img.rgba);
        }
    }
    catch (...)
    {
    }
    return out;
}

namespace
{
// the triangles of one level of detail, sorted by texture
void FillLod(const Poseidon::Model::Mesh& mesh, ModelLod& out)
{
    out = ModelLod();
    std::map<std::string, int> partOf;
    std::vector<std::vector<int>> remap; // per part: the mesh's vertex -> the part's
    auto corner = [&](int part, uint32_t index) -> uint32_t
    {
        int& at = remap[part][index];
        if (at < 0)
        {
            const Poseidon::Model::Vertex& v = mesh.vertices[index];
            ModelLod::Vertex o;
            o.pos[0] = v.position.x, o.pos[1] = v.position.y, o.pos[2] = v.position.z;
            o.normal[0] = v.normal.x, o.normal[1] = v.normal.y, o.normal[2] = v.normal.z;
            o.uv[0] = v.uv.u, o.uv[1] = v.uv.v;
            const uint32_t flags = static_cast<uint32_t>(v.flags);
            o.land = (flags & 0x800) ? 2 : (flags & 0xF00) == 0x100 ? 1 : 0;
            at = static_cast<int>(out.parts[part].vertices.size());
            out.parts[part].vertices.push_back(o);
        }
        return static_cast<uint32_t>(at);
    };
    auto face = [&](uint32_t material, const uint32_t* index, int corners)
    {
        for (int c = 0; c < corners; c++)
            if (index[c] >= mesh.vertices.size())
                return;
        std::string texture = material < mesh.materials.size() ? LowerPath(mesh.materials[material].texturePath) : std::string();
        while (!texture.empty() && texture[0] == '\\')
            texture.erase(0, 1);
        auto it = partOf.find(texture);
        if (it == partOf.end())
        {
            it = partOf.emplace(texture, static_cast<int>(out.parts.size())).first;
            out.parts.emplace_back();
            out.parts.back().texture = texture;
            remap.emplace_back(mesh.vertices.size(), -1);
        }
        const int part = it->second;
        for (int c = 2; c < corners; c++)
        {
            out.parts[part].indices.push_back(corner(part, index[0]));
            out.parts[part].indices.push_back(corner(part, index[c - 1]));
            out.parts[part].indices.push_back(corner(part, index[c]));
            out.triangles++;
        }
    };
    for (const Poseidon::Model::Triangle& f : mesh.triangles)
        face(f.materialIndex, f.indices, 3);
    for (const Poseidon::Model::Quad& f : mesh.quads)
        face(f.materialIndex, f.indices, 4);
    for (const ModelLod::Part& p : out.parts)
        out.vertices += static_cast<int>(p.vertices.size());
}

void FillMesh(const Poseidon::Model::Model& model, ModelMesh& out)
{
    out = ModelMesh();
    out.centre[0] = model.boundingCenter.x;
    out.centre[1] = model.boundingCenter.y;
    out.centre[2] = model.boundingCenter.z;
    // the levels that are for looking at: the finest, and the coarsest
    const Poseidon::Model::LODLevel* first = nullptr;
    const Poseidon::Model::LODLevel* last = nullptr;
    for (const Poseidon::Model::LODLevel& lod : model.lodLevels)
    {
        if (lod.resolution > 900.0f || lod.mesh.vertices.empty() || (lod.mesh.triangles.empty() && lod.mesh.quads.empty()))
            continue;
        if (!first)
            first = &lod;
        last = &lod;
    }
    if (!first)
        return;
    FillLod(first->mesh, out.fine);
    if (last != first)
        FillLod(last->mesh, out.coarse);
    out.valid = !out.fine.Empty();
    out.onSurface = out.valid;
    for (const ModelLod::Part& p : out.fine.parts)
        for (const ModelLod::Vertex& v : p.vertices)
            out.onSurface = out.onSurface && v.land == 1;
}
} // namespace

std::string GameData::ModelFormat(const std::string& name) const
{
    std::vector<char> bytes;
    if (!ReadFile(LowerPath(name), bytes) || bytes.size() < 4)
        return std::string();
    return std::string(bytes.data(), 4);
}

const ModelMesh& GameData::Mesh(const std::string& name) const
{
    Model(name); // reads it, once
    return _meshes[LowerPath(name)];
}

const ModelShape& GameData::Model(const std::string& name) const
{
    const std::string key = LowerPath(name);
    auto it = _models3d.find(key);
    if (it != _models3d.end())
        return it->second;
    ModelShape& out = _models3d[key];
    std::vector<char> bytes;
    if (!ReadFile(key, bytes) || bytes.size() < 8)
        return out;
    Poseidon::Model::Model model;
    try
    {
        if (std::memcmp(bytes.data(), "ODOL", 4) == 0)
            model = Poseidon::Asset::Formats::ODOLLoader::loadFromBuffer(bytes.data(), static_cast<int>(bytes.size()), key);
        else if (std::memcmp(bytes.data(), "MLOD", 4) == 0)
            model = Poseidon::Asset::Formats::MLODLoader::loadFromBuffer(bytes.data(), static_cast<int>(bytes.size()), key);
        else
            return out;
    }
    catch (...)
    {
        return out;
    }
    if (model.lodLevels.empty())
        return out;
    if (std::memcmp(bytes.data(), "MLOD", 4) == 0)
    {
        // Binarized models come centred on the middle of their box, and say where their own
        // origin was.  The others are centred by the game when it loads them: the same code
        // does it here, and what it took off every corner is where the origin was.
        Poseidon::Model::Vertex* probe = nullptr;
        for (Poseidon::Model::LODLevel& lod : model.lodLevels)
            if (!lod.mesh.vertices.empty())
            {
                probe = &lod.mesh.vertices[0];
                break;
            }
        if (probe)
        {
            const Poseidon::Model::Vector3 before = probe->position;
            model.sourceFormat = "MLOD";
            model.compile();
            model.boundingCenter =
                Poseidon::Model::Vector3(before.x - probe->position.x, before.y - probe->position.y, before.z - probe->position.z);
        }
    }
    out.valid = true;
    out.centre[0] = model.boundingCenter.x;
    out.centre[1] = model.boundingCenter.y;
    out.centre[2] = model.boundingCenter.z;
    FillMesh(model, _meshes[key]);
    // the first level of detail with faces is what is seen
    for (const Poseidon::Model::LODLevel& lod : model.lodLevels)
    {
        const Poseidon::Model::Mesh& mesh = lod.mesh;
        if (lod.resolution > 900.0f || mesh.vertices.empty() || (mesh.triangles.empty() && mesh.quads.empty()))
            continue;
        out.bmin[0] = out.bmin[1] = out.bmin[2] = 1e30f;
        out.bmax[0] = out.bmax[1] = out.bmax[2] = -1e30f;
        for (const Poseidon::Model::Vertex& v : mesh.vertices)
        {
            const float p[3] = {v.position.x, v.position.y, v.position.z};
            for (int a = 0; a < 3; a++)
            {
                out.bmin[a] = std::min(out.bmin[a], p[a]);
                out.bmax[a] = std::max(out.bmax[a], p[a]);
            }
        }
        break;
    }
    // road pieces say where the next ones join: four named points in one of the levels
    for (const Poseidon::Model::LODLevel& lod : model.lodLevels)
    {
        const Poseidon::Model::Mesh& mesh = lod.mesh;
        int found = 0;
        auto point = [&](const char* name, float* to)
        {
            for (const Poseidon::Model::NamedSelection& s : mesh.selections)
                if (LowerPath(s.name) == name && !s.vertexIndices.empty() && s.vertexIndices[0] < mesh.vertices.size())
                {
                    const Poseidon::Model::Vector3& p = mesh.vertices[s.vertexIndices[0]].position;
                    to[0] = p.x;
                    to[1] = p.y;
                    to[2] = p.z;
                    found++;
                    return;
                }
        };
        point("lb", out.lb);
        point("pb", out.pb);
        point("le", out.le);
        point("pe", out.pe);
        if (found == 4)
        {
            out.road = true;
            break;
        }
    }
    return out;
}

bool GameData::LoadIsland(const std::string& file, Island& out, std::string& error)
{
    using namespace Poseidon;
    char magic[4] = {};
    {
        std::ifstream f(file, std::ios::binary);
        if (!f)
        {
            error = "cannot open the file";
            return false;
        }
        f.read(magic, 4);
    }
    if (std::memcmp(magic, "OPRW", 4) != 0)
        return out.LoadRaw(file, error);

    WrpReader reader;
    if (!reader.Load(file.c_str()))
    {
        error = reader.GetError() ? reader.GetError() : "cannot read the island";
        return false;
    }
    const int n = reader.GetGridX();
    if (n != reader.GetGridZ() || n < 16 || n > 1024 || (n & (n - 1)) != 0)
    {
        error = "unsupported size " + std::to_string(reader.GetGridX()) + " x " + std::to_string(reader.GetGridZ());
        return false;
    }
    if (reader.GetTextureCount() > kMaxTextures)
    {
        error = "more than 512 ground textures";
        return false;
    }
    const size_t cells = static_cast<size_t>(n) * n;
    std::vector<float> heights(reader.GetHeightmapData(), reader.GetHeightmapData() + cells);
    std::vector<uint16_t> tex(cells, 0);
    if (static_cast<size_t>(reader.GetTexIndices().Size()) >= cells * 2)
        std::memcpy(tex.data(), reader.GetTexIndices().Data(), cells * 2);
    std::vector<std::string> table(kMaxTextures);
    for (int i = 0; i < reader.GetTextureCount(); i++)
        table[i] = static_cast<const char*>(reader.GetTextureName(i));
    std::vector<Object> objects;
    objects.reserve(static_cast<size_t>(reader.GetObjectCount()));
    for (int i = 0; i < reader.GetObjectCount(); i++)
    {
        const WrpObjectInfo& src = reader.GetObject(i);
        Object o;
        o.id = src.id;
        o.model = static_cast<const char*>(src.name);
        if (o.model.empty())
            continue;
        const Matrix4& t = src.transform;
        const Vector3 a = t.DirectionAside(), u = t.DirectionUp(), d = t.Direction(), p = t.Position();
        const float m[12] = {a.X(), a.Y(), a.Z(), u.X(), u.Y(), u.Z(), d.X(), d.Y(), d.Z(), p.X(), p.Y(), p.Z()};
        std::memcpy(o.m, m, sizeof(m));
        objects.push_back(std::move(o));
    }
    out.Adopt(n, std::move(heights), std::move(tex), std::move(table), std::move(objects), file, reader.GetFormatName());
    return true;
}
} // namespace island
