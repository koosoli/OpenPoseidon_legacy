#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/IO/Streams/ArchiveSourceBinding.hpp>
#include <Poseidon/IO/Streams/ModelCompressedSourceBirth.hpp>
#include <Poseidon/IO/Streams/PatnikArchiveReason.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/Core/ModSystem.hpp>
#ifndef _WIN32
#include <climits>
#include <dirent.h>
#endif
#include <cctype>
#include <cstring>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <string>
#include <fstream>
#include <atomic>
#include <cstdlib>
#include <vector>
#include <Poseidon/Foundation/Algorithms/Sha256.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Framework/AppFrame.hpp>
#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Framework/Log.hpp>
#include <Poseidon/Foundation/Types/Memtype.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#endif
#include <Poseidon/Foundation/Strings/Bstring.hpp>
#include <Poseidon/Foundation/platform.hpp>

#include <Poseidon/Foundation/Common/Win.h>
#include <Poseidon/IO/Filesystem/FileOps.hpp>
#include <Poseidon/IO/Filesystem/DirScanner.hpp>
#include <Poseidon/IO/Streams/FileAccessPolicy.hpp>

#if MT_SAFE
#define EXCLUSIVE() ScopeLockSection lock(_lock)
#else
#define EXCLUSIVE()
#endif

namespace Poseidon
{
namespace
{
bool EqualPathComponent(const std::string& a, const char* b, size_t bLen)
{
    if (a.size() != bLen)
        return false;
    for (size_t i = 0; i < bLen; ++i)
    {
        if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

const char* LastPathComponent(const char* path)
{
    const char* last = path;
    for (const char* p = path; *p; ++p)
    {
        if (*p == '/' || *p == '\\')
            last = p + 1;
    }
    return last;
}

struct ModRootAliasContext
{
    const char* prefix;
    size_t prefixLen;
    const char* rest;
    std::string resolved;
};

bool ResolveModRootAliasCallback(RStringB dir, void* opaque)
{
    if (dir.GetLength() == 0)
        return false;

    auto* context = static_cast<ModRootAliasContext*>(opaque);
    if (!EqualPathComponent(LastPathComponent(dir), context->prefix, context->prefixLen))
        return false;

    std::string candidate = (const char*)dir;
    if (!candidate.empty() && candidate.back() != '/' && candidate.back() != '\\')
        candidate += "/";
    candidate += context->rest;
    if (!QIFStream::FileExists(candidate.c_str()))
        return false;

    context->resolved = std::move(candidate);
    return true;
}

std::string ResolveModRootAlias(const char* name)
{
    if (!name || !*name || name[1] == ':' || *name == '/' || *name == '\\')
        return {};

    const char* separator = strpbrk(name, "/\\");
    if (!separator)
        return {};

    ModRootAliasContext context;
    context.prefix = name;
    context.prefixLen = separator - name;
    context.rest = separator + 1;
    if (context.prefixLen == 0 || !*context.rest)
        return {};

    ModSystem::EnumDirectories(ResolveModRootAliasCallback, &context);
    return context.resolved;
}

struct ModOverrideContext
{
    std::string rel;
    std::string resolved;
};

bool ResolveModOverrideCallback(RStringB dir, void* opaque)
{
    if (dir.GetLength() == 0)
        return false; // base game: served by the normal loose open, not treated as an override

    auto* context = static_cast<ModOverrideContext*>(opaque);
    std::string candidate = (const char*)dir;
    if (!candidate.empty() && candidate.back() != '/' && candidate.back() != '\\')
        candidate += "/";
    candidate += context->rel;
    if (!QIFStream::FileExists(candidate.c_str()))
        return false;

    context->resolved = std::move(candidate);
    return true;
}

// Collapse "<seg>/.." pairs into a backslash-joined path. Empty when the name holds no "..",
// or when a ".." would climb above the first component.
std::string CollapseParentDirs(const char* name)
{
    if (!name || !strstr(name, ".."))
        return {};

    std::vector<std::string> parts;
    for (const char* p = name; *p;)
    {
        const char* sep = strpbrk(p, "/\\");
        std::string comp = sep ? std::string(p, sep - p) : std::string(p);
        if (comp == "..")
        {
            if (parts.empty())
                return {};
            parts.pop_back();
        }
        else if (!comp.empty() && comp != ".")
        {
            parts.push_back(std::move(comp));
        }
        if (!sep)
            break;
        p = sep + 1;
    }

    std::string out;
    for (const std::string& part : parts)
    {
        if (!out.empty())
            out += '\\';
        out += part;
    }
    return out;
}

// Strip a leading "addons" off a collapsed path so a root-relative intro path
// ("anims/..\addons\<island>\intro.<world>\...") maps into island <island>'s bank; "" otherwise.
std::string NormalizeAddonBankPath(const char* name)
{
    const std::string collapsed = CollapseParentDirs(name);
    const size_t sep = collapsed.find('\\');
    if (sep == std::string::npos || !EqualPathComponent(collapsed.substr(0, sep), "addons", 6))
        return {};

    return collapsed.substr(sep + 1);
}
} // namespace

std::string ResolveModOverride(const char* relPath)
{
    if (!relPath || !*relPath)
        return {};

    ModOverrideContext context;
    context.rel = relPath;
    for (char& c : context.rel)
        if (c == '\\')
            c = '/';

    ModSystem::EnumDirectories(ResolveModOverrideCallback, &context);
    return context.resolved;
}

QFBank::QFBank()
{
    EXCLUSIVE();
    _handle = nullptr;
    _handleOverlapped = nullptr;
    _serialize = false;
    _error = true; // no open called yet
    _locked = true;
    _lockable = false;
}

#define WIN_DIR '\\'
#define UNIX_DIR '/'

#define BEG_SERIALIZE                \
    {                                \
        PoseidonAssert(!_serialize); \
        _serialize = true;           \
    }
#define END_SERIALIZE               \
    {                               \
        PoseidonAssert(_serialize); \
        _serialize = false;         \
    }

#ifdef NO_MMAP
#define USE_MAPPING 0
#else
#define USE_MAPPING 1
#endif

} // namespace Poseidon

#if USE_MAPPING
#include <Poseidon/IO/Streams/FileMapping.hpp>
#endif

#ifdef _WIN32
#include <Poseidon/IO/Streams/FileOverlapped.hpp>
#endif

namespace Poseidon
{

static int LoadInt(HANDLE f)
{
    int i = 0;
    DWORD rd = 0;
    ReadFile(f, &i, sizeof(i), &rd, nullptr);
    if (rd != sizeof(i))
    {
        i = 0;
    }
    return i;
}

static int LoadInt(QIStream& f)
{
    int i = 0;
    f.read(&i, sizeof(i));
    if (f.fail())
    {
        i = 0;
    }
    return i;
}

static void LoadFileInfo(FileInfoO& i, HANDLE f)
{
    // read zero terminated name
    char name[512];
    char* n = name;
    int maxLen = sizeof(name) - 1;
    DWORD rd;
    for (int l = 0; l < maxLen; l++)
    {
        char c;
        ReadFile(f, &c, sizeof(c), &rd, nullptr);
        if (rd != 1)
        {
            // error during file reading
            i.name = "";
            i.startOffset = 0;
            i.length = 0;
            return;
        }
        if (!c)
        {
            break;
        }
        *n++ = c;
    }
    *n = 0; // zero terminate in any case
    strlwr(name);
    i.name = name;
    i.compressedMagic = LoadInt(f);
    i.uncompressedSize = LoadInt(f);
    i.startOffset = LoadInt(f);
    i.time = LoadInt(f);
    i.length = LoadInt(f);
}

static void LoadFileInfo(FileInfoO& i, QIStream& f)
{
    // read zero terminated name
    char name[512];
    char* n = name;
    int maxLen = sizeof(name) - 1;
    for (int l = 0; l < maxLen; l++)
    {
        int c = f.get();
        if (c < 0)
        {
            // error during file reading
            i.name = "";
            i.startOffset = 0;
            i.length = 0;
            return;
        }
        if (!c)
        {
            break;
        }
        *n++ = c;
    }
    *n = 0; // zero terminate in any case
    strlwr(name);
    i.name = name;
    i.compressedMagic = LoadInt(f);
    i.uncompressedSize = LoadInt(f);
    i.startOffset = LoadInt(f);
    i.time = LoadInt(f);
    i.length = LoadInt(f);
}

} // namespace Poseidon

bool GLogFileOps = false;

namespace Poseidon
{

// simple logging - log what is used, do not check for double files

class QFBankLog : public IBankLog
{
    FILE* _file;
    FindArray<RString> _files; // files aready there

  public:
    void Init(const char* bankName) override;
    void LogFileOp(const char* name) override;
    void Flush(const char* bankName) override;

    QFBankLog();
    ~QFBankLog() override;

