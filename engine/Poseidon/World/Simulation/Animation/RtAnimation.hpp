#pragma once

#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Foundation/Containers/BankArray.hpp>


namespace Poseidon
{
struct AnimationRTPair
{
	char _sel;            // matrix index
	unsigned char _weight; // quantized weight: value * WeightScale
	#define WeightScale 128.0f
	#define InvWeightScale (1.0f/128)
	AnimationRTPair(){}
	AnimationRTPair( int s, float w ):_sel(s),_weight(toIntFloor(w*WeightScale)){}
	int GetSel() const {return _sel;}
	float GetWeight() const {return _weight*InvWeightScale;}
	void SetWeight( float w ) {_weight=toIntFloor(w*WeightScale);}
};


} // namespace Poseidon
#include <Poseidon/Foundation/Containers/SmallArray.hpp>
namespace Poseidon
{

// max 4 matrices per vertex
const int ARTWMaxSize = sizeof(AnimationRTPair)*4+sizeof(int);

class AnimationRTWeight: public VerySmallArray<AnimationRTPair,ARTWMaxSize>
{
	public:
	void Normalize();
	//bool GetSimple() const {return _simple;}
};


class AnimationRTWeights
{
	friend class AnimationRT;

	AutoArray<AnimationRTWeight> _data;
	FindArray<int> _selections;
	bool _needNormalize;
	bool _isSimple;  // each point belongs to exactly one selection

	public:
	AnimationRTWeights();
	AnimationRTWeights( Shape *shape ){Init(shape);}
	~AnimationRTWeights();

	void Init( Shape *shape );
	void AddSelection( Shape *shape, int selIndex, int matrixIndex );
	// FACE-002: move every vertex of a named selection ONTO `matrixIndex` at full weight,
	// discarding whatever it was bound to. Unlike AddSelection this does not blend -- the
	// face rig needs the whole offset, not a share of it. Returns the matrix index those
	// vertices were bound to before (the most common one), or -1 if the selection is empty,
	// unweighted, or does not agree on a single parent bone. Vertices whose bone already
	// lies in [faceLo,faceHi) belong to an earlier face group and are left with it.
	int ReplaceSelection( Shape *shape, int selIndex, int matrixIndex, int faceLo, int faceHi );
	int	MatrixIndex( RStringB name ) const;
	void Normalize();
	bool IsSimple() const {return _isSimple;}
	const AnimationRTWeight &operator [] ( int i )const {return _data[i];}
	const AnimationRTWeight *Data() const {return _data.Data();}

};

class Skeleton;
class AffinePhasePose;

} // namespace Poseidon
namespace Poseidon { namespace Asset { namespace Formats { struct RTMPhase; struct RTMAnimation; } } }
namespace Poseidon
{

class AnimationRTPhase
{
	friend class AnimationRT;
	AutoArray<Matrix4> _matrix;

	public:
	AnimationRTPhase();
	~AnimationRTPhase();

	// returns phase time
	float Load( QIStream &in, Skeleton *skelet, int nSel, bool reversed=false );
	float LoadFromRTM(const Poseidon::Asset::Formats::RTMPhase& phase, const int* boneMap, int nSel, bool reversed=false);

	void Reverse();
	void SerializeBin(SerializeBinStream &f);
};


struct WeightInfoName
{
	Link<LODShapeWithShadow> shape;
	Link<Skeleton> skeleton;
	WeightInfoName(){shape=nullptr,skeleton=nullptr;}
};

// FACE-002: the 2001 face rig moves eight named selections. See Head.hpp.
const int NFaceGroups = 8;

class WeightInfo: public RefCount
{
	WeightInfoName _name;
	AnimationRTWeights _data[MAX_LOD_LEVELS];

	public:
	// FACE-002: index of the first of NFaceGroups synthetic bones reserved on the skeleton
	// for the face rig, or -1 when this (shape, skeleton) pair has no face selections.
	// `_faceParent[level][group]` is the bone those vertices were bound to before the
	// rebind -- the face bone is that bone's matrix with the face offset added to its
	// translation -- or -1 where the group is absent from that level.
	int _faceBoneBase = -1;
	signed char _faceParent[MAX_LOD_LEVELS][NFaceGroups];

	WeightInfo();
	WeightInfo( const WeightInfoName &name );

	const WeightInfoName &GetName() const {return _name;}

	AnimationRTWeights &operator [] (int i){return _data[i];}
	const AnimationRTWeights &operator [] (int i) const {return _data[i];}
};


template<>
struct BankTraits<WeightInfo>
{
	typedef const WeightInfoName &NameType;
	static int CompareNames( const WeightInfoName &n1, const WeightInfoName &n2 )
	{
		int d = (char *)n1.shape.GetTypeRef()-(char *)n2.shape.GetTypeRef();
		if (d) return d;
		d = (char *)n1.skeleton.GetTypeRef()-(char *)n2.skeleton.GetTypeRef();
		return d;
	}
	typedef RefArray<WeightInfo> ContainerType;
};

class Skeleton: public RefCountWithLinks
{
	RStringB _name;
	AutoArray<RStringB> _matrixNames;

	public:
	Skeleton();
	Skeleton( RStringB name );
	RStringB GetName() const {return _name;}

	int NewBone( RStringB name );
	int FindBone( RStringB name ) const;
	int NBones() const {return _matrixNames.Size();}
	RStringB GetBone( int i ) const {return _matrixNames[i];}

