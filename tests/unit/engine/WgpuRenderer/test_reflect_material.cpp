#include "../../../../engine/WgpuRenderer/ReflectMaterial.hpp"
#include <cassert>
#include <cmath>

using namespace Poseidon::ReflectMaterial;

static SurfaceClass ClassOf(Inputs in)
{
    return Classify(in).cls;
}

int main()
{
    // Parts are read from the file name, Czech or English, either slash.
    Inputs glass;
    glass.texture = "Data3D\\Jeep_Sklo.paa";
    glass.isVehicle = true;
    assert(ClassOf(glass) == SurfaceClass::Glass);
    assert(Classify(glass).source == Source::PartRule);

    // A token in a folder name must not classify the texture.
    Inputs folder;
    folder.texture = "\\cars\\glassworks\\body.paa";
    folder.isVehicle = true;
    assert(ClassOf(folder) == SurfaceClass::PaintGloss);

    // "paint_" must not read as interior.
    Inputs paint;
    paint.texture = "\\car\\paint_red.paa";
    paint.isVehicle = true;
    assert(ClassOf(paint) == SurfaceClass::PaintGloss);

    // Owner decision 2: military matt, civil gloss, same part logic on both.
    Inputs body;
    body.texture = "data3d\\humr_body.paa";
    body.isVehicle = true;
    body.military = true;
    assert(ClassOf(body) == SurfaceClass::PaintMatt);
    Inputs milMirror = body;
    milMirror.texture = "data3d\\humr_zrcatko.paa";
    assert(ClassOf(milMirror) == SurfaceClass::Mirror);

    // Owner, 2026-10-23: a military vehicle reflects only its glass -- chrome and metal go matt.
    Inputs milChrome = body;
    milChrome.texture = "data3d\\t72_chrom.paa";
    assert(ClassOf(milChrome) == SurfaceClass::PaintMatt);
    Inputs milGlass = body;
    milGlass.texture = "data3d\\t72_sklo.paa";
    assert(ClassOf(milGlass) == SurfaceClass::Glass);
    Inputs milEnv = body;
    milEnv.texture = "ca\\tracked\\data\\t72_body_co.paa";
    milEnv.hasEnvMap = true;
    assert(ClassOf(milEnv) == SurfaceClass::PaintMatt);

    // Owner decision 1: headlights by era.
    Inputs lamp;
    lamp.texture = "data3d\\skoda_svetla.paa";
    lamp.isVehicle = true;
    assert(ClassOf(lamp) == SurfaceClass::LampGlass);
    lamp.model = "a3\\soft_f_gamma\\hatchback_01\\hatchback_01_f.p3d";
    lamp.texture = "a3\\soft_f_gamma\\hatchback_01\\data\\lights_ca.paa";
    assert(ClassOf(lamp) == SurfaceClass::LampPlastic);
    lamp.model = "ca\\wheeled\\skodovka.p3d";
    assert(ClassOf(lamp) == SurfaceClass::LampGlass);

    // RVMAT: a Glass shader or fresnelGlass is glass even without a name hint.
    Inputs rv;
    rv.texture = "ca\\wheeled\\data\\body_co.paa";
    rv.material = "ca\\wheeled\\data\\body_a.rvmat";
    rv.pixelShader = "Glass";
    assert(ClassOf(rv) == SurfaceClass::Glass);
    assert(Classify(rv).source == Source::Rvmat);
    rv.pixelShader = "Super";
    rv.fresnelGlass = true;
    rv.material = "ca\\wheeled\\data\\mirror.rvmat";
    assert(ClassOf(rv) == SurfaceClass::Mirror);

    // Civil body with an env map reads as metallic; military with one stays matt.
    Inputs env;
    env.texture = "ca\\wheeled\\data\\sedan_body_co.paa";
    env.isVehicle = true;
    env.hasEnvMap = true;
    env.specularPower = 40.0f;
    assert(ClassOf(env) == SurfaceClass::PaintMetallic);
    env.military = true;
    assert(ClassOf(env) == SurfaceClass::PaintMatt);
    // Arma 3 paint: env map AND a high power -- still paint, never chrome.
    Inputs a3paint;
    a3paint.texture = "a3\\soft_f_gamma\\suv_01\\data\\suv_01_ext_co.paa";
    a3paint.material = "a3\\soft_f_gamma\\suv_01\\data\\suv_01_ext.rvmat";
    a3paint.isVehicle = true;
    a3paint.hasEnvMap = true;
    a3paint.specularPower = 120.0f;
    a3paint.specular = 0.8f;
    assert(ClassOf(a3paint) == SurfaceClass::PaintMetallic);

    // Names from the first logged class run (2026-10-22).
    Inputs part;
    part.isVehicle = true;
    part.texture = "data\\v3s_zrc_l_b.paa";
    assert(ClassOf(part) == SurfaceClass::Mirror);
    part.texture = "data\\sk_palubka.pac";
    assert(ClassOf(part) == SurfaceClass::Plastic);
    part.texture = "humr\\hmmwv_in_strop.pac";
    part.military = true;
    assert(ClassOf(part) == SurfaceClass::Plastic);
    part.military = false;
    part.texture = "a3\\soft_f_gamma\\suv_01\\data\\suv_01_int_base_co.paa";
    part.hasEnvMap = true;
    assert(ClassOf(part) == SurfaceClass::Plastic);
    part.texture = "data\\red_sk_kapot.pac"; // body paint must not read as interior
    assert(ClassOf(part) == SurfaceClass::PaintMetallic);
    part.hasEnvMap = false;
    part.texture = "o\\vehl\\cj5_podvozek.pac";
    assert(ClassOf(part) == SurfaceClass::Matt);
    part.texture = "data\\v3s_odrazka_l.paa";
    assert(ClassOf(part) == SurfaceClass::LampGlass);

    // Arma 1/2 Hilux: mirror.rvmat on the body atlas is shiny paint, not a mirror.
    Inputs hilux;
    hilux.isVehicle = true;
    hilux.texture = "data\\coyota_trup1.paa";
    hilux.material = "mirror.rvmat";
    hilux.hasEnvMap = true;
    assert(ClassOf(hilux) == SurfaceClass::PaintMetallic);
    hilux.texture = "data\\hilux_pristroje.paa";
    hilux.material = "data\\default.rvmat";
    assert(ClassOf(hilux) == SurfaceClass::Plastic);
    Inputs suvLamp; // an emissive light material on the body texture is body paint
    suvLamp.isVehicle = true;
    suvLamp.texture = "a3\\soft_f_gamma\\suv_01\\data\\suv_01_ext_co.paa";
    suvLamp.material = "a3\\data_f\\lights\\car_headlight_emit.rvmat";
    assert(ClassOf(suvLamp) == SurfaceClass::PaintGloss);
    suvLamp.texture = "a3\\soft_f_gamma\\hatchback_01\\data\\hatchback_01_lights_ca.paa";
    assert(ClassOf(suvLamp) == SurfaceClass::LampPlastic); // a lamp texture stays a lamp
    Inputs a3mirror; // a mirror material on a non-body texture stays a mirror
    a3mirror.isVehicle = true;
    a3mirror.texture = "#(argb,8,8,3)color(0.75,0.75,0.75,1.0,co)";
    a3mirror.material = "a3\\data_f\\mirror.rvmat";
    assert(ClassOf(a3mirror) == SurfaceClass::Mirror);

    // Declared entries win over every rule and may override single values.
    Inputs own;
    own.texture = "\\p406\\tex\\glass.paa";
    own.declared = ClassFromName("paint_metallic");
    own.declaredCoatRoughness = 0.02f;
    const Params ownP = Classify(own);
    assert(ownP.cls == SurfaceClass::PaintMetallic && ownP.source == Source::Declared);
    assert(std::fabs(ownP.coatRoughness - 0.02f) < 1e-6f);
    assert(ClassFromName("Paint_Pearl") == SurfaceClass::PaintMetallic);
    assert(ClassFromName("velvet") == SurfaceClass::None);

    // Non-vehicles keep their legacy highlight as a roughness.
    Inputs rifle;
    rifle.texture = "data3d\\m16.paa";
    rifle.specular = 0.5f;
    rifle.specularPower = 30.0f;
    const Params rp = Classify(rifle);
    assert(rp.cls == SurfaceClass::Matt && rp.source == Source::Default);
    assert(rp.roughness < 0.6f && rp.roughness > 0.4f);
    assert(RoughnessFromBlinn(0.0f) == 0.85f);
    assert(RoughnessFromBlinn(2000.0f) < RoughnessFromBlinn(20.0f));

    float n = 0, k = 0;
    assert(ParseFresnel("#(ai,64,64,1)fresnel(1.3,7)", n, k) && std::fabs(n - 1.3f) < 1e-5f &&
           std::fabs(k - 7.0f) < 1e-5f);
    assert(!ParseFresnel("#(ai,64,64,1)fresnelGlass()", n, k));
    return 0;
}
