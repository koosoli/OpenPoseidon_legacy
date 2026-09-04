#pragma once

#include <Poseidon/Asset/Formats/Enfusion/EnfusionMount.hpp>
#include <cstring>
#include <vector>
#include <Poseidon/IO/FileServer.hpp>
#include <Poseidon/Foundation/Memory/FastAlloc.hpp>
#include <Poseidon/Foundation/Containers/StaticArray.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/Foundation/Memory/MemFreeReq.hpp>


namespace Poseidon
{
class FileInCache
{
public:
  RString _name;
  QIFStreamB _data;

  FileInCache(){}
  ~FileInCache(){}
  FileInCache( const char *name )
  {
    _name = name;
    _data.AutoOpen(_name);
    // RFG-017: a natively loaded Enfusion world names files INSIDE a `.pak`, which
    // no bank can open. The texture streamer re-opens every mip THROUGH this cache,
    // so a fallback further down (in the DDS source) gets the first read and then
    // every mip after it fails -- which is how Everon's trees kept their textures
    // and still drew white.
    //
    // Last resort on purpose: the file layer is asked first and the mount is closed
    // for every world that is not Enfusion, so this costs one failed AutoOpen on a
    // path that was already failing.
    if (_data.fail())
    {
      const auto &mount = Poseidon::Asset::Formats::Enfusion::EnfusionMount::Instance();
      std::vector<uint8_t> bytes;
      if (mount.IsOpen() && mount.Read(name, bytes) && !bytes.empty())
      {
        FileBufferMemory *buffer = new FileBufferMemory(static_cast<int>(bytes.size()));
        memcpy(buffer->GetWritableData(), bytes.data(), bytes.size());
        _data.OpenBuffer(Ref<IFileBuffer>(buffer));
      }
    }
  }
  FileInCache( QIFStreamB &in, const char *name )
  {
    _name = name;
    _data = in;
  }
  USE_FAST_ALLOCATOR
};

class FileCache: public ::Poseidon::Foundation::MemoryFreeOnDemandHelper
{
	int _size;
	int _maxSize;
	int _maxFiles;

	AutoArray< SRef<FileInCache> > _cache;

	protected:
	int Find( const char *name );
	void MoveToFront( int index );
	void DeleteLast();

	public:
	FileCache( int size=8*1024*1024, int files=1024 );
	~FileCache() override;

	bool IsLoaded(const char *name);
	//! open file and cache it as necessary
	void Open( QIFStream &stream, const char *name );
	//! we have opened the file and we want to give it caching opportunity
	void Store( QIFStreamB &stream, const char *name );
	//! flush any old data
	void Maintain();
	int Load( const char *name );

	void FlushBank(QFBank *bank);

	// implementation of MemoryFreeOnDemandHelper abstract interface
	size_t FreeOneItem() override;
	float Priority() override;

	// memory-budget observability (dev panel + EnforceBudgets). The file cache
	// already carries a real budget — _maxSize, from ENGINE_CONFIG.fileHeapSize.
	const char *DomainName() const override {return "FileCache";}
	size_t HeldBytes() const override {return (size_t)_size;}
	size_t BudgetBytes() const override {return (size_t)_maxSize;}
};


//! file request - waiting in queue to
struct FileRequest
{
	//! source file name
	RString _filename;
	//! file region we are interested in
	int _from,_to;
	//! time when we will need the file
	DWORD _timeNeeded;

	//! stream where result should be stored
	QIFStreamB _in;

	//! overlapped operator performed here
	Ref<IFileBuffer> _filebuf;

	FileRequest(){}
	FileRequest(const char *name, DWORD time, int from=0, int to=INT_MAX)
	:_filename(name),_timeNeeded(time),_from(0),_to(INT_MAX)
	{
	}

	bool operator == (const FileRequest &with) const;
	bool Contains(const FileRequest &with) const;
};


template <>
struct HeapTraits<FileRequest>
{
	static bool IsLess(const FileRequest &a, const FileRequest &b)
	{
		return a._timeNeeded < b._timeNeeded;
	}
	static bool IsLessOrEqual(const FileRequest &a, const FileRequest &b)
	{
		return a._timeNeeded <= b._timeNeeded;
	}
};

class FileServerST: public FileServer
{
	FileCache _cache;
	//! list of satisfied request
	FindArray<FileRequest> _done;
	//! heap of waiting requests
	HeapArray<FileRequest> _queue;

	public:
	FileServerST( int cacheSize )
	:_cache(cacheSize)
	{
	}
	
	//! find any matching request
	bool RequestPresent(const FileRequest &req) const;
	int RequestPresentAndDone(const FileRequest &req) const;
	int RequestPresentAndNotDone(const FileRequest &req) const;

	// say in advance we will need the file - insert it into the queue
	void Request(const FileRequest &req);
	void CancelRequest(const FileRequest &req);

	void Request(const char *name, float time, int from, int to) override;
	void CancelRequest(const char *name, int from, int to) override;
	// say we need the file NOW - load it
	void Open( QIFStream &stream, const char *name ) override;
	void FlushBank(QFBank *bank) override;
	void Start() override{}
	void Stop() override{}

	//! main body of file-server - perform reqular maintenance
	void Update();
};

} // namespace Poseidon

using Poseidon::FileServerST;