  private:
    void Close();
};

void QFBankLog::Init(const char* bankName)
{
    Close();

    char logName[256];
    snprintf(logName, sizeof(logName), "%s", (const char*)"FilesUsed");
    ::CreateDirectory(logName, nullptr);
    strncat(logName, PATH_SEP_STR, sizeof(logName) - strlen(logName) - 1);
    strncat(logName, bankName, sizeof(logName) - strlen(logName) - 1);
    size_t len = strlen(logName);
    if (len > 0 && (logName[len - 1] == '\\' || logName[len - 1] == '/'))
    {
        logName[len - 1] = 0;
    }
    strncat(logName, ".log", sizeof(logName) - strlen(logName) - 1);

#ifndef _WIN32
    unixPath(logName);
#endif
    FILE* file = fopen(logName, "r");
    if (file)
    {
        // read filenames already there
        for (;;)
        {
            char buf[1024];
            *buf = 0;
            fgets(buf, sizeof(buf), file);
            if (!*buf)
            {
                break;
            }
            if (buf[strlen(buf) - 1] == '\n')
            {
                buf[strlen(buf) - 1] = 0;
            }
            _files.Add(buf);
        }
        fclose(file), file = nullptr;
    }
    _file = fopen(logName, "a+");
}

void QFBankLog::LogFileOp(const char* name)
{
    if (!_file)
    {
        return;
    }
    // check if file is already there
    if (_files.Find(name) >= 0)
    {
        return;
    }
    fprintf(_file, "%s\n", name);
}

void QFBankLog::Flush(const char* bankName)
{
    if (_file)
    {
        fflush(_file);
    }
}

QFBankLog::QFBankLog()
{
    _file = nullptr;
}

void QFBankLog::Close()
{
    if (_file)
    {
        fclose(_file);
        _file = nullptr;
    }
}

QFBankLog::~QFBankLog()
{
    Close();
}

RString LoadFromFile(HANDLE file)
{
    BString<1024> buf;
    for (;;)
    {
        char c[2] = {0, 0}; // c[1] must stay NUL: ReadFile writes only c[0], and
                            // buf += c treats c as a C-string — an uninitialized
                            // c[1] would append stack garbage and overrun buf.
        DWORD size = sizeof(char);
        BOOL ok = ReadFile(file, c, size, &size, nullptr);
        if (!ok || size != 1)
        {
            break;
        }
        if (c[0] == 0)
        {
            break;
        }
        buf += c;
    }
    return RString(buf);
}

bool SaveToFile(HANDLE file, RString value)
{
    int len = value.GetLength();
    DWORD size = sizeof(len);
    BOOL ok = WriteFile(file, &len, size, &size, nullptr);
    return ok != FALSE;
}
// Opening may be deferred: this may only remember the parameters and perform the
// real open later. beforeOpen is called once the header is loaded, to decide
// whether the bank should be loaded.
bool QFBank::open(RString name, OpenCallback beforeOpen, BankContextBase* context)
{
    char fullName[1024];
    snprintf(fullName, sizeof(fullName), "%s", (const char*)name);
    strncat(fullName, ".pbo", sizeof(fullName) - strlen(fullName) - 1);

    HANDLE handle = OpenFileForRead(fullName);
    if (handle == INVALID_HANDLE_VALUE)
    {
        Foundation::ErrorMessage("Cannot open file '%s'", fullName);
        _openName = "";
        return false;
    }
    CloseHandle(handle);
    _openName = fullName;

    _openBeforeOpenCallback = beforeOpen;
    _openContext = context;
    _files.Clear();
    _error = false;
    return true;
}

// All banks are locked by default; an app that wants a bank unloadable must unlock it.
void QFBank::Lock() const
{
    _locked = true;
    Load();
}

void QFBank::Unlock() const
{
    _locked = false;
    if (_fileAccess && _fileAccess->RefCounter() == 1)
    {
        Unload();
    }
}

bool QFBank::CanBeUnloaded() const
{
    return _fileAccess && _fileAccess->RefCounter() == 1;
}

// Meyer's singleton for bank functions - no global constructor!
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
static QFBankFunctions& GetDefaultQFBankFunctions()
{
    static QFBankFunctions instance;
    return instance;
}
#pragma clang diagnostic pop

// Constant-init nullptr (SIOF-safe); DefaultFunctions() falls back to a no-op base
// instance until a real implementation registers.
QFBankFunctions* QFBank::_defaultFunctions = nullptr;

// Platform directory separator: entries inside PBO banks are stored
// with the native separator so that lookups work without conversion.
#ifdef __GNUC__
static constexpr char NATIVE_DIR = UNIX_DIR;
static constexpr char FOREIGN_DIR = WIN_DIR;
static const RString NATIVE_DIR_STR("/");
#else
static constexpr char NATIVE_DIR = WIN_DIR;
static constexpr char FOREIGN_DIR = UNIX_DIR;
static const RString NATIVE_DIR_STR("\\");
#endif

static inline RStringB ConvertDirSlash(RStringB name)
{
    const char* change = strchr(name, FOREIGN_DIR);
    if (!change)
    {
        return name;
    }
    // make sure name is mutable
    RString mutableName = name;
    char* mutName = mutableName.MutableData();
    for (;;)
    {
        char* change = strchr(mutName, FOREIGN_DIR);
        if (!change)
        {
            break;
        }
        *change = NATIVE_DIR;
    }
    return mutableName;
}

bool QFBank::Load()
{
    if (!_locked)
    {
        RptF("Cannot open bank %s that is not locked", (const char*)_prefix);
        return false;
    }
    if (_error)
    {
        return false;
    }
    if (_handle)
    {
        return true;
    }
    _files.Clear();
    EXCLUSIVE();
    // note: name should not contain extension
    // automatic optimal bank type is performed with different extensions
    // like .pbf and .pbo
    if (GLogFileOps)
        LOG_DEBUG(Core, "Load bank {}", (const char*)_openName);
    PoseidonAssert(!_fileAccess);

    HANDLE handle = OpenFileForRead((const char*)_openName);
    if (handle == INVALID_HANDLE_VALUE)
    {
        Foundation::ErrorMessage("Cannot open file '%s'", (const char*)_openName);
        _handle = nullptr;
    }
    else
    {
        _handle = (WINHANDLE)(intptr_t)handle;
#ifdef _WIN32
        _handleOverlapped =
            CreateFile(_openName, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (!_handleOverlapped || _handleOverlapped == INVALID_HANDLE_VALUE)
        {
            _handleOverlapped = nullptr;
            LOG_DEBUG(Core, "Cannot open overlapped file access on {}", (const char*)_openName);
        }
#endif
    }

    _pos = _wantPos = 0;
    // int64: a tampered bank with huge per-file lengths must not overflow the
    // running offset (UB); a sum past int range means a corrupt bank — stop.
    int64_t startOffset = 0;
    FileInfoO info;
    for (;;)
    {
        LoadFileInfo(info, (HANDLE)(intptr_t)_handle);

        info.startOffset = static_cast<int>(startOffset);
        startOffset += info.length;
        if (info.name.GetLength() <= 0 || startOffset < 0 || startOffset > INT_MAX)
        {
            break;
        }

        info.name = ConvertDirSlash(info.name);
        _files.Add(info);
    }
    // if bank is empty, it may be "new bank" with product identification
    // check if there is normal terminator, or special
    if (_files.NItems() == 0 && info.compressedMagic == VersionMagic && info.length == 0 && info.time == 0)
    {
        // read properties
        for (;;)
        {
            RString name = LoadFromFile((HANDLE)(intptr_t)_handle);
            if (name.GetLength() == 0)
            {
                break;
            }
            RString value = LoadFromFile((HANDLE)(intptr_t)_handle);
            QFProperty& prop = _properties.Append();
            prop.name = name;
            prop.value = value;
        }

        if (_openBeforeOpenCallback)
        {
            bool ok = _openBeforeOpenCallback(this, _openContext);
            if (!ok)
            {
                CloseHandle(_handle), _handle = nullptr;
                _error = true;
                return false;
            }
        }

        // read normal file headers
        RString encryption = GetProperty("encryption");
        if (encryption.GetLength() > 0)
        {
            // we need to load encrypted headers
            // for this we need to know headers encrypted size
            // load decoded size
            int headersSize = LoadInt((HANDLE)(intptr_t)_handle);
            int headersEncodedSize = LoadInt((HANDLE)(intptr_t)_handle);
            // Both sizes come straight off the wire; reject negative or absurd values
            // before they drive a Temp<char> allocation (a negative int becomes a
            // near-2^64 size_t). A PBO header block far above this is malformed.
            constexpr int kMaxHeaderBytes = 64 * 1024 * 1024;
            if (headersSize < 0 || headersSize > kMaxHeaderBytes || headersEncodedSize < 0 ||
                headersEncodedSize > kMaxHeaderBytes)
            {
                CloseHandle(_handle), _handle = nullptr;
                _error = true;
                return false;
            }
            // read encoded headers into memory
            Temp<char> headers(headersEncodedSize);
            DWORD rd = 0;
            ReadFile(_handle, headers.Data(), headers.Size(), &rd, nullptr);
            if (rd == static_cast<DWORD>(headers.Size()))
            {
                Ref<IFilebankEncryption> ss = CreateFilebankEncryption(encryption, nullptr);
                if (ss)
                {
                    QIStream headersEncoded(headers.Data(), headers.Size());
                    Temp<char> headerDecodedData(headersSize);
                    ss->Decode(headerDecodedData.Data(), headerDecodedData.Size(), headersEncoded);
                    QIStream headersDecoded(headerDecodedData.Data(), headerDecodedData.Size());
                    for (;;)
                    {
                        LoadFileInfo(info, headersDecoded);

                        info.startOffset = startOffset;
                        startOffset += info.length;
                        if (info.name.GetLength() <= 0)
                        {
                            break;
                        }

                        info.name = ConvertDirSlash(info.name);
                        _files.Add(info);
                    }
                }
            }
        }
        else
        {
            for (;;)
            {
                LoadFileInfo(info, (HANDLE)(intptr_t)_handle);

                info.startOffset = startOffset;
                startOffset += info.length;
                if (info.name.GetLength() <= 0)
                {
                    break;
                }

                info.name = ConvertDirSlash(info.name);
                _files.Add(info);
            }
        }
    }
    else
    {
        if (_openBeforeOpenCallback)
        {
            bool ok = _openBeforeOpenCallback(this, _openContext);
            if (!ok)
            {
                _files.Clear();
                CloseHandle(_handle), _handle = nullptr;
                _error = true;
                return false;
            }
        }
    }

    int headerSize = SetFilePointer(_handle, 0, nullptr, FILE_CURRENT);
    BEG_SERIALIZE
    // filemapping uses offset from end of header; direct access uses file offset
    if (_files.NItems() > 0)
    {
        // !!! avoid GetTable when NItems == 0
        for (int i = 0; i < _files.NTables(); i++)
        {
            AutoArray<FileInfoO>& container = _files.GetTable(i);
            for (int j = 0; j < container.Size(); j++)
            {
                FileInfoO& info = container[j];
                // int64 add then truncate: a corrupt bank's offset + headerSize
                // could overflow int (UB); a bogus result fails later on read.
                info.startOffset = static_cast<int>(static_cast<int64_t>(info.startOffset) + headerSize);
            }
        }
    }
    startOffset += headerSize;
    // check integrity - try to seek end of file
    DWORD checkEof = SetFilePointer(_handle, startOffset, nullptr, FILE_BEGIN);
    if ((int)checkEof != startOffset)
    {
        Foundation::ErrorMessage("Data file too short '%s'.", (const char*)_openName);
    }
    else
    {
        _pos = checkEof;
    }

    // do not map headers, only file content
    _fileAccess =
        QFileAccess::TryOpenMappedBankAccess((HANDLE)(intptr_t)_handle, checkEof - headerSize, (const char*)_openName);

    ScanPatchFiles(_prefix, RString());

    END_SERIALIZE
    return true;
}

void QFBank::Unload()
{
    // if error, there is nothing to undo
    if (_error)
    {
        return;
    }
    // if there is no handle, there is nothing to undo
    if (!_handle)
    {
        return;
    }
    if (_fileAccess && _fileAccess->RefCounter() > 1)
    {
        // if some file from bank is still used, we cannot unload it
        LOG_DEBUG(Core, "Cannot unload bank {}, {} files are still open", (const char*)_openName,
                  _fileAccess->RefCounter() - 1);
        return;
    }
    if (GLogFileOps)
        LOG_DEBUG(Core, "Unload bank {}", (const char*)_openName);
    Clear();
}

bool QFBank::error() const
{
    // if bank was not opened yet, it cannot have any fatal errors
    if (!_error)
    {
        return false;
    }
    // if there is no handle, there is some fatal error
    if (!_handle || (HANDLE)(intptr_t)_handle == INVALID_HANDLE_VALUE)
    {
        return true;
    }
    return false;
}

const RString& QFBank::GetProperty(const RString& name) const
{
    for (int i = 0; i < _properties.Size(); i++)
    {
        if (!strcmpi(_properties[i].name, name))
        {
            return _properties[i].value;
        }
    }
// Empty string constant for error return
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
    const static RString empty;
#pragma clang diagnostic pop
    return empty;
}

void QFBank::ScanPatchFiles(RString prefix, RString subdir)
{
#if _ENABLE_PATCHING
    if (prefix.GetLength() <= 0)
    {
        // patching bank with no prefix is nonsense
        // most likely cause is temporary bank (like in single missions)
        return;
    }

    // check if there is folder containing patch files
    BString<1024> wildname;
    strcpy(wildname, prefix);
    strcat(wildname, subdir);

#ifdef _WIN32

    strcat(wildname, "*.*");
    _finddata_t find;
    intptr_t hf = _findfirst(wildname, &find); // _findfirst returns intptr_t (x64)
    if (hf >= 0)
    {
        do
        {
            char lowName[256];
            snprintf(lowName, sizeof(lowName), "%s", (const char*)find.name);
            strlwr(lowName);
            RString name = lowName;
            if (find.attrib & _A_SUBDIR)
            {
                if (!strcmp(find.name, ".") || !strcmp(find.name, ".."))
                {
                    continue;
                }
                ScanPatchFiles(prefix, subdir + name + RString("\\"));
            }
            else
            {
                RString subname = subdir + name;
                const FileInfoO& file = _files[subname];
                if (_files.NotNull(file))
                {
                    LOG_DEBUG(Core, "Plain file version of {} used", (const char*)(subname));
                    FileInfoO fileSet = file;
                    fileSet.loadFromFile = true;
                    _files.Add(fileSet);
                }
            }

        } while (_findnext(hf, &find) == 0);

        _findclose(hf);
    }

#else

    unixPath((char*)(const char*)wildname);
    int len = strlen(wildname);
    if (len > 0 && wildname[len - 1] == UNIX_DIR)
        wildname[--len] = (char)0;
    DIR* dir = opendir(wildname);
    if (!dir)
        return;
    struct dirent* entry;
    while ((entry = readdir(dir)))
    {
        if (entry->d_name[0] == '.' && (!entry->d_name[1] || entry->d_name[1] == '.' && !entry->d_name[2]))
            continue;
        // stat the entry <= subdirectories must be handled differently
        wildname += "/";
        wildname += entry->d_name;
        struct stat st;
        if (!stat(wildname, &st))
        {
            if (S_ISDIR(st.st_mode))
            { // directory
                ScanPatchFiles(prefix, subdir + entry->d_name);
            }
            else
            { // regular file
                RString subname = subdir + entry->d_name;
                const FileInfoO& file = _files[subname];
                if (_files.NotNull(file))
                {
                    LOG_DEBUG(Core, "Plain file version of {} used", (const char*)(subname));
                    FileInfoO fileSet = file;
                    fileSet.loadFromFile = true;
                    _files.Add(fileSet);
                }
            }
        }
        wildname[len] = (char)0;
    }
    closedir(dir);

#endif

#endif
}

void QFBank::SetPrefix(RString prefix)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", (const char*)prefix);
    platformPath(buf);
    _prefix = buf;
    // create log
    if (GLogFileOps)
    {
        _log = new QFBankLog();
        _log->Init(_prefix);
    }
}

const FileInfoO& QFBank::FindFileInfo(const char* name) const
{
    EXCLUSIVE();
    char lowName[128];
    snprintf(lowName, sizeof(lowName), "%s", (const char*)name);
    strlwr(lowName);
    platformPath(lowName);
    return _files[lowName];
}

bool QFBank::FileExists(const char* name) const
{
    if (!Load())
    {
        return false;
    }
    const FileInfoO& info = FindFileInfo(name);
    return NotNull(info);
}

void QFBank::Seek(long pos) const
{
    EXCLUSIVE();
    _wantPos = pos;
}

void QFBank::Read(char* buf, long size, const char* name) const
{
    PoseidonAssert(!_error);
    BEG_SERIALIZE
    EXCLUSIVE(); // seek to wanted position
    if (_pos != _wantPos)
    {
        DWORD nPos = SetFilePointer(_handle, _wantPos, nullptr, FILE_BEGIN);
        if ((int)nPos != _wantPos)
        {
            Foundation::ErrorMessage("Read: Data file seek error (%s: %d,%d).", name, nPos, _wantPos);
        }
        else
        {
            _pos = nPos;
        }
    }
    // read into the temporary buffer
    DWORD bytes;
    if (!ReadFile(_handle, buf, size, &bytes, nullptr) || (int)bytes != size)
    {
        Foundation::ErrorMessage("Data file read error (%s).", name);
    }
    _pos += bytes;
    _wantPos += size;
    END_SERIALIZE
}

} // namespace Poseidon
#include <Poseidon/IO/Streams/FileCompress.hpp>

namespace Poseidon
{

// Orders files within the bank: a smaller result is nearer the start. Equal
// results mean unknown relative order, and the values are not contiguous. Lets a
// caller loading many files schedule reads for near-sequential access.
int QFBank::GetFileOrder(const char* file)
{
    const FileInfoO& info = FindFileInfo(file);
    if (IsNull(info))
    {
        return 0;
    }
    return info.startOffset;
}

#ifdef _WIN32
namespace
{
class OwnedArchiveHandle
{
    HANDLE _handle;
public:
    explicit OwnedArchiveHandle(HANDLE handle) : _handle(handle) {}
    ~OwnedArchiveHandle() { if (_handle && _handle != INVALID_HANDLE_VALUE) CloseHandle(_handle); }
    OwnedArchiveHandle(const OwnedArchiveHandle&) = delete;
    OwnedArchiveHandle& operator=(const OwnedArchiveHandle&) = delete;
    HANDLE Get() const { return _handle; }
    void Release() { _handle = INVALID_HANDLE_VALUE; }
};

struct ArchiveIdentity
{
    FILE_ID_INFO fileId{};
    uint64_t size = 0;
    bool Capture(HANDLE handle)
    {
        LARGE_INTEGER length{};
        if (!handle || handle == INVALID_HANDLE_VALUE || GetFileType(handle) != FILE_TYPE_DISK ||
            !GetFileInformationByHandleEx(handle, FileIdInfo, &fileId, sizeof(fileId)) ||
            !GetFileSizeEx(handle, &length) || length.QuadPart < 0) return false;
        size = static_cast<uint64_t>(length.QuadPart);
        return true;
    }
    bool Same(const ArchiveIdentity& other) const
    {
        return size == other.size && fileId.VolumeSerialNumber == other.fileId.VolumeSerialNumber &&
            std::memcmp(fileId.FileId.Identifier, other.fileId.FileId.Identifier,
                        sizeof(fileId.FileId.Identifier)) == 0;
    }
};
}

// No bank, mapped buffer, engine reference or shared read cursor survives here.
// FILE_SHARE_READ denies writers and deletion while any request copy holds this.
class BankArchiveReadLease
{
public:
    const HANDLE handle;
    const ArchiveIdentity identity;
    const std::string archive;
    const uint64_t offset, bytes;
    BankArchiveReadLease(HANDLE h, const ArchiveIdentity& id, const BankReadRequest& request)
        : handle(h), identity(id), archive(request.archive), offset(request.offset), bytes(request.bytes) {}
    ~BankArchiveReadLease() { CloseHandle(handle); }
    bool Matches(const BankReadRequest& request) const
    {
        return request.archive == archive && request.offset == offset && request.bytes == bytes;
    }
};
#endif

bool BankReadRequest::HasArchiveIdentity() const
{
#ifdef _WIN32
    return archiveLease && archiveLease->Matches(*this);
#else
    return false;
#endif
}

size_t BankReadRequest::ArchiveIdentityBytes() const
{
#ifdef _WIN32
    return archiveLease ? sizeof(BankArchiveReadLease) + archiveLease->archive.capacity() + 1 : 0;
#else
    return 0;
#endif
}

bool BankReadRequest::CopyMemberIdentity(BankReadMemberIdentity& out) const
{
#ifdef _WIN32
    if (!HasArchiveIdentity()) return false;
    BankReadMemberIdentity value;
    value.volume = archiveLease->identity.fileId.VolumeSerialNumber;
    value.archiveBytes = archiveLease->identity.size;
    value.offset = offset; value.bytes = bytes;
    std::memcpy(value.fileId, archiveLease->identity.fileId.FileId.Identifier, sizeof(value.fileId));
    out = value;
    return true;
#else
    (void)out;
    return false;
#endif
}
bool BankReadRequest::SameArchiveMember(const BankReadRequest& other) const
{
#ifdef _WIN32
    return HasArchiveIdentity() && other.HasArchiveIdentity() && offset == other.offset && bytes == other.bytes &&
        archiveLease->identity.Same(other.archiveLease->identity);
#else
    return false;
#endif
}

bool BankReadRequest::Read(std::vector<char>& out) const
{
    out.clear();
    // Bound the optional fast path. Larger or unsupported members retain the VFS
    // fallback; never allocate based on an unchecked archive length.
    if (bytes == 0 || bytes > 64ull * 1024 * 1024) return false;
#ifdef _WIN32
    if (archiveLease)
    {
        if (!HasArchiveIdentity()) return false;
        OwnedArchiveHandle reader(static_cast<HANDLE>(OpenFileForRead(archive.c_str())));
        ArchiveIdentity actual;
        if (!actual.Capture(reader.Get()) || !actual.Same(archiveLease->identity) ||
            offset > actual.size || bytes > actual.size - offset) return false;
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(reader.Get(), position, nullptr, FILE_BEGIN)) return false;
        out.resize(static_cast<size_t>(bytes));
        DWORD read = 0;
        if (!ReadFile(reader.Get(), out.data(), static_cast<DWORD>(bytes), &read, nullptr) || read != bytes)
        {
            out.clear();
            return false;
        }
        return true;
    }
#else
    if (archiveLease) return false;
#endif
    std::ifstream stream(archive, std::ios::binary | std::ios::ate);
    if (!stream) return false;
    const std::streamoff end = stream.tellg();
    if (end < 0 || offset > static_cast<uint64_t>(end) ||
        bytes > static_cast<uint64_t>(end) - offset) return false;
    stream.seekg(static_cast<std::streamoff>(offset));
    out.resize(static_cast<size_t>(bytes));
    if (!stream.read(out.data(), static_cast<std::streamsize>(bytes)))
    {
        out.clear();
        return false;
    }
    return true;
}

std::optional<BankReadRequest> QFBank::CaptureReadRequest(const char* member, bool requireArchiveIdentity) const
{
    // MAIN THREAD ONLY. Copy metadata while the mounted bank owns its archive.
    // Do not expose an unlocked QFBank pointer or a mapped Ref to workers.
    if (!Load()) return std::nullopt;
    const auto& info = FindFileInfo(member);
    if (IsNull(info) || info.compressedMagic != 0 || info.startOffset < 0 ||
        info.length <= 0 || info.length > 64 * 1024 * 1024) return std::nullopt;
#if _ENABLE_PATCHING
    if (info.loadFromFile) return std::nullopt;
#endif
    BankReadRequest request{std::string(static_cast<const char*>(_openName)),
                            static_cast<uint64_t>(info.startOffset), static_cast<uint64_t>(info.length)};
    if (!requireArchiveIdentity) return request;
#ifdef _WIN32
    // Compare the actual mounted archive, not just its path. The bank's existing
    // read-only denial remains held throughout capture; no member bytes are read.
    OwnedArchiveHandle held(static_cast<HANDLE>(OpenFileForRead(request.archive.c_str())));
    ArchiveIdentity mounted, fresh;
    if (!mounted.Capture(static_cast<HANDLE>(_handle)) || !fresh.Capture(held.Get()) || !mounted.Same(fresh) ||
        request.offset > fresh.size || request.bytes > fresh.size - request.offset) return std::nullopt;
    try
    {
        request.archiveLease = std::make_shared<const BankArchiveReadLease>(held.Get(), fresh, request);
        held.Release();
    }
    catch (...)
    {
        return std::nullopt;
    }
    return request;
#else
    return std::nullopt;
#endif
}

bool BankCompressedReadRequest::ReadDecoded(std::vector<char>& out, std::string& decodedSha256) const
{
    // Do not expose partly decoded data or a hash on any malformed stream.
    if (_codec != CompMagic || !_encoded.HasArchiveIdentity() ||
        _encoded.bytes == 0 || _encoded.bytes > MaxEncodedBytes ||
        _decodedBytes == 0 || _decodedBytes > MaxDecodedBytes ||
        _canonicalMember.empty() || _canonicalMember.size() >= 128) return false;
    try
    {
        std::vector<char> encoded;
        if (!_encoded.Read(encoded) || encoded.size() != _encoded.bytes) return false;
        std::vector<char> decoded(_decodedBytes);
        QIStream input(encoded.data(), static_cast<int>(encoded.size()));
        SSCompress compressor;
        if (!compressor.Decode(decoded.data(), static_cast<long>(decoded.size()), input) ||
            input.fail() || input.rest() != 0) return false;
        std::string hash = Foundation::Sha256::Of(decoded.data(), decoded.size());
        out = std::move(decoded);
        decodedSha256 = std::move(hash);
        return true;
    }
    catch (...) { return false; }
}

std::optional<BankCompressedReadRequest> QFBank::CaptureCompressedReadRequest(
    const char* member, uint32_t maxEncodedBytes, uint32_t maxDecodedBytes) const
{
    if (!Foundation::IsMainThread() || !member || !maxEncodedBytes || !maxDecodedBytes) return std::nullopt;
    size_t length = 0;
    while (length < 128 && member[length]) ++length;
    if (!length || length >= 128 || !Load()) return std::nullopt;
    const auto& info = FindFileInfo(member);
    if (IsNull(info) || info.compressedMagic != CompMagic || info.startOffset < 0 ||
        info.length <= 0 || info.uncompressedSize <= 0 ||
        static_cast<uint32_t>(info.length) > BankCompressedReadRequest::MaxEncodedBytes ||
        static_cast<uint32_t>(info.length) > maxEncodedBytes ||
        static_cast<uint32_t>(info.uncompressedSize) > BankCompressedReadRequest::MaxDecodedBytes ||
        static_cast<uint32_t>(info.uncompressedSize) > maxDecodedBytes) return std::nullopt;
#if _ENABLE_PATCHING
    if (info.loadFromFile) return std::nullopt;
#endif
    const std::string canonical((const char*)info.name);
    if (canonical.empty() || canonical.size() >= 128) return std::nullopt;
#ifdef _WIN32
    BankReadRequest encoded{std::string(static_cast<const char*>(_openName)),
                            static_cast<uint64_t>(info.startOffset), static_cast<uint64_t>(info.length)};
    OwnedArchiveHandle held(static_cast<HANDLE>(OpenFileForRead(encoded.archive.c_str())));
    ArchiveIdentity mounted, fresh;
    if (!mounted.Capture(static_cast<HANDLE>(_handle)) || !fresh.Capture(held.Get()) ||
        !mounted.Same(fresh) || encoded.offset > fresh.size ||
        encoded.bytes > fresh.size - encoded.offset) return std::nullopt;
    try
    {
        encoded.archiveLease = std::make_shared<const BankArchiveReadLease>(held.Get(), fresh, encoded);
        held.Release();
        return BankCompressedReadRequest(std::move(encoded), canonical, info.compressedMagic,
                                         static_cast<uint32_t>(info.uncompressedSize));
    }
    catch (...) { return std::nullopt; }
#else
    return std::nullopt;
#endif
}

namespace
{
bool CaptureWarmTextureSourceBinding()
{
    static const bool enabled = [] {
        const char* value = std::getenv("WGR_OBJECT_STREAM_WARM_TEXTURES");
        const char* cold = std::getenv("WGR_OBJECT_STREAM_COLD_TEXTURE_HANDOFF");
        return (value && std::strcmp(value, "1") == 0) || (cold && std::strcmp(cold, "1") == 0);
    }();
    return enabled;
}
bool CaptureBuildingPilotMemberIdentity()
{
    static const bool enabled = [] {
        const char* value = std::getenv("WGR_OBJECT_STREAM_RAP_BUILDING_PILOT");
        const char* multi = std::getenv("WGR_OBJECT_STREAM_RAP_BUILDING_MULTISTAGE");
        return (value && std::strcmp(value, "1") == 0) ||
            (multi && std::strcmp(multi, "1") == 0);
    }();
    return enabled;
}
bool IsBuildingPilotNormal(const char* prefix, const char* member)
{
    using namespace PatnikArchiveReason;
    return MatchesJoined(R"(dz\structures\data\plaster\plaster_flats01_nohq.paa)", prefix, member) ||
        MatchesJoined(R"(dz\structures\data\concrete\concrete_bare4_nohq.paa)", prefix, member) ||
        MatchesJoined(R"(dz\structures\data\plaster\police_station_wall_nohq.paa)", prefix, member) ||
        MatchesJoined(R"(dz\structures\data\plaster\plaster_flats02_nohq.paa)", prefix, member) ||
        MatchesJoined(R"(dz\structures\data\concrete\concrete_panels_dirty_nohq.paa)", prefix, member) ||
        MatchesJoined(R"(dz\structures\data\concrete\concrete_panels_nohq.paa)", prefix, member) ||
        MatchesJoined(R"(dz\structures\data\plaster\plaster_flats03_nohq.paa)", prefix, member);
}
thread_local unsigned ModelReadDepth = 0; // numeric owner-purpose nesting only; no retained resources
thread_local unsigned MaterialPreflightDepth = 0; // owner-only nested stage intent; no names or resources
thread_local unsigned RetailPacReadDepth = 0; // exact selected model only; no retained resources
thread_local std::array<const RetailRigidAssetProfile::Profile*,8> RetailPacReadProfiles{};
thread_local unsigned TenementPaaDepth = 0;
thread_local unsigned TenementPaaIdentities = 0;
thread_local bool TenementPaaIdentityOverflow = false;
struct ArchiveBindingState
{
    static constexpr size_t FixedCharge = sizeof(ArchiveSourceBudget) + 256;
    std::shared_ptr<ArchiveSourceBudget> budget = std::make_shared<ArchiveSourceBudget>(256,
        2 * 1024 * 1024 - FixedCharge);
    std::atomic<uint64_t> wrapped{0}, retained{0}, captureRefused{0}, capacityRefused{0}, allocationRefused{0};
    std::atomic<uint64_t> weakCapacityRefused{0}, weakAllocationRefused{0};
    std::atomic<uint64_t> weakWrappersPublished{0}, weakInitializedHandoffs{0}, weakFailedInitCompletions{0};
};
ArchiveBindingState& SourceBindingState() { static ArchiveBindingState state; return state; }
}

bool ArchiveSourceBinding::RetailPacReadScope::Enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("WGR_GEOMETRY_PAGE_RETAIL_SOURCE");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled && RetailRigidAssetProfile::Selected()!=nullptr;
}
bool ArchiveSourceBinding::RetailPacReadScope::Active()
{
    return Enabled() && Foundation::IsMainThread() && RetailPacReadDepth != 0 &&
        RetailPacReadProfiles[RetailPacReadDepth-1]==RetailRigidAssetProfile::Selected();
}
ArchiveSourceBinding::RetailPacReadScope::RetailPacReadScope(const char* exactModelPath)
{
    const auto* profile=RetailRigidAssetProfile::Selected();
    if (Enabled() && profile && Foundation::IsMainThread() && RetailPacReadDepth < 8 &&
        exactModelPath && profile->MatchesModelPath(exactModelPath))
    { RetailPacReadProfiles[RetailPacReadDepth++]=profile; _entered = true; }
}
ArchiveSourceBinding::RetailPacReadScope::~RetailPacReadScope()
{
    if (_entered) RetailPacReadProfiles[--RetailPacReadDepth]=nullptr;
}
bool ArchiveSourceBinding::RetailPacReadScope::MatchesLogicalName(const char* name)
{
    return Active() && name && RetailPacReadProfiles[RetailPacReadDepth-1]->MatchesTexturePath(name);
}
bool ArchiveSourceBinding::RetailPacReadScope::MatchesBankMember(
    const char* bankPath, const char* prefix, const char* member)
{
    if (!Active() || !bankPath || !prefix || !member ||
        !PatnikArchiveReason::Matches(R"(data\)", prefix) ||
        !RetailPacReadProfiles[RetailPacReadDepth-1]->MatchesTextureMember(member)) return false;
    const size_t length = PatnikArchiveReason::BoundedLength(bankPath);
    return length != 8192 && RetailPacReadProfiles[RetailPacReadDepth-1]->MatchesTextureArchive(
        std::string_view(bankPath,length));
}

bool TenementPhysicalPaaScope::Enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("WGR_OBJECT_STREAM_DAYZ_PHYSICAL_PREFETCH");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
bool TenementPhysicalPaaScope::Active()
{
    return Enabled() && Foundation::IsMainThread() && TenementPaaDepth != 0;
}
bool TenementPhysicalPaaScope::ReserveIdentity()
{
    // The counter is scoped to one owner operation, so a refusal cannot turn a
    // truncated set of identities into an unbounded process-lifetime registry.
    if (!Active()) return false;
    if (TenementPaaIdentities >= 256)
    { TenementPaaIdentityOverflow = true; return false; }
    ++TenementPaaIdentities;
    return true;
}
bool TenementPhysicalPaaScope::Overflowed()
{
    return Active() && TenementPaaIdentityOverflow;
}
TenementPhysicalPaaScope::TenementPhysicalPaaScope(bool selected)
{
    if (selected && Enabled() && Foundation::IsMainThread() && TenementPaaDepth < 8)
    {
        if (TenementPaaDepth == 0)
        { TenementPaaIdentities = 0; TenementPaaIdentityOverflow = false; }
        ++TenementPaaDepth;
        _entered = true;
    }
}
TenementPhysicalPaaScope::~TenementPhysicalPaaScope()
{
    if (_entered) --TenementPaaDepth;
}

bool ArchiveSourceBinding::CacheHandoffEnabled()
{
    static const bool enabled = [] {
        for (const char* name : {"WGR_OBJECT_STREAM_WEAK_CACHE_PROOF", "WGR_OBJECT_STREAM_HOT_SOURCE_PROOF_RETIRE",
            "WGR_OBJECT_STREAM_WARM_TEXTURES", "WGR_OBJECT_STREAM_WARM_TEXTURE_JOBS"})
        {
            const char* value = std::getenv(name);
            if (!value || std::strcmp(value, "1") != 0) return false;
        }
        return true;
    }();
    return enabled;
}
std::shared_ptr<const ArchiveSourceBudget::Ticket> ArchiveSourceBinding::ReserveTraceMetadata(size_t bytes)
{
    try { return SourceBindingState().budget->AcquireTrace(bytes); }
    catch (...) { return {}; }
}
std::shared_ptr<const ArchiveSourceBudget::Ticket> ArchiveSourceBinding::ReserveWeakCell(size_t bytes)
{
    if (!CacheHandoffEnabled()) return {};
    try
    {
        auto& state = SourceBindingState();
        bool capacity = false;
        auto ticket = state.budget->AcquireWeak(bytes, &capacity);
        if (!ticket) (capacity ? state.weakCapacityRefused : state.weakAllocationRefused).fetch_add(1, std::memory_order_relaxed);
        return ticket;
    }
    catch (...) { return {}; } // Optional refusal preserves original strong wrapper.
}

bool ArchiveSourceBinding::ModelReadScope::PurposeRequired()
{
    static const bool required = [] {
        const char* jobs = std::getenv("WGR_OBJECT_STREAM_WARM_TEXTURE_JOBS");
        const char* cold = std::getenv("WGR_OBJECT_STREAM_COLD_TEXTURE_HANDOFF");
        return (CaptureWarmTextureSourceBinding() && jobs && std::strcmp(jobs, "1") == 0) ||
            (cold && std::strcmp(cold, "1") == 0);
    }();
    return required;
}
bool ArchiveSourceBinding::ModelReadScope::MaterialPreflightReservationEnabled()
{
    static const bool enabled = [] {
        const char* source = std::getenv("WGR_OBJECT_STREAM_WARM_TEXTURES");
        const char* jobs = std::getenv("WGR_OBJECT_STREAM_WARM_TEXTURE_JOBS");
        const char* preflight = std::getenv("WGR_OBJECT_STREAM_WARM_MATERIAL_PREFLIGHT");
        return source && std::strcmp(source, "1") == 0 &&
            jobs && std::strcmp(jobs, "1") == 0 &&
            preflight && std::strcmp(preflight, "1") == 0;
    }();
    return enabled;
}
ArchiveSourceBinding::ModelReadScope::ModelReadScope(Intent intent)
{
    if (PurposeRequired() && Foundation::IsMainThread() && ModelReadDepth < 64)
    {
        ++ModelReadDepth; _entered = true;
        if (intent == Intent::MaterialPreflight && MaterialPreflightReservationEnabled() &&
            MaterialPreflightDepth < 64)
        { ++MaterialPreflightDepth; _materialPreflightEntered = true; }
    }
}
ArchiveSourceBinding::ModelReadScope::~ModelReadScope()
{
    if (_materialPreflightEntered) --MaterialPreflightDepth;
    if (_entered) --ModelReadDepth; // same-stack RAII; copies/moves are forbidden
}
bool ArchiveSourceBinding::ModelReadScope::IsActive()
{
    return Foundation::IsMainThread() && ModelReadDepth != 0;
}
bool ArchiveSourceBinding::ModelReadScope::IsMaterialPreflightActive()
{
    return Foundation::IsMainThread() && ModelReadDepth != 0 && MaterialPreflightDepth != 0 &&
        MaterialPreflightReservationEnabled();
}
size_t ArchiveSourceBinding::ModelReadScope::BindingLimit()
{
    return MaterialPreflightReservationEnabled() && !IsMaterialPreflightActive() ? 254 : 256;
}

size_t ArchiveSourceBinding::KnownCppBytes() const
{
    return sizeof(*this) + _request.archive.capacity() + 1 + _request.ArchiveIdentityBytes() + 256;
}

std::shared_ptr<const ArchiveSourceBinding> ArchiveSourceBinding::Create(BankReadRequest request)
{
    if (!request.HasArchiveIdentity() || request.archive.size() > 8191 ||
        request.ArchiveIdentityBytes() > 16384) return {};
    try
    {
        // Created only by the opt-in read path. One process budget, not per source/thread.
        auto& state = SourceBindingState();
        const size_t charge = sizeof(ArchiveSourceBinding) + request.archive.capacity() + 1 +
            request.ArchiveIdentityBytes() + 256;
        bool capacity = false;
        // The budget mutex rechecks the same owner intent after the advisory QFBank
        // count check. Neither path can exceed the original 256/2-MiB hard limits.
        auto ticket = state.budget->Acquire(charge, &capacity, ModelReadScope::BindingLimit());
        if (!ticket)
        {
            (capacity ? state.capacityRefused : state.allocationRefused).fetch_add(1, std::memory_order_relaxed);
            return {};
        }
        return std::shared_ptr<const ArchiveSourceBinding>(new ArchiveSourceBinding(std::move(request), std::move(ticket)));
    }
    catch (...)
    {
        try { SourceBindingState().allocationRefused.fetch_add(1, std::memory_order_relaxed); }
        catch (...) { } // Even the optional counter state may fail to allocate.
        return {};
    }
}

void ArchiveSourceBinding::NoteWrappedRead()
{
    SourceBindingState().wrapped.fetch_add(1, std::memory_order_relaxed);
}
void ArchiveSourceBinding::NoteWeakWrapperPublished()
{
    SourceBindingState().weakWrappersPublished.fetch_add(1, std::memory_order_relaxed);
}
void ArchiveSourceBinding::NoteCacheInitCompletion(bool initialized)
{
    // Only a real first completion of an opted-in cell reaches this hook.
    (initialized ? SourceBindingState().weakInitializedHandoffs : SourceBindingState().weakFailedInitCompletions)
        .fetch_add(1, std::memory_order_relaxed);
}
void ArchiveSourceBinding::NoteInitRetained()
{
    SourceBindingState().retained.fetch_add(1, std::memory_order_relaxed);
}
ArchiveSourceBinding::Stats ArchiveSourceBinding::SnapshotStats()
{
    Stats out;
    out.enabled = CaptureWarmTextureSourceBinding() || RetailPacReadScope::Enabled();
    if (!out.enabled) return out;
    try
    {
        auto& state = SourceBindingState();
        const auto live = state.budget->Observe();
        out.cacheHandoffEnabled = CacheHandoffEnabled();
        out.weakCells = live.weakCells;
        if (out.cacheHandoffEnabled)
        {
            out.weakWrappersPublished = state.weakWrappersPublished.load(std::memory_order_relaxed);
            out.weakInitializedHandoffs = state.weakInitializedHandoffs.load(std::memory_order_relaxed);
            out.weakFailedInitCompletions = state.weakFailedInitCompletions.load(std::memory_order_relaxed);
            out.weakPeakReservationCells = live.weakPeakReservationCells;
            out.weakPeakReservationBytes = live.weakPeakReservationBytes;
        }
        out.weakKnownCppBytes = live.weakBytes;
        out.weakCapacityRefused = state.weakCapacityRefused.load(std::memory_order_relaxed);
        out.weakAllocationRefused = state.weakAllocationRefused.load(std::memory_order_relaxed);
        out.liveBindings = live.bindings;
        out.knownCppBytes = live.bytes + ArchiveBindingState::FixedCharge;
        out.traceKnownCppBytes = live.traceBytes;
        out.wrappedReads = state.wrapped.load(std::memory_order_relaxed);
        out.initRetains = state.retained.load(std::memory_order_relaxed);
        out.captureRefused = state.captureRefused.load(std::memory_order_relaxed);
        out.capacityRefused = state.capacityRefused.load(std::memory_order_relaxed);
        out.allocationRefused = state.allocationRefused.load(std::memory_order_relaxed);
    }
    catch (...) { out.allocationRefused = 1; }
    return out;
}

bool QFBank::MatchesMountedMember(const char* member, const BankReadRequest& initializedSource) const
{
    if (!Foundation::IsMainThread() || !member || !_handle || _error ||
        !initializedSource.HasArchiveIdentity()) return false;
    // FindFileInfo normalizes in a fixed128-byte buffer. Refuse truncation aliases.
    size_t length = 0;
    while (length < 128 && member[length]) ++length;
    if (!length || length >= 128) return false;
#ifdef _WIN32
    const auto& info = FindFileInfo(member); // const metadata lookup, no loading or insertion.
    if (IsNull(info) || info.compressedMagic != 0 || info.startOffset < 0 || info.length <= 0 ||
        static_cast<uint64_t>(info.startOffset) != initializedSource.offset ||
        static_cast<uint64_t>(info.length) != initializedSource.bytes) return false;
#if _ENABLE_PATCHING
    if (info.loadFromFile) return false;
#endif
    ArchiveIdentity mounted;
    return mounted.Capture(static_cast<HANDLE>(_handle)) &&
        mounted.Same(initializedSource.archiveLease->identity);
#else
    return false;
#endif
}

bool QFBank::MatchesMountedCompressedMember(
    const char* member, const BankCompressedReadRequest& initializedSource) const
{
    if (!Foundation::IsMainThread() || !member || !_handle || _error ||
        initializedSource._codec != CompMagic ||
        !initializedSource._encoded.HasArchiveIdentity()) return false;
    size_t length = 0;
    while (length < 128 && member[length]) ++length;
    if (!length || length >= 128) return false;
#ifdef _WIN32
    const auto& info = FindFileInfo(member);
    if (IsNull(info) || info.compressedMagic != initializedSource._codec ||
        info.startOffset < 0 || info.length <= 0 || info.uncompressedSize <= 0 ||
        static_cast<uint32_t>(info.length) > BankCompressedReadRequest::MaxEncodedBytes ||
        static_cast<uint32_t>(info.uncompressedSize) > BankCompressedReadRequest::MaxDecodedBytes ||
        std::string((const char*)info.name) != initializedSource._canonicalMember ||
        static_cast<uint64_t>(info.startOffset) != initializedSource._encoded.offset ||
        static_cast<uint64_t>(info.length) != initializedSource._encoded.bytes ||
        static_cast<uint32_t>(info.uncompressedSize) != initializedSource._decodedBytes) return false;
#if _ENABLE_PATCHING
    if (info.loadFromFile) return false;
#endif
    ArchiveIdentity mounted;
    return mounted.Capture(static_cast<HANDLE>(_handle)) &&
        mounted.Same(initializedSource._encoded.archiveLease->identity);
#else
    return false;
#endif
}

namespace
{
// This wrapper certifies only the mapped bytes selected by this QFBank::Read.
// It holds no archive lease or binding-budget slot. Consumers must compare the
// copied birth identity with their own captured request and the current mount.
class ArchiveMemberIdentityFileBuffer final : public IFileBuffer
{
    Ref<IFileBuffer> _buffer;
    BankReadMemberIdentity _identity;
public:
    ArchiveMemberIdentityFileBuffer(Ref<IFileBuffer> buffer, BankReadMemberIdentity identity)
        : _buffer(buffer), _identity(identity) {}
    bool GetError() const override { return _buffer->GetError(); }
    const char* GetData() const override { return _buffer->GetData(); }
    int GetSize() const override { return _buffer->GetSize(); }
    bool IsFromBank(QFBank* bank) const override { return _buffer->IsFromBank(bank); }
    bool IsReady() const override { return _buffer->IsReady(); }
    bool CopyArchiveMemberIdentity(BankReadMemberIdentity& out) const override
    { out = _identity; return true; }
};
// One cell per opted-in wrapper, bounded separately from live immutable native
// leases. Charge survives weak binding/control-block retention. Members destroy
// weak and strong aliases BEFORE releasing their conservative metadata ticket.
class ArchiveCacheProofCell
{
    std::shared_ptr<const ArchiveSourceBudget::Ticket> _charge;
    std::shared_ptr<const ArchiveSourceBinding> _initial;
    std::weak_ptr<const ArchiveSourceBinding> _weak;
public:
    ArchiveCacheProofCell(std::shared_ptr<const ArchiveSourceBinding> binding,
        std::shared_ptr<const ArchiveSourceBudget::Ticket> charge)
        : _charge(std::move(charge)), _initial(std::move(binding)), _weak(_initial) {}
    std::shared_ptr<const ArchiveSourceBinding> Get() const
    { return Foundation::IsMainThread() ? (_initial ? _initial : _weak.lock()) : nullptr; }
    bool Complete(const std::shared_ptr<const ArchiveSourceBinding>& initialized)
    {
        if (!Foundation::IsMainThread()) return false;
        if (initialized && initialized != Get()) return false; // Wrong source cannot drop debt.
        if (_initial)
        {
            // Repeated cached Init and expired-proof Init are NOT new handoffs.
            _initial.reset(); // Source owns successful exact binding; failure leaves no proof.
            ArchiveSourceBinding::NoteCacheInitCompletion(bool(initialized));
        }
        return true;
    }
};
class ArchiveCacheHandoffFileBuffer final : public IFileBuffer
{
    Ref<IFileBuffer> _buffer;
    std::shared_ptr<ArchiveCacheProofCell> _proof;
public:
    ArchiveCacheHandoffFileBuffer(Ref<IFileBuffer> buffer, std::shared_ptr<ArchiveCacheProofCell> proof)
        : _buffer(buffer), _proof(std::move(proof)) {}
    bool GetError() const override { return _buffer->GetError(); }
    const char* GetData() const override { return _buffer->GetData(); }
    int GetSize() const override { return _buffer->GetSize(); }
    bool IsFromBank(QFBank* bank) const override { return _buffer->IsFromBank(bank); }
    bool IsReady() const override { return _buffer->IsReady(); }
    std::shared_ptr<const ArchiveSourceBinding> GetArchiveSourceBinding() const override { return _proof->Get(); }
    bool CopyArchiveMemberIdentity(BankReadMemberIdentity& out) const override
    { return _buffer->CopyArchiveMemberIdentity(out); }
    bool CompleteArchiveSourceInit(const std::shared_ptr<const ArchiveSourceBinding>& initialized) override
    { return _proof->Complete(initialized); }
};
class ArchiveBoundFileBuffer final : public IFileBuffer
{
    Ref<IFileBuffer> _buffer;
    std::shared_ptr<const ArchiveSourceBinding> _binding;
public:
    ArchiveBoundFileBuffer(Ref<IFileBuffer> buffer, std::shared_ptr<const ArchiveSourceBinding> binding)
        : _buffer(buffer), _binding(std::move(binding)) {}
    bool GetError() const override { return _buffer->GetError(); }
    const char* GetData() const override { return _buffer->GetData(); }
    int GetSize() const override { return _buffer->GetSize(); }
    bool IsFromBank(QFBank* bank) const override { return _buffer->IsFromBank(bank); }
    bool IsReady() const override { return _buffer->IsReady(); }
    std::shared_ptr<const ArchiveSourceBinding> GetArchiveSourceBinding() const override { return _binding; }
    bool CopyArchiveMemberIdentity(BankReadMemberIdentity& out) const override
    { return _buffer->CopyArchiveMemberIdentity(out); }
};
}

namespace
{
class CompressedModelBoundBuffer final : public IFileBuffer
{
    Ref<IFileBuffer> _buffer;
    std::shared_ptr<const ModelCompressedSourceBirth> _birth;
    std::atomic<bool> _claimed{false};
public:
    CompressedModelBoundBuffer(Ref<IFileBuffer> buffer,std::shared_ptr<const ModelCompressedSourceBirth> birth)
        :_buffer(std::move(buffer)),_birth(std::move(birth)){}
    bool GetError() const override{return _buffer->GetError();}
    const char* GetData() const override{return _buffer->GetData();}
    int GetSize() const override{return _buffer->GetSize();}
    bool IsFromBank(QFBank* bank) const override{return _buffer->IsFromBank(bank);}
    bool IsReady() const override{return _buffer->IsReady();}
    std::shared_ptr<const ModelCompressedSourceBirth> GetCompressedModelSourceBirth() const override{return _birth;}
    std::shared_ptr<const ModelCompressedSourceBirth> TakeCompressedModelSourceBirthForParse() override
    {
        if(!_birth||!_birth->Valid()||!ModelCompressedSourceBirth::ReadScope::Active()||
           ModelCompressedSourceBirth::ReadScope::Token()!=_birth->readScopeToken||_claimed.exchange(true))return {};
        return _birth;
    }
};
}
Ref<IFileBuffer> QFBank::Read(const char* name) const
{
    const bool targeted = PatnikArchiveReason::Enabled() &&
        PatnikArchiveReason::MatchesJoined((const char*)GetPrefix(), name);
    auto reason = [&](const char* why, bool bound)
    {
        if (!targeted || !PatnikArchiveReason::ReserveRow()) return;
        try
        {
            const auto stats = ArchiveSourceBinding::SnapshotStats();
            LOG_INFO(Core, "patnik_archive_reason stage=bank reason={} bound={} live={} limit={} capRefused={} captureRefused={} allocationRefused={}",
                why, bound, stats.liveBindings, ArchiveSourceBinding::ModelReadScope::BindingLimit(),
                stats.capacityRefused, stats.captureRefused, stats.allocationRefused);
        }
        catch (...) { } // Diagnostics cannot change the original read.
    };
    if (!Load())
    {
        reason("bankLoadFailed", false);
        return nullptr;
    }
    // log every file opened, even when the open later fails
    if (_log)
    {
        _log->LogFileOp(name);
    }

    const FileInfoO& info = FindFileInfo(name);
    if (IsNull(info))
    {
        reason("memberMissing", false);
        return nullptr;
    }
#if _ENABLE_PATCHING
    if (info.loadFromFile)
    {
        // patch file provided - use it
        char fullName[512];
        snprintf(fullName, sizeof(fullName), "%s", (const char*)GetPrefix());
        strncat(fullName, name, sizeof(fullName) - strlen(fullName) - 1);
        Ref<IFileBuffer> data = QFileAccess::OpenFileBufferAuto(fullName);
        if (!data->GetError())
        {
            reason("patchedLooseFile", false);
            return (IFileBuffer*)data;
        }
    }
#endif

    if (info.compressedMagic == CompMagic) // some compression
    {
        reason("compressedMember", false);
        std::optional<BankCompressedReadRequest> coldSource;
        if(ModelCompressedSourceBirth::ReadScope::Active()&&name) {
            try {
                const std::string prefix((const char*)GetPrefix());
                size_t nameBytes=0;while(nameBytes<128&&name[nameBytes])++nameBytes;
                if(prefix.size()<128&&nameBytes<128&&
                   ModelCompressedSourceBirth::ReadScope::MatchesLogicalName((prefix+name).c_str()))
                    coldSource=CaptureCompressedReadRequest(name);
            }catch(...){} // Optional provenance cannot change ordinary reads.
        }
        Seek(info.startOffset);
        QIStream inBuf;
        // read compressed data into temporary buffer
        Temp<char> cData(info.length);
        inBuf.init(cData, info.length);
        Read(cData.Data(), info.length, name);
        // uncompress
        Ref<FileBufferMemory> data = new FileBufferMemory(info.uncompressedSize);
        SSCompress ss;
        if (!ss.Decode(data->GetWritableData(), info.uncompressedSize, inBuf))
        {
            RptF("Error decoding %s from %s", name, (const char*)_prefix);
            return nullptr;
        }
        if(coldSource&&!inBuf.fail()&&inBuf.rest()==0&&data->GetSize()>0&&
           uint32_t(data->GetSize())<=BankCompressedReadRequest::MaxDecodedBytes) {
            try {
                if(MatchesMountedCompressedMember(name,*coldSource)) {
                    auto birth=ModelCompressedSourceBirth::Capture(std::move(*coldSource),
                        std::span<const char>(data->GetData(),size_t(data->GetSize())));
                    if(birth)return new CompressedModelBoundBuffer(data.GetRef(),std::move(birth));
                }
            }catch(...){} // Fail unknown; the exact original decoded bytes still return.
        }
        return (IFileBuffer*)data;
    }
    else if (info.compressedMagic == EncrMagic)
    {
        reason("encryptedMember", false);
        Seek(info.startOffset);
        QIStream inBuf;
        // read compressed data into temporary buffer
        Temp<char> cData(info.length);
        inBuf.init(cData, info.length);
        Read(cData.Data(), info.length, name);
        // ask compression manager to create encryptor/decryptor
        // uncompress
        Ref<FileBufferMemory> data = new FileBufferMemory(info.uncompressedSize);
        Ref<IFilebankEncryption> ss = CreateFilebankEncryption(GetProperty("encryption"), nullptr);
        if (!ss || !ss->Decode(data->GetWritableData(), info.uncompressedSize, inBuf))
        {
            RptF("Error decoding %s from %s", name, (const char*)_prefix);
            return nullptr;
        }
        return (IFileBuffer*)data;
    }
    else if (info.compressedMagic == 0)
    {
        // no compression

        std::shared_ptr<const ArchiveSourceBinding> binding;
        const char* captureReason = "captureDisabled";
        const bool retailPac = ArchiveSourceBinding::RetailPacReadScope::Active() &&
            ArchiveSourceBinding::RetailPacReadScope::MatchesBankMember(
            (const char*)GetOpenName(), (const char*)GetPrefix(), name);
        if (((CaptureWarmTextureSourceBinding() &&
            (!ArchiveSourceBinding::ModelReadScope::PurposeRequired() || ArchiveSourceBinding::ModelReadScope::IsActive())) || retailPac) &&
            _fileAccess && Foundation::IsMainThread() && name)
        {
            captureReason = "notPaa";
            size_t length = 0;
            while (length < 8192 && name[length]) ++length;
            if ((retailPac || (length >= 4 && length <= 8191 && name[length - 4] == '.' &&
                std::tolower(static_cast<unsigned char>(name[length - 3])) == 'p' &&
                std::tolower(static_cast<unsigned char>(name[length - 2])) == 'a' &&
                std::tolower(static_cast<unsigned char>(name[length - 1])) == 'a')))
            {
                captureReason = "captureRefused";
                try
                {
                    // Advisory count refusal before opening another denial handle. Acquire
                    // remains authoritative for byte capacity and any intervening release.
                    auto& state = SourceBindingState();
                    if (state.budget->Observe().bindings >=
                        ArchiveSourceBinding::ModelReadScope::BindingLimit())
                    {
                        captureReason = "bindingCountCap";
                        state.capacityRefused.fetch_add(1, std::memory_order_relaxed);
                    }
                    else
                    {
                        auto request = CaptureReadRequest(name, true);
                        // Compare against the exact metadata selected by THIS original Read.
                        if (request && request->offset == static_cast<uint64_t>(info.startOffset) &&
                            request->bytes == static_cast<uint64_t>(info.length))
                        {
                            binding = ArchiveSourceBinding::Create(std::move(*request));
                            captureReason = binding ? "captured" : "bindingCreateRefused";
                        }
                        else
                        {
                            captureReason = "readRequestRefusedOrMismatch";
                            SourceBindingState().captureRefused.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
                catch (...) { captureReason = "captureException"; } // Original read result is unchanged.
            }
        }
        if (targeted && std::strcmp(captureReason, "captureDisabled") == 0 &&
            CaptureWarmTextureSourceBinding())
        {
            if (ArchiveSourceBinding::ModelReadScope::PurposeRequired() &&
                !ArchiveSourceBinding::ModelReadScope::IsActive()) captureReason = "noOwnerPurpose";
            else if (!_fileAccess) captureReason = "unmappedArchive";
            else if (!Foundation::IsMainThread()) captureReason = "notMainThread";
        }
        auto bind = [&](Ref<IFileBuffer> buffer) -> Ref<IFileBuffer>
        {
            const bool tenementPaa = TenementPhysicalPaaScope::Active() && name &&
                ([&] { const size_t n = std::strlen(name); if (n < 4 || name[n - 4] != '.') return false;
                    const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(name[n - 3])));
                    const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(name[n - 2])));
                    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(name[n - 1])));
                    return (a == 'p' && b == 'a' && (c == 'a' || c == 'c')); })();
            if ((CaptureBuildingPilotMemberIdentity() || tenementPaa) && Foundation::IsMainThread() && _fileAccess &&
                buffer && !buffer->GetError() && buffer->GetSize() == info.length &&
                (IsBuildingPilotNormal((const char*)GetPrefix(), name) || tenementPaa))
            {
                try
                {
                    BankReadMemberIdentity identity;
                    bool exact = binding && binding->Request().CopyMemberIdentity(identity);
                    if (!exact)
                    {
                        auto request = CaptureReadRequest(name, true);
                        exact = request && request->offset == static_cast<uint64_t>(info.startOffset) &&
                            request->bytes == static_cast<uint64_t>(info.length) &&
                            request->CopyMemberIdentity(identity);
                    }
                    if (exact && (!tenementPaa || TenementPhysicalPaaScope::ReserveIdentity()))
                        buffer = new ArchiveMemberIdentityFileBuffer(buffer, identity);
                }
                catch (...) { } // Optional proof must never change the original read.
            }
            if (!binding || !buffer || buffer->GetError() || buffer->GetSize() != info.length)
            {
                reason(!buffer || buffer->GetError() || buffer->GetSize() != info.length ?
                    "mappedBufferInvalid" : captureReason, false);
                return buffer;
            }
            try
            {
                Ref<IFileBuffer> wrapped;
                if (ArchiveSourceBinding::CacheHandoffEnabled())
                {
                    // Allowance covers cell, wrapper and both shared control blocks,
                    // including the expired binding control retained by weak_ptr.
                    const size_t bytes = sizeof(ArchiveCacheProofCell) + sizeof(ArchiveCacheHandoffFileBuffer) + 512;
                    auto charge = ArchiveSourceBinding::ReserveWeakCell(bytes);
                    if (charge)
                    {
                        try
                        {
                            auto cell = std::make_shared<ArchiveCacheProofCell>(binding, std::move(charge));
                            wrapped = new ArchiveCacheHandoffFileBuffer(buffer, std::move(cell));
                            // Count only the complete wrapper actually selected for return.
                            ArchiveSourceBinding::NoteWeakWrapperPublished();
                        }
                        catch (...) { SourceBindingState().weakAllocationRefused.fetch_add(1, std::memory_order_relaxed); }
                    }
                }
                if (!wrapped) wrapped = new ArchiveBoundFileBuffer(buffer, binding);
                ArchiveSourceBinding::NoteWrappedRead();
                reason("wrapped", true);
                return wrapped;
            }
            catch (...)
            {
                SourceBindingState().allocationRefused.fetch_add(1, std::memory_order_relaxed);
                reason("wrapperAllocationRefused", false);
                return buffer;
            }
        };

