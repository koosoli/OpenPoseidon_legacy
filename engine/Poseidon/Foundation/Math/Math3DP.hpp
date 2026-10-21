#ifdef _MSC_VER
#pragma once
#endif

#ifndef _MATH3DP_HPP
#define _MATH3DP_HPP

// homogenous vector and matrix arithmetics
// select between template and straighforward implementation

#ifdef _T_MATH

// use template version

#include <Poseidon/Foundation/Math/Math3DT.hpp>

// pretend T types are P types (they have same binary format)
#define Vector3P Vector3T
#define Vector3PVal Vector3TVal
#define Vector3PPar Vector3TPar
#define Matrix3P Matrix3T
#define Matrix4P Matrix4T
#define Matrix3PVal const Matrix3T&
#define Matrix4PVal const Matrix4T&
#define Matrix3PPar const Matrix3T&
#define Matrix4PPar const Matrix4T&
#define ConvertToM(x) (x)
#define ConvertToP(x) (x)
#define VZeroP VZeroT
#define VUpP VUpT
#define VForwardP VForwardT
#define VAsideP VAsideT

#define MIdentityP M4IdentityT
#define MZeroP M4ZeroT
#define M4IdentityP M4IdentityT
#define M4ZeroP M4ZeroT
#define M3IdentityP M3IdentityT
#define M3ZeroP M3ZeroT

#else

// straightforward C++ version
#include <Poseidon/Foundation/Math/MathDefs.hpp>

#include <math.h>
#include <float.h>
#include <Poseidon/Foundation/Common/FltOpts.hpp>

#include <Poseidon/Foundation/Math/MathOpt.hpp>

#include <Poseidon/Foundation/Framework/DebugLog.hpp>
#include <Poseidon/Foundation/Containers/TypeOpts.hpp>

// use parameter placeholder to explicitly disable initialization of elements

#ifndef vecAlign
// no alignment required in P implementation
#define vecAlign
#endif

// define how should const references be passed
#define Vector3PPar const Vector3P&
#define Vector3PVal const Vector3P&

#define Matrix3PPar const Matrix3P&
#define Matrix3PVal const Matrix3P&

#define Matrix4PPar const Matrix4P&
#define Matrix4PVal const Matrix4P&

namespace Poseidon::Foundation
{
class Vector3P;
class Matrix3P;
class Matrix4P;

#if _MSC_VER
#define NAKED __declspec(naked)
#else
#define NAKED
#endif

class Vector3P
{
    friend class Matrix4P;
    friend class Matrix3P;

    // 3D type - used for rendering, screen clipping ...
  protected:
    Coord _e[3];
    __forceinline Coord Get(int i) const
    {
#if _DEBUG
        PoseidonAssert(_e[i] != FLT_MAX);
#endif
        return _e[i];
    }
    __forceinline Coord& Set(int i) { return _e[i]; }

  public:
    void SetMultiplyLeft(Vector3PPar v, const Matrix3P& a);
    void SetRotate(const Matrix4P& a, Vector3PPar v);
    void SetFastTransform(const Matrix4P& a, Vector3PPar v);

    __forceinline void SetMultiply(const Matrix3P& a, Vector3PPar v);
    __forceinline void SetMultiply(const Matrix4P& a, Vector3PPar v);
    float SetPerspectiveProject(const Matrix4P& a, Vector3PPar o);

#ifndef NDEBUG
    Vector3P() { _e[0] = _e[1] = _e[2] = FLT_MAX; } // default no init
#else
    __forceinline Vector3P() {}
#endif
    __forceinline void Init() {}
    __forceinline Vector3P(Coord x, Coord y, Coord z) { _e[0] = x, _e[1] = y, _e[2] = z; }
    // explicit so the copy is inlined (the compiler-generated one is not)
    __forceinline Vector3P(const Vector3P& src) { _e[0] = src._e[0], _e[1] = src._e[1], _e[2] = src._e[2]; }
    __forceinline Vector3P& operator=(const Vector3P& src)
    {
        _e[0] = src._e[0], _e[1] = src._e[1], _e[2] = src._e[2];
        return *this;
    }

    __forceinline Vector3P(enum _noInit) {}

