#include <Poseidon/Game/Commands/GameStateExt.hpp>
#include <Poseidon/Game/Commands/GameStateExtCommon.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Poseidon
{
RString GetUserDirectory();
void CreatePath(RString path);
} // namespace Poseidon

namespace
{
enum class DatabaseValueKind : std::uint8_t
{
    Null,
    String,
    Number,
    Boolean,
    List,
    Hash,
};

struct DatabaseValue
{
    DatabaseValueKind kind = DatabaseValueKind::Null;
    std::string string;
    float number = 0.0f;
    bool boolean = false;
    std::vector<DatabaseValue> list;
    std::unordered_map<std::string, DatabaseValue> hash;

    static DatabaseValue MakeHash()
    {
        DatabaseValue value;
        value.kind = DatabaseValueKind::Hash;
        return value;
    }
};

struct DatabaseEntry
{
    DatabaseValue root = DatabaseValue::MakeHash();
    int refCount = 0;
};

AutoArray<DatabaseEntry> GDatabaseEntries;

static DatabaseEntry* GetDatabaseEntry(GameValuePar value)
{
    if (value.GetType() != GameDatabase)
        return nullptr;

    const auto database = static_cast<GameDataDatabase*>(value.GetData())->GetDatabase();
    const int index = database.GetIndex();
    if (index < 0 || index >= GDatabaseEntries.Size())
        return nullptr;

    DatabaseEntry& entry = GDatabaseEntries[index];
    return entry.refCount > 0 ? &entry : nullptr;
}

static std::string ToString(GameValuePar value)
{
    return std::string((const char*)static_cast<RString>(value));
}

static RString DatabaseFilename(GameValuePar value)
{
    const RString filename = value;
    if (filename.GetLength() == 0 || strchr(filename, '/') || strchr(filename, '\\'))
        return RString();

    return Poseidon::GetUserDirectory() + RString("Config/") + filename;
}

static GameDatabaseType NewDatabaseHandle()
{
    DatabaseEntry entry;
    const int index = GDatabaseEntries.Add(entry);
    GameDatabaseType database;
    database.SetIndex(index);
    return database;
}

static bool SplitPath(const std::string& path, std::vector<std::string>& parts)
{
    if (path.empty() || path.front() == '.' || path.back() == '.')
        return false;

    size_t begin = 0;
    while (begin < path.size())
    {
        const size_t end = path.find('.', begin);
        const size_t length = end == std::string::npos ? path.size() - begin : end - begin;
        if (length == 0)
            return false;
        parts.emplace_back(path, begin, length);
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return !parts.empty();
}

static DatabaseValue* FindValue(DatabaseValue& root, const std::string& path, bool create)
{
    std::vector<std::string> parts;
    if (!SplitPath(path, parts))
        return nullptr;

    DatabaseValue* current = &root;
    for (const std::string& part : parts)
    {
        if (current->kind != DatabaseValueKind::Hash)
            return nullptr;

        auto found = current->hash.find(part);
        if (found == current->hash.end())
        {
            if (!create)
                return nullptr;
            found = current->hash.emplace(part, DatabaseValue::MakeHash()).first;
        }
        current = &found->second;
    }
    return current;
}

static DatabaseValue* FindParent(DatabaseValue& root, const std::string& path, std::string& leaf, bool create)
{
    const size_t separator = path.rfind('.');
    if (separator == std::string::npos)
    {
        leaf = path;
        return &root;
    }
    leaf = path.substr(separator + 1);
    return FindValue(root, path.substr(0, separator), create);
}

static bool ToDatabaseValue(GameValuePar source, DatabaseValue& destination)
{
    switch (source.GetType())
    {
        case GameString:
            destination = {};
            destination.kind = DatabaseValueKind::String;
            destination.string = ToString(source);
            return true;
        case GameScalar:
            destination = {};
            destination.kind = DatabaseValueKind::Number;
            destination.number = (float)source;
            return true;
        case GameBool:
            destination = {};
            destination.kind = DatabaseValueKind::Boolean;
            destination.boolean = (bool)source;
            return true;
        case GameArray:
        {
            destination = {};
            destination.kind = DatabaseValueKind::List;
            const GameArrayType& sourceArray = source;
            destination.list.reserve(sourceArray.Size());
            for (int i = 0; i < sourceArray.Size(); ++i)
            {
                DatabaseValue item;
                if (!ToDatabaseValue(sourceArray[i], item))
                    return false;
                destination.list.push_back(std::move(item));
            }
            return true;
        }
        default:
            return false;
    }
}

static GameValue ToGameValue(const GameState* state, const DatabaseValue& source)
{
    switch (source.kind)
    {
        case DatabaseValueKind::String:
            return RString(source.string.c_str());
        case DatabaseValueKind::Number:
            return source.number;
        case DatabaseValueKind::Boolean:
            return source.boolean;
        case DatabaseValueKind::List:
        {
            GameValue result = state->CreateGameValue(GameArray);
            GameArrayType& array = result;
            array.Resize((int)source.list.size());
            for (int i = 0; i < array.Size(); ++i)
                array[i] = ToGameValue(state, source.list[i]);
            return result;
        }
        case DatabaseValueKind::Hash:
        {
            GameValue result = state->CreateGameValue(GameArray);
            GameArrayType& array = result;
            for (const auto& [key, value] : source.hash)
            {
                GameValue pair = state->CreateGameValue(GameArray);
                GameArrayType& pairArray = pair;
                pairArray.Resize(2);
                pairArray[0] = RString(key.c_str());
                pairArray[1] = ToGameValue(state, value);
                array.Add(pair);
            }
            return result;
        }
        default:
            return RString();
    }
}

static GameValue DefaultValue(const GameArrayType& arguments, int index)
{
    return arguments.Size() > index ? arguments[index] : GameValue(RString());
}

static bool WriteBytes(std::ofstream& stream, const void* data, size_t size)
{
    stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return stream.good();
}

static bool ReadBytes(std::ifstream& stream, void* data, size_t size)
{
    stream.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    return stream.good();
}

static bool WriteValue(std::ofstream& stream, const DatabaseValue& value)
{
    const auto kind = static_cast<std::uint8_t>(value.kind);
    if (!WriteBytes(stream, &kind, sizeof(kind)))
        return false;

    switch (value.kind)
    {
        case DatabaseValueKind::String:
        {
            const std::uint32_t size = static_cast<std::uint32_t>(value.string.size());
            return WriteBytes(stream, &size, sizeof(size)) && WriteBytes(stream, value.string.data(), size);
        }
        case DatabaseValueKind::Number:
            return WriteBytes(stream, &value.number, sizeof(value.number));
        case DatabaseValueKind::Boolean:
            return WriteBytes(stream, &value.boolean, sizeof(value.boolean));
        case DatabaseValueKind::List:
        {
            const std::uint32_t count = static_cast<std::uint32_t>(value.list.size());
            if (!WriteBytes(stream, &count, sizeof(count)))
                return false;
            for (const DatabaseValue& item : value.list)
                if (!WriteValue(stream, item))
                    return false;
            return true;
        }
        case DatabaseValueKind::Hash:
        {
            const std::uint32_t count = static_cast<std::uint32_t>(value.hash.size());
            if (!WriteBytes(stream, &count, sizeof(count)))
                return false;
            for (const auto& [key, item] : value.hash)
            {
                const std::uint32_t size = static_cast<std::uint32_t>(key.size());
                if (!WriteBytes(stream, &size, sizeof(size)) || !WriteBytes(stream, key.data(), size) ||
                    !WriteValue(stream, item))
                    return false;
            }
            return true;
        }
        default:
            return true;
    }
}

static bool ReadValue(std::ifstream& stream, DatabaseValue& value, int depth = 0)
{
    if (depth > 64)
        return false;

    std::uint8_t rawKind = 0;
    if (!ReadBytes(stream, &rawKind, sizeof(rawKind)) || rawKind > static_cast<std::uint8_t>(DatabaseValueKind::Hash))
        return false;
    value = {};
    value.kind = static_cast<DatabaseValueKind>(rawKind);

    switch (value.kind)
    {
        case DatabaseValueKind::String:
        {
            std::uint32_t size = 0;
            if (!ReadBytes(stream, &size, sizeof(size)) || size > 16 * 1024 * 1024)
                return false;
            value.string.resize(size);
            return ReadBytes(stream, value.string.data(), size);
        }
        case DatabaseValueKind::Number:
            return ReadBytes(stream, &value.number, sizeof(value.number));
        case DatabaseValueKind::Boolean:
            return ReadBytes(stream, &value.boolean, sizeof(value.boolean));
        case DatabaseValueKind::List:
        {
            std::uint32_t count = 0;
            if (!ReadBytes(stream, &count, sizeof(count)) || count > 1'000'000)
                return false;
            value.list.resize(count);
            for (DatabaseValue& item : value.list)
                if (!ReadValue(stream, item, depth + 1))
                    return false;
            return true;
        }
        case DatabaseValueKind::Hash:
        {
            std::uint32_t count = 0;
            if (!ReadBytes(stream, &count, sizeof(count)) || count > 1'000'000)
                return false;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                std::uint32_t size = 0;
                if (!ReadBytes(stream, &size, sizeof(size)) || size > 16 * 1024 * 1024)
                    return false;
                std::string key(size, '\0');
                if (!ReadBytes(stream, key.data(), size))
                    return false;
                DatabaseValue item;
                if (!ReadValue(stream, item, depth + 1))
                    return false;
                value.hash.emplace(std::move(key), std::move(item));
            }
            return true;
        }
        default:
            return true;
    }
}

void AddDatabaseRef(int index)
{
    if (index >= 0 && index < GDatabaseEntries.Size())
        ++GDatabaseEntries[index].refCount;
}

void ReleaseDatabaseRef(int index)
{
    if (index < 0 || index >= GDatabaseEntries.Size())
        return;
    DatabaseEntry& entry = GDatabaseEntries[index];
    if (entry.refCount > 0)
        --entry.refCount;
}
} // namespace

