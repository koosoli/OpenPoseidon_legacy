#ifdef _MSC_VER
#pragma once
#endif

#ifndef _QBSTREAM_HPP
#define _QBSTREAM_HPP

#include <Poseidon/IO/Streams/QStream.hpp>
#include <Poseidon/IO/Streams/FileInfo.h>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <optional>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <Poseidon/IO/Filesystem/DirScanner.hpp>
#endif

namespace Poseidon
{
class BankArchiveReadLease;
// Opt-in, owner-thread purpose for the exact tenement's physical PAA preparation.
// The scope never carries a filename or grants source authority: only a successful
// mapped member Init can attach a birth identity, and upload rechecks the mount.
class TenementPhysicalPaaScope
{
    bool _entered = false;
  public:
    explicit TenementPhysicalPaaScope(bool selected);
    ~TenementPhysicalPaaScope();
    TenementPhysicalPaaScope(const TenementPhysicalPaaScope&) = delete;
    TenementPhysicalPaaScope& operator=(const TenementPhysicalPaaScope&) = delete;
    static bool Enabled();
    static bool Active();
    static bool ReserveIdentity(); // bounded attempts to tag successful Init buffers
    static bool Overflowed();
};
// Owner-thread snapshot of an uncompressed member. Read owns its stream and never
// accesses bank cursors, the VFS MRU or engine references. Consumers must reject
// results from a departed mount/world generation before publishing them.
// Copied numeric physical member identity, never a source lease or file path.
// Equality mirrors SameArchiveMember for exact successful Windows captures.
struct BankReadMemberIdentity
{
    uint64_t volume = 0, archiveBytes = 0, offset = 0, bytes = 0;
    unsigned char fileId[16]{};
    bool operator==(const BankReadMemberIdentity& rhs) const noexcept
    { return volume == rhs.volume && archiveBytes == rhs.archiveBytes && offset == rhs.offset &&
        bytes == rhs.bytes && std::memcmp(fileId, rhs.fileId, sizeof(fileId)) == 0; }
};
struct BankReadRequest
{
    std::string archive;
    uint64_t offset = 0;
    uint64_t bytes = 0;
    // Optional Windows-only physical archive lease. Copies share only the denial
    // handle; each Read opens its own verified handle and has its own cursor.
    // Public fields must still match the frozen capture; a borrowed lease cannot
    // certify another path or member. No immutability claim exists without it.
    std::shared_ptr<const BankArchiveReadLease> archiveLease;
    bool HasArchiveIdentity() const;
    // Known retained C++ object/string capacity, not kernel handle or RSS cost.
    size_t ArchiveIdentityBytes() const;
    bool SameArchiveMember(const BankReadRequest& other) const;
    bool CopyMemberIdentity(BankReadMemberIdentity& out) const; // No IO; leaves out untouched on refusal.
    bool Read(std::vector<char>& out) const;
};

// Opt-in, bounded lease for a PBO Cprs member. The physical encoded range is
// leased just like BankReadRequest, while the codec, decoded size and canonical
// member name are frozen at capture. ReadDecoded verifies exact consumption and
// returns a hash of the exact decoded bytes; the owner still compares that hash
// with its admitted source and checks the current mount before publication.
class BankCompressedReadRequest
{
    friend class QFBank;
    BankReadRequest _encoded;
    std::string _canonicalMember;
    int _codec = 0;
    uint32_t _decodedBytes = 0;
    BankCompressedReadRequest(BankReadRequest encoded, std::string member, int codec, uint32_t decodedBytes)
        : _encoded(std::move(encoded)), _canonicalMember(std::move(member)),
          _codec(codec), _decodedBytes(decodedBytes) {}
public:
    static constexpr uint32_t MaxEncodedBytes = 128 * 1024;
    static constexpr uint32_t MaxDecodedBytes = 128 * 1024;
    const BankReadRequest& Encoded() const { return _encoded; }
    const std::string& CanonicalMember() const { return _canonicalMember; }
    int Codec() const { return _codec; }
    uint32_t DecodedBytes() const { return _decodedBytes; }
    bool ReadDecoded(std::vector<char>& out, std::string& decodedSha256) const;
};

class QFBank;

//! tests whether a bank/file is accessible in a given context
class IQFBankContext
{
  public:
    virtual bool IsAccessible(QFBank* bank) const = 0;
};

//! stream that is opened from some file bank
class QIFStreamB : public QIFStream
{
    const QFBank* _bank;