    __forceinline Vector3P(enum _vMultiply, const Matrix3P& a, Vector3PPar v) { SetMultiply(a, v); }
    __forceinline Vector3P(enum _vMultiply, const Matrix4P& a, Vector3PPar v) { SetMultiply(a, v); }
    __forceinline Vector3P(enum _vMultiplyLeft, Vector3PPar v, const Matrix3P& a) { SetMultiplyLeft(v, a); }

    __forceinline Vector3P(enum _vRotate, const Matrix4P& a, Vector3PPar v) { SetRotate(a, v); }
    __forceinline Vector3P(enum _vFastTransform, const Matrix4P& a, Vector3PPar v) { SetFastTransform(a, v); }
    __forceinline Vector3P(enum _vFastTransformA, const Matrix4P& a, Vector3PPar v) { SetFastTransform(a, v); }

    __forceinline Coord operator[](int i) const
    {
        PoseidonAssert(_e[i] != FLT_MAX);
        return _e[i];
    }
    __forceinline Coord& operator[](int i) { return _e[i]; }

    __forceinline Coord X() const
    {
        PoseidonAssert(_e[0] != FLT_MAX);
        return _e[0];
    }
    __forceinline Coord Y() const
    {
        PoseidonAssert(_e[1] != FLT_MAX);
        return _e[1];
    }
    __forceinline Coord Z() const
    {
        PoseidonAssert(_e[2] != FLT_MAX);
        return _e[2];
    }

    Coord SquareSize() const { return X() * X() + Y() * Y() + Z() * Z(); }
    __forceinline Coord SquareSizeInline() const { return X() * X() + Y() * Y() + Z() * Z(); }
    // A plain sqrt (PERF 2026-10-14): the 2001 form computed |v| as |v|^2 * InvSqrt(|v|^2) because
    // sqrt was slow then; today sqrtss is one instruction, exact, and right for 0 and for an
    // overflowed (infinite) square, which the product form turned into NaN.
    Coord Size() const { return std::sqrt(SquareSizeInline()); }
    Coord InvSize() const { return InvSqrt(SquareSize()); }
    Coord InvSquareSize() const { return Inv(SquareSize()); }

    Coord SquareSizeXZ() const { return X() * X() + Z() * Z(); }
    Coord SizeXZ() const { return std::sqrt(SquareSizeXZ()); }
    Coord InvSizeXZ() const { return InvSqrt(SquareSizeXZ()); }
    Coord InvSquareSizeXZ() const { return Inv(SquareSizeXZ()); }

    // vector arithmetics
    __forceinline Vector3P operator-() const { return Vector3P(-X(), -Y(), -Z()); }

    __forceinline Vector3P operator+(Vector3PPar op) const
    {
        return Vector3P(X() + op.X(), Y() + op.Y(), Z() + op.Z());
    }
    __forceinline Vector3P operator-(Vector3PPar op) const
    {
        return Vector3P(X() - op.X(), Y() - op.Y(), Z() - op.Z());
    }
    __forceinline friend Vector3P operator*(Coord f, Vector3PPar op)
    {
        return Vector3P(op.Get(0) * f, op.Get(1) * f, op.Get(2) * f);
    }
    __forceinline Vector3P operator*(Coord f) const { return Vector3P(Get(0) * f, Get(1) * f, Get(2) * f); }
    __forceinline Vector3P Modulate(Vector3PPar op) const
    {
        return Vector3P(Get(0) * op.Get(0), Get(1) * op.Get(1), Get(2) * op.Get(2));
    }
    Vector3P operator/(Coord f) const
    {
        float invF = 1 / f;
        return Vector3P(X() * invF, Y() * invF, Z() * invF);
    }

    Vector3P& operator+=(Vector3PPar op)
    {
        _e[0] += op.X();
        _e[1] += op.Y();
        _e[2] += op.Z();
        return *this;
    }
    Vector3P& operator-=(Vector3PPar op)
    {
        _e[0] -= op.X();
        _e[1] -= op.Y();
        _e[2] -= op.Z();
        return *this;
    }
    Vector3P& operator*=(Coord f)
    {
        _e[0] *= f;
        _e[1] *= f;
        _e[2] *= f;
        return *this;
    }
    Vector3P& operator/=(Coord f)
    {
        float invF = 1 / f;
        _e[0] *= invF;
        _e[1] *= invF;
        _e[2] *= invF;
        return *this;
    }