#if USE_MAPPING
        if (_fileAccess)
        {
            // use memory mapped file subsection
            Ref<IFileBuffer> data = new FileBufferSub(_fileAccess, info.startOffset, info.length);
            return bind(data);
        }
#endif
        Seek(info.startOffset);
        // read directly from file
        Ref<FileBufferMemory> data = new FileBufferMemory(info.length);
        Read(data->GetWritableData(), info.length, name);
        reason("unmappedCopied", false);
        return data.GetRef(); // Copied IO lacks a full-read witness; provenance remains Unknown.
    }
    else
    {
        // some unknown compression manager
        Fail("Unknown compression manager");
        return nullptr;
    }
}

// AST-003. See the header for why this hashes logical content and why it is lazy.
RString QFBank::GetContentHash(const char* name) const
{
    if (!Load())
    {
        return RString();
    }
    const FileInfoO& info = FindFileInfo(name);
    if (IsNull(info))
    {
        return RString();
    }
    // Key on the member's canonical name as stored in the index, not on the
    // caller's spelling: "Data\x.paa" and "data/x.paa" are one member and must
    // not occupy two entries with two independently-computed hashes.
    const std::string key((const char*)info.name);
    const auto found = _contentHashes.find(key);
    if (found != _contentHashes.end())
    {
        return RString(found->second.c_str());
    }

    Ref<IFileBuffer> data = Read(name);
    if (!data || data->GetError() || !data->GetData())
    {
        // A member that cannot be read has no content to state. Returning ""
        // rather than the hash of nothing keeps "unknown" distinguishable from
        // "empty" -- an empty member has a real, well-defined SHA-256.
        return RString();
    }
    const int size = data->GetSize();
    if (size < 0)
    {
        return RString();
    }
    const std::string hex = Foundation::Sha256::Of(data->GetData(), static_cast<size_t>(size));
    _contentHashes.emplace(key, hex);
    return RString(hex.c_str());
}

