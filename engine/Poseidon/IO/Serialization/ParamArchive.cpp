#include <Poseidon/Core/Application.hpp>
#include <Poseidon/IO/Serialization/ParamArchive.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>

#include <Poseidon/Foundation/Time/Time.hpp>
#include <Poseidon/Core/Global.hpp>

#include <Poseidon/Foundation/Enums/EnumNames.hpp>
#include <limits.h>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/platform.hpp>

using namespace Poseidon;
using Poseidon::Foundation::EnumName;
using Poseidon::Foundation::Time;
using Poseidon::Foundation::TimeSec;

void TraceError(const char* command)
{
    RptF("Error in statement: %s", command);
}

#define CHECK_INTERNAL(command) \
    {                           \
        LSError err = command;  \
        if (err != LSOK)        \
            return err;         \
    }

ParamArchive::ParamArchive()
{
    _version = -1; // invalid
    _pass = PassUndefined;
    _params = nullptr;
}

RString ParamArchive::_errorContext;

LSError ParamArchive::CheckMaxVersion(int maxVersion)
{
    if (IsLoading() && GetArVersion() > maxVersion)
    {
        OnError(LSVersionTooNew, "version");
        return LSVersionTooNew;
    }
    return LSOK;
}

void ParamArchive::OnError(LSError err, RString context)
{
    PoseidonAssert(_entry);
    const char* member = context.GetLength() == 0 ? nullptr : context;
    _errorContext = _entry->GetContext(member);
}

void ParamArchive::CancelError()
{
    _errorContext = "";
}

const char* ParamArchive::GetErrorName(LSError err)
{
    switch (err)
    {
        case LSOK:
            return "No error";
        case LSFileNotFound:
            return "No such file";
        case LSBadFile:
            return "Bad file (CRC, ...)";
        case LSStructure:
            return "Bad file structure";
        case LSUnsupportedFormat:
            return "Unsupported format";
        case LSVersionTooNew:
            return "Version is too new";
        case LSVersionTooOld:
            return "Version is too old";
        case LSDiskFull:
            return "No such file";
        case LSAccessDenied:
            return "Access denied";
        case LSDiskError:
            return "Disk error";
        case LSNoEntry:
            return "No entry";
        default:
            Fail("LSError");
        case LSUnknownError:
            return "Unknown error";
    }
}

bool ParamArchive::IsSubclass(const RStringB& name)
{
    if (_saving)
    {
        return true;
    }
    else
    {
        return _entry->FindEntry(name) != nullptr;
    }
}

bool ParamArchive::OpenSubclass(const RStringB& name, ParamArchive& ar, bool guaranteedUnique)
{
    ar._version = _version;
    ar._saving = _saving;
    ar._pass = _pass;
    ar._params = _params;

    if (_saving)
    {
        ar._entry = _entry->AddClass(name, guaranteedUnique);
    }
    else
    {
        ParamEntry* entry = _entry->FindEntry(name);
        if (!entry)
        {
            return false;
        }
        ar._entry = entry->GetClassInterface();
    }
    return ar._entry != nullptr;
}

void ParamArchive::CloseSubclass(ParamArchive& ar)
{
    if (_saving)
    {
        ar._entry->Compact();
        ar._entry = nullptr;
    }
}

LSError ParamArchive::Serialize(const RStringB& name, bool& value, int minVersion)
{
    if (_version < minVersion)
    {
        return LSOK;
    }
    if (_saving)
    {
        _entry->Add(name, value);
    }
    else
    {
        if (_pass != PassFirst)
        {
            return LSOK;
        }
        ParamEntry* entry = _entry->FindEntry(name);
        if (!entry)
            ON_ERROR(LSNoEntry, name);
        int intValue = *entry;
        value = intValue != 0;
    }
    return LSOK;
}

LSError ParamArchive::Serialize(const RStringB& name, int& value, int minVersion)
{
    if (_version < minVersion)
    {
        return LSOK;
    }
    if (_saving)
    {
        _entry->Add(name, value);
    }
    else
    {
        if (_pass != PassFirst)
        {
            return LSOK;
        }
        ParamEntry* entry = _entry->FindEntry(name);
        if (!entry)
            ON_ERROR(LSNoEntry, name);
        value = *entry;
    }
    return LSOK;
}

LSError ParamArchive::Serialize(const RStringB& name, unsigned char& value, int minVersion)
{
    if (_version < minVersion)
    {
        return LSOK;
    }
    if (_saving)
    {
        _entry->Add(name, value);
    }
    else
    {
        if (_pass != PassFirst)
        {
            return LSOK;
        }
        ParamEntry* entry = _entry->FindEntry(name);
        if (!entry)
            ON_ERROR(LSNoEntry, name);
        value = entry->GetInt();
    }
    return LSOK;
}