    Vector3P Normalized() const;
    void Normalize(); // no return to avoid using instead of Normalized
    __forceinline Coord DotProduct(Vector3PPar op) const { return X() * op.X() + Y() * op.Y() + Z() * op.Z(); }
    __forceinline Coord operator*(Vector3PPar op) const { return DotProduct(op); }
    __forceinline Vector3P operator*(const Matrix3P& op) const { return Vector3P(VMultiplyLeft, *this, op); }

    float CosAngle(Vector3PPar op) const;
    float Distance(Vector3PPar op) const;

    __forceinline float Distance2(Vector3PPar op) const { return (*this - op).SquareSizeInline(); }
    __forceinline float Distance2Inline(Vector3PPar op) const { return (*this - op).SquareSizeInline(); }

    float DistanceXZ(Vector3PPar op) const;
    float DistanceXZ2(Vector3PPar op) const;

    Vector3P Project(Vector3PPar op) const;
    Vector3P CrossProduct(Vector3PPar op) const;
    Matrix3P Tilda() const;
    bool IsFinite() const;

    bool operator==(Vector3PPar cmp) const { return cmp.X() == X() && cmp.Y() == Y() && cmp.Z() == Z(); }
    bool operator!=(Vector3PPar cmp) const { return cmp.X() != X() || cmp.Y() != Y() || cmp.Z() != Z(); }
};

class Matrix3P
{
    friend class Vector3P;
    friend class Matrix4P;

    // homogenous matrix - transformations
  private:
    Vector3P _aside;
    Vector3P _up;
    Vector3P _dir;
    __forceinline Coord Get(int i, int j) const { return (&_aside)[j][i]; }
    __forceinline Coord& Set(int i, int j) { return (&_aside)[j][i]; }

  public:
    // functions that load matrix with data
    // used internaly in constuctors, but may be useful also to other purpose
    void SetIdentity();
    void SetZero();
    void SetRotationX(Coord angle);
    void SetRotationY(Coord angle);
    void SetRotationZ(Coord angle);
    void SetScale(Coord x, Coord y, Coord z);

    void SetScale(float scale);
    float Scale() const;
    float InvScale() const;

    // Z is the reference axis for dir, Y for up: dir=(0,0,1), up=(0,1,0) gives identity.
    // The result is orthogonalized — the first-named vector is kept, the second is made
    // orthogonal to it.
    void SetDirectionAndUp(Vector3PPar dir, Vector3PPar up); // sets only 3x3 submatrix
    void SetUpAndAside(Vector3PPar up, Vector3PPar aside);
    void SetUpAndDirection(Vector3PPar up, Vector3PPar dir);
    void SetDirectionAndAside(Vector3PPar dir, Vector3PPar aside);

    __forceinline void InlineSetMultiply(const Matrix3P& a, float op) { SetMultiply(a, op); }
    __forceinline void InlineAddMultiply(const Matrix3P& a, float op) { AddMultiply(a, op); }
    __forceinline void InlineSetAdd(const Matrix3P& a, const Matrix3P& b) { SetAdd(a, b); }

    void AddMultiply(const Matrix3P& a, float op);
    void SetMultiply(const Matrix3P& a, const Matrix3P& b);
    void SetAdd(const Matrix3P& a, const Matrix3P& b);
    void SetMultiply(const Matrix3P& a, float op);
    void SetInvertRotation(const Matrix3P& op);
    void SetInvertScaled(const Matrix3P& op);
    void SetInvertGeneral(const Matrix3P& op);
    void SetNormalTransform(const Matrix3P& op);
    void SetTilda(Vector3PPar a);