Ref<IFileBuffer> QFBank::ReadOverlapped(const char* file) const
{
    if (!Load())
    {
        return nullptr;
    }
    if (!_handleOverlapped)
    {
        // overlapped IO not supported - fall back to normal case
        return Read(file);
    }
#ifdef _WIN32
    const FileInfoO& info = FindFileInfo(file);
    if (IsNull(info))
    {
        return nullptr;
    }
#if _ENABLE_PATCHING
    if (info.loadFromFile)
    {
        return Read(file);
    }
#endif
    if (info.compressedMagic == CompMagic)
    {
        Ref<IFileBuffer> data =
            new FileBufferOverlapped(_handleOverlapped, info.uncompressedSize, info.startOffset, info.length);
        return data;
    }
    Ref<IFileBuffer> data = new FileBufferOverlapped(_handleOverlapped, info.startOffset, info.length);
    return data;
#else
    return Read(file);
#endif
}

#if USE_MAPPING
bool QFBank::BufferOwned(const FileBufferMapped* buffer) const
{
    return buffer->GetFileHandle() == (HANDLE)(intptr_t)_handle;
}
#endif

bool QFBank::BufferOwned(const FileBufferOverlapped* buffer) const
{
#ifdef _WIN32
    return buffer->GetFileHandle() == _handleOverlapped;
#else
    return false;
#endif
}

