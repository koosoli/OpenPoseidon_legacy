// SPDX-License-Identifier: GPL-3.0-or-later
//
// Arma 1 configuration, read into its own tree.
//
// The binarized Arma 1 configs (`\0raP`) are decoded here by an original reader,
// turned into config text and parsed with the engine's own ParamFile into a
// separate tree (`Arma1Pars`). That tree is never merged into Pars wholesale: CWA
// stays the source of gameplay. What it is used for:
//   - sound/radio/SFX/environment classes the mission scripts name (fallback after
//     mission, campaign and CWA config),
//   - Arma 1 stringtables (a separate, lowest-priority table),
//   - generated CfgVehicles classes that keep a CWA unit's simulation but draw the
//     Arma 1 model, and for soldiers the Arma 1 move set (CfgMovesMaleSdr, imported
//     under the private name A1_CfgMovesMaleSdr).

#include <Poseidon/Game/Compat/Arma1Compat.hpp>

#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/IO/ParamFileExt.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <Poseidon/UI/Locale/Stringtable/Stringtable.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace Poseidon
{
// Weapons.cpp: every muzzle already built that takes baseMagazine also takes magazine.
void AddMagazineToMuzzlesTaking(const char* baseMagazine, const char* magazine);
} // namespace Poseidon

namespace Poseidon::Arma1Compat
{
const std::vector<std::string>& BankPrefixes(); // Arma1Compat.cpp
std::string RootFolder();                       // Arma1Compat.cpp
std::vector<std::string> DtaFolders();          // Arma1Compat.cpp

namespace
{
std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

ParamFile& Arma1Pars()
{
    static ParamFile pars;
    return pars;
}

bool& ConfigLoaded()
{
    static bool loaded = false;
    return loaded;
}

// ---------------------------------------------------------------------------
// raP -> config text
// ---------------------------------------------------------------------------

// Arma 1 keeps ManPos* as enum names; CWA's config evaluates them to these numbers
// (SoldierOld.hpp, enum ManPos).
int ManPosValue(const std::string& name)
{
    static const char* names[] = {"ManPosDead",       "ManPosWeapon",        "ManPosBinocLying", "ManPosLyingNoWeapon",
                                  "ManPosLying",      "ManPosHandGunLying",  "ManPosCrouch",     "ManPosHandGunCrouch",
                                  "ManPosCombat",     "ManPosHandGunStand",  "ManPosStand",      "ManPosNoWeapon",
                                  "ManPosBinoc",      "ManPosBinocStand"};
    for (int i = 0; i < static_cast<int>(sizeof(names) / sizeof(names[0])); i++)
        if (_stricmp(names[i], name.c_str()) == 0)
            return i;
    if (_stricmp(name.c_str(), "ManPosSwimming") == 0)
        return 10; // no swimming pose in CWA's enum: treat as standing
    return -1000;
}

class RapToText
{
    const std::vector<uint8_t>& _bytes;
    std::string _out;

    void Need(size_t at, size_t n) const
    {
        if (at + n > _bytes.size())
            throw std::runtime_error("truncated raP");
    }
    uint8_t Byte(size_t& at) const
    {
        Need(at, 1);
        return _bytes[at++];
    }
    uint32_t U32(size_t& at) const
    {
        Need(at, 4);
        const uint32_t v = uint32_t(_bytes[at]) | (uint32_t(_bytes[at + 1]) << 8) | (uint32_t(_bytes[at + 2]) << 16) |
                           (uint32_t(_bytes[at + 3]) << 24);
        at += 4;
        return v;
    }
    float F32(size_t& at) const
    {
        const uint32_t bits = U32(at);
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }
    std::string Str(size_t& at) const
    {
        const size_t start = at;
        while (true)
        {
            if (Byte(at) == 0)
                break;
        }
        return std::string(reinterpret_cast<const char*>(_bytes.data() + start), at - start - 1);
    }
    uint32_t Count(size_t& at) const
    {
        uint32_t v = 0;
        for (unsigned shift = 0; shift <= 28; shift += 7)
        {
            const uint8_t b = Byte(at);
            v |= uint32_t(b & 0x7f) << shift;
            if (!(b & 0x80))
                return v;
        }
        throw std::runtime_error("bad raP count");
    }

    static std::string Quote(const std::string& s)
    {
        std::string q = "\"";
        for (char c : s)
        {
            if (c == '"')
                q += "\"\"";
            else if (c == '\r' || c == '\n')
                q += ' ';
            else
                q += c;
        }
        q += '"';
        return q;
    }
    static std::string Float(float f)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.9g", f);
        if (strchr(buf, 'e') || strchr(buf, 'E') || strstr(buf, "inf") || strstr(buf, "nan"))
            snprintf(buf, sizeof(buf), "%.6f", static_cast<double>(f));
        return buf;
    }
    std::string ScalarString(const std::string& s) const
    {
        if (s.rfind("ManPos", 0) == 0)
        {
            const int v = ManPosValue(s);
            if (v != -1000)
                return std::to_string(v);
        }
        return Quote(s);
    }

    std::string Array(size_t& at, int depth)
    {
        if (depth > 32)
            throw std::runtime_error("raP array too deep");
        const uint32_t n = Count(at);
        std::string out = "{";
        for (uint32_t i = 0; i < n; i++)
        {
            if (i)
                out += ",";
            const uint8_t t = Byte(at);
            if (t == 0 || t == 4)
                out += ScalarString(Str(at));
            else if (t == 1)
                out += Float(F32(at));
            else if (t == 2)
                out += std::to_string(static_cast<int32_t>(U32(at)));
            else if (t == 3)
                out += Array(at, depth + 1);
            else
                throw std::runtime_error("bad raP array element");
        }
        return out + "}";
    }

    void Body(size_t at, int depth)
    {
        if (depth > 64)
            throw std::runtime_error("raP class too deep");
        (void)Str(at); // parent name, written by the caller with the class header
        const uint32_t n = Count(at);
        const std::string indent(depth, '\t');
        for (uint32_t i = 0; i < n; i++)
        {
            const uint8_t type = Byte(at);
            if (type == 0)
            {
                const std::string name = Str(at);
                size_t body = U32(at);
                size_t peek = body;
                const std::string base = Str(peek);
                _out += indent + "class " + name + (base.empty() ? "" : " : " + base) + "\n" + indent + "{\n";
                Body(body, depth + 1);
                _out += indent + "};\n";
            }
            else if (type == 1)
            {
                const uint8_t sub = Byte(at);
                const std::string name = Str(at);
                std::string value;
                if (sub == 0 || sub == 4)
                    value = ScalarString(Str(at));
                else if (sub == 1)
                    value = Float(F32(at));
                else if (sub == 2)
                    value = std::to_string(static_cast<int32_t>(U32(at)));
                else
                    throw std::runtime_error("bad raP value");
                _out += indent + name + "=" + value + ";\n";
            }
            else if (type == 2)
            {
                const std::string name = Str(at);
                _out += indent + name + "[]=" + Array(at, 0) + ";\n";
            }
            else if (type == 3)
                _out += indent + "class " + Str(at) + ";\n";
            else if (type == 4)
                (void)Str(at); // "delete X;" -- the CWA config grammar has no delete, and
                               // the classes Arma 1 deletes are UI ones this tree never uses
            else if (type == 5)
            {
                (void)U32(at); // array-append flag
                const std::string name = Str(at);
                _out += indent + name + "[]=" + Array(at, 0) + ";\n";
            }
            else
                throw std::runtime_error("bad raP entry");
        }
    }

  public:
    explicit RapToText(const std::vector<uint8_t>& bytes) : _bytes(bytes) {}
    std::string Convert()
    {
        if (_bytes.size() < 16 || std::memcmp(_bytes.data(), "\0raP", 4) != 0)
            throw std::runtime_error("not a raP config");
        Body(16, 0);
        return _out;
    }
};

// ---------------------------------------------------------------------------
// raP -> tree, merged across every addon before it is parsed (Arma 2)
// ---------------------------------------------------------------------------
//
// Arma 2 configs name base classes that the same file does not declare (a world's
// Lighting classes derive from CfgWorlds' DayLighting* defined by another addon). The
// engine's parser resolves a base while it reads, so such a file cannot be parsed on
// its own. Instead every config.bin is read into this tree, the trees are merged in
// requiredAddons order the way the game merges them, and the merged whole is written
// out as one config text whose classes come after the classes they derive from.

struct RapNode
{
    std::string name;  // original case
    std::string base;  // for classes
    bool isClass = false;
    bool isExtern = false;      // "class X;" -- names a class, defines nothing
    std::string text;           // for values: "name=value;" / "name[]={...};"
    std::vector<std::string> strings; // array values: the top-level string items
    std::vector<std::unique_ptr<RapNode>> children;
    std::map<std::string, size_t> index; // lower-case name -> children slot

    RapNode* Find(const std::string& lowerName) const
    {
        const auto it = index.find(lowerName);
        return it == index.end() ? nullptr : children[it->second].get();
    }
    void Put(std::unique_ptr<RapNode> node)
    {
        const std::string key = Lower(node->name);
        const auto it = index.find(key);
        if (it != index.end())
            children[it->second] = std::move(node);
        else
        {
            index[key] = children.size();
            children.push_back(std::move(node));
        }
    }
    void Remove(const std::string& lowerName)
    {
        const auto it = index.find(lowerName);
        if (it == index.end())
            return;
        children.erase(children.begin() + static_cast<long>(it->second));
        index.clear();
        for (size_t i = 0; i < children.size(); i++)
            index[Lower(children[i]->name)] = i;
    }
    std::unique_ptr<RapNode> Clone() const
    {
        auto n = std::make_unique<RapNode>();
        n->name = name;
        n->base = base;
        n->isClass = isClass;
        n->isExtern = isExtern;
        n->text = text;
        n->strings = strings;
        for (const auto& c : children)
            n->Put(c->Clone());
        return n;
    }
};

class RapTree
{
    const std::vector<uint8_t>& _bytes;

