#pragma once

#include <Poseidon/IO/Streams/QStream.hpp>

// File buffering class. MAIN THREAD ONLY, despite the "MT" in FileServerMT.hpp: the
// only implementation is FileServerST, ENABLE_OVERLAPPED_IO compiles out the request
// queue (FileServer.cpp:19), and FileCache keeps an UNLOCKED MRU array — a worker
// reading through this races the main thread's MoveToFront. Workers use the pure
// loose-file readers instead (ModelCache::LoadLooseFile, ReadPAABlockChain,
// ReadEmatFileLoose). This line said "suitable for background (multithreaded) usage"
// until 2026-08-30; the roadmap ownership table (design notes)
// is where the claim was caught.


namespace Poseidon
{
class QFBank;

class FileServer: public RefCount
{

	public:
	// abstract class - single/multi threaded file loading
	// say in advance we will need the file - insert it into the queue
	virtual void Request( const char *name, float time, int from=0, int to=INT_MAX)= 0;
	virtual void CancelRequest( const char *name, int from=0, int to=INT_MAX )= 0;
	// say we need the file NOW - load it
	virtual void Open( QIFStream &stream, const char *name ) = 0;

	virtual void Start() = 0; // start/stop background activity
	virtual void Stop() = 0;

	virtual void FlushBank(QFBank *bank) = 0;
    
};

extern Ref<FileServer> GFileServer;

} // namespace Poseidon

using Poseidon::GFileServer;
extern bool GEnableCaching;