void QFBank::ForEach(void (*Func)(const FileInfoO& fi, const FileBankType* files, void* context), void* context) const
{
    if (!Load())
    {
        return;
    }
    EXCLUSIVE();
    _files.ForEach(Func, context);
}

void QFBank::Clear()
{
    EXCLUSIVE();
    // clear variables that are not part of Global structure
    _files.Clear();
    // The memo is keyed by member name only, so it must not survive the index it
    // describes: a bank unloaded and reopened over a rewritten archive would
    // otherwise answer with the previous file's content hash.
    _contentHashes.clear();
    _fileAccess.Free();
    if (_handle)
    {
        CloseHandle(_handle), _handle = nullptr;
    }
    if (_handleOverlapped)
    {
        CloseHandle(_handleOverlapped), _handleOverlapped = nullptr;
    }
}

QFBank::~QFBank()
{
    EXCLUSIVE();
    Clear();
}

// AutoArray<QFBank> (BankList) relocates on grow via ModernTraits::MoveData,
// which move-constructs each element into the new storage and then destructs the
// source.  All members except the raw descriptors are reference-counted or value
// types that survive the copy + source destruction; the descriptors must be
// detached from the source so its ~QFBank()->Clear() does not CloseHandle() the
// fd the relocated bank still uses (otherwise a later compressed read — e.g.
// stringtable.csv from O.pbo — seeks a closed fd: "Data file seek error … -1").
QFBank::QFBank(QFBank&& other) : QFBank(static_cast<const QFBank&>(other))
{
    other._handle = nullptr;
    other._handleOverlapped = nullptr;
}