    // placeholder parameter describes constructor type
    Matrix3P(float m00, float m01, float m02, float m10, float m11, float m12, float m20, float m21, float m22)
    {
        Set(0, 0) = m00, Set(0, 1) = m01, Set(0, 2) = m02;
        Set(1, 0) = m10, Set(1, 1) = m11, Set(1, 2) = m12;
        Set(2, 0) = m20, Set(2, 1) = m21, Set(2, 2) = m22;
    }
    __forceinline Matrix3P() = default;
    __forceinline Matrix3P(enum _noInit) {}
    __forceinline Matrix3P(enum _mRotationX, Coord angle) { SetRotationX(angle); }
    __forceinline Matrix3P(enum _mRotationY, Coord angle) { SetRotationY(angle); }
    __forceinline Matrix3P(enum _mRotationZ, Coord angle) { SetRotationZ(angle); }
    __forceinline Matrix3P(enum _mScale, Coord x, Coord y, Coord z) { SetScale(x, y, z); }
    __forceinline Matrix3P(enum _mScale, Coord x) { SetScale(x, x, x); }
    __forceinline Matrix3P(enum _mDirection, Vector3PPar dir, Vector3PPar up) { SetDirectionAndUp(dir, up); }
    __forceinline Matrix3P(enum _mUpAndDirection, Vector3PPar dir, Vector3PPar up) { SetUpAndDirection(dir, up); }
    __forceinline Matrix3P(enum _mMultiply, const Matrix3P& a, const Matrix3P& b) { SetMultiply(a, b); }
    __forceinline Matrix3P(enum _mMultiply, const Matrix3P& a, float op) { SetMultiply(a, op); }
    __forceinline Matrix3P(enum _mInverseRotation, const Matrix3P& a) { SetInvertRotation(a); }
    __forceinline Matrix3P(enum _mInverseGeneral, const Matrix3P& a) { SetInvertGeneral(a); }
    __forceinline Matrix3P(enum _mInverseScaled, const Matrix3P& a) { SetInvertScaled(a); }
    __forceinline Matrix3P(enum _mNormalTransform, const Matrix3P& a) { SetNormalTransform(a); }
    __forceinline Matrix3P(enum _mTilda, Vector3PPar a) { SetTilda(a); }

    // following operators are defined so that no copy constuctor is used
    // if they are expanded inline, copy is not needed
    __forceinline Matrix3P operator*(const Matrix3P& op) const { return Matrix3P(MMultiply, *this, op); }
    __forceinline Vector3P operator*(Vector3PPar op) const { return Vector3P(VMultiply, *this, op); }
    __forceinline Matrix3P operator*(float op) const { return Matrix3P(MMultiply, *this, op); }
    void operator*=(float op);
    Matrix3P operator+(const Matrix3P& a) const;
    Matrix3P operator-(const Matrix3P& a) const;
    Matrix3P& operator+=(const Matrix3P& a);
    Matrix3P& operator-=(const Matrix3P& a);

    __forceinline Matrix3P InverseRotation() const { return Matrix3P(MInverseRotation, *this); }
    __forceinline Matrix3P InverseGeneral() const { return Matrix3P(MInverseGeneral, *this); }
    __forceinline Matrix3P InverseScaled() const { return Matrix3P(MInverseScaled, *this); }
    __forceinline Matrix3P NormalTransform() const { return Matrix3P(MNormalTransform, *this); }
    __forceinline Coord operator()(int i, int j) const { return Get(i, j); }
    __forceinline Coord& operator()(int i, int j) { return Set(i, j); }

    bool IsFinite() const;

    __forceinline const Vector3P& Direction() const { return _dir; }
    __forceinline const Vector3P& DirectionUp() const { return _up; }
    __forceinline const Vector3P& DirectionAside() const { return _aside; }
    void SetDirection(const Vector3P& v) { _dir = v; }
    void SetDirectionUp(const Vector3P& v) { _up = v; }
    void SetDirectionAside(const Vector3P& v) { _aside = v; }

    __forceinline float Distance2(const Matrix3P& mat) const
    {
        return (_dir.Distance2(mat._dir) + _up.Distance2(mat._up) + _aside.Distance2(mat._aside));
    }

    void Orthogonalize();
};

class Matrix4P
{
    friend class Vector3P;

    // homogenous matrix - transformations
  private:
    Matrix3P _orientation;
    Vector3P _position;

    __forceinline Coord Get(int i, int j) const { return _orientation.Get(i, j); }
    __forceinline Coord& Set(int i, int j) { return _orientation.Set(i, j); }

    __forceinline Coord GetPos(int i) const { return _position.Get(i); }
    __forceinline Coord& SetPos(int i) { return _position.Set(i); }

  public:
    // functions that load matrix with data
    // used internaly in constuctors, but may be useful also to other purpose
    void SetIdentity();
    void SetZero();
    void SetTranslation(Vector3PPar offset);
    void SetRotationX(Coord angle);
    void SetRotationY(Coord angle);
    void SetRotationZ(Coord angle);
    void SetScale(Coord x, Coord y, Coord z);
    void SetPerspective(Coord cLeft, Coord cTop);