GameDatabaseType::GameDatabaseType(const GameDatabaseType& src) : _index(-1)
{
    SetIndex(src.GetIndex());
}

GameDatabaseType::~GameDatabaseType()
{
    SetIndex(-1);
}

void GameDatabaseType::operator=(const GameDatabaseType& src)
{
    if (this != &src)
        SetIndex(src.GetIndex());
}

void GameDatabaseType::SetIndex(int index)
{
    if (_index == index)
        return;
    if (_index >= 0)
        ReleaseDatabaseRef(_index);
    _index = index;
    if (_index >= 0)
        AddDatabaseRef(_index);
}

DEFINE_FAST_ALLOCATOR(GameDataDatabase)

RString GameDataDatabase::GetText() const
{
    if (_value.GetIndex() < 0 || _value.GetIndex() >= GDatabaseEntries.Size())
        return "No database";
    return "Database";
}

bool GameDataDatabase::IsEqualTo(const GameData* data) const
{
    return _value.GetIndex() == static_cast<const GameDataDatabase*>(data)->GetDatabase().GetIndex();
}

LSError GameDataDatabase::Serialize(ParamArchive& ar)
{
    PARAM_CHECK(base::Serialize(ar));
    if (ar.IsLoading())
        _value.SetIndex(-1);
    return LSOK;
}

