#pragma once

#include <Poseidon/Foundation/Types/Memtype.h>
#include <Poseidon/Core/Types.hpp>
#include <Poseidon/Foundation/Memory/Heap.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <Poseidon/Graphics/Rendering/Colors.hpp>

#include <Poseidon/Foundation/Containers/BoolArray.hpp>

#include <cstdint>


#define DO_FILLS 0 // fill on Alloc and Free




namespace Poseidon
{
struct BankReadMemberIdentity;
class MemoryHeap: public Poseidon::Foundation::Heap<byte *,int>
{
	#if DO_FILLS
		#define NEW_FILL 0xfffdfefc // it can be decoded as 1555 ARGB
		#define DEL_FILL 0x02010301 // it can be decoded as 1555 ARGB

		public:
		HeapItem *Alloc( int size );
		void Free( HeapItem *pos );
	#endif
};

typedef Poseidon::Foundation::Heap<byte *,int>::HeapItem MemoryItem;




class SystemHeap: public MemoryHeap
{
	private:
	byte *_alocated;
	int _size;

	public:
	SystemHeap();
	void Release();
	~SystemHeap();
	void Init( int size );
	void *Base() const {return Memory();}
	bool Check() const;
};


enum PacFormat
{
	PacP8,PacAI88,PacRGB565,PacARGB1555,PacARGB4444,
	PacARGB8888,
	PacDXT1,PacDXT2,PacDXT3,PacDXT4,PacDXT5,
	// RFG-047 -- the DXGI-era block formats, which the 2001 Pac CONTAINER cannot hold
	// and never will (they have no PAA magic word; PixelFormat.hpp says so). They are
	// here only as an in-memory format LABEL, so a source that read them out of a DDS/EDDS
	// can hand them to a backend that speaks them without a decode. Nothing writes them
	// to a .paa/.pac, and PAADecoder's `format >= PacDXT1 && format <= PacDXT5` range
	// tests deliberately exclude them: appended AFTER PacDXT5 so those keep their meaning.
	PacBC4,PacBC5,PacBC7,
	PacFormatN
};

class PacPalette;

// get() returns -1 at EOF; mask each byte and combine in unsigned so a -1 never
// becomes a negative shift and a high byte never overflows int (both UB). Callers
// check f.eof()/f.fail() rather than the return value, so the EOF result is moot.
inline int fgetiw( QIStream &f )
{
	unsigned c=f.get()&0xff,r=f.get()&0xff;
	return (int)((r<<8)|c);
}
inline int fgeti24( QIStream &f )
{
	unsigned c=f.get()&0xff,r=f.get()&0xff;
	c|=r<<8;
	r=f.get()&0xff;
	c|=r<<16;
	return (int)c;
}
inline int fgetil( QIStream &f )
{
	unsigned c=f.get()&0xff,r=f.get()&0xff;
	c|=r<<8;
	r=f.get()&0xff;
	c|=r<<16;
	r=f.get()&0xff;
	c|=r<<24;
	return (int)c;
}

#define CHECKSUMS 0

class PacLevelMem
{
	public:
	// short for texture dimensions and pitch should be enough
	short _w,_h;
	short _pitch; // aligned row pitch (may exceed _w)

	SizedEnum<PacFormat,char> _sFormat; // source (file) format
	SizedEnum<PacFormat,char> _dFormat; // destination (memory) format

	int _start; // where in the file this mipmap level start



	public:
	PacLevelMem();

	PackedColor GetPixel(void *data, float u, float v) const;
	PackedColor GetPixelInt(void *data, int u, int v) const;

	static void DecompressDXT1(void *dst, const void *src, int w, int h);

	protected:

	// paa 16b formats loading
	int LoadPaaBin16( QIStream &in, void *mem, const PacPalette *pal ) const;
	int LoadPaaDXT( QIStream &in, void *mem, const PacPalette *pal ) const;


	// pac formats loading
	int LoadPacARGB1555( QIStream &in, void *mem, const PacPalette *pal ) const;
	int LoadPacRGB565( QIStream &in, void *mem, const PacPalette *pal ) const;
	int LoadPacP8( QIStream &in, void *mem, const PacPalette *pal ) const;
	
	public:
	void Interpolate
	(
		void *data, void *withData,
		const PacLevelMem &with, float factor
	);

	int LoadPac( QIStream &in, void *mem, const PacPalette *pal ) const; // format set by init

	int LoadPaa( QIStream &in, void *mem, const PacPalette *pal ) const; // format set by init

