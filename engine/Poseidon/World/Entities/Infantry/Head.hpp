#pragma once

#include <Poseidon/World/Simulation/Animation/Animation.hpp>
#include <Poseidon/World/Simulation/Animation/RtAnimation.hpp>
#include <Poseidon/Foundation/Time/Time.hpp>


namespace Poseidon
{
DECL_ENUM(SimulationImportance)

struct RandomVector3Type
{
	Vector3 rng;
	float minT, maxT;

	void Load(const ParamEntry &cfg);
};

class RandomVector3	
{
	Vector3 _cur;
	Vector3 _spd;
	float _timeToWanted;

	public:
	RandomVector3();

	operator const Vector3 &() const {return _cur;}
	__forceinline float X() const {return _cur.X();}
	__forceinline float Y() const {return _cur.Y();}
	__forceinline float Z() const {return _cur.Z();}

	void SetWanted(Vector3Par wanted, float time);
	bool Simulate(float deltaT);
	void SetRandomTgt(Vector3Par rng, float minT, float maxT);

	bool SimulateAndSetRandomTgt
	(
		float deltaT,
		Vector3Par rng, float minT, float maxT
	);
	bool SimulateAndSetRandomTgt
	(
		float deltaT, const RandomVector3Type &type
	);
};

class HeadType
{
public:

	AnimationSection _personality;
	AnimationSection _glasses;

	Animation _lBrow, _mBrow, _rBrow;
	Animation _lMouth, _mMouth, _rMouth;

	Animation _eyelid;
	Animation _lip;

	RandomVector3Type _lBrowRandom, _mBrowRandom, _rBrowRandom;
	RandomVector3Type _lMouthRandom, _mMouthRandom, _rMouthRandom;

	Ref<Texture> _textureOrig;