LSError ParamArchive::Serialize(const RStringB& name, float& value, int minVersion)
{
    if (_version < minVersion)
    {
        return LSOK;
    }
    if (_saving)
    {
        _entry->Add(name, value);
    }
    else
    {
        if (_pass != PassFirst)
        {
            return LSOK;
        }
        ParamEntry* entry = _entry->FindEntry(name);
        if (!entry)
            ON_ERROR(LSNoEntry, name);
        value = *entry;
    }
    return LSOK;
}

LSError ParamArchive::Serialize(const RStringB& name, Time& value, int minVersion)
{
    if (_version < minVersion)
    {
        return LSOK;
    }
    if (_saving)
    {
        __int64 nDiff = (__int64)value.toInt() - (__int64)Poseidon::Glob.time.toInt();
        float fDiff = 1e-3 * nDiff;
        _entry->Add(name, fDiff);
    }
    else
    {
        if (_pass != PassFirst)
        {
            return LSOK;
        }
        ParamEntry* entry = _entry->FindEntry(name);
        if (!entry)
            ON_ERROR(LSNoEntry, name);
        float fDiff = *entry;
        __int64 nVal = (__int64)(1e3 * fDiff) + (__int64)Poseidon::Glob.time.toInt();
        if (nVal < -INT_MAX)
        {
            nVal = -INT_MAX;
        }
        else if (nVal > INT_MAX)
        {
            nVal = INT_MAX;
        }
        value = Time((int)nVal);
    }
    return LSOK;
}

LSError ParamArchive::Serialize(const RStringB& name, TimeSec& value, int minVersion)
{
    if (_version < minVersion)
    {
        return LSOK;
    }
    if (_saving)
    {
        __int64 nDiff = (__int64)Time(value).toInt() - (__int64)Poseidon::Glob.time.toInt();
        float fDiff = 1e-3 * nDiff;
        _entry->Add(name, fDiff);
    }
    else
    {
        if (_pass != PassFirst)
        {
            return LSOK;
        }
        ParamEntry* entry = _entry->FindEntry(name);
        if (!entry)
            ON_ERROR(LSNoEntry, name);
        float fDiff = *entry;
        __int64 nVal = (__int64)(1e3 * fDiff) + (__int64)Poseidon::Glob.time.toInt();
        if (nVal < -INT_MAX)
        {
            nVal = -INT_MAX;
        }
        else if (nVal > INT_MAX)
        {
            nVal = INT_MAX;
        }
        value = TimeSec(Time((int)nVal));
    }
    return LSOK;
}

LSError ParamArchive::Serialize(const RStringB& name, RString& value, int minVersion)
{
    if (_version < minVersion)
    {
        return LSOK;
    }
    if (_saving)
    {
        _entry->Add(name, value);
    }
    else
    {
        if (_pass != PassFirst)
        {
            return LSOK;
        }
        ParamEntry* entry = _entry->FindEntry(name);
        if (!entry)
            ON_ERROR(LSNoEntry, name);
        value = *entry;
    }
    return LSOK;
}

LSError ParamArchive::Serialize(const RStringB& name, Vector3& value, int minVersion)
{
    if (_version < minVersion)
    {
        return LSOK;
    }
    if (_saving)
    {
        ParamEntry* array = _entry->AddArray(name);
        array->ReserveArrayElements(3);
        for (int i = 0; i < 3; i++)
        {
            array->AddValue(value[i]);
        }
        array->Compact();
    }
    else
    {
        if (_pass != PassFirst)
        {
            return LSOK;
        }
        ParamEntry* array = _entry->FindEntry(name);
        if (!array)
            ON_ERROR(LSNoEntry, name);
        if (!array->IsArray())
            ON_ERROR(LSStructure, name);
        if (array->GetSize() != 3)
            ON_ERROR(LSStructure, name);
        for (int i = 0; i < 3; i++)
        {
            value[i] = (*array)[i];
        }
    }
    return LSOK;
}

