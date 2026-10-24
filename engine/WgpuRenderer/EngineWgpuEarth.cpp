#include "EngineWgpu.hpp"
#include "EarthWorldgen.hpp"
#include "EarthRoadCoverage.hpp"
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/Foundation/Logging/Logging.hpp>
#include <Poseidon/Graphics/Rendering/Shape/Shape.hpp>
#include <Poseidon/World/MapTypes.hpp>
#include <cmath>
#include <cstring>

namespace Poseidon
{
namespace
{
class EarthRoadShape final : public LODShapeWithShadow
{
public:
    explicit EarthRoadShape(const LODShapeWithShadow& source)
    {
        // LODShape's assignment DEEP-copies every Shape and its sections; its
        // copy deliberately clears the name, which this independent owner keeps.
        static_cast<LODShape&>(*this) = source;
        _name = source.Name();
        ScanProperties();
    }
};
}
bool EngineWgpu::CreateEarthWorldgenObject(uint32_t id, const char* modelName, const Matrix4& transform)
{
    if (!Foundation::IsMainThread() || !_gpuDriven || !_renderer || !modelName || !modelName[0] ||
        _earthWorldgenInstances.size() >= EarthWorldgen::MaxInstances)
        return false;
    if (_earthWorldgenInstances.count(id))
        return true;
    AcquireProducerWindow("Earth Worldgen installed model");
    ShapeBank::WorldModelScope worldModel(Shapes);
    const bool roadCoverage = EarthRoadCoverage::StockRoad(modelName);
    Ref<LODShapeWithShadow> shape;
    if (roadCoverage)
    {
        auto found = _earthWorldgenRoadShapes.find(modelName);
        if (found != _earthWorldgenRoadShapes.end()) shape = found->second;
        else
        {
            Ref<LODShapeWithShadow> source = Shapes.New(modelName, false, true);
            if (source)
            {
                shape = new EarthRoadShape(*source);
                _earthWorldgenRoadShapes.emplace(modelName, shape);
                LOG_INFO(Graphics, "Earth Worldgen road alpha coverage: {} dedicated owner, cutout/A2C alphaRef 0.5, lift 0.02m; approximate blend edges", modelName);
            }
        }
    }
    else shape = Shapes.New(modelName, false, true);
    if (!shape || !shape->NLevels() || !shape->Level(0) || shape->Level(0)->NPos() <= 0 ||
        shape->Level(0)->NPos() > 250000)
    {
        LOG_INFO(Graphics, "Earth Worldgen model unavailable/bounded: {}", modelName);
        return false;
    }
    const uint32_t model = RegisterGpuModel(shape, roadCoverage);
    if (model == WGR_INVALID_MODEL)
    {
        LOG_INFO(Graphics, "Earth Worldgen model not retained: {}", modelName);
        return false;
    }
    // Separate registry deliberately uses rigid mode zero. The adapter supplies
    // Worldgen's complete affine pitch/shear/scale and resident ground offset.
    // ClipLand selectors remain on the shared model but do not select the
    // unrelated native island heightmap. Native instance admission is unchanged.
    WgrInstance instance{};
    GfxMatrix matrix;
    Matrix4 placed = transform;
    if (roadCoverage)
        placed.SetPosition(transform.Position() + transform.DirectionUp().Normalized() * EarthRoadCoverage::SurfaceLiftMetres);
    ConvertMatrix(matrix, placed);
    std::memcpy(instance.world.m, &matrix, sizeof(instance.world.m));
    instance.model = model;
    const auto position = transform.Position();
    // Frobenius norm bounds the largest singular value, including forest shear.
    const float radiusScale = std::hypot(std::hypot(transform.DirectionAside().Size(), transform.DirectionUp().Size()),
                                         transform.Direction().Size());
    if (!std::isfinite(radiusScale) || radiusScale <= 0 || radiusScale > 40)
        return false;
    instance.center = {position.X(), position.Y(), position.Z(), radiusScale};
    instance.flags = WGR_INSTANCE_FAR_AUTHORED;
    if (shape->GetMapType() == MapBush)
        instance.flags |= WGR_INSTANCE_CANOPY_BUSH;
    else if (shape->GetMapType() == MapTree || shape->GetMapType() == MapSmallTree)
        instance.flags |= WGR_INSTANCE_CANOPY_TREE;
    const uint32_t slot = EnqueueInstanceAdd(instance);
    ++_ops.instanceAdd;
    RetainGpuModel(shape);
    if (++_earthWorldgenModelRefs[shape] == 1)
        LOG_INFO(Graphics,
                 "Earth Worldgen installed model: {} retained {} levels {} positions {} mapType {} rigidAuthored 1",
                 modelName, model, shape->NLevels(), shape->Level(0)->NPos(), int(shape->GetMapType()));
    _earthWorldgenInstances.emplace(id, FarGpuInstance{slot, shape});
    return true;
}
bool EngineWgpu::CreateEarthTreeObject(uint64_t id, const char* modelName, float x, float groundY, float z, float yaw,
                                       float scale)
{
    if (!Foundation::IsMainThread() || !_gpuDriven || !_renderer || !modelName || !modelName[0])
        return false;
    if (_earthTreeInstances.count(id))
        return true;
    AcquireProducerWindow("Earth tree model");
    Ref<LODShapeWithShadow> shape;
    if (auto found = _earthTreeShapes.find(modelName); found != _earthTreeShapes.end())
        shape = found->second;
    else
    {
        ShapeBank::WorldModelScope worldModel(Shapes);
        shape = Shapes.New(modelName, false, true);
        if (shape && (!shape->NLevels() || !shape->Level(0) || shape->Level(0)->NPos() <= 0))
            shape = nullptr;
        _earthTreeShapes.emplace(modelName, shape);
        LOG_INFO(Graphics, "Earth tree model {}: {}", modelName,
                 shape ? "loaded" : "unavailable (the procedural trees stand in)");
    }
    if (!shape)
        return false;
    const uint32_t model = RegisterGpuModel(shape, false);
    if (model == WGR_INVALID_MODEL)
        return false;
    // The model's origin is its bounding centre; its lowest point (the trunk foot) goes on the ground.
    Matrix4 transform(MRotationY, yaw);
    transform.SetOrientation(transform.Orientation() * scale);
    transform.SetPosition(Vector3(x, groundY - shape->Min().Y() * scale, z));
    WgrInstance instance{};
    GfxMatrix matrix;
    ConvertMatrix(matrix, transform);
    std::memcpy(instance.world.m, &matrix, sizeof(instance.world.m));
    instance.model = model;
    const auto position = transform.Position();
    instance.center = {position.X(), position.Y(), position.Z(), scale};
    instance.flags = WGR_INSTANCE_FAR_AUTHORED |
                     (shape->GetMapType() == MapBush ? WGR_INSTANCE_CANOPY_BUSH : WGR_INSTANCE_CANOPY_TREE);
    const uint32_t slot = EnqueueInstanceAdd(instance);
    ++_ops.instanceAdd;
    RetainGpuModel(shape);
    _earthTreeInstances.emplace(id, FarGpuInstance{slot, shape});
    return true;
}
void EngineWgpu::KeepEarthTreeObjects(const std::vector<uint64_t>& keep)
{
    for (auto it = _earthTreeInstances.begin(); it != _earthTreeInstances.end();)
    {
        if (std::binary_search(keep.begin(), keep.end(), it->first))
        {
            ++it;
            continue;
        }
        EnqueueInstanceRemove(it->second.slot);
        ++_ops.instanceRemove;
        ReleaseGpuModel(it->second.shape);
        it = _earthTreeInstances.erase(it);
    }
}
void EngineWgpu::ClearEarthTreeObjects()
{
    for (const auto& [id, instance] : _earthTreeInstances)
    {
        EnqueueInstanceRemove(instance.slot);
        ++_ops.instanceRemove;
        ReleaseGpuModel(instance.shape);
    }
    _earthTreeInstances.clear();
}
void EngineWgpu::ClearEarthWorldgenObjects()
{
    for (const auto& [id, instance] : _earthWorldgenInstances)
    {
        EnqueueInstanceRemove(instance.slot);
        ++_ops.instanceRemove;
        ReleaseGpuModel(instance.shape);
    }
    _earthWorldgenInstances.clear();
    _earthWorldgenModelRefs.clear();
    _earthWorldgenRoadShapes.clear();
}
} // namespace Poseidon
