#pragma once

#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
namespace Poseidon { class ParamEntry; }


namespace Poseidon
{

struct RecoilItem
{
	float _time;
	Matrix4 _mat;
};

struct RecoilFFRamp
{
	float _startTime,_duration;
	float _begAmplitude,_endAmplitude;
};

class RecoilFunction: public RefCount
{
	AutoArray<RecoilItem> _queue;
	RStringB _name;

	public:
	RecoilFunction();
	RecoilFunction( RStringB name );
	~RecoilFunction() override;

	const RStringB &GetName() const;

	void AddItem(const RecoilItem &item);
	void AddItem(float time, float offset, float angle);

	float GetRecoilRotX( float time) const;
	//! Largest rotation anywhere in the curve. Stands in for Arma 3's per-weapon
	//! muzzleOuter magnitude: the authored OFP curves already encode how hard each weapon
	//! kicks, so scaling by this gives per-weapon recoil without inventing a new config.
	float GetPeakRotX() const;

	// Samples the curve by linear interpolation between keyframes.
	//
	// NOTE this is no longer how the weapon or the aim move. The authored curves keyframe both
	// the rotation and the straight-back shove as coarse linear ramps, so velocity steps at
	// every keyframe and reverses instantly at each peak - which is what read as the weapon
	// snapping. Both are now springs in EntityAI, fed per shot from GetPeakRotX /
	// which is smooth by construction and compounds correctly across a burst.
	// The only remaining caller is the third-person camera angle in Man::GetCameraTransform.
	//
	// `translationOnly` keeps the shove and discards the rotation: Man's kickback uses it, so
	// the shove back along the bore stays while the spring in EntityAI owns the angles.
	void ApplyRecoil(float time, Matrix4 &mat, float factor=1, bool translationOnly=false) const;
	bool GetTerminated( float time ) const;

	void GetFFEdge(int index, float &time, float &amplitude) const;
	bool GetFFRamp(int index, RecoilFFRamp &ramp) const;
};

}  // namespace Poseidon