  public:
    static QFBank* AutoBank(const char* name);
    static bool FileExist(const char* name, IQFBankContext* context = nullptr);

    static void ClearBanks();

    QIFStreamB();

    void AutoOpen(const char* name, IQFBankContext* context = nullptr); // autoselect bank/file

    void open(const QFBank& bank, const char* name); // open and preload file
    bool IsFromBank(const QFBank* bank) const;
};

class FindBank
{
#ifdef _WIN32
    char _wild[1024];
    // check all "pb?" file banks
    void* _info;
    intptr_t _handle; // Must be intptr_t for 64-bit compatibility (_findfirst returns intptr_t)

#else
    DirScanner _scanner;
#endif
  public:
    FindBank();
    ~FindBank();

    bool First(const char* path);
    bool Next();
    void Close();

    const char* GetName() const;
};

class BankContextBase : public RefCount
{
};

typedef bool (*OpenCallback)(QFBank* bank, BankContextBase* context);

// Where a bank gets mounted in the virtual filesystem.
//
// An archive may declare its own virtual root in its header-extension block, and
// where it does, that declaration is the only correct answer: its entries are
// stored relative to it, so a bank mounted anywhere else cannot resolve a single
// path that references it. The caller-derived name is what the OFP-era archives
// need, because they carry no header-extension block at all.
//
// Measured across the local installs before this rule was adopted: of 215 CWA
// retail and 14 Demo archives, 3 declare a prefix and none of them differs from
// its filename, so preferring the declaration changes nothing there. Of 487
// Arma 3 archives all 487 declare one and 483 differ -- rocks_f.pbo declares
// a3\rocks_f, and anims_f_data.pbo declares a3\anims_f\data, which no rule based
// on the filename could ever produce.
//
// Free function so the rule can be tested without a filesystem or an open bank.
inline RString ResolveBankMountName(const RString& declaredPrefix, const RString& bankPrefix, const RString& bName,
                                    bool emptyPrefix)
{
    // Guard the one thing a declaration must not do: escape the virtual
    // namespace. This value came out of a file and is about to become the prefix
    // that lookups are resolved against.
    const bool declaredIsSafe = declaredPrefix.GetLength() > 0 && declaredPrefix.GetLength() < 240 &&
                                strstr(declaredPrefix, "..") == nullptr && strchr(declaredPrefix, ':') == nullptr;
    if (declaredIsSafe)
        return declaredPrefix;
    if (emptyPrefix)
        return bName;
    return bankPrefix + bName;
}

class BankList : public AutoArray<QFBank>
{
  public:
    void Load(const RString& path, const RString& bankPrefix, const RString& bName, bool emptyPrefix,
              OpenCallback beforeOpen = nullptr, OpenCallback afterOpen = nullptr, BankContextBase* context = nullptr);
    void Unload(const RString& bankPrefix, const RString& bName, bool emptyPrefix);
    void Lock(const RString& prefix);
    void Unlock(const RString& prefix);
    void SetLockable(const RString& prefix, bool lockable);

    bool UnloadUnused();
};

extern BankList GFileBanks;

int CmpStartStr(const char* str, const char* start);

struct FileInfoO
{
    RStringB name;

    int compressedMagic;
    int uncompressedSize;
    int32_t startOffset;
    int32_t time;
    int32_t length;
#if _ENABLE_PATCHING
    bool loadFromFile; // fast patching enabled
#endif

    FileInfoO()
    {
#if _ENABLE_PATCHING
        loadFromFile = false;
#endif
    }

    const char* GetKey() const { return name; }
};

#include <Poseidon/Foundation/Containers/Array.hpp>

typedef MapStringToClass<FileInfoO, AutoArray<FileInfoO>> FileBankType;

typedef void* WINHANDLE;

// interface for creating file log

class IBankLog : public RefCount
{
  public:
    virtual void Init(const char* bankName) = 0;
    virtual void LogFileOp(const char* name) = 0;
    virtual void Flush(const char* bankName) = 0;
};

class FileBufferMapped;
class FileBufferOverlapped;

//! any bank may have several properties attached
struct QFProperty
{
    RString name;
    RString value;
};

// Virtual read-only file system: access is faster and storage more compact than
// going through OS file services.
class QFBank;

class QFBankFunctions
{
  public:
    QFBankFunctions() = default;
    virtual ~QFBankFunctions() = default;

