// REN-INTERP-001 -- the regression guard for the camera overwrite found in review.
//
// The first version of the render interpolation set the camera transform once from the
// vehicle's interpolated frame and then let every branch of the camera-type switch
// overwrite it with the tick-time WorldTransform(), so the normal game camera was never
// smoothed. No unit test here can build a World, so this is a SOURCE check in the same
// spirit as test_sim_stage_evidence.cpp: it reads World.cpp and SceneDraw.cpp and fails
// if the final camera derivation or the object draw reaches for the tick-time frame again.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
std::string SourceOf(const char* relative)
{
    const std::filesystem::path p =
        std::filesystem::path(TESTS_ROOT_DIR).parent_path() / "engine" / "Poseidon" / relative;
    std::ifstream in(p, std::ios::binary);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The text between the camera-type switch and the point where the camera takes the result.
std::string CameraSwitchRegion(const std::string& world)
{
    const std::size_t sw = world.find("switch (_camType)");
    REQUIRE(sw != std::string::npos);
    const std::size_t end = world.find("camera.SetTransform(transform);", sw);
    REQUIRE(end != std::string::npos);
    return world.substr(sw, end - sw);
}
} // namespace

TEST_CASE("the final game camera derives from the vehicle's drawn frame in every branch", "[scene][interp]")
{
    const std::string region = CameraSwitchRegion(SourceOf("World/World.cpp"));
    INFO("camera switch region:\n" << region);
    // What the review caught: the tick-time frame composed with the seat offset.
    CHECK(region.find("cameraVehicle->WorldTransform()") == std::string::npos);
    CHECK(region.find("cameraVehicle->Transform()") == std::string::npos);
    // The external/group views build from CameraPosition / GetCameraDirection / Direction,
    // which are tick-time; they must go through the render delta.
    CHECK(region.find("renderDelta.FastTransform(cameraVehicle->CameraPosition())") != std::string::npos);
    CHECK(region.find("renderDelta.Rotate(cameraVehicle->GetCameraDirection(") != std::string::npos);
    CHECK(region.find("renderDelta.Rotate(_cameraOn->Direction())") != std::string::npos);
    CHECK(region.find("= cameraVehicle->CameraPosition();") == std::string::npos);
    CHECK(region.find("VUp, _cameraOn->Direction())") == std::string::npos);
    // And the inside views compose the seat offset with the drawn frame.
    CHECK(region.find("renderWorld * cameraVehicle->InsideCamera(_camType)") != std::string::npos);
}

TEST_CASE("objects and their shadows draw at the interpolated frame, not at *obj", "[scene][interp]")
{
    const std::string scene = SourceOf("World/Scene/SceneDraw.cpp");
    // DrawSortObject: both the normal and the cockpit draw take drawFrame.
    CHECK(scene.find("obj->Draw(oi->drawLOD, oi->orClip, drawFrame);") != std::string::npos);
    CHECK(scene.find("obj->Draw(oi->drawLOD, oi->orClip, *obj);") == std::string::npos);
    // DrawExShadow: the shadow's frame is the drawn one.
    const std::size_t sh = scene.find("void Scene::DrawExShadow(SortObject* oi)");
    REQUIRE(sh != std::string::npos);
    const std::string shadow = scene.substr(sh, 2000);
    CHECK(shadow.find("obj->RenderFrame(interpFrame)") != std::string::npos);
    CHECK(shadow.find("const FrameBase& pos = *obj;") == std::string::npos);
}

TEST_CASE("the tick loop captures the previous frame before EACH tick", "[scene][interp]")
{
    const std::string world = SourceOf("World/World.cpp");
    const std::size_t loop = world.find("for (std::size_t step = 0; step < steps; ++step)");
    REQUIRE(loop != std::string::npos);
    const std::string body = world.substr(loop, 600);
    const std::size_t capture = body.find("CaptureRenderPrevFrames();");
    const std::size_t stepCall = body.find("StepSimulation(");
    REQUIRE(capture != std::string::npos);
    REQUIRE(stepCall != std::string::npos);
    CHECK(capture < stepCall);
    // Alpha is published once per frame, after the loop and after the fixed-step frame.
    CHECK(world.find("Object::SetRenderInterpAlpha(stepFrame.running ? stepFrame.alpha : 1.0f);") !=
          std::string::npos);
}

TEST_CASE("RenderDelta answers through the hierarchy, not through the object's own history", "[scene][interp]")
{
    // A passenger never has a previous frame of its own (crew leave the world lists), but
    // the vehicle does. RenderWorldTransform() composes the parent's drawn frame; RenderDelta()
    // must not short-circuit on the child's flag or the outside views of a passenger stay on
    // tick time (measured 37 % displacement jitter against 6 % for the inside view).
    const std::string object = SourceOf("World/Scene/Object.cpp");
    const std::size_t fn = object.find("Matrix4 Object::RenderDelta() const");
    REQUIRE(fn != std::string::npos);
    const std::string body = object.substr(fn, object.find("\n}", fn) - fn);
    CHECK(body.find("_renderPrevValid") == std::string::npos);
    CHECK(body.find("RenderWorldTransform() * WorldInvTransform()") != std::string::npos);
    // And Entity's override walks the parent for anything not in the world.
    const std::string simul = SourceOf("World/Simulation/Simul.cpp");
    CHECK(simul.find("_hierParent->RenderWorldTransform() * _hierParent->WorldInvTransform() * WorldTransform()") !=
          std::string::npos);
}