void QIFStreamB::open(const QFBank& bank, const char* name)
{
    _error = LSUnknownError;
    _sharedData = bank.Read(name);
    if (!_sharedData)
    {
        return;
    }
    init(_sharedData->GetData(), _sharedData->GetSize());
    _bank = &bank;
}

// Global file bank state
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wglobal-constructors"
BankList GFileBanks;
RString GFileBankPrefix; // HW config dependent banks
#pragma clang diagnostic pop

} // namespace Poseidon

bool GUseFileBanks;

namespace Poseidon
{

bool QFBankFunctions::FreeUnusedBanks(size_t sizeNeeded) const
{
    return GFileBanks.UnloadUnused();
}

#ifdef _WIN32

FindBank::FindBank()
{
    _info = nullptr; // Initialize to prevent invalid delete
    _handle = -1;    // -1 is the error value for _findfirst
}

FindBank::~FindBank()
{
    Close();
}

bool FindBank::First(const char* path)
{
    strcpy(_wild, path);
    strcat(_wild, "\\*.pbo");
    _info = new _finddata_t;
    _handle = _findfirst(_wild, (_finddata_t*)_info);
    return _handle != -1;
}

bool FindBank::Next()
{
    if (!_info || _handle == -1)
    {
        return false;
    }
    return _findnext(_handle, (_finddata_t*)_info) == 0;
}

void FindBank::Close()
{
    if (_info)
    {
        delete (_finddata_t*)_info;
        _info = nullptr;
    }
    if (_handle != -1)
    {
        _findclose(_handle);
        _handle = -1;
    }
}

const char* FindBank::GetName() const
{
    if (!_info)
    {
        return "";
    }
    return ((_finddata_t*)_info)->name;
}

#else

FindBank::FindBank() {}

FindBank::~FindBank()
{
    Close();
}

bool FindBank::First(const char* path)
{
    Close();
    LocalPath(dirName, path);
    return _scanner.First(dirName, ".pbo");
}

bool FindBank::Next()
{
    return _scanner.Next();
}

void FindBank::Close()
{
    _scanner.Close();
}

const char* FindBank::GetName() const
{
    return _scanner.GetName();
}

#endif

// The callback may be invoked even after Load returns; the context is stored in
// the bank meanwhile.
void BankList::Load(const RString& path, const RString& bankPrefix, const RString& bName, bool emptyPrefix,
                    OpenCallback beforeOpen, OpenCallback afterOpen, BankContextBase* context)
{
    int oldSize = Size();
    int index = Add();
    QFBank& bank = Set(index);
    // path can differ from bankPrefix (for example if Mod is used)
    if (!bank.open(path + bName, beforeOpen, context))
    {
        Resize(oldSize);
        return;
    }
    Log("Open bank %s", (const char*)(bankPrefix + bName));

    // open() only verifies the file exists and records its name -- it reads no
    // header at all. The entry table, the properties, and the beforeOpen product
    // check all happen inside Load(), which Lock() forces. Without this the
    // archive's declared prefix is never available at mount time and every bank
    // silently falls back to its filename, which is how Arma 3 archives came to
    // mount as `rocks_f\` instead of the `a3\rocks_f\` they declare. Banks are
    // locked by default anyway; only an app that wants one unloadable unlocks it.
    bank.Lock();

    if (bank.error())
    {
        Resize(oldSize);
        return;
    }

    // The archive's own declaration wins where it has one; see ResolveBankMountName.
    const RString prefix = ResolveBankMountName(bank.GetProperty(RString("prefix")), bankPrefix, bName, emptyPrefix);

    RString prefixPath = prefix + NATIVE_DIR_STR;
    bank.SetPrefix(prefixPath);

    if (afterOpen && !afterOpen(&bank, context))
    {
        Resize(oldSize);
        return;
    }
}

void BankList::Unload(const RString& bankPrefix, const RString& bName, bool emptyPrefix)
{
    // Derives the mount name the same way Load's *fallback* does, so it can only
    // find banks mounted under their filename. A bank that declared its own
    // prefix is mounted under that instead and will not be found here. Left as
    // is because nothing calls this today; a caller that needs to unload a
    // later-generation addon has to match on the declared prefix.
    RString prefix;
    if (emptyPrefix)
    {
        prefix = bName;
    }
    else
    {
        prefix = bankPrefix + bName;
    }

    RString prefixPath = prefix + NATIVE_DIR_STR;

    for (int i = 0; i < Size(); i++)
    {
        const QFBank& b = Get(i);
        if (b.GetPrefix() == prefixPath)
        {
            Delete(i);
            return;
        }
    }
}

void BankList::Lock(const RString& prefix)
{
    for (int i = 0; i < Size(); i++)
    {
        const QFBank& b = Get(i);
        if (b.GetPrefix() == prefix)
        {
            b.Lock();
            return;
        }
    }
    LOG_DEBUG(Core, "Lock: Bank {} not found", (const char*)prefix);
}
void BankList::Unlock(const RString& prefix)
{
    for (int i = 0; i < Size(); i++)
    {
        const QFBank& b = Get(i);
        if (b.GetPrefix() == prefix)
        {
            b.Unlock();
            return;
        }
    }
    LOG_DEBUG(Core, "Unlock: Bank {} not found", (const char*)prefix);
}

void BankList::SetLockable(const RString& prefix, bool lockable)
{
    for (int i = 0; i < Size(); i++)
    {
        QFBank& b = Set(i);
        if (b.GetPrefix() == prefix)
        {
            b.SetLockable(lockable);
            return;
        }
    }
    LOG_DEBUG(Core, "MakeLockable: Bank {} not found", (const char*)prefix);
}

bool BankList::UnloadUnused()
{
    // if it is not locked, is is probably already unloaded
    // but we still may want to try it first
    for (int i = 0; i < GFileBanks.Size(); i++)
    {
        QFBank& bank = GFileBanks[i];
        if (bank.IsLocked())
        {
            continue;
        }
        if (!bank.CanBeUnloaded())
        {
            continue;
        }
        LOG_DEBUG(Core, "Unloading bank {}", (const char*)bank.GetPrefix());
        bank.Unload();
        return true;
    }
    // if no unlocked bank is available for unloading, try locked banks
    for (int i = 0; i < GFileBanks.Size(); i++)
    {
        QFBank& bank = GFileBanks[i];
        if (!bank.CanBeUnloaded())
        {
            continue;
        }
        LOG_DEBUG(Core, "Unloading locked bank {}", (const char*)bank.GetPrefix());
        bank.Unload();
        return true;
    }
    return false;
}

void QIFStreamB::ClearBanks()
{
    GFileBanks.Clear();
}

QFBank* QIFStreamB::AutoBank(const char* name)
{
    if (!*name)
    {
        return nullptr;
    }
    if (name[1] == ':')
    {
        return nullptr;
    }
    // Longest matching prefix, not the first.
    //
    // Callers strip exactly GetPrefix().GetLength() characters and look the
    // remainder up inside the bank, so picking a shorter prefix that also matches
    // hands the wrong archive a path it cannot possibly contain, and the lookup
    // fails instead of falling through to the right bank.
    //
    // No OFP-era prefix is a prefix of another -- they are all single addon names
    // -- so first-match and longest-match agree there and this changes nothing.
    // Arma 3 nests them: `a3\map_stratis\data` and `a3\map_stratis\data\layers`
    // are both mounted, and every lookup into the layers archive was being
    // answered by the shorter one.
    QFBank* best = nullptr;
    int bestLength = -1;
    for (int i = 0; i < GFileBanks.Size(); i++)
    {
        QFBank& bank = GFileBanks[i];
        if (!CmpStartStr(name, bank.GetPrefix()))
        {
            const int length = bank.GetPrefix().GetLength();
            if (length > bestLength)
            {
                bestLength = length;
                best = &bank;
            }
        }
    }
    return best;
}

QIFStreamB::QIFStreamB() : _bank(nullptr) {}

void QIFStreamB::AutoOpen(const char* name, IQFBankContext* context)
{
    const char* name0 = name;
    if (GUseFileBanks)
    {
        QFBank* bank = AutoBank(name);
        if (bank)
        {
            if (context && !context->IsAccessible(bank))
            {
                RptF("AutoOpen %s: access denied", name0);
                return;
            }
            // check if we should use bank version of the file
            // skip bank name
            name += bank->GetPrefix().GetLength();
            open(*bank, name);
            if (_sharedData)
            {
                _bank = bank;
                return;
            }
            // if file does not exist in bank, try to open it from file
            LOG_DEBUG(Core, "File {} not in bank", name0);
        }
    }
    QIFStream::open(name0);
    if (_sharedData && !_sharedData->GetError())
    {
        return;
    }

    std::string modAlias = ResolveModRootAlias(name0);
    if (!modAlias.empty())
    {
        QIFStream::open(modAlias.c_str());
        if (_sharedData && !_sharedData->GetError())
        {
            return;
        }
    }

    // A path can leave its own prefix and re-enter by mod folder name
    // ("voice\\..\\<mod>\\voice\\..."); only the collapsed form names a mod root.
    const std::string collapsed = CollapseParentDirs(name0);
    if (!collapsed.empty())
    {
        QIFStream::open(collapsed.c_str());
        if (_sharedData && !_sharedData->GetError())
        {
            return;
        }

        std::string collapsedAlias = ResolveModRootAlias(collapsed.c_str());
        if (!collapsedAlias.empty())
        {
            QIFStream::open(collapsedAlias.c_str());
            if (_sharedData && !_sharedData->GetError())
            {
                return;
            }
        }
    }

    if (GUseFileBanks)
    {
        std::string norm = NormalizeAddonBankPath(name0);
        if (!norm.empty())
        {
            QFBank* bank = AutoBank(norm.c_str());
            if (bank)
            {
                if (context && !context->IsAccessible(bank))
                {
                    RptF("AutoOpen %s: access denied", name0);
                    return;
                }
                open(*bank, norm.c_str() + bank->GetPrefix().GetLength());
                if (_sharedData)
                {
                    _bank = bank;
                }
            }
        }
    }
}

bool QIFStreamB::IsFromBank(const QFBank* bank) const
{
    // check request to flush all banks
    if (!bank)
    {
        return true;
    }
    return bank == _bank;
}

bool QIFStreamB::FileExist(const char* name, IQFBankContext* context)
{
    if (GUseFileBanks)
    {
        QFBank* bank = AutoBank(name);
        if (bank)
        {
            if (context && !context->IsAccessible(bank))
            {
                const char* rName = name + bank->GetPrefix().GetLength();
                if (bank->FileExists(rName))
                {
                    return false;
                }
                LOG_DEBUG(Core, "FileExist {}: access denied", name);
                return false;
            }
            const char* rName = name + bank->GetPrefix().GetLength();
            if (bank->FileExists(rName))
            {
                return true;
            }
        }
    }
    if (QIFStream::FileExists(name))
    {
        return true;
    }

    std::string modAlias = ResolveModRootAlias(name);
    if (!modAlias.empty())
    {
        return true;
    }

    const std::string collapsed = CollapseParentDirs(name);
    if (!collapsed.empty())
    {
        if (QIFStream::FileExists(collapsed.c_str()))
        {
            return true;
        }
        if (!ResolveModRootAlias(collapsed.c_str()).empty())
        {
            return true;
        }
    }

    if (GUseFileBanks)
    {
        std::string norm = NormalizeAddonBankPath(name);
        if (!norm.empty())
        {
            QFBank* bank = AutoBank(norm.c_str());
            if (bank && (!context || context->IsAccessible(bank)) &&
                bank->FileExists(norm.c_str() + bank->GetPrefix().GetLength()))
            {
                return true;
            }
        }
    }
    return false;
}

struct EncryptorInformation
{
    RString name;
    IFilebankEncryption* (*createFunction)(const void* context);
};

template <>
struct FindArrayKeyTraits<EncryptorInformation>
{
    typedef const char* KeyType;
    static bool IsEqual(const char* a, const char* b) { return !strcmpi(a, b); }
    static const char* GetKey(const EncryptorInformation& a) { return a.name; }
};

// Global encryption registry
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wglobal-constructors"
static FindArrayKey<EncryptorInformation> GEncryptors;
#pragma clang diagnostic pop

void RegisterFilebankEncryption(const char* name, IFilebankEncryption* (*createFunction)(const void* context))
{
    // check if given encyption already exists
    int index = GEncryptors.FindKey(name);
    if (index >= 0)
    {
        LOG_ERROR(Core, "Ecryption {} already registered", name);
        return;
    }
    EncryptorInformation& ei = GEncryptors.Append();
    ei.name = name;
    ei.createFunction = createFunction;
}

Ref<IFilebankEncryption> CreateFilebankEncryption(const char* name, const void* context)
{
    int index = GEncryptors.FindKey(name);
    if (index < 0)
    {
        LOG_ERROR(Core, "Unknown encryption {}", name);
        return nullptr;
    }
    return GEncryptors[index].createFunction(context);
}

} // namespace Poseidon