LSError ParamArchive::Serialize(const RStringB& name, Matrix4& value, int minVersion)
{
    if (_version < minVersion)
    {
        return LSOK;
    }
    ParamArchive arSubcls;
    if (!OpenSubclass(name, arSubcls))
        ON_ERROR(LSNoEntry, name);
    if (_saving)
    {
        Vector3& pos = const_cast<Vector3&>(value.Position());
        Vector3& dir = const_cast<Vector3&>(value.Direction());
        Vector3& up = const_cast<Vector3&>(value.DirectionUp());
        Vector3& aside = const_cast<Vector3&>(value.DirectionAside());
        PARAM_CHECK(arSubcls.Serialize("pos", pos, 1))
        PARAM_CHECK(arSubcls.Serialize("dir", dir, 1))
        PARAM_CHECK(arSubcls.Serialize("up", up, 1))
        PARAM_CHECK(arSubcls.Serialize("aside", aside, 1))
    }
    else
    {
        if (_pass != PassFirst)
        {
            return LSOK;
        }
        Vector3 pos;
        Vector3 dir;
        Vector3 up;
        Vector3 aside;
        PARAM_CHECK(arSubcls.Serialize("pos", pos, 1))
        PARAM_CHECK(arSubcls.Serialize("dir", dir, 1))
        PARAM_CHECK(arSubcls.Serialize("up", up, 1))
        PARAM_CHECK(arSubcls.Serialize("aside", aside, 1))
        value.SetPosition(pos);
        value.SetDirection(dir);
        value.SetDirectionUp(up);
        value.SetDirectionAside(aside);
    }
    CloseSubclass(arSubcls);
    return LSOK;
}

LSError ParamArchive::Serialize(const RStringB& name, Color& value, int minVersion)
{
    if (_version < minVersion)
    {
        return LSOK;
    }
    if (_saving)
    {
        // Note: use a static array (AUTO_STATIC_ARRAY) here
        AutoArray<float> array;
        array.Resize(4);
        array[0] = value.R();
        array[1] = value.G();
        array[2] = value.B();
        array[3] = value.A();
        PARAM_CHECK(SerializeArray(name, array, 1))
    }
    else
    {
        if (_pass != PassFirst)
        {
            return LSOK;
        }
        AutoArray<float> array;
        PARAM_CHECK(SerializeArray(name, array, 1))
        PoseidonAssert(array.Size() == 4);
        value = Color(array[0], array[1], array[2], array[3]);
    }
    return LSOK;
}

LSError ParamArchive::Serialize(const RStringB& name, SerializeClass& value, int minVersion)
{
    if (_version < minVersion)
    {
        return LSOK;
    }
    // call Serialize for both passes
    ParamArchive arSubcls;
    if (!OpenSubclass(name, arSubcls))
        ON_ERROR(LSNoEntry, name);
    LSError err = value.Serialize(arSubcls);
    CloseSubclass(arSubcls);
    return err;
}

LSError ParamArchive::SerializeArray(const RStringB& name, AutoArray<Vector3>& value, int minVersion)
{
    if (_version < minVersion)
    {
        return LSOK;
    }
    if (!_saving && _pass != PassFirst)
    {
        return LSOK;
    }
    ParamArchive arCls;
    if (!OpenSubclass(name, arCls))
        ON_ERROR(LSNoEntry, name);
    int n;
    if (_saving)
    {
        n = value.Size();
        PARAM_CHECK(arCls.Serialize("items", n, minVersion))
    }
    else
    {
        PARAM_CHECK(arCls.Serialize("items", n, minVersion))
        value.Realloc(n);
        value.Resize(n);
    }
    for (int i = 0; i < n; i++)
    {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "vector%d", i);
        PARAM_CHECK(arCls.Serialize(buffer, value[i], minVersion))
    }
    CloseSubclass(arCls);
    return LSOK;
}

LSError ParamArchive::SerializeEnumValue(const RStringB& name, int& value, int minVersion, const EnumName* names)
{
    if (GetArVersion() < minVersion)
    {
        return LSOK;
    }
    if (IsSaving())
    {
        bool found = false;
        RString str = "ERROR";
        int i;
        for (i = 0; names[i].IsValid(); i++)
        {
            if (names[i].value == value)
            {
                str = names[i].name;
                found = true;
                break;
            }
        }
        if (!found)
        {
            int val = value;
            RptF("Saving undefined enum value %d / %d, context %s", val, i, (const char*)_entry->GetContext(name));
        }
        CHECK_INTERNAL(Serialize(name, str, minVersion))
        return LSOK;
    }
    else
    {
        if (GetPass() != ParamArchive::PassFirst)
        {
            return LSOK;
        }
        RString str;
        CHECK_INTERNAL(Serialize(name, str, minVersion))

        for (int i = 0; names[i].IsValid(); i++)
        {
            if (stricmp(names[i].name, str) == 0)
            {
                value = names[i].value;
                return LSOK;
            }
        }
        ON_ERROR(LSStructure, name);
    }
}