    virtual bool FreeUnusedBanks(size_t sizeNeeded) const;
};

// Filebank compression/encryption interface. An instance already holds whatever
// key material it needs to encrypt or decrypt.
class IFilebankEncryption : public RefCount
{
  public:
    virtual bool Decode(char* dst, long lensb, QIStream& in) = 0;
    virtual void Encode(QOStream& out, const char* dst, long lensb) = 0;
};

Ref<IFilebankEncryption> CreateFilebankEncryption(const char* name, const void* context);
void RegisterFilebankEncryption(const char* name, IFilebankEncryption* (*createFunction)(const void* context));

#define MT_SAFE 0
class QFBank
{
#if MT_SAFE
    mutable CriticalSection _lock;
#endif
#if !_RELASE
    mutable bool _serialize; // help finding bugs
#endif
    Ref<IBankLog> _log;

    friend class QIFStream;

  private:
    // remember parameters necessary for opening
    //! name provided by open call
    RString _openName;
    //! callback provided by open call
    OpenCallback _openBeforeOpenCallback;
    //! context provided by open call
    Ref<BankContextBase> _openContext;

    //! locked banks cannot be unloaded
    mutable bool _locked;
    //! only lockable banks can be locked/unlocked
    bool _lockable;

    RString _prefix;
    FileBankType _files;
    AutoArray<QFProperty> _properties;

    Ref<IFileBuffer> _fileAccess;
    //! handle used for normal file access
    WINHANDLE _handle;
    //! handle used for overlapped file access
    WINHANDLE _handleOverlapped;
    mutable long _pos;
    mutable long _wantPos;

    //! set true when an attempt to open (DoOpen) failed
    bool _error;

    // AST-003 -- memoised SHA-256 of member content, keyed by the member's
    // canonical (lowercased, native-slash) name. Empty until something asks.
    // Mutable because GetContentHash is const for the same reason Load() is:
    // it changes nothing an observer can see except how long the next call takes.
    mutable std::map<std::string, std::string> _contentHashes;

    static QFBankFunctions* _defaultFunctions;
    static void SetDefaultFunctions(QFBankFunctions* f) { _defaultFunctions = f; }

  public:
    // If nothing registered the slot, fall back to a no-op base QFBankFunctions
    // (SIOF-safe lazy init).
    static QFBankFunctions* DefaultFunctions()
    {
        if (_defaultFunctions)
            return _defaultFunctions;
        static QFBankFunctions fallback;
        return &fallback;
    }

    QFBank();
    ~QFBank();
    // QFBank owns raw descriptors (_handle/_handleOverlapped) that Clear() closes.
    // AutoArray<QFBank> relocates elements by move-construct + destruct-source
    // (ModernTraits::MoveData); the implicit move falls back to copy (the declared
    // destructor suppresses it), which would copy the descriptor and then let the
    // source's destructor close it — invalidating the relocated bank. Provide an
    // ownership-transferring move ctor; keep copy defaulted for the container's
    // copy/insert paths.
    QFBank(const QFBank&) = default;
    QFBank& operator=(const QFBank&) = default;
    QFBank(QFBank&& other);
    RString GetPrefix() const { return _prefix; }
    RString GetOpenName() const { return _openName; }
    void ScanPatchFiles(RString prefix, RString subdir);
    void SetPrefix(RString prefix);
    bool open(RString name, OpenCallback beforeOpen = nullptr, BankContextBase* context = nullptr);
    //! load bank - physically performs the action
    bool Load();
    //! open bank and mark it as locked (cannot be un-opened)
    void Lock() const;
    void SetLockable(bool lockable) { _lockable = lockable; }
    bool GetLockable() const { return _lockable; }
    //! mark bank as unlocked and unload if possible
    void Unlock() const;
    bool IsLocked() const { return _locked; }
    //! a bank in use cannot be unloaded
    bool CanBeUnloaded() const;
    //! const overload - modifies state via const_cast
    bool Load() const { return const_cast<QFBank*>(this)->Load(); }
    void Unload();
    //! const overload - modifies state via const_cast
    void Unload() const
    {
        if (!_handle)
        {
            return;
        }
        const_cast<QFBank*>(this)->Unload();
    }
    void Clear(); // release all files
    void close() { Clear(); }

    bool error() const;

    const RString& GetProperty(const RString& name) const;