	HeadType();
	void Load(const ParamEntry &cfg);
	void InitShape(const ParamEntry &cfg, LODShape *shape);
};

// FACE-001: the 2001 face rig is not bones. Head::Animate offsets the named selections
// "spodni ret" / "lkoutek" / "pkoutek" / "vicka" / the brow set directly in CPU vertex
// space, and a GPU-skinned mesh is uploaded ONCE at bind pose and posed by the vertex
// shader -- so on the wgpu backend those offsets were written and thrown away, and NPCs
// stopped moving their mouths when they speak.
//
// DEFAULT OFF, AND MEASURED SO. The CPU-pose route below is a dead end on wgpu and the
// numbers say why (2026-09-07, asset-obs-001 c9fdf538+dirty, dev-missions\face-talk.eden):
//
//   * Every unit of a model shares ONE LODShape, so ONE Shape, so ONE VertexBuffer, so ONE
//     GPU mesh. Probe: the talking soldier and the silent player report the SAME
//     VertexBuffer address (vbSet == vbUnset == 0x2aac63444a0).
//   * Because of that, each frame the posing unit uploads its CPU-posed vertices and the
//     non-posing unit immediately uploads the bind pose back into the same range:
//     sent=510, restore=510 over the same window -- exactly one restore per pose.
//   * `wgpu::Queue::write_buffer` is NOT ordered against recorded draws: every write in a
//     frame lands before ANY draw in that frame executes. So the last write wins for the
//     whole frame, and the bind-pose restore always wins.
//   * Kill door: displacing EVERY uploaded vertex of the posed mesh by 1.5 m
//     (POSEIDON_FACE_LIFT) moved nothing on screen, and scaling the face offsets x30
//     (POSEIDON_FACE_GAIN) left the mouth spread at 1.17 against a GL33 arm of 32.85.
//     The upload itself is fine -- no exit is taken and the uploader accepts it
//     (shared=0 clean=0 cnt=0 hash=0 refused=0, sent == updPosed).
//
// GL33 does not have the problem because its per-draw buffer update is ordered with the
// draw that follows it. Making this work on wgpu needs one of:
//   (a) a private per-OBJECT mesh for the frames a character is face-posing (the draw must
//       be emitted against that handle, not the shared one), or
//   (b) the face pose done on the GPU -- a per-vertex face-group id plus a small per-draw
//       uniform of offsets, applied in the vertex shader alongside the bone palette.
// (b) is the cheaper one at runtime and the only one that scales to several talkers, but it
// costs a vertex-format field and a shader change in every mesh pipeline.
// FACE-002 -- THE FIX FOR THE ABOVE, and it needs no renderer change at all.
//
// FACE-001 concluded that making the face move on wgpu needs either a private per-object
// mesh or a new per-draw offset channel plus a shader change. Both are true of the CPU-pose
// route; neither is necessary, because the channel already exists.
//
// A wgpu skinned draw already carries a PER-OBJECT, per-draw bone palette: 128 mat4 in a
// dynamic-offset UBO, rewritten for every draw (EngineWgpu::BeginMeshTL) and read by
// vs_skinned as `palette.m[bones.x]` with `bones` a per-vertex Uint8x4. An OFP soldier
// skeleton uses a few dozen of those 128 slots. So:
//
//   * reserve NFaceGroups synthetic bones on the man skeleton (they match no named
//     selection, so Skeleton::Prepare ignores them),
//   * at type-load time rebind the vertices of the eight face selections off the head bone
//     and onto those synthetic bones -- this is SHARED, identical for every soldier of the
//     model, so it changes nothing per object and rides the existing one-time SetSkinData,
//   * each frame, before ApplyMatrices, set faceBone[k] = parentBone[k] with the face
//     offset added to its translation.
//
// The offset is then per-OBJECT because the palette is per-object, which is exactly what
// the shared vertex buffer could never be. Nothing is uploaded per frame that was not
// already being uploaded, no vertex format changes, no WGSL changes, no WgrDraw3D field,
// no ABI bump. GL33 gets the same motion from the same matrices through
// ApplyMatricesSimple/Complex, so there is one path, not two.
//
// Two known approximations, both stated rather than hidden:
//   * where two face selections share a vertex, the vertex follows the FIRST group that
//     claims it (AnimationRTWeights::ReplaceSelection) instead of summing both offsets.
//   * a group whose vertices do not all hang off one parent bone is refused and that group
//     simply does not move; the rest still do.
// POSEIDON_FACE_BONES=0 disables the rig and restores the (wgpu-broken) CPU-pose route.
enum FaceGroup
{
	FaceEyelid, FaceLip, FaceLMouth, FaceMMouth, FaceRMouth, FaceLBrow, FaceMBrow, FaceRBrow
};
static_assert(FaceRBrow + 1 == NFaceGroups, "FaceGroup and NFaceGroups disagree");

bool FaceBoneRigEnabled();

// Reserve the face bones on `skeleton` and rebind the face selections of every graphical
// level of `lShape` onto them. Idempotent; call after Skeleton::Prepare has filled `weights`.
void SetupFaceBoneRig(Skeleton *skeleton, LODShapeWithShadow *lShape, WeightInfo &weights);

struct FaceAnimationLook
{
    bool enabled = false; //!< POSEIDON_FACE_ANIM=1 -- re-arm the CPU-pose route (does not work; see above)
    bool always = false;  //!< POSEIDON_FACE_ANIM_ALWAYS=1 -- wink and mimics too, not just speech
};
FaceAnimationLook& GFaceAnimation();

// FACE-001 diagnostics. Every counter here has a "did not run" reading: a zero means the
// site was never reached, and the sites are ordered so the first zero names the break.
// POSEIDON_FACE_DEBUG=1 prints the row roughly once a second.
//
// NOTE: the renderer-side half that fills the upd*/draw* columns lives in
// engine/WgpuRenderer/EngineWgpu.cpp and is NOT in this commit -- that file currently also
// carries another agent's netting work, so it was left uncommitted rather than swept in.
// Without it these columns stay at 0; the request/offset/skin columns still work.
struct FaceAnimDiag
{
	long long request = 0;     //!< Man::RequestFaceCpuPose entered
	long long requestNoVb = 0; //!< ... but the level had no VertexBuffer
	long long requestSet = 0;  //!< ... and HasVertexAnimation() was true
	long long offsets = 0;     //!< OffsetAnimation moved >=1 vertex
	long long cpuSkinKept = 0; //!< ApplyMatrices saw cpuPosed and stayed on the CPU
	long long gpuSkinned = 0;  //!< ApplyMatrices handed the palette to the GPU instead
	long long updSkinPosed = 0;//!< VertexBufferWgpu::Update uploaded a CPU-posed skinned mesh
	long long updSkinSkip = 0; //!< ... returned early because the mesh is GPU-skinned
	long long drawPosed = 0;   //!< BeginMeshTL saw cpuPosed (no palette bound)
	long long drawPalette = 0; //!< BeginMeshTL bound a bone palette
	long long updBytes = 0;    //!< vertices actually pushed by the posed path
	long long updShared = 0;   //!< posed Update bailed: slot->sharedProducer != 0
	long long updClean = 0;    //!< posed Update bailed: bufferDirty false
	long long updCount = 0;    //!< posed Update bailed: src.NVertex() != vertexCount
	long long updHash = 0;     //!< posed Update bailed: content hash unchanged
	long long updSent = 0;     //!< posed Update actually pushed bytes to the GPU
	int poseLevel = -1;        //!< last level RequestFaceCpuPose set the flag on
	int poseVerts = 0;         //!< that level's vertex count
	int drawVerts = 0;         //!< vertex count of the buffer the posed Update uploaded
	long long updPending = 0;  //!< posed upload went into slot->verts (create not drained)
	long long updUploader = 0; //!< posed upload went through the uploader queue
	long long updDirect = 0;   //!< posed upload went straight to wgr_mesh_update
	long long updRefused = 0;  //!< the uploader REFUSED the posed write (returned 0)
	long long updRestore = 0;  //!< a NON-posing unit re-uploaded the bind pose into the SAME mesh
	unsigned long long vbSet = 0;   //!< VertexBuffer address of a unit that IS face-posing
	unsigned long long vbUnset = 0; //!< VertexBuffer address of a unit that is NOT
	int eyeSelSize = -1;
	int lmSelSize = -1;
	int rmSelSize = -1;
	int lipSelSize = -1;       //!< vertices in the lip selection at the animated level
	double poseMaxDelta = 0;   //!< max |Pos - OrigPos| seen on a posed upload (metres)
};
FaceAnimDiag& GFaceDiag();
bool FaceDebugEnabled();
void FaceDiagTick(const char* where);

struct ManLipInfoItem
{
	float time;
	int phase;
};

class ManLipInfo
{
protected:
	AutoArray<ManLipInfoItem> _items;
	int _current;
	float _freq;
	Foundation::Time _start;
	float _frame;
	float _invFrame;

public:
	ManLipInfo() {_frame = 0.11; _invFrame = 1.0 / _frame;}
	bool AttachWave(IWave *wave, float freq = 1.0f);
	bool GetPhase(int &phase);
	float GetPhase();
	int PhonemeCount() const { return _items.Size(); }
	float ElapsedOffset() const;
	int CurrentCursor() const { return _current; }
};

class Head
{
public:
	Ref<Texture> _glasses;