    void Need(size_t at, size_t n) const
    {
        if (at + n > _bytes.size())
            throw std::runtime_error("truncated raP");
    }
    uint8_t Byte(size_t& at) const
    {
        Need(at, 1);
        return _bytes[at++];
    }
    uint32_t U32(size_t& at) const
    {
        Need(at, 4);
        const uint32_t v = uint32_t(_bytes[at]) | (uint32_t(_bytes[at + 1]) << 8) | (uint32_t(_bytes[at + 2]) << 16) |
                           (uint32_t(_bytes[at + 3]) << 24);
        at += 4;
        return v;
    }
    float F32(size_t& at) const
    {
        const uint32_t bits = U32(at);
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }
    std::string Str(size_t& at) const
    {
        const size_t start = at;
        while (Byte(at) != 0)
        {
        }
        return std::string(reinterpret_cast<const char*>(_bytes.data() + start), at - start - 1);
    }
    uint32_t Count(size_t& at) const
    {
        uint32_t v = 0;
        for (unsigned shift = 0; shift <= 28; shift += 7)
        {
            const uint8_t b = Byte(at);
            v |= uint32_t(b & 0x7f) << shift;
            if (!(b & 0x80))
                return v;
        }
        throw std::runtime_error("bad raP count");
    }
    static std::string Quote(const std::string& s)
    {
        std::string q = "\"";
        for (char c : s)
        {
            if (c == '"')
                q += "\"\"";
            else if (c == '\r' || c == '\n')
                q += ' ';
            else
                q += c;
        }
        return q + "\"";
    }
    static std::string Float(float f)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.9g", f);
        if (strchr(buf, 'e') || strchr(buf, 'E') || strstr(buf, "inf") || strstr(buf, "nan"))
            snprintf(buf, sizeof(buf), "%.6f", static_cast<double>(f));
        return buf;
    }
    static std::string Scalar(const std::string& s)
    {
        if (s.rfind("ManPos", 0) == 0)
        {
            const int v = ManPosValue(s);
            if (v != -1000)
                return std::to_string(v);
        }
        return Quote(s);
    }
    std::string Array(size_t& at, int depth, std::vector<std::string>* strings)
    {
        if (depth > 32)
            throw std::runtime_error("raP array too deep");
        const uint32_t n = Count(at);
        std::string out = "{";
        for (uint32_t i = 0; i < n; i++)
        {
            if (i)
                out += ",";
            const uint8_t t = Byte(at);
            if (t == 0 || t == 4)
            {
                const std::string v = Str(at);
                if (strings)
                    strings->push_back(v);
                out += Scalar(v);
            }
            else if (t == 1)
                out += Float(F32(at));
            else if (t == 2)
                out += std::to_string(static_cast<int32_t>(U32(at)));
            else if (t == 3)
                out += Array(at, depth + 1, nullptr);
            else
                throw std::runtime_error("bad raP array element");
        }
        return out + "}";
    }
    void Body(size_t at, RapNode& node, int depth)
    {
        if (depth > 64)
            throw std::runtime_error("raP class too deep");
        (void)Str(at); // parent name, read with the class header
        const uint32_t n = Count(at);
        for (uint32_t i = 0; i < n; i++)
        {
            const uint8_t type = Byte(at);
            auto child = std::make_unique<RapNode>();
            if (type == 0)
            {
                child->name = Str(at);
                child->isClass = true;
                size_t body = U32(at);
                size_t peek = body;
                child->base = Str(peek);
                Body(body, *child, depth + 1);
                node.Put(std::move(child));
            }
            else if (type == 1)
            {
                const uint8_t sub = Byte(at);
                child->name = Str(at);
                std::string value;
                if (sub == 0 || sub == 4)
                    value = Scalar(Str(at));
                else if (sub == 1)
                    value = Float(F32(at));
                else if (sub == 2)
                    value = std::to_string(static_cast<int32_t>(U32(at)));
                else
                    throw std::runtime_error("bad raP value");
                child->text = child->name + "=" + value + ";";
                node.Put(std::move(child));
            }
            else if (type == 2 || type == 5)
            {
                if (type == 5)
                    (void)U32(at); // "+=" flag: appended arrays are taken whole
                child->name = Str(at);
                child->text = child->name + "[]=" + Array(at, 0, &child->strings) + ";";
                node.Put(std::move(child));
            }
            else if (type == 3)
            {
                child->name = Str(at);
                child->isClass = true;
                child->isExtern = true;
                if (!node.Find(Lower(child->name)))
                    node.Put(std::move(child));
            }
            else if (type == 4)
            {
                // "delete X;" -- kept as a marker the merge applies to the destination
                child->name = Str(at);
                child->text = "\x01" "delete";
                node.Put(std::move(child));
            }
            else
                throw std::runtime_error("bad raP entry");
        }
    }

  public:
    explicit RapTree(const std::vector<uint8_t>& bytes) : _bytes(bytes) {}
    std::unique_ptr<RapNode> Read()
    {
        if (_bytes.size() < 16 || std::memcmp(_bytes.data(), "\0raP", 4) != 0)
            throw std::runtime_error("not a raP config");
        auto root = std::make_unique<RapNode>();
        root->isClass = true;
        Body(16, *root, 0);
        return root;
    }
};

// Merges `src` into `dst` like an addon config updating the config before it.
void MergeRap(RapNode& dst, const RapNode& src)
{
    for (const auto& child : src.children)
    {
        const std::string key = Lower(child->name);
        if (child->text == "\x01" "delete")
        {
            dst.Remove(key);
            continue;
        }
        RapNode* existing = dst.Find(key);
        if (child->isClass)
        {
            if (existing && existing->isClass)
            {
                if (child->isExtern)
                    continue;
                if (!child->base.empty() || existing->isExtern)
                    existing->base = child->base;
                existing->isExtern = false;
                MergeRap(*existing, *child);
            }
            else if (!child->isExtern)
            {
                // An extern ("class Sounds;") names a class the scope inherits or that
                // another addon defines: adding it as an empty class would shadow the
                // inherited one (LandVehicle >> Sounds hiding AllVehicles >> Sounds).
                dst.Put(child->Clone());
            }
        }
        else
        {
            dst.Put(child->Clone());
        }
    }
}

void CollectRapBases(const RapNode& node, std::set<std::string>& names)
{
    if (node.isClass && !node.base.empty())
        names.insert(Lower(node.base));
    for (const auto& c : node.children)
        if (c->isClass)
            CollectRapBases(*c, names);
}

// Writes the merged tree as config text. Within a class, values come first, then the
// classes, each after the siblings its subtree derives from.
void WriteRap(const RapNode& node, std::string& out, int depth)
{
    const std::string pad(static_cast<size_t>(depth), '\t');
    for (const auto& c : node.children)
        if (!c->isClass && (c->text.empty() || c->text[0] != '\x01')) // not a pending "delete"
            out += pad + c->text + "\r\n";
    std::vector<int> state(node.children.size(), 0);
    std::function<void(size_t)> visit = [&](size_t i)
    {
        if (state[i])
            return;
        state[i] = 1;
        const RapNode& c = *node.children[i];
        std::set<std::string> bases;
        CollectRapBases(c, bases);
        for (const std::string& b : bases)
        {
            const auto it = node.index.find(b);
            if (it != node.index.end() && it->second != i && !node.children[it->second]->isExtern)
                visit(it->second);
        }
        out += pad + "class " + c.name;
        if (!c.base.empty())
            out += ": " + c.base;
        out += "\r\n" + pad + "{\r\n";
        WriteRap(c, out, depth + 1);
        out += pad + "};\r\n";
        state[i] = 2;
    };
    for (size_t i = 0; i < node.children.size(); i++)
        if (node.children[i]->isClass && !node.children[i]->isExtern)
            visit(i);
}

std::vector<uint8_t> ReadArchiveFile(const std::string& name)
{
    std::vector<uint8_t> bytes;
    QIFStreamB in;
    in.AutoOpen(name.c_str());
    while (!in.fail())
    {
        const int c = in.get();
        if (in.eof() || c < 0)
            break;
        bytes.push_back(static_cast<uint8_t>(c));
    }
    return bytes;
}

QFBank* FindBank(const std::string& prefix)
{
    for (int i = 0; i < GFileBanks.Size(); i++)
    {
        if (_stricmp((const char*)GFileBanks[i].GetPrefix(), prefix.c_str()) == 0)
            return &GFileBanks[i];
    }
    return nullptr;
}

std::vector<std::string> BankFiles(QFBank& bank, bool (*want)(const std::string& lowerName))
{
    struct Ctx
    {
        std::vector<std::string> names;
        bool (*want)(const std::string&);
    } ctx{{}, want};
    bank.ForEach(
        [](const FileInfoO& fi, const FileBankType*, void* context)
        {
            auto* c = static_cast<Ctx*>(context);
            const std::string name = (const char*)fi.name;
            if (c->want(Lower(name)))
                c->names.push_back(name);
        },
        &ctx);
    std::sort(ctx.names.begin(), ctx.names.end());
    return ctx.names;
}

bool IsConfigBin(const std::string& n)
{
    return n == "config.bin" || (n.size() > 11 && n.compare(n.size() - 11, 11, "\\config.bin") == 0) ||
           (n.size() > 11 && n.compare(n.size() - 11, 11, "/config.bin") == 0);
}

bool IsStringtable(const std::string& n)
{
    const size_t slash = n.find_last_of("\\/");
    const std::string leaf = slash == std::string::npos ? n : n.substr(slash + 1);
    return leaf.rfind("stringtable", 0) == 0 && leaf.size() > 4 &&
           (leaf.compare(leaf.size() - 4, 4, ".csv") == 0 || leaf.compare(leaf.size() - 4, 4, ".xml") == 0);
}

struct LoadedConfig
{
    std::string source;
    std::unique_ptr<ParamFile> file;
    std::unique_ptr<RapNode> tree; // Arma 2: merged as raP trees, parsed once (RapTree)
    std::vector<std::string> patches;
    std::vector<std::string> requiredAddons;
    bool isBase = false;
};