GameData* CreateGameDataDatabase()
{
    return new GameDataDatabase();
}

GameValue DatabaseNew(const GameState* /*state*/)
{
    return GameValueExt(NewDatabaseHandle());
}

GameValue DatabaseLoad(const GameState* /*state*/, GameValuePar oper1)
{
    const RString filename = DatabaseFilename(oper1);
    if (filename.GetLength() == 0)
        return GameValueExt(GameDatabaseType());

    std::ifstream input((const char*)filename, std::ios::binary);
    if (!input)
    {
        LOG_ERROR(Core, "[DATABASE] Load failed for '{}'", (const char*)filename);
        return GameValueExt(GameDatabaseType());
    }

    char magic[4]{};
    std::uint32_t version = 0;
    DatabaseValue root;
    if (!ReadBytes(input, magic, sizeof(magic)) || std::memcmp(magic, "PDB1", 4) != 0 ||
        !ReadBytes(input, &version, sizeof(version)) || version != 1 || !ReadValue(input, root) ||
        root.kind != DatabaseValueKind::Hash)
    {
        LOG_ERROR(Core, "[DATABASE] Load failed for '{}' (invalid database format)", (const char*)filename);
        return GameValueExt(GameDatabaseType());
    }

    const GameDatabaseType database = NewDatabaseHandle();
    GDatabaseEntries[database.GetIndex()].root = std::move(root);
    return GameValueExt(database);
}