    // sets only 3x3 submatrix
    __forceinline void SetDirectionAndUp(Vector3PPar dir, Vector3PPar up) { _orientation.SetDirectionAndUp(dir, up); }
    __forceinline void SetUpAndAside(Vector3PPar up, Vector3PPar aside) { _orientation.SetUpAndAside(up, aside); }
    __forceinline void SetUpAndDirection(Vector3PPar up, Vector3PPar dir) { _orientation.SetUpAndDirection(up, dir); }
    __forceinline void SetDirectionAndAside(Vector3PPar dir, Vector3PPar aside)
    {
        _orientation.SetDirectionAndAside(dir, aside);
    }

    __forceinline void SetScale(float scale) { _orientation.SetScale(scale); }
    __forceinline float Scale() const { return _orientation.Scale(); }

    void SetOriented(Vector3PPar dir, Vector3PPar up); // sets whole 4x4 matrix
    void SetView(Vector3PPar point, Vector3PPar dir, Vector3PPar up);
    void SetMultiply(const Matrix4P& a, const Matrix4P& b);
    void SetAdd(const Matrix4P& a, const Matrix4P& b);
    void SetMultiply(const Matrix4P& a, float b);
    void AddMultiply(const Matrix4P& a, float b);
    __forceinline void InlineSetMultiply(const Matrix4P& a, float op) { SetMultiply(a, op); }
    __forceinline void InlineAddMultiply(const Matrix4P& a, float op) { AddMultiply(a, op); }
    __forceinline void InlineSetAdd(const Matrix4P& a, const Matrix4P& b) { SetAdd(a, b); }

    void SetMultiplyByPerspective(const Matrix4P& A, const Matrix4P& B);
    void SetInvertRotation(const Matrix4P& op);
    void SetInvertScaled(const Matrix4P& op);
    void SetInvertGeneral(const Matrix4P& op);

    // placeholder parameter describes constructor type
    Matrix4P(float m00, float m01, float m02, float m03, float m10, float m11, float m12, float m13, float m20,
             float m21, float m22, float m23)
    {
        Set(0, 0) = m00, Set(0, 1) = m01, Set(0, 2) = m02;
        Set(1, 0) = m10, Set(1, 1) = m11, Set(1, 2) = m12;
        Set(2, 0) = m20, Set(2, 1) = m21, Set(2, 2) = m22;
        SetPos(0) = m03, SetPos(1) = m13, SetPos(2) = m23;
    }
    __forceinline Matrix4P() = default;
    __forceinline Matrix4P(enum _noInit) {}
    __forceinline Matrix4P(enum _mTranslation, Vector3PPar offset) { SetTranslation(offset); }
    __forceinline Matrix4P(enum _mRotationX, Coord angle) { SetRotationX(angle); }
    __forceinline Matrix4P(enum _mRotationY, Coord angle) { SetRotationY(angle); }
    __forceinline Matrix4P(enum _mRotationZ, Coord angle) { SetRotationZ(angle); }
    __forceinline Matrix4P(enum _mScale, Coord x, Coord y, Coord z) { SetScale(x, y, z); }
    __forceinline Matrix4P(enum _mScale, Coord x) { SetScale(x, x, x); }
    __forceinline Matrix4P(enum _mPerspective, Coord cLeft, Coord cTop) { SetPerspective(cLeft, cTop); }
    __forceinline Matrix4P(enum _mDirection, Vector3PPar dir, Vector3PPar up) { SetOriented(dir, up); }
    __forceinline Matrix4P(enum _mView, Vector3PPar point, Vector3PPar dir, Vector3PPar up) { SetView(point, dir, up); }
    __forceinline Matrix4P(enum _mMultiply, const Matrix4P& a, const Matrix4P& b) { SetMultiply(a, b); }
    __forceinline Matrix4P(enum _mMultiply, const Matrix4P& a, float b) { SetMultiply(a, b); }
    __forceinline Matrix4P(enum _mInverseRotation, const Matrix4P& a) { SetInvertRotation(a); }
    __forceinline Matrix4P(enum _mInverseScaled, const Matrix4P& a) { SetInvertScaled(a); }
    __forceinline Matrix4P(enum _mInverseGeneral, const Matrix4P& a) { SetInvertGeneral(a); }