void ImportIntoPars(const std::string& text, const char* what)
{
    ParamFile tmp;
    QIStream in(text.data(), static_cast<int>(text.size()));
    tmp.Parse(in);
    const ParamAccessMode access = Pars.GetAccessMode();
    Pars.SetAccessMode(PAReadAndWrite);
    for (int i = 0; i < tmp.GetEntryCount(); i++)
    {
        if (ParamEntry* top = Pars.FindEntryNoInheritance(tmp.GetEntry(i).GetName()))
            top->SetAccessMode(PAReadAndWrite);
    }
    Pars.Update(tmp);
    Pars.SetAccessMode(access);
    Pars.SetFile(&Pars);
    LOG_INFO(Mission, "{}: imported {} ({} bytes of config)", Label(), what, text.size());
}

// Config text for a class of the merged Arma 1 tree. Not ParamEntry::Save: a merged
// tree keeps members in merge order, so a class can precede the sibling it derives
// from (bin defines DefaultDie, the anims addon adds Default later) and the text would
// not parse back. Here every class is written after the siblings its subtree derives
// from.
void QuoteConfigString(std::string& out, const char* value)
{
    out += '"';
    for (const char* c = value ? value : ""; *c; c++)
    {
        if (*c == '"')
            out += "\"\"";
        else if (*c == '\n' || *c == '\r')
            out += ' ';
        else
            out += *c;
    }
    out += '"';
}

void AppendFloat(std::string& out, float value)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", value);
    if (!std::strpbrk(buf, ".eEnN"))
        std::strcat(buf, ".0");
    out += buf;
}

void AppendArrayValue(std::string& out, const IParamArrayValue& v)
{
    if (v.IsArrayValue())
    {
        out += '{';
        for (int i = 0; i < v.GetItemCount(); i++)
        {
            if (i)
                out += ',';
            AppendArrayValue(out, *v.GetItem(i));
        }
        out += '}';
    }
    else if (v.IsFloatValue())
        AppendFloat(out, v.GetFloat());
    else if (v.IsIntValue())
        out += std::to_string(v.GetInt());
    else
        QuoteConfigString(out, (const char*)v.GetValueRaw());
}

void CollectBaseNames(const ParamEntry& cls, std::set<std::string>& names)
{
    if (const ParamClass* c = cls.GetClassInterface())
        if (const char* base = c->GetBaseName(); base && *base)
            names.insert(Lower(base));
    for (int i = 0; i < cls.GetEntryCount(); i++)
        if (cls.GetEntry(i).IsClass())
            CollectBaseNames(cls.GetEntry(i), names);
}

void AppendEntry(std::string& out, const ParamEntry& e, int indent);

void AppendMembers(std::string& out, const ParamEntry& cls, int indent)
{
    const int n = cls.GetEntryCount();
    for (int i = 0; i < n; i++)
        if (!cls.GetEntry(i).IsClass())
            AppendEntry(out, cls.GetEntry(i), indent);
    std::map<std::string, int> byName;
    for (int i = 0; i < n; i++)
        if (cls.GetEntry(i).IsClass())
            byName[Lower((const char*)cls.GetEntry(i).GetName())] = i;
    std::vector<int> state(n, 0); // 0 todo, 1 in progress, 2 written
    std::function<void(int)> visit = [&](int i)
    {
        if (state[i])
            return;
        state[i] = 1;
        std::set<std::string> bases;
        CollectBaseNames(cls.GetEntry(i), bases);
        const std::string self = Lower((const char*)cls.GetEntry(i).GetName());
        for (const std::string& b : bases)
        {
            auto it = byName.find(b);
            if (it != byName.end() && b != self)
                visit(it->second);
        }
        AppendEntry(out, cls.GetEntry(i), indent);
        state[i] = 2;
    };
    for (int i = 0; i < n; i++)
        if (cls.GetEntry(i).IsClass())
            visit(i);
}

void AppendEntry(std::string& out, const ParamEntry& e, int indent)
{
    const std::string pad(static_cast<size_t>(indent), '\t');
    const std::string name = (const char*)e.GetName();
    if (e.IsClass())
    {
        out += pad + "class " + name;
        if (const ParamClass* c = e.GetClassInterface())
            if (const char* base = c->GetBaseName(); base && *base)
                out += std::string(": ") + base;
        out += "\r\n" + pad + "{\r\n";
        AppendMembers(out, e, indent + 1);
        out += pad + "};\r\n";
    }
    else if (e.IsArray())
    {
        out += pad + name + "[]={";
        for (int i = 0; i < e.GetSize(); i++)
        {
            if (i)
                out += ',';
            AppendArrayValue(out, e[i]);
        }
        out += "};\r\n";
    }
    else
    {
        out += pad + name + "=";
        if (e.IsFloatValue())
            AppendFloat(out, (float)e);
        else if (e.IsIntValue())
            out += std::to_string(e.GetInt());
        else
            QuoteConfigString(out, (const char*)e.GetValueRaw());
        out += ";\r\n";
    }
}

std::string SaveText(const ParamEntry& entry)
{
    std::string out;
    AppendEntry(out, entry, 0);
    return out;
}

void ReplaceFirst(std::string& text, const std::string& from, const std::string& to)
{
    const size_t at = text.find(from);
    if (at != std::string::npos)
        text.replace(at, from.size(), to);
}

bool MovesImported()
{
    static int state = 0; // 0 untried, 1 ok, -1 failed
    if (state != 0)
        return state > 0;
    state = -1;
    const ParamEntry* basic = Arma1Pars().FindEntryNoInheritance("CfgMovesBasic");
    const ParamEntry* male = Arma1Pars().FindEntryNoInheritance("CfgMovesMaleSdr");
    if (!basic || !male || !male->FindEntry("States"))
    {
        LOG_WARN(Mission, "{}: CfgMovesMaleSdr not found, soldiers keep CWA models", Label());
        return false;
    }
    std::string basicText = SaveText(*basic);
    std::string maleText = SaveText(*male);
    ReplaceFirst(basicText, "class CfgMovesBasic", "class A1_CfgMovesBasic");
    ReplaceFirst(maleText, "class CfgMovesMaleSdr: CfgMovesBasic", "class A1_CfgMovesMaleSdr: A1_CfgMovesBasic");
    ReplaceFirst(maleText, "class CfgMovesMaleSdr", "class A1_CfgMovesMaleSdr");
    // CWA vehicles name their crew poses through CfgVehicleActions (vehicleActions).
    const size_t open = maleText.find('{');
    if (open == std::string::npos)
        return false;
    maleText.insert(open + 1, "\r\nvehicleActions=\"A1_CfgVehicleActions\";\r\n");

    // A1_CfgVehicleActions: every CWA vehicle action -> the closest Arma 1 crew state.
    const ParamEntry& states = (*male) >> "States";
    const auto has = [&](const char* s) { return states.FindEntry(s) != nullptr; };
    std::string actions = "class A1_CfgVehicleActions\r\n{\r\n";
    if (const ParamEntry* cwa = Pars.FindEntry("CfgVehicleActions"))
    {
        for (int i = 0; i < cwa->GetEntryCount(); i++)
        {
            const ParamEntry& e = cwa->GetEntry(i);
            const std::string name = (const char*)e.GetName();
            const std::string value = e.IsClass() ? std::string() : std::string((const char*)(RStringB)e);
            const std::string n = Lower(name + " " + value);
            const auto any = [&](std::initializer_list<const char*> words)
            {
                for (const char* w : words)
                    if (n.find(w) != std::string::npos)
                        return true;
                return false;
            };
            const bool out = any({"out"});
            const bool cargo = any({"cargo", "codriver", "passenger"});
            const bool gunner = !cargo && any({"gunner"});
            const bool commander = !cargo && any({"commander"});
            const bool pilot = any({"pilot"});
            std::string pick;
            if (has(name.c_str()))
                pick = name;
            else if (!value.empty() && has(value.c_str()))
                pick = value;
            else if (any({"cobra", "ah1", "uh60", "mi17", "mi24", "heli", "ch47", "oh58", "kiowa", "hind", "hip"}))
                pick = cargo ? "UH60_Cargo01" : gunner ? "UH60_Gunner" : "UH60_Pilot";
            else if (any({"a10", "su25", "plane", "cessna", "camel", "harrier", "mig"}))
                pick = "A10_Pilot";
            else if (any({"boat", "zodiac", "pbr", "rhib"}))
                pick = cargo ? "Zodiac_Cargo01" : "Zodiac_Driver";
            else if (any({"tank", "t72", "t80", "t55", "m1a", "m60", "abrams", "bmp", "m113", "m2a2", "bradley",
                          "brdm", "zsu", "vulcan", "apc", "bmd", "shilka"}))
            {
                if (cargo)
                    pick = "BMP2_Cargo04";
                else if (commander)
                    pick = out ? "T72_CommanderOut" : "T72_Commander";
                else if (gunner)
                    pick = out ? "T72_GunnerOut" : "T72_Gunner";
                else
                    pick = out ? "T72_DriverOut" : "T72_Driver";
            }
            else if (any({"truck", "ural", "5t", "bus", "tractor"}))
                pick = cargo ? "Truck_Cargo01" : gunner ? "Truck_Gunner" : "Truck_Driver";
            else if (any({"static", "m2", "dshk", "mg", "d30", "mortar", "nest"}))
                pick = "M2_Gunner";
            else if (any({"bike", "motorcycle"}))
                pick = cargo ? "Bike_Cargo" : "Bike_Driver";
            else if (cargo)
                pick = "UAZ_Cargo01";
            else if (gunner)
                pick = "UAZ_Gunner01";
            else if (commander)
                pick = "T72_Commander";
            else if (pilot || any({"driver", "jeep", "uaz", "car", "skoda", "hmmwv", "hummer"}))
                pick = "UAZ_Driver";
            else
                pick = "Crew";
            if (!has(pick.c_str()))
                pick = gunner ? "BasicSittingGunner" : (cargo ? "Crew" : "BasicDriver");
            actions += name + "=\"" + pick + "\";\r\n";
        }
    }
    actions += "};\r\n";

    ImportIntoPars(basicText + maleText + actions, "CfgMovesMaleSdr as A1_CfgMovesMaleSdr");
    if (!Pars.FindEntry("A1_CfgMovesMaleSdr"))
        return false;
    state = 1;
    return true;
}

