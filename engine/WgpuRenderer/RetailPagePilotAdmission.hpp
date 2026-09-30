#pragma once

#include <Poseidon/Graphics/Rendering/GeometryPageRigidFinalHierarchyProduct.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageRigidFinalSurfaceCertificate.hpp>
#include <Poseidon/IO/Streams/ArchiveSourceBinding.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <wgpu_renderer.hpp>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>

namespace Poseidon::render
{
// Explicit main-owner handoff for ONE selected rigid retail source. The producer
// supplies its already verified ACTUAL final Shape export and material; this
// record-only pilot never manufactures a controlled MLOD source, white texture,
// world Object identity, or shadow/all-pass coverage. sourceCurrent must check
// the exact held model and initialized texture mount/birth/slot on the owner
// thread, including the decoded model hash. It must not retain a QFBank pointer.
struct RetailPagePilotAdmission
{
    std::shared_ptr<const GeometryPages::RigidOdol7FinalExport> actual;
    std::shared_ptr<const GeometryPages::RigidFinalHierarchyProduct> product;
    // Present only for the exact default-off actual-world Auto pilot. This is
    // independently decoded Root/Fine GHP geometry, never the authored coarse
    // source LOD or a claim about pixels/all other render passes.
    std::shared_ptr<const GeometryPages::RigidFinalSurface::Proof> certifiedSurface;
    uint64_t knownProofBytes = 0;
    std::shared_ptr<const BankCompressedReadRequest> modelLease;
    Ref<Texture> texture;
    std::shared_ptr<const ArchiveSourceBinding> textureBirth;
    WgrModelSection section{}; // actual classified source; .mesh replaced per page/model
    WgrModelMaterial material{}; // actual PAC handle/coefficients, never a default
    float sphereRadius = 0;
    uint64_t textureHandle = 0;
    uint32_t textureSlotLease = 0;
    bool pixelSourceVerified = false;
    std::array<uint8_t,32> pixelSourceSha256{};
    // Extra held lease/birth/closure/control-block capacity, including the
    // optional certified-surface proof charge. Actual ShapeExport and product
    // capacities are measured and charged separately by record admission.
    uint64_t knownAuthorityBytes = 0;
    std::function<bool()> sourceCurrent;
};
} // namespace Poseidon::render
