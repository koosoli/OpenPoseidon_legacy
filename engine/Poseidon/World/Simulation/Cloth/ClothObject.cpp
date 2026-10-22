#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp>
#include <Poseidon/World/Simulation/Cloth/ClothObject.h>

#include <Poseidon/Foundation/Containers/StaticArray.hpp>
#include <cmath>
#include <Poseidon/Foundation/Common/FltOpts.hpp>
#include <Poseidon/Foundation/Math/MathOpt.hpp>

using namespace Poseidon;
bool EnableVisualEffects(Vector3Par effPos, SimulationImportance prec);

namespace Poseidon
{
ClothObject::ClothObject() = default;

ClothObject::~ClothObject()
{
    _knots.Clear();
}

void ClothObject::Init(const ParamEntry& config,
                       Matrix4Val pos, // initial position and orientation
                       float xMin, float yMin, float sizeX, float sizeY)
{
    _xMin = xMin, _yMin = yMin;
    _xSize = sizeX;
    _ySize = sizeY;
    //
    // Create a rectangular cloth
    //
    int iRowPoints = config >> "rowPoints";
    int iColPoints = config >> "colPoints";
    _knots.Dim(iColPoints, iRowPoints);

    InitPos(pos, VZero);

    _maxStep = config >> "stepSize";

    _stretchCoef = config >> "stretchCoef";
    _fricCoef = config >> "fricCoef";
    _windCoef = config >> "windCoef";
    _gravCoef = config >> "gravCoef";
}

void ClothObject::InitPos(Matrix4Val pos, Vector3Val vel)
{
    // set initial positions
    float xGrid = _xSize / (_knots.W() - 1);
    float yGrid = _ySize / (_knots.H() - 1);
    Vector3 norm = pos.Rotate(VAside);
    for (int y = 0; y < _knots.H(); y++)
    {
        for (int x = 0; x < _knots.W(); x++)
        {
            ClothKnot& knot = _knots(x, y);
            Vector3 relPos(x * xGrid + _xMin, y * yGrid + _yMin, 0);
            knot._pos = pos.FastTransform(relPos);
            knot._vel = vel;
            knot._norm = norm;
        }
    }
}

// G_CONST: Vehicle.hpp.

void ClothObject::Simulate(Matrix4Val pos,      // world space position of fixed points
                           Vector3Val velocity, // world space velocity of fixed points
                           float time, Vector3Par wind, Vector3Par inertia, SimulationImportance importance)
{
    if (!EnableVisualEffects(pos.Position(), importance))
    {
        InitPos(pos, velocity);
        return;
    }

    // create acceleration array
    int nKnots = _knots.W() * _knots.H();
    AUTO_STATIC_ARRAY(Vector3, accel, 64 * 64);
    accel.Realloc(nKnots);
    accel.Resize(nKnots);
    // init acceleration of all knots with gravity
    Vector3 grav(0, -G_CONST * _gravCoef, 0);

    struct Neighbourh
    {
        int dx, dy;
        float factor;
        int align;
    };
    static const Neighbourh neighbourhs[] = {{-1, 0, 1.0f},  {+1, 0, 1.0f},  {0, -1, 1.0f},  {0, +1, 1.0f},
                                             {-1, -1, 0.5f}, {+1, +1, 0.5f}, {-1, +1, 0.5f}, {+1, -1, 0.5f}};
    // calculate stretch forces
    const float xGrid = _xSize / (_knots.W() - 1);
    const float yGrid = _ySize / (_knots.H() - 1);
    const int nNeighbourhs = sizeof(neighbourhs) / sizeof(*neighbourhs);

    struct NormNeighbourh
    {
        int dx1, dy1;
        int dx2, dy2;
    };
    static const NormNeighbourh normNeighbourhs[] = {
        {-1, 0, 0, +1},
        {0, +1, +1, 0},
        {+1, 0, 0, -1},
        {0, -1, -1, 0},
    };
    const int nNormNeighbourhs = sizeof(normNeighbourhs) / sizeof(*normNeighbourhs);

    while (time > 0)
    {
        float maxStep = _maxStep;
        float step = floatMin(time, maxStep);
        time -= maxStep;

        for (int i = 0; i < nKnots; i++)
        {
            accel[i] = grav;
        }

        for (int y = 0; y < _knots.H(); y++)
        {
            for (int x = 0; x < _knots.W(); x++)
            {
                int offset = _knots.CoordOffset(x, y);
                ClothKnot& knot = _knots(x, y);
                for (int i = 0; i < nNeighbourhs; i++)
                {
                    int xd = neighbourhs[i].dx;
                    int yd = neighbourhs[i].dy;
                    int xn = x + xd;
                    int yn = y + yd;
                    if (xn < 0 || xn >= _knots.W())
                    {
                        continue;
                    }
                    if (yn < 0 || yn >= _knots.H())
                    {
                        continue;
                    }
                    float regDist2 = Square(xd * xGrid) + Square(yd * yGrid);
                    const ClothKnot& nKnot = _knots(xn, yn);

                    float nFactor = neighbourhs[i].factor;
                    Vector3 dir = nKnot._pos - knot._pos;

                    float dist2 = dir.SquareSize();

                    float regDist = regDist2 * InvSqrt(regDist2);
                    const float minDist = 1e-3;
                    float invDist = dist2 > Square(minDist) ? InvSqrt(dist2) : (1 / minDist);
                    float dist = dist2 * invDist;

                    float diff = dist - regDist;
                    saturate(diff, -regDist * 20, +regDist * 20);
                    accel[offset] += dir * (invDist * diff * _stretchCoef * nFactor);

                    Vector3 velDiff = nKnot._vel - knot._vel;
                    accel[offset] += velDiff * (_fricCoef * nFactor);
                }

                Vector3 airDiff = wind - knot._vel;
                if (airDiff.SquareSize() > Square(1e-3))
                {
                    Vector3 airDiffDir = airDiff.Normalized();

                    float windCosAngle = airDiffDir.DotProduct(knot._norm);
                    accel[offset] += airDiff * (_windCoef * fabs(windCosAngle));
                }
            }
        }

        for (int i = 0; i < nKnots; i++)
        {
            ClothKnot& knot = _knots[i];
            Vector3 velDelta = step * accel[i];
            Vector3 midVel = knot._vel + velDelta * 0.5;
            knot._vel += velDelta;
            knot._pos += midVel * step;
        }

        // constrained points are all with x==0
        for (int y = 0; y < _knots.H(); y++)
        {
            int offset = _knots.CoordOffset(0, y);
            Vector3 relPos(0 * xGrid + _xMin, y * yGrid + _yMin, 0);
            ClothKnot& knot = _knots[offset];
            knot._pos = pos.FastTransform(relPos);
            knot._vel = velocity;
            accel[offset] = VZero;
        }

#if 1
        for (int y = 0; y < _knots.H(); y++)
        {
            for (int x = 1; x < _knots.W(); x++)
            {
                ClothKnot& knot = _knots(x, y);
                ClothKnot& pKnot = _knots(x - 1, y);
                Vector3 dir = knot._pos - pKnot._pos;
                float dist2 = dir.SquareSize();
                const float minDistFactor = 0.9; //  max. distance allowed
                const float maxDistFactor = 1.1; //  min. distance allowed

                if (dist2 > Square(xGrid * maxDistFactor))
                {
                    knot._pos = pKnot._pos + dir * (xGrid * maxDistFactor * InvSqrt(dist2));
                }
                else if (dist2 < Square(xGrid * minDistFactor))
                {
                    knot._pos = pKnot._pos + dir * (xGrid * minDistFactor * InvSqrt(dist2));
                }

                Vector3 dirVel = knot._vel - velocity;
                float distVel2 = dirVel.SquareSize();
                const float maxVelocity = 10; // max vel. distance allowed

                // float factor = xGrid/dist;
                if (distVel2 > Square(maxVelocity))
                {
                    knot._vel = velocity + dirVel * (InvSqrt(distVel2) * maxVelocity);
                }
            }
        } // fixup/limit velocity loop
#endif

        for (int y = 0; y < _knots.H(); y++)
        {
            for (int x = 0; x < _knots.W(); x++)
            {
                ClothKnot& knot = _knots(x, y);
                knot._norm = VZero;
                for (int i = 0; i < nNormNeighbourhs; i++)
                {
                    int nx1 = x + normNeighbourhs[i].dx1;
                    int ny1 = y + normNeighbourhs[i].dy1;
                    int nx2 = x + normNeighbourhs[i].dx2;
                    int ny2 = y + normNeighbourhs[i].dy2;
                    if (nx1 < 0 || nx1 >= _knots.W())
                    {
                        continue;
                    }
                    if (ny1 < 0 || ny1 >= _knots.H())
                    {
                        continue;
                    }
                    if (nx2 < 0 || nx2 >= _knots.W())
                    {
                        continue;
                    }
                    if (ny2 < 0 || ny2 >= _knots.H())
                    {
                        continue;
                    }
                    Vector3 n1Dif = _knots(nx1, ny1)._pos - knot._pos;
                    Vector3 n2Dif = _knots(nx2, ny2)._pos - knot._pos;

                    Vector3 nNormal = n1Dif.CrossProduct(n2Dif);
                    knot._norm += nNormal;
                }
                if (knot._norm.SquareSize() > Square(0.001))
                {
                    knot._norm.Normalize();
                }
                else
                {
                    knot._norm = VUp;
                }
            }
        }
    }
}

void ClothObject::SetKnot(float x, float y, Vector3Par pos, Vector3Par vel, Vector3Par norm)
{
    ClothKnot& knot = _knots.Set((int)x, (int)y);
    knot._pos = pos;
    knot._vel = vel;
}

bool ClothObject::IsConstraint(int x, int y)
{
    return x == 0;
}

Vector3Val ClothObject::GetPosition(float x, float y) const
{
    int xInt = toInt(x * (_knots.W() - 1));
    int yInt = toInt(y * (_knots.H() - 1));
    saturate(xInt, 0, _knots.W() - 1);
    saturate(yInt, 0, _knots.H() - 1);
    const ClothKnot& knot = _knots.Get(xInt, yInt);
    return knot._pos;
}

Vector3Val ClothObject::GetNormal(float x, float y) const
{
    int xInt = toInt(x * (_knots.W() - 1));
    int yInt = toInt(y * (_knots.H() - 1));
    saturate(xInt, 0, _knots.W() - 1);
    saturate(yInt, 0, _knots.H());
    const ClothKnot& knot = _knots.Get(xInt, yInt);
    return knot._norm;
}

} // namespace Poseidon