// Values an Arma 1 man class carries for engine-facing names (bones, memory
// points, selections, hit points). Copied verbatim into the generated class; the
// engine reads them where present and keeps OFP's names otherwise.
const char* const kManNameKeys[] = {
    "boneHead", "weaponBone", "memoryPointLStep", "memoryPointRStep", "memoryPointAim", "memoryPointCameraTarget",
    "memoryPointAimingAxis", "memoryPointHeadAxis", "selectionHeadWound", "selectionBodyWound",
    "selectionLArmWound", "selectionRArmWound", "selectionLLegWound", "selectionRLegWound", "selectionHeadHide",
    "selectionNeckHide"};

std::string StringValue(const ParamEntry& cls, const char* key)
{
    const ParamEntry* e = cls.FindEntry(key);
    if (!e || e->IsClass() || e->IsArray())
        return {};
    return (const char*)(RStringB)*e;
}

bool IsKindOf(const ParamEntry* cls, const char* base)
{
    const ParamEntry* vehicles = Arma1Pars().FindEntry("CfgVehicles");
    const ParamEntry* baseEntry = vehicles ? vehicles->FindEntry(base) : nullptr;
    const ParamClass* c = cls ? cls->GetClassInterface() : nullptr;
    const ParamClass* b = baseEntry ? baseEntry->GetClassInterface() : nullptr;
    return c && b && (c == b || c->IsDerivedFrom(*b));
}
} // namespace

void LoadConfig()
{
    if (!Active() || ConfigLoaded())
        return;
    ConfigLoaded() = true;

    std::vector<LoadedConfig> configs;
    int stringtables = 0;

    // Dta\bin.pbo: the base config and global stringtable (Arma 2 keeps its global
    // strings in Dta\languagecore.pbo; Operation Arrowhead adds Expansion\Dta). Opened
    // as private banks, never added to GFileBanks, so CWA's own bin\ keeps answering
    // CWA's lookups. Later Dta folders override earlier ones.
    int baseConfigs = 0;
    for (const std::string& dtaFolder : DtaFolders())
    {
        for (const char* archive : {"bin", "languagecore"})
        {
            const std::string binPath = dtaFolder + "\\" + archive;
            QFBank dta;
            if (!dta.open(RString(binPath.c_str())))
            {
                if (std::string(archive) == "bin")
                    LOG_WARN(Mission, "{}: {}.pbo not readable, base config missing", Label(), binPath);
                continue;
            }
            dta.Lock();
            const auto readPrivate = [&](const std::string& name)
            {
                std::vector<uint8_t> bytes;
                Ref<IFileBuffer> buf = dta.Read(name.c_str());
                if (buf && buf->GetSize() > 0)
                    bytes.assign(reinterpret_cast<const uint8_t*>(buf->GetData()),
                                 reinterpret_cast<const uint8_t*>(buf->GetData()) + buf->GetSize());
                return bytes;
            };
            for (const std::string& name : BankFiles(dta, IsStringtable))
            {
                const std::vector<uint8_t> bytes = readPrivate(name);
                if (bytes.empty())
                    continue;
                LoadArma1StringtableFromMemory(("dta:" + binPath + "\\" + name).c_str(),
                                               reinterpret_cast<const char*>(bytes.data()), bytes.size());
                stringtables++;
            }
            for (const std::string& name : BankFiles(dta, IsConfigBin))
            {
                const std::string path = binPath + ".pbo:" + name;
                try
                {
                    LoadedConfig cfg;
                    cfg.source = path;
                    cfg.isBase = true;
                    if (Generation() == 2)
                    {
                        cfg.tree = RapTree(readPrivate(name)).Read();
                        LOG_INFO(Mission, "{}: read {}", Label(), path);
                    }
                    else
                    {
                        const std::string text = RapToText(readPrivate(name)).Convert();
                        LOG_INFO(Mission, "{}: parsing {} ({} bytes)", Label(), path, text.size());
                        cfg.file = std::make_unique<ParamFile>();
                        QIStream in(text.data(), static_cast<int>(text.size()));
                        cfg.file->Parse(in);
                    }
                    configs.insert(configs.begin() + baseConfigs, std::move(cfg));
                    baseConfigs++;
                }
                catch (const std::exception& e)
                {
                    LOG_WARN(Mission, "{}: config {} not read: {}", Label(), path, e.what());
                }
            }
            dta.close();
        }
    }
    for (const std::string& prefix : BankPrefixes())
    {
        QFBank* bank = FindBank(prefix);
        if (!bank)
            continue;
        for (const std::string& name : BankFiles(*bank, IsStringtable))
        {
            LoadStringtable("Arma1", RString((prefix + name).c_str()), 0, false);
            stringtables++;
        }
        for (const std::string& name : BankFiles(*bank, IsConfigBin))
        {
            const std::string path = prefix + name;
            try
            {
                LoadedConfig cfg;
                cfg.source = path;
                if (Generation() == 2)
                {
                    cfg.tree = RapTree(ReadArchiveFile(path)).Read();
                    if (const RapNode* patches = cfg.tree->Find("cfgpatches"))
                    {
                        for (const auto& patch : patches->children)
                        {
                            if (!patch->isClass || patch->isExtern)
                                continue;
                            cfg.patches.push_back(Lower(patch->name));
                            if (const RapNode* req = patch->Find("requiredaddons"))
                                for (const std::string& r : req->strings)
                                    cfg.requiredAddons.push_back(Lower(r));
                        }
                    }
                    configs.push_back(std::move(cfg));
                    continue;
                }
                const std::string text = RapToText(ReadArchiveFile(path)).Convert();
                LOG_INFO(Mission, "{}: parsing {} ({} bytes)", Label(), path, text.size());
                cfg.file = std::make_unique<ParamFile>();
                QIStream in(text.data(), static_cast<int>(text.size()));
                cfg.file->Parse(in);
                if (const ParamEntry* patches = cfg.file->FindEntry("CfgPatches"))
                {
                    for (int i = 0; i < patches->GetEntryCount(); i++)
                    {
                        const ParamEntry& patch = patches->GetEntry(i);
                        if (!patch.IsClass())
                            continue;
                        cfg.patches.push_back(Lower((const char*)patch.GetName()));
                        if (const ParamEntry* req = patch.FindEntry("requiredAddons"))
                            for (int k = 0; k < req->GetSize(); k++)
                                cfg.requiredAddons.push_back(Lower((const char*)(RStringB)(*req)[k]));
                    }
                }
                configs.push_back(std::move(cfg));
            }
            catch (const std::exception& e)
            {
                LOG_WARN(Mission, "{}: config {} not read: {}", Label(), path, e.what());
            }
        }
    }

    auto mergedTree = std::make_unique<RapNode>();
    mergedTree->isClass = true;
    // requiredAddons order (Kahn), bin first; unknown requirements are ignored.
    std::set<std::string> known;
    for (const auto& c : configs)
        known.insert(c.patches.begin(), c.patches.end());
    std::set<std::string> done;
    std::vector<bool> merged(configs.size(), false);
    size_t mergedCount = 0;
    while (mergedCount < configs.size())
    {
        bool progress = false;
        for (size_t i = 0; i < configs.size(); i++)
        {
            if (merged[i])
                continue;
            bool ready = true;
            for (const std::string& r : configs[i].requiredAddons)
                if (known.count(r) && !done.count(r) &&
                    std::find(configs[i].patches.begin(), configs[i].patches.end(), r) == configs[i].patches.end())
                    ready = false;
            if (!ready)
                continue;
            LOG_DEBUG(Mission, "{}: merging {}", Label(), configs[i].source);
            if (configs[i].tree)
                MergeRap(*mergedTree, *configs[i].tree);
            else
                Arma1Pars().Update(*configs[i].file);
            done.insert(configs[i].patches.begin(), configs[i].patches.end());
            merged[i] = true;
            mergedCount++;
            progress = true;
        }
        if (!progress)
        {
            for (size_t i = 0; i < configs.size(); i++)
                if (!merged[i])
                {
                    LOG_WARN(Mission, "{}: dependency cycle, merging {} anyway", Label(), configs[i].source);
                    if (configs[i].tree)
                        MergeRap(*mergedTree, *configs[i].tree);
                    else
                        Arma1Pars().Update(*configs[i].file);
                    merged[i] = true;
                    mergedCount++;
                }
        }
    }
    if (!mergedTree->children.empty())
    {
        // Arma 2: the merged tree, written once with every base ahead of its users.
        std::string text;
        WriteRap(*mergedTree, text, 0);
        LOG_INFO(Mission, "{}: parsing the merged config ({} bytes of text)", Label(), text.size());
        QIStream in(text.data(), static_cast<int>(text.size()));
        Arma1Pars().Parse(in);
    }
    Arma1Pars().SetFile(&Arma1Pars());
    const ParamEntry* vehicles = Arma1Pars().FindEntry("CfgVehicles");
    LOG_INFO(Mission, "{}: config read from {} archives ({} CfgVehicles classes, {} stringtables)", Label(),
             configs.size(), vehicles ? vehicles->GetEntryCount() : 0, stringtables);
}

namespace
{
bool IsMissionSqm(const std::string& n)
{
    return n == "mission.sqm" || (n.size() > 12 && n.compare(n.size() - 12, 12, "\\mission.sqm") == 0);
}
} // namespace

std::vector<std::string> MissionFiles()
{
    std::vector<std::string> out;
    if (!Active())
        return out;
    std::set<std::string> seen;
    for (const std::string& prefix : BankPrefixes())
    {
        if (!seen.insert(Lower(prefix)).second)
            continue;
        QFBank* bank = FindBank(prefix);
        if (!bank)
            continue;
        for (const std::string& name : BankFiles(*bank, IsMissionSqm))
            out.push_back(prefix + name);
    }
    return out;
}

const ParamEntry* ConfigRoot()
{
    return Active() && ConfigLoaded() ? &Arma1Pars() : nullptr;
}

const ParamEntry* FindConfigClass(const char* root, const char* name)
{
    if (!Active() || !ConfigLoaded() || !root || !name || !*name)
        return nullptr;
    const ParamEntry* cls = Arma1Pars().FindEntry(root);
    return cls ? cls->FindEntry(name) : nullptr;
}

