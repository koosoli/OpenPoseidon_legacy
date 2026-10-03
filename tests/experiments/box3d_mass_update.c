// Dependency-only oracle: no engine headers, game process or rendered scene.
// Build the identical source against the released control and experimental pin.
// The convex mass/impulse sequence mirrors SpawnDynamicPieces, which is also
// used by SpawnArticulatedPiece. A non-box volume forces meaningful mass scaling.
#include <box3d/box3d.h>
#include <math.h>
#include <stdio.h>

static int Near(const char* name, float actual, float expected)
{
    const int passed = isfinite(actual) && fabsf(actual-expected) <= .002f*fmaxf(1,fabsf(expected));
    printf("%s actual=%.9g expected=%.9g %s\n",name,actual,expected,passed ? "PASS" : "FAIL");
    return passed;
}

int main(void)
{
    b3WorldDef wd = b3DefaultWorldDef();
    wd.gravity = (b3Vec3){0,0,0};
    wd.workerCount = 1;
    b3WorldId world = b3CreateWorld(&wd);
    if (!b3World_IsValid(world)) return 2;
    b3BodyDef bd = b3DefaultBodyDef();
    bd.type = b3_dynamicBody;
    // Quarter turn about Y. World X inertia must become local Z inertia.
    bd.rotation = (b3Quat){{0,.7071067811865475f,0},.7071067811865475f};
    bd.enableSleep = false;
    b3BodyId body = b3CreateBody(world,&bd);
    const b3Vec3 vertices[] = {{.2f,0,0},{-.2f,0,0},{0,.3f,0},{0,-.3f,0},{0,0,.4f},{0,0,-.4f}};
    b3HullData* hull = b3CreateHull(vertices,6,64);
    if (!hull) { b3DestroyWorld(world); return 2; }
    b3ShapeDef sd = b3DefaultShapeDef();
    sd.density = 4.0f/(.4f*.6f*.8f); // engine's bounding-box estimate
    b3ShapeId shape = b3CreateHullShape(body,&sd,hull);
    b3DestroyHull(hull);
    if (!b3Shape_IsValid(shape)) { b3DestroyWorld(world); return 2; }
    b3MassData md = b3Body_GetMassData(body);
    const float scale = 4.0f/md.mass;
    printf("computedMass=%.9g requestedMass=4 scale=%.9g\n",md.mass,scale);
    md.mass *= scale;
    md.inertia.cx = b3MulSV(scale,md.inertia.cx);
    md.inertia.cy = b3MulSV(scale,md.inertia.cy);
    md.inertia.cz = b3MulSV(scale,md.inertia.cz);
    b3Body_SetMassData(body,md);

    // A uniform octahedron has Izz = M(a*a+b*b)/10 = .052 kg m^2.
    // Its quarter turn makes that the world X inertia. Check the actual cache
    // before stepping: waiting for Step can hide a stale inverse-world tensor.
    const float expectedInverseX = 1.0f/(4.0f*(.2f*.2f+.3f*.3f)/10.0f);
    const b3Matrix3 inverse = b3Body_GetWorldInverseRotationalInertia(body);
    int ok = Near("requestedMass",b3Body_GetMass(body),4);
    ok &= Near("immediateWorldInverseInertiaX",inverse.cx.x,expectedInverseX);
    // This is the engine's ApplyImpulse path, including a real lever arm.
    b3Body_ApplyLinearImpulse(body,(b3Vec3){0,1,0},(b3Pos){0,0,.2f},true);
    ok &= Near("immediateAngularVelocityX",b3Body_GetAngularVelocity(body).x,-.2f*expectedInverseX);

    // A density-only edit plus an explicit custom body mass must also survive
    // stepping. This preservation control is expected to pass on both versions;
    // it is not evidence that the released API carried a dirty-mass flag.
    b3Shape_SetDensity(shape,sd.density*2,false);
    b3Body_SetMassData(body,md);
    b3World_Step(world,1.0f/60,4);
    ok &= Near("massAfterDensityEditAndStep",b3Body_GetMass(body),4);
    b3DestroyWorld(world);
    printf("mass-update-regression %s\n",ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
