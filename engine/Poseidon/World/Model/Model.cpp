
#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/World/Model/ModelComputation.hpp>
#include <algorithm>

namespace Poseidon::Model
{

bool Model::compile()
{
    if (lodLevels.empty())
    {
        return false;
    }

    // ODOL files have all data pre-computed by Oxygen (vertices already auto-centered,
    // bounding volumes stored in file). Only compute missing per-LOD data and validate.
    bool isODOL = (sourceFormat == "ODOL");

    for (auto& lod : lodLevels)
    {
        auto& mesh = lod.mesh;
        if (mesh.vertices.empty())
        {
            continue;
        }

        if (mesh.boundingBox.min.x == 0.0f && mesh.boundingBox.max.x == 0.0f)
        {
            mesh.boundingBox = ModelComputation::computeBoundingBox(mesh.vertices);
        }
    }

    bool hasAnyGeometry = false;

    if (!isODOL)
    {
        // MLOD only — ODOL has model-wide bounds from the file
        bool first = true;
        for (const auto& lod : lodLevels)
        {
            if (lod.mesh.vertices.empty())
                continue;

            hasAnyGeometry = true;
            const auto& lodBox = lod.mesh.boundingBox;

            if (first)
            {
                boundingBox = lodBox;
                first = false;
            }
            else
            {
                boundingBox.min.x = std::min(boundingBox.min.x, lodBox.min.x);
                boundingBox.min.y = std::min(boundingBox.min.y, lodBox.min.y);
                boundingBox.min.z = std::min(boundingBox.min.z, lodBox.min.z);
                boundingBox.max.x = std::max(boundingBox.max.x, lodBox.max.x);
                boundingBox.max.y = std::max(boundingBox.max.y, lodBox.max.y);
                boundingBox.max.z = std::max(boundingBox.max.z, lodBox.max.z);
            }
        }

        // MLOD only — ODOL vertices are already centered
        bool shouldAutoCenter = true;
        if (!lodLevels.empty())
        {
            for (const auto& prop : lodLevels[0].mesh.properties)
            {
                if (prop.name == "autocenter" && prop.value == "0")
                {
                    shouldAutoCenter = false;
                    break;
                }
            }
        }

        if (shouldAutoCenter && hasAnyGeometry)
        {
            Vector3 centerOffset((boundingBox.min.x + boundingBox.max.x) * 0.5f,
                                 (boundingBox.min.y + boundingBox.max.y) * 0.5f,
                                 (boundingBox.min.z + boundingBox.max.z) * 0.5f);

            // OnSurface shapes don't center Y component
            if (!lodLevels.empty())
            {
                const auto& firstMesh = lodLevels[0].mesh;
                constexpr uint32_t ClipLandOn = 0x0100;
                bool isOnSurface =
                    (static_cast<uint32_t>(firstMesh.andHints) & ClipLandOn) &&
                    (static_cast<uint32_t>(firstMesh.special) & static_cast<uint32_t>(SpecialFlags::OnSurface));

                if (isOnSurface)
                    centerOffset.y = 0.0f;
            }

            for (auto& lod : lodLevels)
            {
                auto& mesh = lod.mesh;
                if (mesh.vertices.empty())
                    continue;

                for (auto& vertex : mesh.vertices)
                {
                    vertex.position.x -= centerOffset.x;
                    vertex.position.y -= centerOffset.y;
                    vertex.position.z -= centerOffset.z;
                }

                mesh.boundingBox.min.x -= centerOffset.x;
                mesh.boundingBox.min.y -= centerOffset.y;
                mesh.boundingBox.min.z -= centerOffset.z;
                mesh.boundingBox.max.x -= centerOffset.x;
                mesh.boundingBox.max.y -= centerOffset.y;
                mesh.boundingBox.max.z -= centerOffset.z;
            }

            boundingBox.min.x -= centerOffset.x;
            boundingBox.min.y -= centerOffset.y;
            boundingBox.min.z -= centerOffset.z;
            boundingBox.max.x -= centerOffset.x;
            boundingBox.max.y -= centerOffset.y;
            boundingBox.max.z -= centerOffset.z;
        }
    }
    else
    {
        for (const auto& lod : lodLevels)
        {
            if (!lod.mesh.vertices.empty())
            {
                hasAnyGeometry = true;
                break;
            }
        }
    }

    for (auto& lod : lodLevels)
    {
        auto& mesh = lod.mesh;
        if (mesh.vertices.empty() && mesh.triangles.empty())
            continue;

        if (mesh.boundingSphere.radius <= 0.0f)
            mesh.boundingSphere = ModelComputation::computeBoundingSphere(mesh.boundingBox);

        if (mesh.sections.empty() && (!mesh.triangles.empty() || !mesh.quads.empty()))
            mesh.sections = ModelComputation::generateSections(mesh.triangles, mesh.quads);

        if (mesh.proxies.empty() && !mesh.selections.empty())
            mesh.proxies = ModelComputation::generateProxiesFromSelections(mesh.vertices, mesh.selections);

        if (mesh.HasGeometry() && !ModelComputation::validateMesh(mesh))
            return false;
    }

    if (!isODOL && hasAnyGeometry)
    {
        boundingSphere = ModelComputation::computeBoundingSphere(boundingBox);
    }

    return true;
}

bool Model::isCompiled() const
{
    if (lodLevels.empty())
    {
        return false;
    }

    bool hasAnyGeometry = false;

    for (const auto& lod : lodLevels)
    {
        const auto& mesh = lod.mesh;

        // Special LODs like hitpoints may be empty
        if (mesh.vertices.empty() && mesh.triangles.empty())
        {
            continue;
        }

        if (mesh.HasGeometry())
        {
            hasAnyGeometry = true;

            if (mesh.boundingSphere.radius <= 0.0f)
            {
                return false;
            }
        }

        // Point-only LODs (e.g., hitpoints) may have vertices without triangles — valid
    }

    return !hasAnyGeometry || boundingSphere.radius > 0.0f;
}

} // namespace Poseidon::Model