namespace
{
const ParamEntry* CwaVehicle(const char* name)
{
    const ParamEntry* vehicles = Pars.FindEntry("CfgVehicles");
    const ParamEntry* e = vehicles && name && *name ? vehicles->FindEntryNoInheritance(name) : nullptr;
    return e && e->IsClass() ? e : nullptr;
}

bool UsableCwaBase(const char* name, const std::string& sim, int side, bool needScope)
{
    const ParamEntry* c = CwaVehicle(name);
    if (!c)
        return false;
    if (needScope)
    {
        const ParamEntry* scope = c->FindEntry("scope");
        if (!scope || (int)*scope < 1)
            return false;
    }
    if (Lower(StringValue(*c, "simulation")) != sim)
        return false;
    const bool sided = sim == "soldier" || sim == "car" || sim == "tank" || sim == "helicopter" ||
                       sim == "airplane" || sim == "ship" || sim == "motorcycle";
    if (sided && side >= 0)
    {
        const ParamEntry* s = c->FindEntry("side");
        if (s && (int)*s != side)
            return false;
    }
    return true;
}

// A concrete CWA class derived from abstract `base` (scope >= 2, else >= 1) with the same
// simulation: the one that adds the fewest entries of its own, i.e. the plainest one, so
// that as little as possible of its own character (ladders, animations, destruction)
// rides along under the Arma 1 model. Cached per base; "" when there is none.
RString ConcreteCwaDescendant(const char* base)
{
    static std::map<std::string, RString> cache;
    const std::string key = Lower(base);
    if (const auto it = cache.find(key); it != cache.end())
        return it->second;
    RString found;
    const ParamEntry* vehicles = Pars.FindEntry("CfgVehicles");
    const ParamEntry* baseEntry = CwaVehicle(base);
    const ParamClass* b = baseEntry ? baseEntry->GetClassInterface() : nullptr;
    if (vehicles && b)
    {
        const std::string sim = Lower(StringValue(*baseEntry, "simulation"));
        int fewest = 0;
        for (int want = 2; want >= 1 && found.GetLength() == 0; want--)
        {
            for (int i = 0; i < vehicles->GetEntryCount(); i++)
            {
                const ParamEntry& e = vehicles->GetEntry(i);
                if (!e.IsClass())
                    continue;
                const ParamClass* c = e.GetClassInterface();
                if (!c || c == b || !c->IsDerivedFrom(*b))
                    continue;
                const ParamEntry* scope = e.FindEntry("scope");
                if (!scope || (int)*scope < want || Lower(StringValue(e, "simulation")) != sim)
                    continue;
                if (_strnicmp((const char*)e.GetName(), "A1x_", 4) == 0)
                    continue;
                if (found.GetLength() == 0 || e.GetEntryCount() < fewest)
                {
                    found = RString((const char*)e.GetName());
                    fewest = e.GetEntryCount();
                }
            }
        }
    }
    cache[key] = found;
    return found;
}
} // namespace

bool Arma1RapFileToText(const char* fileName, std::string& text)
{
    if (!fileName || !*fileName || !QIFStreamB::FileExist(fileName))
        return false;
    QIFStreamB in;
    in.AutoOpen(fileName);
    const int size = in.rest();
    if (in.fail() || size < 16)
        return false;
    // Arma 1 layout: "\0raP" followed by a zero word (OFP's carries version 2..4).
    // Only the header is read for every other file (this sits on the text parse path).
    uint8_t head[8] = {};
    in.read(head, sizeof(head));
    if (std::memcmp(head, "\0raP", 4) != 0 || head[4] || head[5] || head[6] || head[7])
        return false;
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    std::memcpy(bytes.data(), head, sizeof(head));
    in.read(bytes.data() + sizeof(head), size - static_cast<int>(sizeof(head)));
    try
    {
        text = RapToText(bytes).Convert();
    }
    catch (const std::exception& e)
    {
        LOG_WARN(Mission, "{}: binarised {} not readable: {}", Label(), fileName, e.what());
        return false;
    }
    return true;
}

RString FallbackCwaClass(const RString& a1Name)
{
    if (!Active() || !ConfigLoaded())
        return RString();
    const ParamEntry* a1 = FindConfigClass("CfgVehicles", a1Name);
    if (!a1 || !a1->IsClass())
        return RString();
    const std::string sim = Lower(StringValue(*a1, "simulation"));
    int side = -1;
    if (const ParamEntry* s = a1->FindEntry("side"); s && !s->IsClass() && !s->IsArray())
        side = (int)*s;
    // 1. the nearest Arma 1 ancestor that CWA defines as a usable class of the same kind
    //    (Civilian13 -> Civilian, hilux1_civil_3_open -> ... -> Car)
    for (int pass = 0; pass < 2; pass++)
    {
        const ParamClass* c = a1->GetClassInterface();
        for (int guard = 0; c && guard < 40; guard++)
        {
            const char* base = c->GetBaseName();
            if (!base || !*base)
                break;
            if (UsableCwaBase(base, sim, pass == 0 ? side : -1, true))
                return RString(base);
            const ParamEntry* next = FindConfigClass("CfgVehicles", base);
            c = next ? next->GetClassInterface() : nullptr;
        }
    }
    // 2. a CWA class by simulation and side
    struct Default
    {
        const char* sim;
        const char* bySide[4]; // east, west, resistance, civilian
    };
    static const Default defaults[] = {
        {"soldier", {"SoldierEB", "SoldierWB", "SoldierGB", "Civilian"}},
        {"car", {"UAZ", "Jeep", "GJeep", "Skoda"}},
        {"tank", {"T72", "M1Abrams", "T72", "T72"}},
        {"helicopter", {"Mi17", "UH60", "UH60", "UH60"}},
        {"airplane", {"Su25", "A10", "A10", "Cessna"}},
        {"ship", {"BoatE", "BoatW", "BoatW", "BoatW"}},
        {"motorcycle", {"Kolo", "Kolo", "Kolo", "Kolo"}},
        {"parachute", {"ParachuteEast", "ParachuteWest", "ParachuteG", "ParachuteC"}},
    };
    const int s = side >= 0 && side <= 3 ? side : 3;
    for (const Default& d : defaults)
        if (sim == d.sim && CwaVehicle(d.bySide[s]))
            return RString(d.bySide[s]);
    // 3. the nearest CWA ancestor at all (houses, things, fires, flags ...). An abstract
    //    one (Strategic, ReammoBox, Fence: scope 0) is never instantiated by CWA itself and
    //    lacks entries concrete classes carry -- creating it crashed Building's constructor
    //    and tripped "Size: not an array" -- so it stands for its first concrete descendant.
    const ParamClass* c = a1->GetClassInterface();
    for (int guard = 0; c && guard < 40; guard++)
    {
        const char* base = c->GetBaseName();
        if (!base || !*base)
            break;
        if (const ParamEntry* cwa = CwaVehicle(base))
        {
            const ParamEntry* scope = cwa->FindEntry("scope");
            if (scope && (int)*scope >= 1)
                return RString(base);
            if (const RString concrete = ConcreteCwaDescendant(base); concrete.GetLength() > 0)
                return concrete;
        }
        const ParamEntry* next = FindConfigClass("CfgVehicles", base);
        c = next ? next->GetClassInterface() : nullptr;
    }
    return RString();
}

// Arma 1 soldiers carry their own weapons with the Arma 1 models unless OP_A1_WEAPONS=0.
bool Arma1OwnWeapons()
{
    static const bool on = [] {
        const char* e = std::getenv("OP_A1_WEAPONS");
        return !(e && *e == '0');
    }();
    return on;
}

namespace
{
bool A2Derives(const ParamEntry* cls, const char* base)
{
    const ParamEntry* b = FindConfigClass("CfgWeapons", base);
    const ParamClass* c = cls ? cls->GetClassInterface() : nullptr;
    const ParamClass* bc = b ? b->GetClassInterface() : nullptr;
    return c && bc && (c == bc || c->IsDerivedFrom(*bc));
}

// The CWA weapon for an Arma 2 weapon class, by ancestry and ammunition.
std::string A2Calibre(const ParamEntry* weapon)
{
    // first magazine -> its ammo name ("B_545x39_Ball", "R_PG7V_AT", ...)
    const ParamEntry* mags = weapon ? weapon->FindEntry("magazines") : nullptr;
    if (!mags || !mags->IsArray() || mags->GetSize() == 0)
        return {};
    const ParamEntry* mag = FindConfigClass("CfgMagazines", (const char*)(RStringB)(*mags)[0]);
    return Lower(mag ? StringValue(*mag, "ammo") + " " + std::string((const char*)(RStringB)(*mags)[0])
                     : std::string((const char*)(RStringB)(*mags)[0]));
}

const char* CwaWeaponFor(const std::string& lowerName, const ParamEntry* weapon, const std::string& calibre)
{
    const auto has = [&](const char* w) { return calibre.find(w) != std::string::npos || lowerName.find(w) != std::string::npos; };
    if (A2Derives(weapon, "Binocular") || lowerName.find("binocular") != std::string::npos ||
        lowerName.find("laserdesignator") != std::string::npos)
        return "Binocular";
    if (A2Derives(weapon, "NVGoggles") || lowerName.find("nvgoggles") != std::string::npos)
        return "NVGoggles";
    const bool launcher = A2Derives(weapon, "LauncherCore") || A2Derives(weapon, "Launcher");
    if (launcher)
    {
        if (has("pg7") || has("rpg") || has("og7"))
            return "RPGLauncher";
        // CWA's AA launchers: AALauncher (Stinger) and 9K32Launcher (Strela).
        if (has("stinger") || has("igla") || has("strela"))
            return has("stinger") ? "AALauncher" : "9K32Launcher";
        // The M136 is the AT4.
        if (has("m136"))
            return "AT4Launcher";
        if (has("m72") || has("law"))
            return "LAWLauncher";
        if (has("at13") || has("metis") || has("javelin") || has("dragon") || has("at5") || has("konkurs"))
            return "AT4Launcher";
        return "CarlGustavLauncher";
    }
    const bool pistol = A2Derives(weapon, "PistolCore") || A2Derives(weapon, "Pistol");
    if (pistol)
        return has("9x18") || has("makarov") ? "Tokarev" : has("9x19") ? "Beretta" : "Revolver";
    const bool mgun = A2Derives(weapon, "MGunCore") || A2Derives(weapon, "MGun") || has("m249") || has("m240") ||
                      has("mk_48") || has("pk") || has("rpk") || has("100rnd") || has("200rnd");
    const bool sniper = has("m24") || has("m40") || has("dmr") || has("svd") || has("ksvk") || has("m107") ||
                        has("as50") || has("vss") || has("sniper") || has("ksvk");
    if (has("127x") || has("762x54") || has("762x51"))
    {
        if (mgun && !sniper)
            return has("762x54") ? "PK" : "M60";
        return has("762x54") || has("127x108") ? "SVDDragunov" : "M21";
    }
    const bool gl = lowerName.find("gl") != std::string::npos || lowerName.find("gp25") != std::string::npos ||
                    lowerName.find("m203") != std::string::npos;
    if (has("545x39"))
        return mgun ? "PK" : gl ? "AK74GrenadeLauncher" : (has("aks") ? "AK74SU" : "AK74");
    if (has("762x39"))
        return mgun ? "PK" : "AK47";
    if (has("556x45"))
        return mgun ? "M60" : gl ? "M16GrenadeLauncher" : "M16";
    if (has("9x19") || has("mp5") || has("bizon") || has("9x18"))
        return "HK";
    if (has("9x39") || has("vss"))
        return "SVDDragunov";
    if (has("22_lr") || has("hmr") || has("huntingrifle"))
        return "HuntingRifle";
    if (A2Derives(weapon, "RifleCore") || A2Derives(weapon, "Rifle"))
        return "M16";
    return nullptr;
}
} // namespace

