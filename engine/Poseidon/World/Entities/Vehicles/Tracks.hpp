#pragma once

#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp>


namespace Poseidon
{
enum {TrackLODLevels=1};

class TrackStep: public Vehicle //,public CLTLink<1>
{
	private:
	Foundation::Time _startTime;
	float _alpha; // current alpha value
	Ref<Texture> _texture;

	public:

	TrackStep(float alpha, Texture *texture);
	~TrackStep() override;

	void Change
	(
		int level,
		Vector3Par lastLeft, Vector3Par lastRight, float lastV,
		Vector3Par left, Vector3Par right, float v
	);
	void Final(); // perform surface split
	void StartTime();
	Foundation::Time GetStartTime() const {return _startTime;}
	//bool Disappear( float deltaT ); // disappear a litte
	//void AnimateAlpha( int level );
	void Simulate( float deltaT, SimulationImportance prec ) override;

	void UpdateAlpha();

	bool IsAnimated( int level ) const override; // appearence changed with Animate
	bool IsAnimatedShadow( int level ) const override; // shadow changed with Animate
	void Animate( int level ) override;
	void Deanimate( int level ) override;

	bool MustBeSaved() const override {return false;}

	int PassNum( int lod ) override;

	USE_FAST_ALLOCATOR
};

class TrackDraw: public RefCount
{
	struct TrackLODLevel
	{
		bool _initDone;
		Vector3 _lastL,_lastR;
		float _lastV; // update v depending on distance
		TrackLODLevel():_initDone(false),_lastV(0){}
	};
	TrackLODLevel _lods[TrackLODLevels];
	Ref<TrackStep> _lastPart;
	bool _offsets;
	Vector3 _lOffset,_rOffset; // points in object model
	Vector3 _snowLast = VZero;
	bool _snowLastValid = false;
	
	Texture *_texture;
	float _alpha;
	
	public:
	TrackDraw();
	void SetOffsets
	(
		Vector3Par lOffset, Vector3Par rOffset,
		Texture *texture, float alpha
	);
	TrackStep *Update( const Frame &pos, bool force );
	void UpdateSnow(const Frame& pos, bool terminate);

	Vector3Val LeftPos() const {return _lOffset;}
	bool HasOffsets() const {return _offsets;}
	Vector3Val RightPos() const {return _rOffset;}
	const Vector3 CenterPos() const
	{
		Vector3 sum = _lOffset+_rOffset;
		return sum*0.5;
	}
	
	void Skip( const Frame &pos );
};

class Mark: public Vehicle
{
	float _alpha,_alphaSpeed;
	bool _bootRelief = false;

	public:
	Mark( LODShapeWithShadow *shape, float alpha=1.0, float timeToLive=10 );
	~Mark() override;
	void SetBootRelief(bool enabled) { _bootRelief = enabled; }
	bool HasBootRelief() const { return _bootRelief; }
	float BootReliefOpacity() const { return _alpha; }

	void Simulate( float deltaT, SimulationImportance prec ) override;

	void UpdateAlpha();

	bool IsAnimated( int level ) const override;
	bool IsAnimatedShadow( int level ) const override;
	int PassNum( int lod ) override;
	void Animate( int level ) override;
	void Deanimate( int level ) override;

	bool MustBeSaved() const override {return false;}

	USE_FAST_ALLOCATOR;
};

class TrackAccumulator
{
	protected:
	Ref<TrackStep> _accumulate;
	bool Merge( TrackStep *merge );
	void Terminate();
};

class TrackOptimized: public TrackAccumulator
{
	TrackDraw _left,_right;
	Vector3 _snowHullFront = VZero, _snowHullBack = VZero, _snowHullLast = VZero;
	float _snowHullRadius = 0, _snowHullTime = 0;
	bool _snowHullValid = false;
	
	public:
	TrackOptimized( const LODShape *lShape );
	void Update( const Frame &pos, float deltaT, bool terminate );
	void UpdateSnow(const Frame& pos, bool terminate) { _left.UpdateSnow(pos, terminate); _right.UpdateSnow(pos, terminate); }
	void UpdateTrackedSnow(const Frame& pos, bool terminate, float deltaT);

	Vector3 LeftPos() const {return _left.CenterPos();}
	Vector3 RightPos() const {return _right.CenterPos();}
};

class TrackOptimizedFour: public TrackAccumulator
{
	Ref<TrackStep> _accumulate;
	TrackDraw _fLeft,_fRight;
	TrackDraw _bLeft,_bRight;
	Vector3 _snowHullLast = VZero;
	float _snowHullTime = 0;
	bool _snowHullValid = false;
	
	public:
	TrackOptimizedFour( const LODShape *lShape );
	void Update( const Frame &pos, float deltaT, bool terminate );
	void UpdateSnow(const Frame& pos, bool terminate, float deltaT);

	Vector3 BackLeftPos() const {return _bLeft.CenterPos();}
	Vector3 BackRightPos() const {return _bRight.CenterPos();}
	Vector3 FrontLeftPos() const {return _fLeft.CenterPos();}
	Vector3 FrontRightPos() const {return _fRight.CenterPos();}
};

}  // namespace Poseidon