	void Prepare(LODShape *lShape, WeightInfo &weights);
};

template<>
struct BankTraits<Skeleton>
{
	typedef const RStringB &NameType;
	static int CompareNames( NameType n1, NameType n2 )
	{
		return n1!=n2;
	}
	typedef RefArray<Skeleton> ContainerType;
};

extern BankArray<Skeleton> Skeletons;

typedef StaticArrayAuto<Matrix4> Matrix4Array;

#define MATRIX_4_ARRAY(name,size) AUTO_STATIC_ARRAY(Matrix4,name,size)


} // namespace Poseidon
#include <Poseidon/Foundation/Memory/FastAlloc.hpp>
namespace Poseidon
{

struct BlendAnimInfo
{
	//RStringB name;
	int matrixIndex;
	float factor;
};

struct AnimationRTName
{
	RStringB name;
	Ref<Skeleton> skeleton; // skeleton lifetime is tied to the animation
};

class AnimationRT: public RefCount, public CLDLink
{
	AutoArray<AnimationRTPhase> _phases;
	AutoArray<float> _phaseTimes;
	Vector3 _step;              // movement per animation cycle
	AutoArray<RStringB> _selections;
	AnimationRTName _name;

	bool _looped;
	bool _reversed;

	int _preloadCount;
	int _loadCount;
	int _nPhases;

	protected:
	void Find( int &prevIndex, int &nextIndex, float time ) const;
	static void ApplyMatricesComplex
	(
		const AnimationRTWeights &lWeights, LODShape *lShape, int level,
		const Matrix4Array &matrices
	);
	static void ApplyMatricesSimple
	(
		const AnimationRTWeights &lWeights, LODShape *lShape, int level,
		const Matrix4Array &matrices
	);
	static void ApplyMatricesIdentity
	(
		LODShape *lShape, int level
	);

	private:
	AnimationRT( const AnimationRT &src ); // no copy
	void operator = ( const AnimationRT &src );

	public:
	AnimationRT();
	AnimationRT( const AnimationRTName &name, bool reversed=true );
	// Dev corpse proof: copies the actual loaded phase pair, without changing playback.
	bool CapturePhasePose(AffinePhasePose &pose, float time) const;
	~AnimationRT() override;

	void AddPreloadCount();
	void ReleasePreloadCount();
	void Load(QIStream &in, Skeleton *skelet, const char *name, bool reversed);
	void FullLoad(Skeleton *skelet, const char *file);
	void FullRelease();

	void AddLoadCount();
	void ReleaseLoadCount();

	// ScopeLock interface
	void Lock() const   { const_cast<AnimationRT *>(this)->AddLoadCount(); }
	void Unlock() const { const_cast<AnimationRT *>(this)->ReleaseLoadCount(); }

	void Load(Skeleton *skelet, const char *file, bool reversed=true);
	void Reverse();
	void SerializeBin(SerializeBinStream &f);

	bool LoadOptimized(Skeleton *skelet, QIStream &f);
	void SaveOptimized(Skeleton *skelet, QOStream &f);

	static void TransformMatrix
	(
		Matrix4 &mat,
		const AnimationRTWeights &lWeights, Shape *shape,
		Matrix4Par trans,
		const BlendAnimInfo *blend, int nBlend, int index
	);
	static void TransformPoint
	(
		Vector3 &pos,
		const AnimationRTWeights &lWeights, Shape *shape,
		Matrix4Par trans,
		const BlendAnimInfo *blend, int nBlend, int index
	);
	static void CombineTransform
	(
		const WeightInfo &lWeights, LODShape *lShape, int level,
		Matrix4Array &matrices, Matrix4Par trans,
		const BlendAnimInfo *blend, int nBlend
	);
	void PrepareMatrices
	(
		Matrix4Array &matrices, float time, float factor
	) const;
	static void ApplyMatrices
	(
		const WeightInfo &lWeights, LODShape *lShape, int level,
		const Matrix4Array &matrices
	);
	static Vector3 ApplyMatricesPoint
	(
		const AnimationRTWeights &lWeights, LODShape *lShape, int level,
		const Matrix4Array &matrices, int pointIndex
	);

	void Apply
	(
		const WeightInfo &lWeights, LODShape *lShape, int level, float time
	) const;
	void Matrix
	(
		Matrix4 &mat, float time, const AnimationRTWeight &wgt
	) const;
	Vector3 Point
	(
		const WeightInfo &lWeights, LODShape *lShape, int level,
		float time, int index
	) const;
	const AnimationRTName &GetName() const {return _name;}
	const char *Name() const {return _name.name;}
	float GetStepLength() const {return _step.Z();}
	float GetStepLengthX() const {return _step.X();}
	Vector3Val GetStep() const {return _step;}
	int GetKeyframeCount() const {return _nPhases;}
	const Skeleton *GetSkeleton() const {return _name.skeleton;}

	void Prepare(LODShape *lShape, Skeleton *skelet, WeightInfo &weights, bool looped);
	void RemoveLoopFrame();
	void ForceMatrixOrientation(int matIndex, const Matrix3 &orient, float factor);
	void IntroduceStep();
	void IntroduceXStep();

	void SetLooped( bool looped );
	bool GetLooped() const {return _looped;}
	
	USE_FAST_ALLOCATOR
};

class AnimationRTLoad
{
};

} // namespace Poseidon