GameValue DatabaseGet(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    DatabaseEntry* database = GetDatabaseEntry(oper1);
    if (!database || oper2.GetType() != GameArray)
        return RString();
    const GameArrayType& arguments = oper2;
    if (arguments.Size() < 1 || arguments[0].GetType() != GameString)
        return RString();
    DatabaseValue* value = FindValue(database->root, ToString(arguments[0]), false);
    return value ? ToGameValue(state, *value) : DefaultValue(arguments, 1);
}

GameValue DatabaseSet(const GameState* /*state*/, GameValuePar oper1, GameValuePar oper2)
{
    DatabaseEntry* database = GetDatabaseEntry(oper1);
    if (!database || oper2.GetType() != GameArray)
        return false;
    const GameArrayType& arguments = oper2;
    if (arguments.Size() != 2 || arguments[0].GetType() != GameString)
        return false;

    DatabaseValue value;
    if (!ToDatabaseValue(arguments[1], value))
        return false;
    std::string leaf;
    DatabaseValue* parent = FindParent(database->root, ToString(arguments[0]), leaf, true);
    if (!parent || parent->kind != DatabaseValueKind::Hash || leaf.empty())
        return false;
    parent->hash[leaf] = std::move(value);
    return true;
}

GameValue DatabaseDelete(const GameState* /*state*/, GameValuePar oper1, GameValuePar oper2)
{
    DatabaseEntry* database = GetDatabaseEntry(oper1);
    if (!database || oper2.GetType() != GameString)
        return false;
    std::string leaf;
    DatabaseValue* parent = FindParent(database->root, ToString(oper2), leaf, false);
    if (!parent || parent->kind != DatabaseValueKind::Hash)
        return false;
    return parent->hash.erase(leaf) > 0;
}

GameValue DatabaseExists(const GameState* /*state*/, GameValuePar oper1, GameValuePar oper2)
{
    DatabaseEntry* database = GetDatabaseEntry(oper1);
    return database && oper2.GetType() == GameString && FindValue(database->root, ToString(oper2), false) != nullptr;
}

GameValue DatabaseKeys(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    GameValue result = state->CreateGameValue(GameArray);
    GameArrayType& keys = result;
    DatabaseEntry* database = GetDatabaseEntry(oper1);
    if (!database || oper2.GetType() != GameString)
        return result;
    DatabaseValue* value = FindValue(database->root, ToString(oper2), false);
    if (!value || value->kind != DatabaseValueKind::Hash)
        return result;
    for (const auto& [key, ignored] : value->hash)
        keys.Add(RString(key.c_str()));
    return result;
}