	void SetStart( int offset ){_start=offset;}
	int Init(QIStream &in, PacFormat sFormat);
	void SetDestFormat(PacFormat dFormat, int align);
	void SeekLevel( QIStream &in ) const;
	PacFormat SrcFormat() const {return _sFormat;}
	PacFormat DstFormat() const {return _dFormat;}
	int Pitch() const {return _pitch;}
	int Size() const {return _pitch*_h;}
	bool TooLarge( int max=256 ) const {return _w>max || _h>max;}
};

PacFormat PacFormatFromDesc( int desc, bool &alpha );

class MipInfo
{
	public:
	Texture *_texture;
	int _level;

	MipInfo( Texture *texture, int level )
	:_texture(texture),_level(level)
	{}
	MipInfo()
	:_texture(nullptr),_level(-1)
	{}

	bool IsOK() const {return _level>=0;}
};



#define TRANSPARENT1_RGB 0xff00ff
#define TRANSPARENT2_RGB 0x00ffff

class PacPalette
{

	public:
	int _nColors; // palette size
	DWORD *_palette; // 24-bit RGB format

	Color _averageColor;
	PackedColor _averageColor32;

	// this information is same for all mipmaps, but is also copied in mipmap header
	int _transparentColor;
	bool _isAlpha,_isTransparent;

	#if CHECKSUMS
		mutable	bool _checksumValid;
		mutable	int _checksum;
	#endif

	public:
	PacPalette();
	~PacPalette();

	int Load( QIStream &in, int *startOffsets, int maxStartOffsets );
	static int Skip( QIStream &in );

	ColorVal AverageColor() const {return _averageColor;}
	PackedColor AverageColor32() const {return _averageColor32;}

	NoCopy(PacPalette)
};


//! One level of a batched mipmap read.  See ITextureSource::GetMipmapChain.
struct MipmapRead
{
	void *mem;					//!< destination, sized for _mip_'s destination format
	const PacLevelMem *mip;		//!< that level's layout, as Init() filled it in
	int level;					//!< level index, 0 = full resolution
};

class ITextureSource
{
	public:
	virtual ~ITextureSource() {}

	// Init() must be called before any other method.
	virtual bool Init(const char *name, PacLevelMem *mips, int maxMips) = 0;
	virtual int GetMipmapCount() const = 0;
	virtual PacFormat GetFormat() const = 0;

	virtual bool IsAlpha() const = 0;
	virtual bool IsTransparent() const = 0;
	virtual bool GetMipmapData(void *mem, const PacLevelMem &mip, int level) const = 0;

	//! Read a run of levels in ONE call.  Returns false if any level failed, in which case
	//! the destinations hold undefined bytes and the caller must not upload them.
	//!
	//! Exists because the per-level entry point is where a source pays its per-CALL cost,
	//! not its per-BYTE cost: TextureSourcePac answers every GetMipmapData by re-opening
	//! the texture through the VFS, so uploading a texture cost one VFS open per LEVEL --
	//! up to MAX_MIPMAPS (7) of them, and 3 even for the 64-square test fixture. The
	//! bytes still have to be LZO-decompressed either way; the opens did not.
	//! The default implementation is exactly that loop, so a source with no per-call
	//! overhead (DDS, JPEG) keeps its current behaviour byte for byte.
	virtual bool GetMipmapChain(const MipmapRead *reads, int count) const;

	virtual PackedColor GetAverageColor() const = 0;

	// Force alpha blending when known from elsewhere.
	virtual void ForceAlpha() = 0;

	// Appended: evidence from the exact buffer used by successful Init. Unknown
	// for cached unwrapped/loose/transformed sources; never recaptured by name.
	virtual std::shared_ptr<const ArchiveSourceBinding> GetArchiveSourceBinding() const { return {}; }
	// Copied only after successful Init from the exact parsed PBO buffer.
	// Consumers must also verify the current mount before using prepared data.
	virtual bool CopyArchiveMemberIdentity(BankReadMemberIdentity&) const { return false; }

	// Appended owner-only optional retirement of this source's Init evidence.
	// Does not change headers/pixels/reads; copied aliases keep their own debt.
	virtual bool ReleaseArchiveSourceBinding() { return false; }
};

//! Count of VFS opens made to read texture mip data, process-wide since start.
//!
//! The point of the batched read above is to lower this number, and "structurally less
//! work" is not a measurement — so the number is readable, both by the unit test that
//! pins the batching and by whoever is attributing a streaming transient at runtime.
uint64_t TextureMipOpenCount();

class ITextureSourceFactory
{
	public:
	virtual bool Check(const char *name) = 0;
	// Optional: preload files so a later Create() is quick.
	virtual void PreInit(const char *name) = 0;
	virtual ITextureSource *Create(const char *name, PacLevelMem *mips, int maxMips) = 0;
};

// Pick a texture source factory from the file name's extension.
ITextureSourceFactory *SelectTextureSourceFactory(const char *name);


} // namespace Poseidon