LSError ParamArchive::Serialize(const RStringB& name, bool& value, int minVersion, bool defValue) PARS_4_TO_3 LSError
    ParamArchive::Serialize(const RStringB& name, int& value, int minVersion, int defValue) PARS_4_TO_3 LSError
    ParamArchive::Serialize(const RStringB& name, unsigned char& value, int minVersion, unsigned char defValue)
        PARS_4_TO_3 LSError ParamArchive::Serialize(const RStringB& name, float& value, int minVersion, float defValue)
            PARS_4_TO_3 LSError ParamArchive::Serialize(const RStringB& name, Time& value, int minVersion,
                                                        Time defValue) PARS_4_TO_3 LSError
    ParamArchive::Serialize(const RStringB& name, TimeSec& value, int minVersion, TimeSec defValue) PARS_4_TO_3 LSError
    ParamArchive::Serialize(const RStringB& name, RString& value, int minVersion, RString defValue) PARS_4_TO_3 LSError
    ParamArchive::Serialize(const RStringB& name, Vector3& value, int minVersion, Vector3Par defValue)
        PARS_4_TO_3 LSError ParamArchive::Serialize(const RStringB& name, Matrix4& value, int minVersion,
                                                    Matrix4& defValue) PARS_4_TO_3_NODEFAULTS LSError
    ParamArchive::Serialize(const RStringB& name, Color& value, int minVersion, Color defValue) PARS_4_TO_3_NODEFAULTS

    ParamArchiveLoad::ParamArchiveLoad()
{
    _open = false;
    _saving = false;
    _pass = PassFirst;
    _params = nullptr;
    _file = new ParamFile;
    _entry = _file;
}

ParamArchiveLoad::ParamArchiveLoad(const char* name, void* params)
{
    _open = false;
    _saving = false;
    _pass = PassFirst;
    _params = params;
    _file = new ParamFile;
    _entry = _file;
    Load(name);
}

ParamArchiveLoad::~ParamArchiveLoad()
{
    if (_open)
    {
        Close();
    }
}

ParamFile* ParamArchiveLoad::GetParamFile()
{
    PoseidonAssert(_entry);
    ParamEntry* entry = _entry;
    PoseidonAssert(dynamic_cast<ParamFile*>(entry));
    return static_cast<ParamFile*>(entry);
}

LSError ParamArchiveLoad::Load(const char* name)
{
    if (_open)
    {
        Close();
    }
    GetParamFile()->Parse(name);
    ParamEntry* entry = _entry->FindEntry("version");
    if (!entry)
        ON_ERROR(LSNoEntry, "version");
    _version = *entry;
    _open = true;
    return LSOK;
}

bool ParamArchiveLoad::LoadBin(const char* name)
{
    if (_open)
    {
        Close();
    }
    bool result = GetParamFile()->ParseBin(name);
    ParamEntry* entry = _entry->FindEntry("version");
    if (!entry)
    {
        OnError(LSNoEntry, name);
        return false;
    }
    _version = *entry;
    _open = true;
    return result;
}

void ParamArchiveLoad::Close()
{
    if (!_open)
    {
        return;
    }
    GetParamFile()->Clear();
}

ParamArchiveSave::ParamArchiveSave(int version, void* params)
{
    _saved = false;
    _saving = true;
    _version = version;
    _params = params;
    _file = new ParamFile;
    _entry = _file;
    _entry->Add("version", version);
}

ParamArchiveSave::~ParamArchiveSave()
{
    if (!_saved)
    {
        LOG_DEBUG(Core, "Warning: Archive not saved");
    }
}

ParamFile* ParamArchiveSave::GetParamFile()
{
    PoseidonAssert(_entry);
    ParamEntry* entry = _entry;
    PoseidonAssert(dynamic_cast<ParamFile*>(entry));
    return static_cast<ParamFile*>(entry);
}

LSError ParamArchiveSave::Parse(const char* name)
{
    // used for config files - not all values will be serialized
    LSError err = GetParamFile()->Parse(name);
    _entry->Add("version", _version);
    return err;
}

LSError ParamArchiveSave::Save(const char* name)
{
    if (_saved)
    {
        LOG_DEBUG(Core, "Warning: Archive saved twice");
    }

    LSError err = GetParamFile()->Save(name);
    if (err == LSOK)
    {
        _saved = true;
    }
    return err;
}

bool ParamArchiveSave::ParseBin(const char* name)
{
    // used for config files - not all values will be serialized
    bool result = GetParamFile()->ParseBin(name);
    _entry->Add("version", _version);
    return result;
}

bool ParamArchiveSave::SaveBin(const char* name)
{
    if (_saved)
    {
        LOG_DEBUG(Core, "Warning: Archive saved twice");
    }

    _saved = GetParamFile()->SaveBin(name);
    return _saved;
}