GameValue DatabaseSave(const GameState* /*state*/, GameValuePar oper1, GameValuePar oper2)
{
    DatabaseEntry* database = GetDatabaseEntry(oper1);
    const RString filename = oper2.GetType() == GameString ? DatabaseFilename(oper2) : RString();
    if (!database || filename.GetLength() == 0)
        return false;

    Poseidon::CreatePath(filename);
    const std::string temp = std::string((const char*)filename) + ".tmp";
    std::ofstream output(temp, std::ios::binary | std::ios::trunc);
    const char magic[] = "PDB1";
    const std::uint32_t version = 1;
    const bool written = output && WriteBytes(output, magic, 4) && WriteBytes(output, &version, sizeof(version)) &&
                         WriteValue(output, database->root);
    output.close();
    if (!written || !output)
    {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        LOG_ERROR(Core, "[DATABASE] Save failed for '{}'", (const char*)filename);
        return false;
    }

    std::error_code error;
    std::filesystem::remove(std::filesystem::path((const char*)filename), error);
    error.clear();
    std::filesystem::rename(temp, std::filesystem::path((const char*)filename), error);
    if (error)
    {
        std::filesystem::remove(temp, error);
        LOG_ERROR(Core, "[DATABASE] Save failed for '{}' (filesystem error {})", (const char*)filename, error.value());
        return false;
    }
    LOG_INFO(Core, "[DATABASE] Saved '{}'", (const char*)filename);
    return true;
}

static DatabaseValue* GetList(DatabaseEntry* database, const std::string& key)
{
    DatabaseValue* value = FindValue(database->root, key, false);
    return value && value->kind == DatabaseValueKind::List ? value : nullptr;
}

static DatabaseValue* GetHash(DatabaseEntry* database, const std::string& key, bool create)
{
    DatabaseValue* value = FindValue(database->root, key, create);
    if (!value)
        return nullptr;
    if (value->kind == DatabaseValueKind::Null && create)
        value->kind = DatabaseValueKind::Hash;
    return value->kind == DatabaseValueKind::Hash ? value : nullptr;
}

GameValue DatabaseListPush(const GameState* /*state*/, GameValuePar oper1, GameValuePar oper2)
{
    DatabaseEntry* database = GetDatabaseEntry(oper1);
    if (!database || oper2.GetType() != GameArray)
        return -1.0f;
    const GameArrayType& arguments = oper2;
    if (arguments.Size() != 2 || arguments[0].GetType() != GameString)
        return -1.0f;
    std::string leaf;
    DatabaseValue* parent = FindParent(database->root, ToString(arguments[0]), leaf, true);
    if (!parent || parent->kind != DatabaseValueKind::Hash)
        return -1.0f;
    auto found = parent->hash.find(leaf);
    if (found == parent->hash.end())
        found = parent->hash.emplace(leaf, DatabaseValue()).first;
    DatabaseValue* value = &found->second;
    if (value->kind == DatabaseValueKind::Null)
        value->kind = DatabaseValueKind::List;
    if (value->kind != DatabaseValueKind::List)
        return -1.0f;
    DatabaseValue item;
    if (!ToDatabaseValue(arguments[1], item))
        return -1.0f;
    value->list.push_back(std::move(item));
    return (float)value->list.size();
}

GameValue DatabaseListPushMany(const GameState* /*state*/, GameValuePar oper1, GameValuePar oper2)
{
    DatabaseEntry* database = GetDatabaseEntry(oper1);
    if (!database || oper2.GetType() != GameArray)
        return -1.0f;
    const GameArrayType& arguments = oper2;
    if (arguments.Size() != 2 || arguments[0].GetType() != GameString || arguments[1].GetType() != GameArray)
        return -1.0f;

    std::vector<DatabaseValue> items;
    const GameArrayType& source = arguments[1];
    items.reserve(source.Size());
    for (int i = 0; i < source.Size(); ++i)
    {
        DatabaseValue item;
        if (!ToDatabaseValue(source[i], item))
            return -1.0f;
        items.push_back(std::move(item));
    }

    std::string leaf;
    DatabaseValue* parent = FindParent(database->root, ToString(arguments[0]), leaf, true);
    if (!parent || parent->kind != DatabaseValueKind::Hash)
        return -1.0f;
    auto found = parent->hash.find(leaf);
    if (found == parent->hash.end())
        found = parent->hash.emplace(leaf, DatabaseValue()).first;
    DatabaseValue& list = found->second;
    if (list.kind == DatabaseValueKind::Null)
        list.kind = DatabaseValueKind::List;
    if (list.kind != DatabaseValueKind::List)
        return -1.0f;
    for (DatabaseValue& item : items)
        list.list.push_back(std::move(item));
    return (float)list.list.size();
}