	float _winkPhase;
	int _forceWinkPhase;
	Foundation::Time _nextWink;

	Ref<Texture> _texture;
	Ref<Texture> _textureWounded;

	Vector3 _lBrow, _mBrow, _rBrow;
	Vector3 _lMouth, _mMouth, _rMouth;
	Vector3 _lBrowOld, _mBrowOld, _rBrowOld;
	Vector3 _lMouthOld, _mMouthOld, _rMouthOld;

	RandomVector3 _lBrowRandom, _mBrowRandom,	_rBrowRandom;
	RandomVector3 _lMouthRandom, _mMouthRandom, _rMouthRandom;

	RString _forceMimic;
	float _mimicPhase;
	const ParamEntry *_mimicMode;
	float _nextMimicTime;

	SRef<ManLipInfo> _lipInfo;
	
	bool _randomLip;
	float _actualRandomLip;
	float _wantedRandomLip;
	Foundation::Time _nextChangeRandomLip;
	float _speedRandomLip;

	Head(const HeadType &type, LODShape *lShape);
	void Animate
	(
		const HeadType &type, LODShape *lShape, int level, bool isDead, Matrix3Par trans, bool hiddenHead
	);
	void Deanimate
	(
		const HeadType &type, LODShape *lShape, int level, bool isDead, Matrix3Par trans, bool hiddenHead
	);

	void Simulate( const HeadType &type, float deltaT, SimulationImportance prec, bool dead );

	int GetFaceAnimation() const {return _forceWinkPhase;}
	void SetFaceAnimation(int phase) {_forceWinkPhase = phase;}

	void SetFace(const HeadType &type, bool women, LODShape *lShape, RString name, RString player = "");
	RString GetFaceTextureName() const;
	void SetGlasses(const HeadType &type, LODShape *lShape, RString name);
	void SetForceMimic(RStringB name);
	void SetMimic(RStringB name);
	void SetMimicMode(RStringB modeName);
	RStringB GetMimicMode() const;

	void AttachWave(IWave *wave, float freq = 1.0f);
	void SetRandomLip(bool set = true); 

	// FACE-001: true when this head is applying CPU-space vertex offsets that no bone
	// matrix can express, so the shape must be skinned on the CPU this frame.
	bool HasVertexAnimation() const;

	// FACE-002: write this head's face pose into the reserved face bones of `matrices`,
	// which must already be sized by PrepareMatrices. Call once per level per frame,
	// BEFORE AnimationRT::ApplyMatrices. Returns true when the rig carried the pose, in
	// which case Animate/Deanimate must not also offset the shared vertices.
	bool ApplyFaceBones(const HeadType &type, const WeightInfo &weights, int level,
	                    Matrix4Array &matrices, bool isDead);
	// Same dead-face arithmetic without changing this head, lip state or counters.
	bool EvaluateDeadFaceBones(const HeadType &type, const WeightInfo &weights, int level,
	                           Matrix4Array &matrices, bool isDead) const;

	// FACE-002: set by ApplyFaceBones; read by Animate/Deanimate to suppress the CPU offsets.
	bool _faceBonesActive = false;

protected:
	bool ApplyFaceBonesImpl(const HeadType &type, const WeightInfo &weights, int level,
	                        Matrix4Array &matrices, bool isDead, bool recordState);
	void NextRandomLip();
};

}  // namespace Poseidon