    // following operators are defined so that no copy constuctor is used
    // if they are expanded inline, copy is not needed
    __forceinline Matrix4P operator*(const Matrix4P& op) const { return Matrix4P(MMultiply, *this, op); }
    __forceinline Matrix4P operator*(float op) const { return Matrix4P(MMultiply, *this, op); }

    Matrix4P operator+(const Matrix4P& op) const;
    Matrix4P operator-(const Matrix4P& op) const;
    void operator+=(const Matrix4P& op);
    void operator-=(const Matrix4P& op);

    __forceinline Vector3P Rotate(Vector3PPar op) const { return Vector3P(VRotate, *this, op); }
    __forceinline Vector3P FastTransform(Vector3PPar op) const { return Vector3P(VFastTransform, *this, op); }
    __forceinline Vector3P FastTransformA(Vector3PPar op) const { return Vector3P(VFastTransformA, *this, op); }
    __forceinline Vector3P operator*(Vector3PPar op) const { return Vector3P(VMultiply, *this, op); }
    __forceinline Matrix4P InverseRotation() const { return Matrix4P(MInverseRotation, *this); }
    __forceinline Matrix4P InverseScaled() const { return Matrix4P(MInverseScaled, *this); }
    __forceinline Matrix4P InverseGeneral() const { return Matrix4P(MInverseGeneral, *this); }

    __forceinline Coord operator()(int i, int j) const { return Get(i, j); }
    __forceinline Coord& operator()(int i, int j) { return Set(i, j); }

    float Characteristic() const; // used in fast comparison
    bool IsFinite() const;

    __forceinline const Matrix3P& Orientation() const { return _orientation; }
    void SetOrientation(const Matrix3P& m) { _orientation = m; }

    __forceinline const Vector3P& Position() const { return _position; }
    __forceinline const Vector3P& Direction() const { return _orientation._dir; }
    __forceinline const Vector3P& DirectionUp() const { return _orientation._up; }
    __forceinline const Vector3P& DirectionAside() const { return _orientation._aside; }
    void SetPosition(const Vector3P& v) { _position = v; }
    void SetDirection(const Vector3P& v) { _orientation._dir = v; }
    void SetDirectionUp(const Vector3P& v) { _orientation._up = v; }
    void SetDirectionAside(const Vector3P& v) { _orientation._aside = v; }