RString Arma2WeaponFallback(const RString& name)
{
    if (Generation() < 1 || !ConfigLoaded() || name.GetLength() == 0)
        return RString();
    const std::string lower = Lower(name.Data());
    const ParamEntry* cwaWeapons = Pars.FindEntry("CfgWeapons");
    if (!cwaWeapons)
        return RString();
    // A weapon
    if (const ParamEntry* weapon = FindConfigClass("CfgWeapons", name); weapon && weapon->IsClass())
    {
        const char* cwa = CwaWeaponFor(lower, weapon, A2Calibre(weapon));
        return cwa && cwaWeapons->FindEntry(cwa) ? RString(cwa) : RString();
    }
    // A magazine: grenades, explosives by name; else the CWA weapon of its calibre,
    // whose own first magazine stands for it.
    const ParamEntry* mag = FindConfigClass("CfgMagazines", name);
    if (!mag || !mag->IsClass())
        return RString();
    const std::string ammo = Lower(StringValue(*mag, "ammo"));
    const auto any = [&](std::initializer_list<const char*> words)
    {
        for (const char* w : words)
            if (lower.find(w) != std::string::npos || ammo.find(w) != std::string::npos)
                return true;
        return false;
    };
    const char* direct = nullptr;
    if (any({"smokeshellred"}))
        direct = "SmokeShellRed";
    else if (any({"smokeshellgreen"}))
        direct = "SmokeShellGreen";
    else if (any({"smokeshell", "smoke"}))
        direct = "SmokeShell";
    else if (any({"handgrenade", "grenadehand"}))
        direct = "HandGrenade";
    else if (any({"pipebomb", "satchel"}))
        direct = "PipeBomb";
    else if (any({"timebomb"}))
        direct = "TimeBomb";
    else if (any({"minee", "mine_e", "tm46"}))
        direct = "MineE";
    else if (any({"mine"}))
        direct = "Mine";
    else if (any({"m203", "gp25", "40mm", "g_40"}))
        direct = "GrenadeLauncher";
    if (direct)
        return cwaWeapons->FindEntry(direct) ? RString(direct) : RString();
    // launcher rounds: the CWA launcher of the kind, whose own magazine stands for them
    // (PG7VL and OG7 found no counterpart at all; a magazine has no ancestry to test)
    const char* weapon = nullptr;
    if (any({"pg7", "og7", "rpg"}))
        weapon = "RPGLauncher";
    else if (any({"smaw", "maaws", "carlgustav"}))
        weapon = "CarlGustavLauncher";
    else if (any({"m136", "m72", "law"}))
        weapon = "LAWLauncher";
    else if (any({"stinger"}))
        weapon = "AALauncher";
    else if (any({"strela", "igla"}))
        weapon = "9K32Launcher";
    else if (any({"at13", "metis", "javelin", "dragon", "at5", "konkurs"}))
        weapon = "AT4Launcher";
    if (!weapon)
        weapon = CwaWeaponFor(lower, nullptr, ammo + " " + lower);
    if (!weapon)
        return RString();
    const ParamEntry* w = cwaWeapons->FindEntry(weapon);
    if (!w)
        return RString();
    // OFP's magazines[]={"this"} means the weapon is its own magazine (PK, M21, the
    // launchers): "this" names no class, and every RPK, DMR, Bizon and RPG round looked
    // up that way found no CWA magazine (run 17: 75Rnd_545x39_RPK, 20Rnd_762x51_DMR, ...).
    if (const ParamEntry* mags = w->FindEntry("magazines"); mags && mags->IsArray() && mags->GetSize() > 0)
    {
        const RString first = (RStringB)(*mags)[0];
        if (stricmp(first.Data(), "this") != 0)
            return first;
    }
    return RString(weapon);
}

bool Arma2IsHandWeapon(const RString& name)
{
    if (Generation() != 2 || !ConfigLoaded() || name.GetLength() == 0)
        return false;
    const ParamEntry* weapon = FindConfigClass("CfgWeapons", name);
    if (!weapon || !weapon->IsClass())
        return false;
    return A2Derives(weapon, "RifleCore") || A2Derives(weapon, "Rifle") || A2Derives(weapon, "PistolCore") ||
           A2Derives(weapon, "Pistol") || A2Derives(weapon, "LauncherCore") || A2Derives(weapon, "Launcher");
}

RString Arma2WeaponModelClass(const RString& name, const RString& cwaWeapon)
{
    if (Generation() < 1 || !ConfigLoaded() || name.GetLength() == 0 || cwaWeapon.GetLength() == 0)
        return cwaWeapon;
    if (Generation() == 1 && !Arma1OwnWeapons())
        return cwaWeapon;
    static const bool off = [] {
        const char* e = std::getenv("OP_A2_WEAPON_MODELS");
        return e && *e == '0';
    }();
    if (off)
        return cwaWeapon;
    const ParamEntry* a2 = FindConfigClass("CfgWeapons", name);
    const ParamEntry* cwaWeapons = Pars.FindEntry("CfgWeapons");
    const ParamEntry* cwa = cwaWeapons ? cwaWeapons->FindEntry(cwaWeapon) : nullptr;
    if (!a2 || !a2->IsClass() || !cwa)
        return cwaWeapon;
    // Only weapons carry a model of their own here; binoculars and goggles keep CWA's.
    if (A2Derives(a2, "Binocular") || A2Derives(a2, "NVGoggles"))
        return cwaWeapon;
    std::string model = StringValue(*a2, "model");
    if (model.empty() || model == "\\" || model == "-")
        return cwaWeapon;
    std::string shape = model;
    if (shape.size() < 4 || Lower(shape.substr(shape.size() - 4)) != ".p3d")
        shape += ".p3d";
    if (!QIFStreamB::FileExist(shape[0] == '\\' ? shape.c_str() + 1 : shape.c_str()))
        return cwaWeapon;

    const std::string generated = "A1x_" + std::string(name.Data());
    if (cwaWeapons->FindEntry(generated.c_str()))
        return RString(generated.c_str());
    std::string text = "class CfgWeapons\r\n{\r\n\tclass " + std::string(cwaWeapon.Data()) + ";\r\n\tclass " +
                       generated + ": " + std::string(cwaWeapon.Data()) + "\r\n\t{\r\n";
    text += "\t\tmodel=\"" + model + "\";\r\n";
    const std::string displayName = StringValue(*a2, "displayName");
    if (!displayName.empty())
        text += "\t\tdisplayName=\"" + displayName + "\";\r\n";
    // Arma 1: the gear and briefing show the Arma 1 weapon's own picture
    // ("\CA\weapons\data\equip\w_m4aim_ca.paa") instead of the CWA carrier's.
    if (Generation() == 1)
    {
        std::string picture = StringValue(*a2, "picture");
        if (picture.size() > 1 && picture[0] == '\\')
        {
            std::string file = picture.substr(1);
            if (file.size() < 4 || file[file.size() - 4] != '.')
                file += ".paa";
            if (QIFStreamB::FileExist(file.c_str()))
                text += "\t\tpicture=\"" + picture + "\";\r\n";
        }
    }
    if (const ParamEntry* scope = cwa->FindEntry("scope"); !scope || (int)*scope < 1)
        text += "\t\tscope=1;\r\n";
    // OFP weapons often list magazines[]={this}: the magazine is the weapon class itself
    // (M60, RPGLauncher, LAWLauncher). Inherited, "this" would name the generated class,
    // which no soldier carries -- the weapon stayed empty (A1x_M240 with M60 belts: 0
    // rounds). The generated class names CWA's magazines explicitly.
    if (const ParamEntry* mags = cwa->FindEntry("magazines"); mags && mags->IsArray())
    {
        bool hasThis = false;
        std::string list;
        for (int i = 0; i < mags->GetSize(); i++)
        {
            std::string m = (const char*)(RStringB)(*mags)[i];
            if (Lower(m) == "this")
            {
                hasThis = true;
                m = cwaWeapon.Data();
            }
            list += std::string(i ? "," : "") + "\"" + m + "\"";
        }
        if (hasThis)
            text += "\t\tmagazines[]={" + list + "};\r\n";
    }
    text += "\t};\r\n};\r\n";
    ImportIntoPars(text, generated.c_str());
    if (!(Pars >> "CfgWeapons").FindEntry(generated.c_str()))
        return cwaWeapon;
    LOG_INFO(Mission, "{}: weapon '{}' uses the Arma model {} on CWA '{}'", Label(), name.Data(), model,
             cwaWeapon.Data());
    return RString(generated.c_str());
}

namespace
{
// CWA magazine (lower case) -> magazines generated on it.
std::map<std::string, std::vector<RString>>& GeneratedMagazines()
{
    static std::map<std::string, std::vector<RString>> generated;
    return generated;
}
} // namespace

