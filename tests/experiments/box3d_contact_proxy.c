// Explicit local mass-center/origin witness for the inset contact calibration.
// The engine seam's transform-origin assertion is in test_box3d_mass_update.cpp.
#include <box3d/box3d.h>
#include <math.h>
#include <stdio.h>

int main(void)
{
    const float lift = .00325f+.0027923584f;
    b3WorldDef wd = b3DefaultWorldDef(); wd.workerCount = 1;
    b3WorldId world = b3CreateWorld(&wd);
    if (!b3World_IsValid(world)) return 2;
    b3BodyDef bd = b3DefaultBodyDef(); bd.type = b3_dynamicBody;
    bd.position = (b3Pos){6530.8667f,161.021027f,6467.71436f};
    b3BodyId body = b3CreateBody(world,&bd);
    b3Vec3 points[] = {{.03f,0,0},{-.03f,0,0},{0,.0412f,0},{0,-.0412f,0},{0,0,.05f},{0,0,-.05f}};
    for (int i=0;i<6;++i) points[i].y += lift;
    b3HullData* hull = b3CreateHull(points,6,64);
    if (!hull) { b3DestroyWorld(world); return 2; }
    b3ShapeDef sd = b3DefaultShapeDef(); sd.density = 2.0f/(.06f*.0824f*.10f);
    b3ShapeId shape = b3CreateHullShape(body,&sd,hull); b3DestroyHull(hull);
    if (!b3Shape_IsValid(shape)) { b3DestroyWorld(world); return 2; }
    b3MassData mass = b3Body_GetMassData(body);
    float scale = 2/mass.mass; mass.mass *= scale;
    mass.inertia.cx = b3MulSV(scale,mass.inertia.cx);
    mass.inertia.cy = b3MulSV(scale,mass.inertia.cy);
    mass.inertia.cz = b3MulSV(scale,mass.inertia.cz);
    b3Body_SetMassData(body,mass);
    mass = b3Body_GetMassData(body);
    const b3WorldTransform transform = b3Body_GetTransform(body);
    const int pass = fabsf(mass.center.y-lift) < .000001f && fabsf(mass.mass-2) < .00001f &&
        transform.p.x == bd.position.x && transform.p.y == bd.position.y && transform.p.z == bd.position.z;
    printf("proxyLift=%.9g actualLocalMassCenterY=%.9g actualMass=%.9g bodyOriginUnchanged=%d %s\n",
        lift,mass.center.y,mass.mass,(int)(transform.p.y == bd.position.y),pass ? "PASS" : "FAIL");
    b3DestroyWorld(world);
    return pass ? 0 : 1;
}