GameValue DatabaseListPop(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    DatabaseEntry* database = GetDatabaseEntry(oper1);
    if (!database || oper2.GetType() != GameArray)
        return RString();
    const GameArrayType& arguments = oper2;
    if (arguments.Size() < 1 || arguments[0].GetType() != GameString)
        return RString();
    DatabaseValue* list = GetList(database, ToString(arguments[0]));
    if (!list || list->list.empty())
        return DefaultValue(arguments, 2);
    size_t index = list->list.size() - 1;
    if (arguments.Size() > 1)
    {
        if (arguments[1].GetType() != GameScalar || (float)arguments[1] < 0)
            return DefaultValue(arguments, 2);
        index = static_cast<size_t>((float)arguments[1]);
        if (index >= list->list.size())
            return DefaultValue(arguments, 2);
    }
    DatabaseValue result = std::move(list->list[index]);
    list->list.erase(list->list.begin() + index);
    return ToGameValue(state, result);
}

GameValue DatabaseListGet(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    DatabaseEntry* database = GetDatabaseEntry(oper1);
    if (!database || oper2.GetType() != GameArray)
        return RString();
    const GameArrayType& arguments = oper2;
    if (arguments.Size() < 2 || arguments[0].GetType() != GameString || arguments[1].GetType() != GameScalar)
        return DefaultValue(arguments, 2);
    DatabaseValue* list = GetList(database, ToString(arguments[0]));
    const int index = (int)(float)arguments[1];
    return list && index >= 0 && index < (int)list->list.size() ? ToGameValue(state, list->list[index])
                                                               : DefaultValue(arguments, 2);
}

GameValue DatabaseListSet(const GameState* /*state*/, GameValuePar oper1, GameValuePar oper2)
{
    DatabaseEntry* database = GetDatabaseEntry(oper1);
    if (!database || oper2.GetType() != GameArray)
        return false;
    const GameArrayType& arguments = oper2;
    if (arguments.Size() != 3 || arguments[0].GetType() != GameString || arguments[1].GetType() != GameScalar)
        return false;
    DatabaseValue* list = GetList(database, ToString(arguments[0]));
    const int index = (int)(float)arguments[1];
    DatabaseValue item;
    if (!list || index < 0 || index >= (int)list->list.size() || !ToDatabaseValue(arguments[2], item))
        return false;
    list->list[index] = std::move(item);
    return true;
}

GameValue DatabaseListRemove(const GameState* /*state*/, GameValuePar oper1, GameValuePar oper2)
{
    DatabaseEntry* database = GetDatabaseEntry(oper1);
    if (!database || oper2.GetType() != GameArray)
        return false;
    const GameArrayType& arguments = oper2;
    if (arguments.Size() != 2 || arguments[0].GetType() != GameString || arguments[1].GetType() != GameScalar)
        return false;
    DatabaseValue* list = GetList(database, ToString(arguments[0]));
    const int index = (int)(float)arguments[1];
    if (!list || index < 0 || index >= (int)list->list.size())
        return false;
    list->list.erase(list->list.begin() + index);
    return true;
}

GameValue DatabaseListSize(const GameState* /*state*/, GameValuePar oper1, GameValuePar oper2)
{
    DatabaseEntry* database = GetDatabaseEntry(oper1);
    DatabaseValue* list = database && oper2.GetType() == GameString ? GetList(database, ToString(oper2)) : nullptr;
    return list ? (float)list->list.size() : 0.0f;
}

GameValue DatabaseHashSet(const GameState* /*state*/, GameValuePar oper1, GameValuePar oper2)
{
    DatabaseEntry* database = GetDatabaseEntry(oper1);
    if (!database || oper2.GetType() != GameArray)
        return false;
    const GameArrayType& arguments = oper2;
    if (arguments.Size() != 3 || arguments[0].GetType() != GameString || arguments[1].GetType() != GameString)
        return false;
    DatabaseValue* hash = GetHash(database, ToString(arguments[0]), true);
    DatabaseValue item;
    if (!hash || !ToDatabaseValue(arguments[2], item))
        return false;
    hash->hash[ToString(arguments[1])] = std::move(item);
    return true;
}