std::vector<RString> GeneratedMagazinesFor(const char* cwaMagazine)
{
    if (!Active() || !cwaMagazine || !*cwaMagazine)
        return {};
    const auto found = GeneratedMagazines().find(Lower(cwaMagazine));
    return found != GeneratedMagazines().end() ? found->second : std::vector<RString>();
}

bool GeneratedMagazineFits(const char* weaponClass, const char* magazine)
{
    if (!Active() || !weaponClass || !magazine || _strnicmp(weaponClass, "A1x_", 4) != 0 ||
        _strnicmp(magazine, "A1x_", 4) != 0)
        return true;
    const ParamEntry* a1 = FindConfigClass("CfgWeapons", weaponClass + 4);
    if (!a1 || !a1->IsClass())
        return true;
    const char* armaMagazine = magazine + 4;
    bool listed = false;
    bool any = false;
    const auto scan = [&](const ParamEntry& cls)
    {
        const ParamEntry* mags = cls.FindEntry("magazines");
        if (!mags || !mags->IsArray())
            return;
        for (int i = 0; i < mags->GetSize(); i++)
        {
            any = true;
            const RString listedName = (RStringB)(*mags)[i];
            if (_stricmp(listedName.Data(), armaMagazine) == 0)
                listed = true;
        }
    };
    scan(*a1);
    if (const ParamEntry* muzzles = a1->FindEntry("muzzles"); muzzles && muzzles->IsArray())
        for (int i = 0; i < muzzles->GetSize(); i++)
        {
            const RString muzzle = (RStringB)(*muzzles)[i];
            if (muzzle.GetLength() == 0 || _stricmp(muzzle.Data(), "this") == 0)
                continue;
            if (const ParamEntry* cls = a1->FindEntry(muzzle.Data()); cls && cls->IsClass())
                scan(*cls);
        }
    return listed || !any;
}

RString Arma1MagazineClass(const RString& name, const RString& cwaMagazine)
{
    if (Generation() != 1 || !ConfigLoaded() || !Arma1OwnWeapons() || name.GetLength() == 0 ||
        cwaMagazine.GetLength() == 0 || _stricmp(name.Data(), cwaMagazine.Data()) == 0 ||
        _strnicmp(name.Data(), "A1x_", 4) == 0)
        return cwaMagazine;
    static const bool off = [] {
        const char* e = std::getenv("OP_A1_MAGAZINES");
        return e && *e == '0';
    }();
    if (off)
        return cwaMagazine;
    const ParamEntry* a1 = FindConfigClass("CfgMagazines", name);
    if (!a1 || !a1->IsClass())
        return cwaMagazine;
    // Arma 1 names some launcher rounds like the launcher (M136, Stinger): A1x_<name> is
    // the weapon's class there, and one round per launcher is what CWA's carrier holds too.
    if (FindConfigClass("CfgWeapons", name))
        return cwaMagazine;
    const ParamEntry* cwaWeapons = Pars.FindEntry("CfgWeapons");
    const ParamEntry* cwa = cwaWeapons ? cwaWeapons->FindEntry(cwaMagazine) : nullptr;
    if (!cwa || !cwa->IsClass())
        return cwaMagazine;
    const ParamEntry* a1Count = a1->FindEntry("count");
    const ParamEntry* cwaCount = cwa->FindEntry("count");
    if (!a1Count || !cwaCount || a1Count->IsClass() || a1Count->IsArray() || cwaCount->IsClass() || cwaCount->IsArray())
        return cwaMagazine;
    const int count = static_cast<int>(std::lround((float)*a1Count));
    const int carrierCount = static_cast<int>(std::lround((float)*cwaCount));
    if (count <= 0 || count > 10000 || count == carrierCount)
        return cwaMagazine;

    const std::string generated = "A1x_" + std::string(name.Data());
    if (!cwaWeapons->FindEntry(generated.c_str()))
    {
        std::string text = "class CfgWeapons\r\n{\r\n\tclass " + std::string(cwaMagazine.Data()) + ";\r\n\tclass " +
                           generated + ": " + std::string(cwaMagazine.Data()) + "\r\n\t{\r\n";
        text += "\t\tcount=" + std::to_string(count) + ";\r\n";
        const std::string displayName = StringValue(*a1, "displayName");
        if (!displayName.empty())
            text += "\t\tdisplayNameMagazine=\"" + displayName + "\";\r\n";
        const std::string shortName = StringValue(*a1, "displayNameShort");
        if (!shortName.empty())
            text += "\t\tshortNameMagazine=\"" + shortName + "\";\r\n";
        text += "\t};\r\n};\r\n";
        ImportIntoPars(text, generated.c_str());
        if (!(Pars >> "CfgWeapons").FindEntry(generated.c_str()))
            return cwaMagazine;
        GeneratedMagazines()[Lower(cwaMagazine.Data())].push_back(RString(generated.c_str()));
        // Muzzles built before this magazine existed learn it now; later ones read it
        // through GeneratedMagazinesFor when they are built.
        AddMagazineToMuzzlesTaking(cwaMagazine, RString(generated.c_str()));
        LOG_INFO(Mission, "{}: magazine '{}' holds {} rounds on CWA '{}' ({})", Label(), name.Data(), count,
                 cwaMagazine.Data(), carrierCount);
    }
    return RString(generated.c_str());
}

namespace
{
// weapons[] and magazines[] of an Arma 2 soldier class, each resolved to what this
// game can carry (A1x_ weapons with the Arma 2 model, CWA magazines of the same
// calibre). Items CWA has no counterpart for (ItemMap, ItemRadio, ...) are left out.
std::string Arma2Loadout(const ParamEntry& soldier)
{
    const ParamEntry* cwaWeapons = Pars.FindEntry("CfgWeapons");
    if (!cwaWeapons)
        return {};
    std::string out;
    const auto list = [&](const char* key, RString (*resolve)(const RString&))
    {
        const ParamEntry* arr = soldier.FindEntry(key);
        if (!arr || !arr->IsArray())
            return;
        std::string items;
        int n = 0;
        for (int i = 0; i < arr->GetSize(); i++)
        {
            const RString name = (RStringB)(*arr)[i];
            if (name.GetLength() == 0)
                continue;
            const RString cwa = resolve(name);
            if (cwa.GetLength() == 0 || !cwaWeapons->FindEntry(cwa))
                continue;
            items += std::string(n++ ? "," : "") + "\"" + std::string(cwa.Data()) + "\"";
        }
        out += std::string("\t\t") + key + "[]={" + items + "};\r\n";
    };
    list("weapons", &ResolveWeaponClass);
    list("magazines", &ResolveMagazineClass);
    return out;
}

// TransportWeapons / TransportMagazines of an Arma 1 crate or vehicle, resolved like a
// soldier's loadout: A1x_ weapons with the Arma 1 models, CWA magazines of the same
// calibre. Inherited, the CWA class's own cargo would hand out CWA weapons from an Arma 1
// crate. Entries with no CWA counterpart are left out; `items` counts what is kept.
std::string Arma1Cargo(const ParamEntry& cls, int& items)
{
    items = 0;
    const ParamEntry* cwaWeapons = Pars.FindEntry("CfgWeapons");
    if (!cwaWeapons)
        return {};
    std::string out;
    const auto section = [&](const char* key, const char* field, RString (*resolve)(const RString&))
    {
        const ParamEntry* sec = cls.FindEntry(key);
        if (!sec || !sec->IsClass())
            return;
        out += std::string("\t\tclass ") + key + "\r\n\t\t{\r\n";
        for (int i = 0; i < sec->GetEntryCount(); i++)
        {
            const ParamEntry& item = sec->GetEntry(i);
            if (!item.IsClass())
                continue;
            const std::string name = StringValue(item, field);
            const ParamEntry* countEntry = item.FindEntry("count");
            const int count =
                countEntry && !countEntry->IsClass() && !countEntry->IsArray() ? (int)(float)*countEntry : 0;
            if (name.empty() || count <= 0)
                continue;
            const RString cwa = resolve(RString(name.c_str()));
            if (cwa.GetLength() == 0 || !cwaWeapons->FindEntry(cwa))
                continue;
            out += "\t\t\tclass " + std::string((const char*)item.GetName()) + "\r\n\t\t\t{\r\n\t\t\t\t" + field +
                   "=\"" + std::string(cwa.Data()) + "\";\r\n\t\t\t\tcount=" + std::to_string(count) +
                   ";\r\n\t\t\t};\r\n";
            items++;
        }
        out += "\t\t};\r\n";
    };
    section("TransportWeapons", "weapon", &ResolveWeaponClass);
    section("TransportMagazines", "magazine", &ResolveMagazineClass);
    for (const char* key : {"transportMaxWeapons", "transportMaxMagazines"})
    {
        const ParamEntry* e = cls.FindEntry(key);
        if (e && !e->IsClass() && !e->IsArray())
        {
            const float value = (float)*e;
            if (value > 0.0f && value < 1e6f)
                out += std::string("\t\t") + key + "=" + std::to_string((int)value) + ";\r\n";
        }
    }
    return out;
}
} // namespace

RString Arma2ModuleInit(const RString& className)
{
    if (Generation() != 2 || !ConfigLoaded() || className.GetLength() == 0)
        return RString();
    const ParamEntry* cls = FindConfigClass("CfgVehicles", className);
    if (!cls || !cls->IsClass() || !IsKindOf(cls, "Logic"))
        return RString();
    const ParamEntry* handlers = cls->FindEntry("EventHandlers");
    if (!handlers || !handlers->IsClass())
        handlers = cls->FindEntry("Eventhandlers");
    const std::string code = handlers && handlers->IsClass() ? StringValue(*handlers, "init") : std::string();
    if (code.empty())
        return RString();
    LOG_INFO(Mission, "{}: module '{}' starts from its init event handler", Label(), className.Data());
    return RString(code.c_str());
}

