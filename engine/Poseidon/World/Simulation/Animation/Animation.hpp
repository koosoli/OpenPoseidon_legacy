#pragma once

#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/Foundation/Time/Time.hpp>


namespace Poseidon
{
class Animation
{
	protected:
	int _selection[MAX_LOD_LEVELS];

	void DoConstruct();

	public:
	Animation();
	void Init( LODShape *shape, const char *nameSel, const char *altName );
	void Deinit();

	void Transform( LODShape *shape, const Matrix4 &trans, int level ) const;
	void TransformOver( LODShape *shape, const Matrix4 &trans, int level ) const;
	void TransformWithWeight( LODShape *shape, const Matrix4 &trans, int level ) const;

	Vector3 Transform( LODShape *shape, const Matrix4 &trans, int level, int index ) const;
	Vector3 TransformWithWeight( LODShape *shape, const Matrix4 &trans, int level, int index ) const;

	void Restore( LODShape *shape, int level ) const;

	int GetSelection( int level ) const {return _selection[level];}
	bool IsEmpty() const;

};

//! A section's own material while a hidden selection shows another one (setObjectMaterial,
//! hiddenSelectionsMaterials). Kept by the entity between Animate and Deanimate, because the
//! shape -- and so the section -- is shared by every instance of the model.
struct SavedSectionMaterial
{
	int level = -1;
	int section = -1;
	Ref<TexMaterial> material;
};

class AnimationSection: public Animation
{
	protected:
	void DoConstruct();

	public:
	AnimationSection();
	void Register( LODShape *shape, const char *nameSel, const char *altName );
	void Init( LODShape *shape, const char *nameSel, const char *altName );
	void Deinit();

	void SetTexture( LODShape *shape, int level, Texture *texture ) const;
	Texture *GetTexture(LODShape *shape) const;
	//! Show `material` on this selection's sections of `level`, remembering each section's own
	//! material in `saved` (once per section and level, however often it is called).
	void SetMaterial( LODShape *shape, int level, TexMaterial *material, AutoArray<SavedSectionMaterial> &saved ) const;
	//! Put back every material `saved` holds for `level`, and forget those entries.
	static void RestoreMaterials( LODShape *shape, int level, AutoArray<SavedSectionMaterial> &saved );
	void Hide( LODShape *shape, int level ) const;
	void Unhide( LODShape *shape, int level ) const;
};

class AnimationUV
{
	protected:
	int _selection[MAX_LOD_LEVELS];
	Temp<float> _origU[MAX_LOD_LEVELS];
	Temp<float> _origV[MAX_LOD_LEVELS];

	void DoConstruct();

	public:
	AnimationUV(){DoConstruct();}
	void Init( LODShape *shape, const char *nameSel );
	void UVOffset( LODShape *shape, float offsetU, float offsetV, int level ) const;
	void Restore( LODShape *shape, int level ) const;
	int GetSelection( int level ) const {return _selection[level];}
};

class AnimationAnimatedTexture: public AnimationSection
{
	Temp< Ref<Texture> > _animTexF[MAX_LOD_LEVELS];
	Temp< Ref<Texture> > _animTexS[MAX_LOD_LEVELS];

	void DoConstruct();

	public:
	AnimationAnimatedTexture(){DoConstruct();}
	void Init( LODShape *shape, const char *nameSel, const char *altNameSel );
	void Deinit();

	void SetPhase(LODShape *shape, int level, int phase) const;
	void AnimateTexture(LODShape *shape, int level, float anim) const;
};

class AnimationWithCenter: public Animation
{
	private:
	int _centerSelection[MAX_LOD_LEVELS]; // named selection index
	Vector3 _center[MAX_LOD_LEVELS];
	
	public:
	AnimationWithCenter();
	void Init
	(
		LODShape *shape, const char *name, const char *altName,
		const char *center, const char *altCenter=nullptr
	);
	void Deinit();