    __forceinline float Distance2(const Matrix4P& mat) const
    {
        return (_orientation.Distance2(mat._orientation) + _position.Distance2(mat._position));
    }
    void Orthogonalize();
};

// Point and Vector are deliberately one type. Some operations are meaningless on a
// point (Point+Point, Point dot Point, Point cross Point, translating a vector), but
// building the arithmetic twice is not worth it, so the types are treated as equal.

inline Vector3P sign(Vector3PPar v)
{
    return Vector3P(fSign(v[0]), fSign(v[1]), fSign(v[2]));
}

Vector3P VectorMin(Vector3PPar a, Vector3PPar b);
Vector3P VectorMax(Vector3PPar a, Vector3PPar b);

#define Limit(speed, min, max) saturateFast(speed, min, max)

float Interpolativ(float control, float cMin, float cMax, float vMin, float vMax);
float AngleDifference(float a, float b);

void CheckMinMax(Vector3P& min, Vector3P& max, Vector3PPar val);

__forceinline void CheckMinMaxInline(Vector3P& min, Vector3P& max, Vector3PPar val)
{
    saturateMin(min[0], val[0]), saturateMax(max[0], val[0]);
    saturateMin(min[1], val[1]), saturateMax(max[1], val[1]);
    saturateMin(min[2], val[2]), saturateMax(max[2], val[2]);
}

void SaturateMin(Vector3P& min, Vector3PPar val);
void SaturateMax(Vector3P& min, Vector3PPar val);

__forceinline Matrix3P Vector3P::Tilda() const
{
    return Matrix3P(MTilda, *this);
}

__forceinline void Vector3P::SetRotate(const Matrix4P& a, Vector3PPar v)
{
    SetMultiply(a.Orientation(), v);
}

inline float Vector3P::SetPerspectiveProject(const Matrix4P& a, Vector3PPar o)
{
    // optimize: suppose that matrix is scaled and shifted perspective projection, i.e.
    // member [3,2] is 1.0
    // zero members:
    // [0,1], [0,3],
    // [1,0], [1,3], [2,0], [2,1], [2,2]
    // [3,0], [3,1], [3,3]

    // note: [0,0] and [1,1] need not be 1,
    Coord oow = coord(1) / o.Get(2);
    Set(0) = a.Get(0, 2) + a.Get(0, 0) * o.Get(0) * oow;
    Set(1) = a.Get(1, 2) + a.Get(1, 1) * o.Get(1) * oow;
    Set(2) = a.Get(2, 2) + a.GetPos(2) * oow;
    return oow;
}

__forceinline void Vector3P::SetMultiply(const Matrix4P& a, Vector3PPar o)
{
    // same as SetFastTransform, but inlined
    float r0 = a.Get(0, 0) * o[0] + a.Get(0, 1) * o[1] + a.Get(0, 2) * o[2] + a.GetPos(0);
    float r1 = a.Get(1, 0) * o[0] + a.Get(1, 1) * o[1] + a.Get(1, 2) * o[2] + a.GetPos(1);
    float r2 = a.Get(2, 0) * o[0] + a.Get(2, 1) * o[1] + a.Get(2, 2) * o[2] + a.GetPos(2);
    Set(0) = r0;
    Set(1) = r1;
    Set(2) = r2;
}

__forceinline void Vector3P::SetMultiply(const Matrix3P& a, Vector3PPar o)
{ // vector rotation - only 3x3 matrix is used, translation is ignored
    // u=M*v
    float o0 = o[0], o1 = o[1], o2 = o[2];
    Set(0) = a.Get(0, 0) * o0 + a.Get(0, 1) * o1 + a.Get(0, 2) * o2;
    Set(1) = a.Get(1, 0) * o0 + a.Get(1, 1) * o1 + a.Get(1, 2) * o2;
    Set(2) = a.Get(2, 0) * o0 + a.Get(2, 1) * o1 + a.Get(2, 2) * o2;
}

// Meyers singletons - no global constructors
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"

inline const Vector3P& VZeroP()
{
    static const Vector3P instance(0, 0, 0);
    return instance;
}

inline const Vector3P& VUpP()
{
    static const Vector3P instance(0, 1, 0);
    return instance;
}

inline const Vector3P& VForwardP()
{
    static const Vector3P instance(0, 0, 1);
    return instance;
}

inline const Vector3P& VAsideP()
{
    static const Vector3P instance(1, 0, 0);
    return instance;
}

inline const Matrix3P& M3ZeroP()
{
    static const Matrix3P instance(0, 0, 0, 0, 0, 0, 0, 0, 0);
    return instance;
}

inline const Matrix3P& M3IdentityP()
{
    static const Matrix3P instance(1, 0, 0, 0, 1, 0, 0, 0, 1);
    return instance;
}

inline const Matrix4P& M4ZeroP()
{
    static const Matrix4P instance(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    return instance;
}

inline const Matrix4P& M4IdentityP()
{
    static const Matrix4P instance(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0);
    return instance;
}

#pragma clang diagnostic pop

// Spell the math constants as the bare names while resolving to the singleton
// accessors above, so `VZeroP` reads as a value rather than a call.
#define VZeroP ::Poseidon::Foundation::VZeroP()
#define VUpP ::Poseidon::Foundation::VUpP()
#define VForwardP ::Poseidon::Foundation::VForwardP()
#define VAsideP ::Poseidon::Foundation::VAsideP()
#define M3ZeroP ::Poseidon::Foundation::M3ZeroP()
#define M3IdentityP ::Poseidon::Foundation::M3IdentityP()
#define M4ZeroP ::Poseidon::Foundation::M4ZeroP()
#define M4IdentityP ::Poseidon::Foundation::M4IdentityP()

#endif // _T_MATH

} // namespace Poseidon::Foundation

using ::Poseidon::Foundation::AngleDifference;
using ::Poseidon::Foundation::CheckMinMax;
using ::Poseidon::Foundation::Interpolativ;
using ::Poseidon::Foundation::Matrix3P;
using ::Poseidon::Foundation::Matrix4P;
using ::Poseidon::Foundation::SaturateMax;
using ::Poseidon::Foundation::SaturateMin;
using ::Poseidon::Foundation::Vector3P;
using ::Poseidon::Foundation::VectorMax;
using ::Poseidon::Foundation::VectorMin;

#endif