GameValue DatabaseHashSetMany(const GameState* /*state*/, GameValuePar oper1, GameValuePar oper2)
{
    DatabaseEntry* database = GetDatabaseEntry(oper1);
    if (!database || oper2.GetType() != GameArray)
        return false;
    const GameArrayType& arguments = oper2;
    if (arguments.Size() != 2 || arguments[0].GetType() != GameString || arguments[1].GetType() != GameArray)
        return false;

    struct HashAssignment
    {
        std::string key;
        DatabaseValue value;
    };
    std::vector<HashAssignment> assignments;
    const GameArrayType& pairs = arguments[1];
    assignments.reserve(pairs.Size());
    for (int i = 0; i < pairs.Size(); ++i)
    {
        if (pairs[i].GetType() != GameArray)
            return false;
        const GameArrayType& pair = pairs[i];
        if (pair.Size() != 2 || pair[0].GetType() != GameString)
            return false;
        DatabaseValue value;
        if (!ToDatabaseValue(pair[1], value))
            return false;
        assignments.push_back({ToString(pair[0]), std::move(value)});
    }

    DatabaseValue* hash = GetHash(database, ToString(arguments[0]), true);
    if (!hash)
        return false;
    for (HashAssignment& assignment : assignments)
        hash->hash[std::move(assignment.key)] = std::move(assignment.value);
    return true;
}

GameValue DatabaseHashGet(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    DatabaseEntry* database = GetDatabaseEntry(oper1);
    if (!database || oper2.GetType() != GameArray)
        return RString();
    const GameArrayType& arguments = oper2;
    if (arguments.Size() < 2 || arguments[0].GetType() != GameString || arguments[1].GetType() != GameString)
        return DefaultValue(arguments, 2);
    DatabaseValue* hash = GetHash(database, ToString(arguments[0]), false);
    if (!hash)
        return DefaultValue(arguments, 2);
    auto found = hash->hash.find(ToString(arguments[1]));
    return found == hash->hash.end() ? DefaultValue(arguments, 2) : ToGameValue(state, found->second);
}

GameValue DatabaseHashDelete(const GameState* /*state*/, GameValuePar oper1, GameValuePar oper2)
{
    DatabaseEntry* database = GetDatabaseEntry(oper1);
    if (!database || oper2.GetType() != GameArray)
        return false;
    const GameArrayType& arguments = oper2;
    if (arguments.Size() != 2 || arguments[0].GetType() != GameString || arguments[1].GetType() != GameString)
        return false;
    DatabaseValue* hash = GetHash(database, ToString(arguments[0]), false);
    return hash && hash->hash.erase(ToString(arguments[1])) > 0;
}

GameValue DatabaseHashExists(const GameState* /*state*/, GameValuePar oper1, GameValuePar oper2)
{
    DatabaseEntry* database = GetDatabaseEntry(oper1);
    if (!database || oper2.GetType() != GameArray)
        return false;
    const GameArrayType& arguments = oper2;
    if (arguments.Size() != 2 || arguments[0].GetType() != GameString || arguments[1].GetType() != GameString)
        return false;
    DatabaseValue* hash = GetHash(database, ToString(arguments[0]), false);
    return hash && hash->hash.find(ToString(arguments[1])) != hash->hash.end();
}

GameValue DatabaseHashKeys(const GameState* state, GameValuePar oper1, GameValuePar oper2)
{
    GameValue result = state->CreateGameValue(GameArray);
    GameArrayType& keys = result;
    DatabaseEntry* database = GetDatabaseEntry(oper1);
    DatabaseValue* hash = database && oper2.GetType() == GameString ? GetHash(database, ToString(oper2), false) : nullptr;
    if (hash)
        for (const auto& [key, ignored] : hash->hash)
            keys.Add(RString(key.c_str()));
    return result;
}