	void Apply( LODShape *shape, const Matrix4 &trans, int level ) const;
	void ApplyWithWeight( LODShape *shape, const Matrix4 &trans, int level ) const;
	Vector3 Center() const
	{
		for( int i=0; i<MAX_LOD_LEVELS; i++ ) if( _selection[i]>=0 ) return _center[i];
		return VZero;
	}
	Vector3 Center( int level ) const
	{
		if( _selection[level]>=0 ) return _center[level];
		return VZero;
	}
	int GetCenterSelection( int level ) const {return _centerSelection[level];}
	void SetCenter( Vector3Par center )
	{
		for( int i=0; i<MAX_LOD_LEVELS; i++ ) _center[i]=center;
	}
	void CalculateCenter( LODShape *shape );
};

class AnimationRotation: public Animation
{
	Vector3 _center[MAX_LOD_LEVELS];
	Vector3 _direction[MAX_LOD_LEVELS];

	public:
	AnimationRotation();
	void Init
	(
		LODShape *shape, const char *name, const char *altName,
		const char *axis, const char *altAxis = nullptr, bool inMemory = true
	);
	void Init2
	(
		LODShape *shape, const char *name, const char *begin, const char *end, bool inMemory = true
	);
	void Deinit();

	void GetRotation( Matrix4 &mat, float angle, int level ) const;
	void Rotate( LODShape *shape, float angle, int level ) const;
	void RotateWithWeight( LODShape *shape, float angle, int level ) const;
	Vector3 Center() const
	{
		for( int i=0; i<MAX_LOD_LEVELS; i++ ) if( _selection[i]>=0 ) return _center[i];
		return Vector3(0,0,0);
	}
	Vector3 Direction() const
	{
		for( int i=0; i<MAX_LOD_LEVELS; i++ ) if( _selection[i]>=0 ) return _direction[i];
		return Vector3(0,0,0);
	}
	void SetCenter( Vector3Par center, Vector3Par direction )
	{
		for( int i=0; i<MAX_LOD_LEVELS; i++ ) _center[i]=center,_direction[i]=direction;
	}
	void CalculateCenter( LODShape *shape );
};

class AnimationType : public RefCount
{
protected:
	RString _name;
	float _animSpeed;

public:
	static AnimationType *CreateObject(const ParamEntry &cls, LODShape *shape);
	virtual void Init(const ParamEntry &cls, LODShape *shape);

	RString GetName() const {return _name;}
	float GetAnimSpeed() const {return _animSpeed;}

	virtual int GetSelection(int level) const = 0;
	virtual void Animate
	(
		LODShape *shape, int level, float phase, Matrix4Par baseAnim
	) = 0;
	virtual void Deanimate(LODShape *shape, int level) = 0;
};

class AnimationRotationType : public AnimationType
{
protected:
	typedef AnimationType base;
    
	AnimationRotation _animation;
	float _angle0;
	float _angle1;

public:
	void Init(const ParamEntry &cls, LODShape *shape) override;

	int GetSelection(int level) const override;
	void Animate(LODShape *shape, int level, float phase, Matrix4Par baseAnim) override;
	void Deanimate(LODShape *shape, int level) override;
};

class AnimationInstance
{
protected:
	Ref<AnimationType> _type;
	float _phase;
	float _phaseWanted;
	Foundation::Time _lastAnimation;
    
public:
	AnimationInstance();
	void Init();
	
	RString GetName() const {return _type ? _type->GetName() : "";}
	float GetPhase() const {return _phase;}
	float GetPhaseWanted() const {return _phaseWanted;}

	void SetType(AnimationType *type) {_type = type;}
	void SetPhaseWanted(float phase) {_phaseWanted = phase;}

	int GetSelection(int level) const;
	void Animate(LODShape *shape, int level, Matrix4Par baseAnim);
	void Deanimate(LODShape *shape, int level);

protected:
	void AdvanceTime();
};

}  // namespace Poseidon