    const FileInfoO& FindFileInfo(const char* name) const;
    // Required identity is unsupported on POSIX and returns nullopt. The default
    // retains the existing unleased read path and performs no native capture.
    std::optional<BankReadRequest> CaptureReadRequest(const char* member, bool requireArchiveIdentity = false) const;
    // Model-page preparation only: exact Cprs member, bounded encoded/decoded
    // sizes and mandatory Windows physical archive lease. Existing raw API is
    // deliberately unchanged and continues to refuse compressed members.
    std::optional<BankCompressedReadRequest> CaptureCompressedReadRequest(
        const char* member, uint32_t maxEncodedBytes = BankCompressedReadRequest::MaxEncodedBytes,
        uint32_t maxDecodedBytes = BankCompressedReadRequest::MaxDecodedBytes) const;
    // Owner-only observation of an ALREADY loaded mount. Never Load/Open/Read/Seek;
    // false includes unsupported/unknown. The source must retain its original lease.
    bool MatchesMountedMember(const char* member, const BankReadRequest& initializedSource) const;
    bool MatchesMountedCompressedMember(const char* member, const BankCompressedReadRequest& initializedSource) const;
    bool FileExists(const char* name) const;
    // low level access to bank
    void Read(char* buf, long size, const char* name) const; // read raw bytes from bank
    void Seek(long pos) const;

    //! read file - uncompress if necessary
    Ref<IFileBuffer> Read(const char* file) const;
    //! read file using overlapped I/O - uncompress if necessary
    Ref<IFileBuffer> ReadOverlapped(const char* file) const;

    // AST-003 -- SHA-256 (lowercase hex) of a member's CONTENT, or "" when the
    // member does not exist or cannot be read.
    //
    // This is the producer half of DerivedAssetKey (AST-020): a derived artefact
    // may only be reused when every input's *content* is unchanged, and until now
    // nothing could state the content of an asset living inside a PBO -- which is
    // nearly all of them. The PBO header carries name, packing magic, uncompressed
    // size, offset and mtime per member, and no checksum of any kind; the 21-byte
    // trailer that Arma-2-era and later archives carry is a SHA-1 of the WHOLE
    // FILE, so it identifies an archive, never a member, and this reader has never
    // read it. Hence: hash the bytes.
    //
    // What is hashed is the LOGICAL content -- what Read() hands back, after
    // decompression or decryption. Two archives storing identical bytes, one raw
    // and one Cprs, must agree, because for the consumer they are the same asset.
    // Hashing the stored bytes would be cheaper and would make the packer's choice
    // part of the asset's identity, which is exactly the kind of accidental input
    // the key exists to exclude.
    //
    // LAZY, not computed at index time: hashing every member on Load() is a second
    // full pass over the archive (0.47 GiB across the 44 retail CWA archives, 44.2
    // GiB across the 546 Arma 3 ones) for members that are mostly never opened.
    // The first call for a member pays a hash of its bytes; later calls are a map
    // lookup. Nothing in the engine calls this yet -- it is opt-in by construction.
    RString GetContentHash(const char* name) const;

    // Number of members whose hash has been computed. Exists so a test can prove
    // memoisation rather than assume it.
    size_t ContentHashCacheSize() const { return _contentHashes.size(); }
    int GetFileOrder(const char* file);

    void ForEach(void (*Func)(const FileInfoO& fi, const FileBankType* files, void* context),
                 void* context) const; // call Func for all files
    static bool IsNull(const FileInfoO& value) { return FileBankType::IsNull(value); }
    static bool NotNull(const FileInfoO& value) { return FileBankType::NotNull(value); }
    static FileInfoO& Null() { return FileBankType::Null(); }

    bool BufferOwned(const FileBufferMapped* buffer) const;
    bool BufferOwned(const FileBufferOverlapped* buffer) const;

    static bool FreeUnusedBanks(size_t sizeNeeded) { return DefaultFunctions()->FreeUnusedBanks(sizeNeeded); }
};

extern RString GFileBankPrefix;

// Resolve a base-relative path to an enabled mod's copy of the same file, "" if none.
std::string ResolveModOverride(const char* relPath);

} // namespace Poseidon

using ::Poseidon::BankList;
using ::Poseidon::CmpStartStr;
using ::Poseidon::FileBankType;
using ::Poseidon::FileInfoO;
using ::Poseidon::FindBank;
using ::Poseidon::GFileBankPrefix;
using ::Poseidon::GFileBanks;
using ::Poseidon::QFBank;
using ::Poseidon::QIFStreamB;

extern bool GLogFileOps;
extern bool GUseFileBanks;

#endif