bool ProxyKind(const char* modelName, std::string& simulation, int& crewPosition)
{
    simulation.clear();
    crewPosition = -1;
    if (!Active() || !ConfigLoaded() || !modelName || !*modelName)
        return false;
    const char* leaf = modelName;
    for (const char* q = modelName; *q; q++)
        if (*q == '\\' || *q == '/')
            leaf = q + 1;
    std::string name = leaf;
    // "cargo.01" style sequence suffixes are not part of the class name
    if (const size_t dot = name.find('.'); dot != std::string::npos)
        name.resize(dot);
    const ParamEntry* cls = FindConfigClass("CfgNonAIVehicles", ("Proxy" + name).c_str());
    if (!cls || !cls->IsClass())
        return false;
    simulation = Lower(StringValue(*cls, "simulation"));
    if (simulation == "proxycrew")
    {
        const std::string pos = Lower(StringValue(*cls, "crewPosition"));
        crewPosition = pos == "cpdriver" ? 0 : pos == "cpgunner" ? 1 : pos == "cpcommander" ? 2 : pos == "cpcargo" ? 3 : -1;
    }
    return !simulation.empty();
}

RString Arma1ModelClass(const RString& missionClass, const RString& cwaClass)
{
    if (!Active() || !ConfigLoaded())
        return cwaClass;
    const ParamEntry* a1 = FindConfigClass("CfgVehicles", missionClass);
    const ParamEntry* cwaVehicles = Pars.FindEntry("CfgVehicles");
    const ParamEntry* cwa = cwaVehicles ? cwaVehicles->FindEntry(cwaClass) : nullptr;
    if (!a1 || !cwa)
        return cwaClass;
    const bool man = IsKindOf(a1, "CAManBase") || IsKindOf(a1, "Man");
    // Arma 2 soldiers use their own models and moves (binarised BMTR animations, converted
    // on load). OP_A2_SOLDIERS=0 brings back the CWA soldier's model and moves; the soldier
    // still carries his own weapons.
    static const bool a2Soldiers = [] {
        const char* e = std::getenv("OP_A2_SOLDIERS");
        return !(e && *e == '0');
    }();
    std::string model = StringValue(*a1, "model");
    bool useModel = !(model.empty() || model == "\\" || model.find("bmp.p3d") != std::string::npos);
    if (useModel)
    {
        std::string shape = model;
        if (shape.size() < 4 || Lower(shape.substr(shape.size() - 4)) != ".p3d")
            shape += ".p3d";
        useModel = QIFStreamB::FileExist(shape[0] == '\\' ? shape.c_str() + 1 : shape.c_str());
    }
    if (man && ((Generation() == 2 && !a2Soldiers) || !MovesImported()))
        useModel = false;
    // Arma 1 soldiers (with their own model and moves) carry their own weapons too: the
    // Arma 1 weapons[] and magazines[], shown with the Arma 1 weapon models (OP_A1_WEAPONS=0
    // keeps the CWA soldier's loadout).
    const bool loadout = man && (Generation() == 2 || (Generation() == 1 && useModel && Arma1OwnWeapons()));
    if (!useModel && !loadout)
        return cwaClass;

    const std::string generated = "A1x_" + std::string(missionClass.Data());
    if (cwaVehicles->FindEntry(generated.c_str()))
        return RString(generated.c_str());

    std::string text = "class CfgVehicles\r\n{\r\n\tclass " + std::string(cwaClass.Data()) + ";\r\n\tclass " +
                       generated + ": " + std::string(cwaClass.Data()) + "\r\n\t{\r\n";
    if (useModel)
        text += "\t\tmodel=\"" + model + "\";\r\n";
    // A scope-0 base makes the generated class abstract too: VehicleTypes then builds a
    // plain EntityAIType while NewVehicle still constructs by simulation (a Building on a
    // type that is no BuildingType -- the 0xC0000005 in Building's constructor).
    if (const ParamEntry* scope = cwa->FindEntry("scope"); !scope || (int)*scope < 1)
        text += "\t\tscope=1;\r\n";
    const std::string displayName = StringValue(*a1, "displayName");
    if (!displayName.empty())
        text += "\t\tdisplayName=\"" + displayName + "\";\r\n";
    if (loadout)
        text += Arma2Loadout(*a1);
    // Arma 1 crates and vehicles hand out Arma 1 weapons (OP_A1_WEAPONS=0: CWA's cargo).
    int cargoItems = 0;
    if (!man && useModel && Generation() == 1 && Arma1OwnWeapons())
        text += Arma1Cargo(*a1, cargoItems);
    if (useModel && Generation() == 2)
    {
        // Arma 2 colours one model several ways through hiddenSelections plus
        // hiddenSelectionsTextures (the LHD deck crew's jerseys and helmets, desert and
        // woodland vehicles). Only selections WITH a texture are carried over: an OFP
        // hidden selection without one is hidden, an Arma 2 one keeps its model texture.
        const ParamEntry* sels = a1->FindEntry("hiddenSelections");
        const ParamEntry* texs = a1->FindEntry("hiddenSelectionsTextures");
        if (sels && sels->IsArray() && texs && texs->IsArray())
        {
            std::string names, textures;
            int n = 0;
            for (int i = 0; i < sels->GetSize() && i < texs->GetSize(); i++)
            {
                const std::string sel = Lower((const char*)(RStringB)(*sels)[i]);
                std::string tex = (const char*)(RStringB)(*texs)[i];
                if (sel.empty() || tex.empty())
                    continue;
                // a texture this install does not have would hide the selection instead
                if (tex[0] != '#' && !QIFStreamB::FileExist(tex[0] == '\\' ? tex.c_str() + 1 : tex.c_str()))
                    continue;
                names += std::string(n ? "," : "") + "\"" + sel + "\"";
                textures += std::string(n ? "," : "") + "\"" + tex + "\"";
                n++;
            }
            if (n > 0)
                text += "\t\thiddenSelections[]={" + names + "};\r\n\t\thiddenSelectionsTextures[]={" + textures +
                        "};\r\n";
        }
    }
    if (!man && useModel && Generation() == 2)
    {
        // A vehicle with the Arma 2 model: its own speed, armour, fuel and seats, so a
        // Ural is not as fast as the CWA truck behind it and a Black Hawk seats its own
        // cargo. Steering, turrets and weapons stay with the CWA class for now.
        const std::string sim = Lower(StringValue(*a1, "simulation"));
        if (sim == "car" || sim == "tank" || sim == "helicopter" || sim == "airplane" || sim == "ship" ||
            sim == "motorcycle")
        {
            // Its crew: the Arma 2 crew class (a TK tank is crewed by TK crewmen, not CWA's
            // SoldierECrew), resolved like any mission class.
            if (const std::string crew = StringValue(*a1, "crew"); !crew.empty())
            {
                const RString resolved = ResolveVehicleClass(RString(crew.c_str()));
                if (const ParamEntry* now = Pars.FindEntry("CfgVehicles"); resolved.GetLength() > 0 && now && now->FindEntry(resolved))
                    text += "\t\tcrew=\"" + std::string(resolved.Data()) + "\";\r\n";
            }
            for (const char* key : {"maxSpeed", "armor", "fuelCapacity", "transportSoldier"})
            {
                const ParamEntry* e = a1->FindEntry(key);
                if (e && !e->IsClass() && !e->IsArray())
                {
                    const float value = (float)*e;
                    if ((value > 0.0f || (value == 0.0f && strcmp(key, "transportSoldier") == 0)) && value < 1e6f)
                    {
                        char line[96];
                        std::snprintf(line, sizeof(line), "\t\t%s=%g;\r\n", key, value);
                        text += line;
                    }
                }
            }
        }
    }
    if (man && useModel)
    {
        text += "\t\tmoves=\"A1_CfgMovesMaleSdr\";\r\n";
        for (const char* key : kManNameKeys)
        {
            const std::string value = StringValue(*a1, key);
            if (!value.empty() || a1->FindEntry(key))
                text += std::string("\t\t") + key + "=\"" + value + "\";\r\n";
        }
        static const char* hits[][2] = {{"HitHead", "hitNameHead"},
                                        {"HitBody", "hitNameBody"},
                                        {"HitHands", "hitNameHands"},
                                        {"HitLegs", "hitNameLegs"}};
        for (const auto& hit : hits)
        {
            const ParamEntry* h = a1->FindEntry(hit[0]);
            if (h && h->IsClass())
            {
                const std::string name = StringValue(*h, "name");
                if (!name.empty())
                    text += std::string("\t\t") + hit[1] + "=\"" + name + "\";\r\n";
            }
        }
        // Skeleton bones the OFP code names in Czech.
        text += "\t\tboneLauncher=\"launcher\";\r\n\t\tboneLeftHand=\"lefthand\";\r\n\t\tboneRightHand=\"righthand\";\r\n";
        // An Arma 2 man is stored facing -z like every Arma/OFP model (toes, eyes and the
        // "pilot" camera point lie at -z), so he keeps CWA's reversed=1 and is turned at
        // load; his moves (BMTR) pose the turned model (ConvertBMTRToRTM, measured with
        // diag30). OP_A2_MAN_XMIRROR=1 brings back the earlier pair: moves read mirrored in
        // x and the man left unturned (reversed=0), which made him his own mirror image --
        // limbs to the front, face to the back, the first-person camera looking out of the
        // back of his head at his backpack.
        static const bool a2ManXMirror = [] {
            const char* e = std::getenv("OP_A2_MAN_XMIRROR");
            return e && *e == '1';
        }();
        if (Generation() == 2 && a2ManXMirror)
            text += "\t\treversed=0;\r\n";
    }
    text += "\t};\r\n};\r\n";
    ImportIntoPars(text, generated.c_str());
    if (!(Pars >> "CfgVehicles").FindEntry(generated.c_str()))
        return cwaClass;
    if (useModel)
        LOG_INFO(Mission, "{}: '{}' uses the Arma 1 model {} on CWA '{}'{}", Label(), missionClass.Data(), model,
                 cwaClass.Data(), man ? " with the Arma 1 moves" : "");
    if (cargoItems > 0)
        LOG_INFO(Mission, "{}: '{}' carries its own cargo ({} weapon/magazine entries)", Label(), missionClass.Data(),
                 cargoItems);
    else
        LOG_INFO(Mission, "{}: '{}' is the CWA soldier '{}' with its own weapons", Label(), missionClass.Data(),
                 cwaClass.Data());
    return RString(generated.c_str());
}
} // namespace Poseidon::Arma1Compat
