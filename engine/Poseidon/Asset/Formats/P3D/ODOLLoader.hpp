#pragma once

#include <Poseidon/World/Model/Model.hpp>
#include <Poseidon/Asset/Formats/P3D/P3DStructures.hpp>
#include <Poseidon/Asset/Formats/P3D/OdolRevision.hpp>
#include <Poseidon/Asset/Formats/P3D/Odol40.hpp>
#include <Poseidon/Asset/Formats/P3D/Odol49.hpp>
#include <Poseidon/Asset/Formats/P3D/Odol73.hpp>
#include <Poseidon/Asset/Formats/BISBinaryStream.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <cctype>
#include <map>
#include <unordered_map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Poseidon::Asset::Formats
{

class ODOLLoader
{
  public:
    using Model          = Poseidon::Model::Model;
    using LODLevel       = Poseidon::Model::LODLevel;
    using Mesh           = Poseidon::Model::Mesh;
    using Vertex         = Poseidon::Model::Vertex;
    using Triangle       = Poseidon::Model::Triangle;
    using Quad           = Poseidon::Model::Quad;
    using Material       = Poseidon::Model::Material;
    using MaterialStage  = Poseidon::Model::MaterialStage;
    using NamedSelection = Poseidon::Model::NamedSelection;
    using NamedProperty  = Poseidon::Model::NamedProperty;
    using Proxy          = Poseidon::Model::Proxy;
    using Section        = Poseidon::Model::Section;
    using Vector3        = Poseidon::Model::Vector3;
    using Vector2        = Poseidon::Model::Vector2;
    using Matrix4x3      = Poseidon::Model::Matrix4x3;
    using VertexFlags    = Poseidon::Model::VertexFlags;
    using FaceFlags      = Poseidon::Model::FaceFlags;
    using RenderHints    = Poseidon::Model::RenderHints;
    using BoundingBox    = Poseidon::Model::BoundingBox;
    using BoundingSphere = Poseidon::Model::BoundingSphere;

    static Model load(const std::string& filePath)
    {
        std::ifstream file(filePath, std::ios::binary | std::ios::ate);
        if (!file)
            throw std::runtime_error("Failed to open file: " + filePath);
        auto          size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<char> buffer(static_cast<size_t>(size));
        file.read(buffer.data(), size);
        return loadFromBuffer(buffer.data(), static_cast<int>(buffer.size()), filePath);
    }

    // The conversion from the parsed static-model structs to the engine's Model
    // IR, without a file.
    //
    // Exposed because AST-019's defect lived entirely here: the container parsed
    // correctly -- every LOD closed on its declared boundary and the face stream
    // matched its declared size -- and the conversion then read a named
    // selection's face list in the wrong unit. A test that starts from bytes
    // cannot isolate that; one that starts from the structs states it exactly.
    static Model convertStaticModel(const P3D::Odol73StaticModel& source, const std::string& sourcePath,
                                    uint32_t sourceVersion)
    {
        return convertOdolStatic(source, sourcePath, sourceVersion, "ODOLStatic", sourceVersion == 73);
    }

    static Model loadFromBuffer(const char* data, int size, const std::string& sourcePath)
    {
        QIStream     stream(const_cast<char*>(data), size);
        BinaryReader reader(stream);

        // AST-012A: route on the exact revision before any body is read.
        //
        // Dispatching first is what keeps a later revision from being fed to the v7
        // reader, which would not fail cleanly -- it would desynchronise and report
        // whatever the misaligned bytes said, the same failure mode AST-011A found
        // in the MLOD path. The revision is read without consuming it so the chosen
        // parser still sees its own header.
        const P3D::OdolRevisionInfo revision = P3D::PeekOdolRevision(reader);
        // AST-012B: revision 73 has one implemented shape. It gets its own reader
        // and its own converter -- it is never fed to readModel(), whose layout is
        // v7's -- and anything the narrow reader refuses comes back out as the
        // same unsupported-revision failure, carrying the constraint that broke.
        if (revision.support == P3D::OdolSupport::NarrowSubset && revision.version == 73)
            return loadOdol73Static(reader, size, sourcePath, revision);
        // AST-013: revision 40 is Arma 1's only revision, and its layout differs
        // from the A2 family in more than version gates -- LZSS rather than LZO,
        // full-float UVs and normals, clip flags in the LOD header -- so it gets
        // its own reader rather than another gate inside Odol49.
        if (revision.support == P3D::OdolSupport::NarrowSubset && revision.version == 40)
            return loadOdol40Static(reader, size, sourcePath, revision);
        if (revision.support == P3D::OdolSupport::NarrowSubset && revision.version == 48)
            return loadOdol48Static(reader, size, sourcePath, revision);
        if (revision.support == P3D::OdolSupport::NarrowSubset && revision.version == 49)
            return loadOdol49Static(reader, size, sourcePath, revision);
        if (revision.support == P3D::OdolSupport::NarrowSubset && revision.version == 50)
            return loadOdol50Static(reader, size, sourcePath, revision);
        if (revision.support == P3D::OdolSupport::NarrowSubset && revision.version == 52)
            return loadOdol52Static(reader, size, sourcePath, revision);
        if (revision.support == P3D::OdolSupport::NarrowSubset && revision.version == 54)
            return loadOdol54Static(reader, size, sourcePath, revision);
        if (revision.support != P3D::OdolSupport::Parsed)
            throw P3D::UnsupportedOdolRevision(revision);

        auto odolModel = P3D::readModel(reader);
        return convertToModel(odolModel, sourcePath);
    }

  private:
    static Model loadOdol73Static(BinaryReader& reader, int size, const std::string& sourcePath,
                                  const P3D::OdolRevisionInfo& revision)
    {
        try
        {
            return convertOdolStatic(P3D::ReadOdol73StaticModel(reader, size), sourcePath, 73, "ODOL73", true);
        }
        catch (const P3D::UnsupportedOdolRevision&)
        {
            throw;
        }
        catch (const std::runtime_error& error)
        {
            throw P3D::UnsupportedOdolRevision(revision, error.what());
        }
    }

    static Model loadOdol40Static(BinaryReader& reader, int size, const std::string& sourcePath, const P3D::OdolRevisionInfo& revision)
    {
        // measuredBasis is false: AST-018's winding conclusion was measured on a
        // revision-73 fixture and rests on that revision's packed-normal scale
        // being negative. Revision 40 stores full-float normals with no such
        // factor, so the same conclusion does not carry across untested.
        try { return convertOdolStatic(P3D::ReadOdol40StaticModel(reader, size), sourcePath, 40, "ODOL40", false); }
        catch (const P3D::UnsupportedOdolRevision&) { throw; }
        catch (const std::runtime_error& error) { throw P3D::UnsupportedOdolRevision(revision, error.what()); }
    }

    static Model loadOdol48Static(BinaryReader& reader, int size, const std::string& sourcePath, const P3D::OdolRevisionInfo& revision)
    {
        try { return convertOdolStatic(P3D::ReadOdol48StaticModel(reader, size), sourcePath, 48, "ODOL48", false); }
        catch (const P3D::UnsupportedOdolRevision&) { throw; }
        catch (const std::runtime_error& error) { throw P3D::UnsupportedOdolRevision(revision, error.what()); }
    }

    static Model loadOdol49Static(BinaryReader& reader, int size, const std::string& sourcePath, const P3D::OdolRevisionInfo& revision)
    {
        try { return convertOdolStatic(P3D::ReadOdol49StaticModel(reader, size), sourcePath, 49, "ODOL49", false); }
        catch (const P3D::UnsupportedOdolRevision&) { throw; }
        catch (const std::runtime_error& error) { throw P3D::UnsupportedOdolRevision(revision, error.what()); }
    }

    static Model loadOdol50Static(BinaryReader& reader, int size, const std::string& sourcePath, const P3D::OdolRevisionInfo& revision)
    {
        try { return convertOdolStatic(P3D::ReadOdol50StaticModel(reader, size), sourcePath, 50, "ODOL50", false); }
        catch (const P3D::UnsupportedOdolRevision&) { throw; }
        catch (const std::runtime_error& error) { throw P3D::UnsupportedOdolRevision(revision, error.what()); }
    }

    static Model loadOdol52Static(BinaryReader& reader, int size, const std::string& sourcePath, const P3D::OdolRevisionInfo& revision)
    {
        try { return convertOdolStatic(P3D::ReadOdol52StaticModel(reader, size), sourcePath, 52, "ODOL52", false); }
        catch (const P3D::UnsupportedOdolRevision&) { throw; }
        catch (const std::runtime_error& error) { throw P3D::UnsupportedOdolRevision(revision, error.what()); }
    }

    static Model loadOdol54Static(BinaryReader& reader, int size, const std::string& sourcePath, const P3D::OdolRevisionInfo& revision)
    {
        // measuredBasis is false for the same reason revisions 40 and 48-52 leave
        // it false: AST-018's winding conclusion was measured on a revision-73
        // fixture, and nothing has re-measured it here.
        try { return convertOdolStatic(P3D::ReadOdol54StaticModel(reader, size), sourcePath, 54, "ODOL54", false); }
        catch (const P3D::UnsupportedOdolRevision&) { throw; }
        catch (const std::runtime_error& error) { throw P3D::UnsupportedOdolRevision(revision, error.what()); }
    }

    static Model convertOdolStatic(const P3D::Odol73StaticModel& source, const std::string& sourcePath, uint32_t sourceVersion, const char* sourceEncoding, bool measuredBasis)
    {
        const auto& info = source.directory.model;
        Model model;
        model.sourcePath    = sourcePath;
        model.sourceFormat  = "ODOL";
        model.sourceVersion = sourceVersion;
        model.special       = static_cast<float>(info.special);
        model.boundingSphere.radius = info.boundingSphere;
        model.geometrySphere.radius = info.geometrySphere;
        model.boundingBox.min = Vector3(info.bboxMin.x, info.bboxMin.y, info.bboxMin.z);
        model.boundingBox.max = Vector3(info.bboxMax.x, info.bboxMax.y, info.bboxMax.z);
        model.aimingCenter    = Vector3(info.aimingCenter.x, info.aimingCenter.y, info.aimingCenter.z);
        model.color           = info.color;
        model.viewDensity     = info.viewDensity;
        model.remarksFlags    = info.remarks;
        model.andHints        = static_cast<RenderHints>(info.andHints);
        model.orHints         = static_cast<RenderHints>(info.orHints);

        // Raw SOURCE facts, independent of what the static IR converter keeps.
        // Public conversion of an unverified manually assembled struct does
        // not establish declared-body coverage or known absence of motion.
        const bool auditedRevision = sourceVersion == 40 || sourceVersion == 48 || sourceVersion == 49 ||
            sourceVersion == 50 || sourceVersion == 52 || sourceVersion == 54 || sourceVersion == 73;
        if (auditedRevision && source.decodedRevision == sourceVersion && source.declaredLodBodiesDecoded && !source.lods.empty() &&
            source.lods.size() == source.directory.starts.size() && source.lods.size() == info.resolutions.size())
        {
            auto& audit = model.sourceAudit;
            audit.producerVersion = 1;
            audit.sourceRevision = sourceVersion;
            audit.geometryCoverage = Poseidon::Model::SourceGeometryCoverage::DeclaredLodsDecoded;
            audit.declaredLods = uint32_t(source.directory.starts.size());
            audit.decodedLods = uint32_t(source.lods.size());
            audit.observations = Poseidon::Model::SourceAuditAllObservations;
            audit.skeletonDeclared = !source.directory.skeletonName.empty();
            audit.skeletonBones = uint32_t(source.directory.bones.size());
            audit.directoryHasAnimations = source.directory.hasAnimations;
            audit.directoryAnimationClasses = uint32_t(source.directory.animations.classes.size());
            for (const auto& lod : source.lods)
            {
                // Parser limits bound these sums to <=100 LODs x1M records.
                audit.keyframeCount += lod.observedKeyframes;
                audit.vertexBoneReferenceCount += uint32_t(lod.rest.vertexBoneRefs.size());
                audit.neighbourBoneReferenceCount += uint32_t(lod.rest.neighbourBoneRefs.size());
                if (!lod.keyframesObserved)
                    audit.observations &= ~uint32_t(Poseidon::Model::SourceAuditObservation::Keyframes);
            }
            audit.keyframePayloadDiscarded = audit.keyframeCount != 0;
        }

        model.lodLevels.reserve(source.lods.size());
        for (size_t i = 0; i < source.lods.size(); ++i)
        {
            LODLevel lod;
            lod.resolution     = info.resolutions[i];
            lod.purpose        = Poseidon::Model::ClassifyLodResolution(lod.resolution);
            lod.sourceEncoding = sourceEncoding;
            // AST-018. Measured, not assumed: for all 16 faces of the reference
            // fixture the cross product of the first two edges in index order
            // points opposite the model's own stored vertex normal, unanimously.
            // The indices are therefore clockwise with respect to those normals.
            //
            // The name says "relative to stored normals" because that is the
            // whole of what was measured. It rests on the packed-normal scale
            // factor being negative (DecodeOdol73Normal); flip that sign and the
            // conclusion flips with it. No render has confirmed which way the
            // faces actually point on screen.
            lod.basis.winding            = measuredBasis ? Poseidon::Model::SourceWinding::ClockwiseRelativeToStoredNormals : Poseidon::Model::SourceWinding::Unknown;
            lod.basis.windingConfidence  = measuredBasis ? Poseidon::Model::BasisConfidence::Measured : Poseidon::Model::BasisConfidence::Unknown;
            lod.basis.normals            = Poseidon::Model::NormalOrientation::StoredPerVertex;
            lod.basis.normalsConfidence  = measuredBasis ? Poseidon::Model::BasisConfidence::Measured : Poseidon::Model::BasisConfidence::Assumed;
            // ODOL's packed S/T pair is preserved by the raw reader but its
            // basis convention is not established. Rendering therefore derives
            // a tangent frame from geometry and UV0 instead of guessing here.
            lod.basis.tangents           = Poseidon::Model::TangentHandedness::Unknown;
            lod.basis.tangentsConfidence = Poseidon::Model::BasisConfidence::Unknown;
            lod.basis.origin             = Poseidon::Model::SourceOrigin::ModelSpace;
            lod.basis.originConfidence   = Poseidon::Model::BasisConfidence::Measured;
            lod.sourceWinding = Poseidon::Model::DescribeWinding(lod.basis.winding, lod.basis.windingConfidence);
            lod.uvSetCount     = static_cast<int32_t>(source.lods[i].rest.uvSetCount);
            lod.mesh           = convertOdol73Mesh(source.lods[i]);
            // AST-016/017 stage 2: give the 2001 animation machinery its named
            // selections back. From ODOL 48 onward BI moved skinning out of the
            // selections into per-vertex bone references; the selection NAMES
            // survive with zero vertices, and everything OFP animates -- turret
            // rotation, wheel spin, the static/blur rotor swap -- resolves
            // bone -> named selection -> vertices and finds nothing.
            synthesizeBoneSelections(source.directory.bones, source.lods[i], lod.mesh, sourcePath, i);
            model.lodLevels.push_back(std::move(lod));
        }

        // COL-001: the physical body. Until this was carried the converter left
        // every later-revision model at mass 0, which Object::IsPassable() reads
        // as "a soldier may walk through this" -- the walk-through-walls report
        // on every generation except OFP's own ODOL 7, whose converter (below)
        // always copied these.
        model.massArray.assign(info.massArray.begin(), info.massArray.end());
        model.mass     = info.mass;
        model.invMass  = info.invMass;
        model.armor    = info.armor;
        model.invArmor = info.invArmor;
        model.centerOfMass = Vector3(info.centerOfMass.x, info.centerOfMass.y, info.centerOfMass.z);
        for (int row = 0; row < 3; ++row)
        {
            model.invInertia[row * 3 + 0] = info.invInertia[row].x;
            model.invInertia[row * 3 + 1] = info.invInertia[row].y;
            model.invInertia[row * 3 + 2] = info.invInertia[row].z;
        }
        model.metadata["odolPropertyClass"]  = info.propertyClass;
        model.metadata["odolPropertyDamage"] = info.propertyDamage;
        // The mass total is what the binariser wrote, but a mass array with a
        // zero total is a file that never had one computed. Sum it here so the
        // IR carries the same number LODShape::CalculateMass would reach.
        if (!(model.mass > 0.0f) && !model.massArray.empty())
        {
            double total = 0.0;
            for (float m : model.massArray)
                total += m;
            if (total > 0.0)
            {
                model.mass    = static_cast<float>(total);
                model.invMass = static_cast<float>(1.0 / total);
            }
        }

        AssignSpecialLodIndices(model);
        return model;
    }

    // COL-001: the special-LOD index table, derived from the LOD resolutions.
    //
    // The file carries the same table as a run of signed bytes, and the runtime
    // (LODShape::ScanShapes) recomputes it from the resolutions whenever
    // OptimizeShapes runs, so the resolutions are the authority the engine
    // actually uses. Deriving here rather than copying the bytes also sidesteps
    // the one thing not measured about the byte run: revisions 54 and 73 widen
    // it by one and two slots whose position inside the run is not established.
    //
    // The fallbacks mirror ScanShapes exactly: view geometry defaults to the
    // geometry LOD, fire geometry to the view geometry, and a `firegeometry` /
    // `viewgeometry` property on the geometry LOD redirects either back to it.
    static void AssignSpecialLodIndices(Model& model)
    {
        model.memoryIdx = model.geometryIdx = model.geometryFireIdx = model.geometryViewIdx = -1;
        model.geometryViewPilotIdx = model.geometryViewGunnerIdx = -1;
        model.geometryViewCommanderIdx = model.geometryViewCargoIdx = -1;
        model.landContactIdx = model.roadwayIdx = model.pathsIdx = model.hitpointsIdx = -1;

        const int lodCount = static_cast<int>(std::min<size_t>(model.lodLevels.size(), 127));
        for (int i = 0; i < lodCount; ++i)
        {
            const int8_t index = static_cast<int8_t>(i);
            switch (Poseidon::Model::ClassifyLodResolution(model.lodLevels[i].resolution))
            {
                case Poseidon::Model::LodPurpose::Geometry:              model.geometryIdx = index; break;
                case Poseidon::Model::LodPurpose::Memory:                model.memoryIdx = index; break;
                case Poseidon::Model::LodPurpose::LandContact:           model.landContactIdx = index; break;
                case Poseidon::Model::LodPurpose::Roadway:               model.roadwayIdx = index; break;
                case Poseidon::Model::LodPurpose::Paths:                 model.pathsIdx = index; break;
                case Poseidon::Model::LodPurpose::HitPoints:             model.hitpointsIdx = index; break;
                case Poseidon::Model::LodPurpose::ViewGeometry:          model.geometryViewIdx = index; break;
                case Poseidon::Model::LodPurpose::FireGeometry:          model.geometryFireIdx = index; break;
                case Poseidon::Model::LodPurpose::ViewPilotGeometry:     model.geometryViewPilotIdx = index; break;
                case Poseidon::Model::LodPurpose::ViewGunnerGeometry:    model.geometryViewGunnerIdx = index; break;
                case Poseidon::Model::LodPurpose::ViewCommanderGeometry: model.geometryViewCommanderIdx = index; break;
                case Poseidon::Model::LodPurpose::ViewCargoGeometry:     model.geometryViewCargoIdx = index; break;
                default: break;
            }
        }
        if (model.geometryViewIdx < 0)
            model.geometryViewIdx = model.geometryIdx;
        if (model.geometryFireIdx < 0)
            model.geometryFireIdx = model.geometryViewIdx;
        if (model.geometryIdx >= 0)
        {
            const auto& properties = model.lodLevels[static_cast<size_t>(model.geometryIdx)].mesh.properties;
            for (const auto& property : properties)
            {
                if (property.name == "firegeometry" && std::atoi(property.value.c_str()) > 0)
                    model.geometryFireIdx = model.geometryIdx;
                if (property.name == "viewgeometry" && std::atoi(property.value.c_str()) > 0)
                    model.geometryViewIdx = model.geometryIdx;
            }
        }
    }

    // AST-016/017 stage 2: fill EMPTY bone-named selections from the LOD's
    // per-vertex bone references, so `Skeleton::Prepare`, `AnimationWithCenter`
    // and the rotor static/blur swap work on ODOL 48+ exactly as they do on
    // OFP and Arma 1 models, whose selections still carry vertices.
    //
    // Layout of one reference (measured on the A2 T-72 and the AST-016/017
    // fixtures): `count` pairs of (subSkeletonBoneIndex:u8, weight:u8) in
    // `data`, at most four. The index is into the LOD's sub-skeleton, remapped
    // to the model skeleton through `subSkeletonsToSkeleton` when that table is
    // present. Because the byte ORDER of the pair is the classic silent-failure
    // spot (an index read as a weight still "works"), the layout is VALIDATED
    // per LOD before use: every index byte must land inside the remap table (or
    // the bone table when there is no remap). If the pairs fail validation both
    // ways round, the LOD is left untouched and a log line says so -- a frozen
    // rotor names itself; a rotor glued to the hull by garbage indices does not.
    //
    // Only selections that are EMPTY and whose name matches a skeleton bone are
    // filled -- populated selections (OFP, A1, and A2's sectional lists) are
    // never modified, so this cannot perturb anything that already works.
    // Dominant influence only: vehicle parts are rigid, and OFP's own
    // animations treat membership as binary; weight 255 keeps ShapeAdapter's
    // linear scale honest.
    static void synthesizeBoneSelections(const std::vector<P3D::Odol73Bone>& bones, const P3D::Odol73StaticLod& source,
                                         Mesh& mesh, const std::string& sourcePath, size_t lodIndex)
    {
        const auto& refs  = source.rest.vertexBoneRefs;
        const auto& remap = source.header.subSkeletonsToSkeleton;
        if (bones.empty() || refs.empty())
            return;

        // Does any empty selection even name a bone? (cheap out for the
        // majority of world props)
        auto lowered = [](std::string s) {
            for (char& c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        std::unordered_map<std::string, uint32_t> boneByName;
        for (size_t b = 0; b < bones.size(); ++b)
            boneByName.emplace(lowered(bones[b].name), static_cast<uint32_t>(b));

        bool wanted = false;
        for (const auto& sel : mesh.selections)
            if (sel.vertexIndices.empty() && boneByName.count(lowered(sel.name)))
            {
                wanted = true;
                break;
            }
        if (!wanted)
            return;

        // Validate the (index, weight) byte order on this LOD's actual data.
        const uint32_t indexLimit =
            remap.empty() ? static_cast<uint32_t>(bones.size()) : static_cast<uint32_t>(remap.size());
        auto validCount = [&](bool indexFirst) {
            size_t valid = 0, total = 0;
            for (const auto& r : refs)
            {
                const int pairs = std::min<int>(r.count, 4);
                for (int p = 0; p < pairs; ++p)
                {
                    const uint8_t index = r.data[static_cast<size_t>(p) * 2 + (indexFirst ? 0 : 1)];
                    ++total;
                    if (index < indexLimit)
                        ++valid;
                }
            }
            return total == 0 ? 0.0 : static_cast<double>(valid) / static_cast<double>(total);
        };
        const double indexFirstScore  = validCount(true);
        const double weightFirstScore = validCount(false);
        if (indexFirstScore < 0.999 && weightFirstScore < 0.999)
        {
            LOG_WARN(Graphics,
                     "Bone-selection synthesis skipped for {} lod {}: reference bytes fit neither layout "
                     "({:.3f} / {:.3f} valid vs limit {})",
                     sourcePath, lodIndex, indexFirstScore, weightFirstScore, indexLimit);
            return;
        }
        const bool indexFirst = indexFirstScore >= weightFirstScore;

        // The second classic silent failure: BI stores the index byte
        // PREMULTIPLIED BY TWO in this record (the runtime's own accessor
        // halves it). A premultiplied byte still passes the bounds check for
        // every bone in the lower half of the table and lands each vertex on
        // the WRONG bone -- which is not a frozen turret but a turret that
        // rotates into the hull, exactly what the first live test produced.
        // Measured, not assumed: if essentially every index byte is even AND
        // the halved maximum still addresses the table, the bytes are
        // premultiplied. A genuine unmultiplied ID stream goes odd within a
        // handful of vertices (bone 1 exists on every rig this size).
        size_t evenCount = 0, totalCount = 0;
        uint32_t maxIndexByte = 0;
        for (const auto& r : refs)
        {
            const int pairs = std::min<int>(r.count, 4);
            for (int p = 0; p < pairs; ++p)
            {
                const uint8_t index = r.data[static_cast<size_t>(p) * 2 + (indexFirst ? 0 : 1)];
                ++totalCount;
                if ((index & 1u) == 0)
                    ++evenCount;
                maxIndexByte = std::max<uint32_t>(maxIndexByte, index);
            }
        }
        const bool premultiplied = totalCount > 0 &&
                                   static_cast<double>(evenCount) / static_cast<double>(totalCount) >= 0.999 &&
                                   (maxIndexByte / 2) < indexLimit;
        const uint32_t indexShift = premultiplied ? 1u : 0u;

        // Dominant model-skeleton bone -- computed per REFERENCE RECORD, then
        // spread to vertices. The records are parallel to the LOD's POINTS,
        // not its vertices: on the A2 T-72's LOD 0 there are 13,104 references
        // against 15,165 vertices, and treating them as per-vertex assigned a
        // third of the turret to bones belonging to whatever geometry happened
        // to share the truncated index range -- the turret rotated into the
        // hull and vanished on the first live test. `vertexToPoint` (parsed by
        // every one of these readers) is the bridge: each vertex takes its
        // point's dominant bone. When the map is absent or sized wrong the
        // records are treated as per-vertex, which is exact whenever the two
        // counts agree.
        // Chosen by MEASURED SIZE, not by field name: the map that has one
        // entry per VERTEX is the vertex->record bridge, whichever of the two
        // arrays it is (the first live test proved trusting the names is how a
        // turret disappears).
        const std::vector<int32_t>* vertexMap = nullptr;
        if (source.vertexToPoint.size() == mesh.vertices.size())
            vertexMap = &source.vertexToPoint;
        else if (source.pointToVertex.size() == mesh.vertices.size())
            vertexMap = &source.pointToVertex;
        const auto& vertexToPoint = vertexMap ? *vertexMap : source.vertexToPoint;
        const bool  perPoint = vertexMap != nullptr && refs.size() != mesh.vertices.size();
        std::vector<int32_t> dominantByRecord(refs.size(), -1);
        size_t influenced = 0;
        for (size_t v = 0; v < refs.size(); ++v)
        {
            const auto& r = refs[v];
            const int pairs = std::min<int>(r.count, 4);
            int bestWeight = -1;
            uint32_t bestBone = 0;
            for (int p = 0; p < pairs; ++p)
            {
                const uint32_t index =
                    static_cast<uint32_t>(r.data[static_cast<size_t>(p) * 2 + (indexFirst ? 0 : 1)]) >> indexShift;
                const uint8_t weight = r.data[static_cast<size_t>(p) * 2 + (indexFirst ? 1 : 0)];
                if (index >= indexLimit)
                    continue;
                const uint32_t modelBone = remap.empty() ? index : static_cast<uint32_t>(remap[index]);
                if (modelBone >= bones.size())
                    continue;
                if (static_cast<int>(weight) > bestWeight)
                {
                    bestWeight = weight;
                    bestBone   = modelBone;
                }
            }
            if (bestWeight >= 0)
            {
                dominantByRecord[v] = static_cast<int32_t>(bestBone);
                ++influenced;
            }
        }
        if (influenced == 0)
            return;

        // Spread record bones to vertices, then bucket per bone and fill
        // exactly the empty bone-named selections.
        std::vector<int32_t> dominantBone(mesh.vertices.size(), -1);
        for (size_t v = 0; v < mesh.vertices.size(); ++v)
        {
            size_t record = v;
            if (perPoint)
            {
                const int32_t point = vertexToPoint[v];
                if (point < 0)
                    continue;
                record = static_cast<size_t>(point);
            }
            if (record < dominantByRecord.size())
                dominantBone[v] = dominantByRecord[record];
        }
        std::unordered_map<uint32_t, std::vector<uint32_t>> verticesByBone;
        for (size_t v = 0; v < dominantBone.size(); ++v)
            if (dominantBone[v] >= 0)
                verticesByBone[static_cast<uint32_t>(dominantBone[v])].push_back(static_cast<uint32_t>(v));

        // A bone-named selection takes its bone's vertices AND EVERY
        // DESCENDANT BONE'S. This is the difference between a rotor and a bare
        // hub. OFP's "velka vrtule" was one flat selection holding everything
        // that turns with the main rotor; Arma 2 kept the name but factored
        // the part into a subtree, and on the Mi-35 that subtree is
        //     velka vrtule dive -> velka vrtule -> {rotordive, rotor static dive}
        // with the blade geometry on the CHILDREN. Filling the selection from
        // bone 1 alone spins the hub and leaves the blades standing still,
        // which is exactly what the owner reported as "A2 heli rotor spinning
        // wrong" -- and the same shape of bug waits on every nested part
        // (otocvez -> otochlaven -> gatling_1, gear_1_1 -> _damper -> _stabil),
        // where OFP's selection likewise covered the whole chain. The closure
        // walks DOWN only: "velka vrtule" must not pick up its parent's dive.
        std::unordered_map<std::string, std::vector<uint32_t>> childrenOf;
        for (size_t b = 0; b < bones.size(); ++b)
            childrenOf[lowered(bones[b].parent)].push_back(static_cast<uint32_t>(b));

        size_t filled = 0, byDescent = 0;
        for (auto& sel : mesh.selections)
        {
            if (!sel.vertexIndices.empty())
                continue;
            const auto bone = boneByName.find(lowered(sel.name));
            if (bone == boneByName.end())
                continue;

            std::vector<uint32_t> stack{bone->second};
            std::vector<bool> visited(bones.size(), false);
            std::vector<uint32_t> verts;
            size_t descendants = 0;
            while (!stack.empty())
            {
                const uint32_t b = stack.back();
                stack.pop_back();
                if (b >= bones.size() || visited[b])
                    continue;
                visited[b] = true;
                const auto bucket = verticesByBone.find(b);
                if (bucket != verticesByBone.end())
                    verts.insert(verts.end(), bucket->second.begin(), bucket->second.end());
                const auto kids = childrenOf.find(lowered(bones[b].name));
                if (kids != childrenOf.end())
                    for (uint32_t kid : kids->second)
                        if (kid < bones.size() && !visited[kid])
                        {
                            stack.push_back(kid);
                            ++descendants;
                        }
            }
            if (verts.empty())
                continue;
            std::sort(verts.begin(), verts.end());
            verts.erase(std::unique(verts.begin(), verts.end()), verts.end());
            sel.vertexIndices = std::move(verts);
            sel.vertexWeights.assign(sel.vertexIndices.size(), 255);
            ++filled;
            if (descendants > 0)
                ++byDescent;
        }
        if (filled > 0)
        {
            LOG_INFO(Graphics,
                     "Bone-selection synthesis: {} lod {} filled {} selections ({} with descendants) from {} "
                     "influenced vertices ({}-first pairs{}, {} bones, remap {}, records {})",
                     sourcePath, lodIndex, filled, byDescent, influenced, indexFirst ? "index" : "weight",
                     premultiplied ? " premultiplied" : "", bones.size(),
                     remap.empty() ? "identity" : "sub-skeleton", perPoint ? "per-point" : "per-vertex");
        }
    }

    static Mesh convertOdol73Mesh(const P3D::Odol73StaticLod& source)
    {
        Mesh mesh;
        const auto& rest = source.rest;
        mesh.vertices.reserve(rest.positions.size());
        for (size_t i = 0; i < rest.positions.size(); ++i)
        {
            Vertex vertex;
            vertex.position = Vector3(rest.positions[i].x, rest.positions[i].y, rest.positions[i].z);
            vertex.normal   = Vector3(rest.normals[i].x, rest.normals[i].y, rest.normals[i].z);
            vertex.uv       = Vector2(rest.uv0.uv[i][0], rest.uv0.uv[i][1]);
            // Channel 1 is promoted onto the vertex, as the MLOD path does. Any
            // further channels stay on the source record rather than being
            // silently folded into these two.
            if (!rest.extraUvSets.empty() && i < rest.extraUvSets[0].uv.size())
                vertex.uv1 = Vector2(rest.extraUvSets[0].uv[i][0], rest.extraUvSets[0].uv[i][1]);
            vertex.flags    = static_cast<VertexFlags>(rest.clip[i]);
            mesh.vertices.push_back(vertex);
        }

        for (const auto& texture : source.header.textures)
            mesh.materials.emplace_back(texture, texture);
        // The embedded RVMAT a section names is the shader description for the
        // texture that section draws with; recording it against the wrong slot
        // would be worse than leaving it empty.
        // Which material slot each SECTION resolves to. A textured section resolves to the
        // slot its texture created, which is what the loop below has always done. A section
        // with NO texture used to resolve to nothing at all, and that is what made Arma 3
        // structures render white: `bricks_v1_f` LOD0 has 458 faces, zero textures and one
        // embedded RVMAT, and the material list was built purely from the texture list, so
        // it came out empty and the section was skipped for having no texture index. The
        // RVMAT naming its base colour was discarded, the section reached the draw with no
        // texture and no material, and an unbound hull is white.
        //
        // A section that names a material is describing how it is shaded whether or not it
        // also names a legacy texture, so give it a slot either way.
        std::vector<uint32_t> sectionMaterialSlot(source.sections.size(), UINT32_MAX);
        const auto            fillEmbedded = [&](Material& material, const auto& embedded) {
            material.materialPath = embedded.name;
            material.embeddedStages.clear();
            material.embeddedStages.reserve(embedded.stageTextures.size());
            for (size_t stage = 0; stage < embedded.stageTextures.size(); ++stage)
            {
                MaterialStage entry;
                entry.sourceStage = static_cast<uint32_t>(stage);
                entry.texturePath = embedded.stageTextures[stage];
                if (stage < embedded.texGens.size())
                    entry.uvSource = embedded.texGens[stage].uvSource;
                material.embeddedStages.push_back(std::move(entry));
            }
        };
        for (size_t s = 0; s < source.sections.size(); ++s)
        {
            const auto& section     = source.sections[s];
            const bool  hasTexture  = section.textureIndex >= 0 &&
                                    static_cast<size_t>(section.textureIndex) < mesh.materials.size();
            const bool  hasEmbedded = section.materialIndex >= 0 &&
                                     static_cast<size_t>(section.materialIndex) < source.materials.size();
            if (hasTexture)
            {
                sectionMaterialSlot[s] = static_cast<uint32_t>(section.textureIndex);
                if (hasEmbedded)
                    fillEmbedded(mesh.materials[section.textureIndex], source.materials[section.materialIndex]);
            }
            else if (hasEmbedded)
            {
                // Texture-less: append a slot that carries the RVMAT and no legacy texture.
                // The renderer's authored Stage0 is what supplies this section's base colour.
                sectionMaterialSlot[s] = static_cast<uint32_t>(mesh.materials.size());
                mesh.materials.emplace_back(std::string(), std::string());
                fillEmbedded(mesh.materials.back(), source.materials[section.materialIndex]);
            }
        }

        // Section bounds are offsets into the face stream, not face indices, so
        // the offsets are rebuilt and checked against the LOD's own declared
        // face-data size before any face is assigned a material from them.
        const auto offsets = P3D::Odol73FaceStreamOffsets(source.polygons);
        if (offsets.back() != source.polygons.faceDataSize)
            throw std::runtime_error("ODOL 73 face-stream offsets disagree with the LOD's declared face-data size");

        for (size_t face = 0; face < source.polygons.faces.size(); ++face)
        {
            const auto& indices = source.polygons.faces[face];
            uint32_t materialIndex = UINT32_MAX;
            for (const auto& section : source.sections)
                if (offsets[face] >= static_cast<uint32_t>(section.faceLower) &&
                    offsets[face] < static_cast<uint32_t>(section.faceUpper) && section.textureIndex >= 0)
                    materialIndex = static_cast<uint32_t>(section.textureIndex);
            if (indices.size() == 3)
            {
                Triangle triangle;
                for (int i = 0; i < 3; ++i) triangle.indices[i] = indices[i];
                triangle.materialIndex = materialIndex;
                triangle.originalIndex = static_cast<uint32_t>(face);
                mesh.triangles.push_back(triangle);
            }
            else
            {
                Quad quad;
                for (int i = 0; i < 4; ++i) quad.indices[i] = indices[i];
                quad.materialIndex = materialIndex;
                quad.originalIndex = static_cast<uint32_t>(face);
                mesh.quads.push_back(quad);
            }
        }

        mesh.sections.reserve(source.sections.size());
        for (size_t sectionIndex = 0; sectionIndex < source.sections.size(); ++sectionIndex)
        {
            const auto& source_section = source.sections[sectionIndex];
            Section     section;
            section.materialIndex = sectionMaterialSlot[sectionIndex];
            section.specialMaterial = source_section.materialIndex;
            // MAT-046: carry the section's hidden bits through, so proxy marker
            // triangles stop drawing. Both existing skips (RegisterGpuModel and
            // Shape::Draw) already test these; they were simply never given a
            // non-zero value on this generation, because the reader dropped the
            // CommonFaceFlags word.
            //
            // MASKED to the two hidden bits rather than passed whole, which is
            // the one thing that differs from the v7 path below. The full word
            // also carries NoClamp (set on every section), ClampU/V,
            // IsAlphaOrdered and ZBias, and on this generation it would reach
            // render::IsGpuOwnedSectionSpec, which classifies blend/surface from
            // the spec -- so handing it all through could reroute sections
            // between the retained and CPU paths as a side effect of a fix about
            // hiding markers. Widening this is a separate, measurable change.
            constexpr uint32_t kHiddenMask = static_cast<uint32_t>(RenderHints::IsHidden)
                                             | static_cast<uint32_t>(RenderHints::IsHiddenProxy);
            section.hints = static_cast<RenderHints>(source_section.commonFaceFlags & kHiddenMask);
            // Face-stream offsets, as on the v7 path: not triangle indices.
            section.startTriangle = static_cast<uint32_t>(source_section.faceLower);
            section.triangleCount = static_cast<uint32_t>(source_section.faceUpper - source_section.faceLower);
            // AST-018: and the same boundary as a face range. The offsets were
            // already rebuilt and checked against the LOD's declared face-data
            // size above, so this is a conversion, not an interpretation.
            // Known for every revision-73 section, including an empty one: a
            // zero faceCount here means the section covers no faces, not that
            // the range could not be worked out.
            section.faceRangeKnown = true;
            for (size_t face = 0; face < source.polygons.faces.size(); ++face)
            {
                if (offsets[face] < static_cast<uint32_t>(source_section.faceLower)) continue;
                if (offsets[face] >= static_cast<uint32_t>(source_section.faceUpper)) break;
                if (section.faceCount == 0) section.firstFace = static_cast<uint32_t>(face);
                ++section.faceCount;
            }
            mesh.sections.push_back(section);
        }

        // Named selections.
        //
        // AST-019: `selectedFaces` holds FACE INDICES, not the face-stream byte
        // offsets a section's bounds use. The two live in the same LOD and are not
        // the same unit, which is the whole reason this went unnoticed: the
        // sections above are byte offsets and are handled correctly, so the
        // offset table exists and looks like the obvious thing to reuse.
        //
        // Measured through this reader on real files of both generations, every
        // non-empty selection in the sample:
        //
        //   A_Mosque_big_hq_EP1 (ODOL 49)  736 selections with faces:
        //       every entry a valid face index   736
        //       every entry a valid byte offset    0
        //   Hospital_F          (ODOL 73)  382 selections with faces:
        //       every entry a valid face index   382
        //       every entry a valid byte offset    0
        //
        // Wider sweep over 120 models of structures_e (ODOL 48-52): 31,420
        // non-proxy selections all-index, 0 all-offset, 0 neither. The handful
        // that ALSO satisfy the offset test are selections of face 0 alone, where
        // index 0 and offset 0 coincide.
        //
        // The clincher is shape rather than counts: `component01` selects faces
        // [187,188,189,190,191,192] in a LOD of 1,841 faces whose stream is 18,188
        // bytes. Consecutive integers are indices; byte offsets would step by 8.
        //
        // Reading them as offsets is why the previous code produced an empty face
        // list for essentially every selection on both generations -- and named
        // selections drive proxy-marker hiding, hit points, damage and destruction,
        // so this was not only a proxy defect.
        mesh.selections.reserve(source.namedSelections.size());
        for (const auto& sourceSelection : source.namedSelections)
        {
            NamedSelection selection;
            selection.name = sourceSelection.name;
            selection.vertexIndices.reserve(sourceSelection.selectedVertices.size());
            for (int32_t index : sourceSelection.selectedVertices)
                if (index >= 0 && static_cast<size_t>(index) < mesh.vertices.size())
                    selection.vertexIndices.push_back(static_cast<uint32_t>(index));
            selection.vertexWeights = sourceSelection.vertexWeights;
            selection.sourceVertexWeights = sourceSelection.vertexWeights;
            selection.triangleIndices.reserve(sourceSelection.selectedFaces.size());
            for (int32_t face : sourceSelection.selectedFaces)
                if (face >= 0 && static_cast<size_t>(face) < source.polygons.faces.size())
                    selection.triangleIndices.push_back(static_cast<uint32_t>(face));
            selection.sectionIndices.reserve(sourceSelection.sections.size());
            for (int32_t index : sourceSelection.sections)
                if (index >= 0) selection.sectionIndices.push_back(static_cast<uint32_t>(index));
            selection.needsSections = sourceSelection.isSectional;
            mesh.selections.push_back(std::move(selection));
        }

        // AST-016B: proxies, for every revision that lands here (40, 48, 49, 50,
        // 52, 54, 73). The readers have always parsed the LOD header's proxy
        // array in full -- model path, transform, sequence id, named-selection
        // and bone indices -- and this conversion then dropped it on the floor,
        // so a crew member, a weapon or a muzzle flash reached the engine on the
        // ODOL v7 path only. `convertProxies` below is the v7 twin of this loop.
        //
        // The transform needs no reinterpretation between the two: BIS stores
        // `Matrix4x3` as `Vector3 rows[4]` = [aside, up, dir, pos]
        // (`BISStructures.hpp:33`), and the IR's `Matrix4x3::Set` takes the same
        // basis-then-translation order that ShapeAdapter reads back out as
        // SetDirectionAside / SetDirectionUp / SetDirection / SetPosition. The
        // rows are copied straight out of the file by every one of these readers
        // (e.g. `Odol40.hpp:243`), so the only change here is transposing the
        // translation out of row 3 into the fourth column.
        //
        // `name` carries the raw model path (`\ca\temp\proxies\t72\driver`).
        // ShapeAdapter's normalizeProxyModelName strips an optional `proxy:`
        // prefix and an optional `.NN` suffix, so a bare path is accepted as-is
        // and the sequence id is supplied separately through `id` -- which is
        // what the engine prefers anyway (`proxy.id >= 0 ? proxy.id : name.id`).
        mesh.proxies.reserve(source.header.proxies.size());
        for (const auto& p : source.header.proxies)
        {
            // A proxy with no model names nothing to instantiate; NewProxyObject
            // would be handed an empty shape name.
            if (p.model.empty())
                continue;
            Proxy proxy;
            proxy.name           = p.model;
            proxy.selectionIndex = static_cast<uint32_t>(p.namedSelectionIndex >= 0 ? p.namedSelectionIndex : 0);
            proxy.id             = p.sequenceId;
            proxy.transform.Set(p.transform.rows[0].x, p.transform.rows[0].y, p.transform.rows[0].z,
                                p.transform.rows[3].x,
                                p.transform.rows[1].x, p.transform.rows[1].y, p.transform.rows[1].z,
                                p.transform.rows[3].y,
                                p.transform.rows[2].x, p.transform.rows[2].y, p.transform.rows[2].z,
                                p.transform.rows[3].z);
            mesh.proxies.push_back(std::move(proxy));
        }

        mesh.properties.reserve(source.namedProperties.size());
        for (const auto& property : source.namedProperties)
            mesh.properties.emplace_back(property.first, property.second);

        mesh.boundingBox.min = Vector3(source.header.bboxMin.x, source.header.bboxMin.y, source.header.bboxMin.z);
        mesh.boundingBox.max = Vector3(source.header.bboxMax.x, source.header.bboxMax.y, source.header.bboxMax.z);
        mesh.bCenter  = Vector3(source.header.bboxCenter.x, source.header.bboxCenter.y, source.header.bboxCenter.z);
        mesh.bRadius  = source.header.bboxRadius;
        mesh.orHints  = static_cast<RenderHints>(source.header.orHints);
        mesh.andHints = static_cast<RenderHints>(source.header.andHints);
        mesh.iconColor     = static_cast<uint32_t>(source.endData.colorTop);
        mesh.selectedColor = static_cast<uint32_t>(source.endData.color);
        mesh.special       = static_cast<Poseidon::Model::SpecialFlags>(source.endData.special);
        return mesh;
    }

    static Model convertToModel(const P3D::Model& odol, const std::string& sourcePath)
    {
        Model model;
        model.sourcePath   = sourcePath;
        model.sourceFormat = "ODOL";
        model.sourceVersion = odol.header.version;

        // ODOL7's whole declared LOD loop and stored animation flag have been
        // read by readModel. Skeleton/bone-reference fields do not exist in
        // that wire format; their absence is format evidence, not IR inference.
        if (odol.header.version == 7 && !odol.lods.empty() && odol.lods.size() == odol.header.lodCount &&
            odol.resolutions.size() == odol.header.lodCount)
        {
            auto& audit = model.sourceAudit;
            audit.producerVersion = 1;
            audit.sourceRevision = 7;
            audit.geometryCoverage = Poseidon::Model::SourceGeometryCoverage::DeclaredLodsDecoded;
            audit.observations = Poseidon::Model::SourceAuditAllObservations;
            audit.declaredLods = odol.header.lodCount;
            audit.decodedLods = uint32_t(odol.lods.size());
            audit.directoryHasAnimations = odol.allowAnimation;
            for (const auto& lod : odol.lods) audit.keyframeCount += uint32_t(lod.frames.frames.size());
            // These frames are retained by convertLOD; no pose claim follows.
        }

        model.lodLevels.reserve(odol.lods.size());
        for (size_t i = 0; i < odol.lods.size(); ++i)
        {
            float resolution = (i < odol.resolutions.size()) ? odol.resolutions[i] : 0.0f;
            model.lodLevels.push_back(convertLOD(odol.lods[i], resolution));
        }

        model.special              = static_cast<float>(odol.special);
        model.boundingSphere.radius = odol.boundingSphere;
        model.geometrySphere.radius = odol.geometrySphere;
        model.boundingCenter = Vector3(odol.boundingCenter.x, odol.boundingCenter.y, odol.boundingCenter.z);
        model.geometryCenter = Vector3(odol.geometryCenter.x, odol.geometryCenter.y, odol.geometryCenter.z);
        model.boundingBox.min = Vector3(odol.minMax[0].x, odol.minMax[0].y, odol.minMax[0].z);
        model.boundingBox.max = Vector3(odol.minMax[1].x, odol.minMax[1].y, odol.minMax[1].z);
        model.aimingCenter   = Vector3(odol.aimingCenter.x, odol.aimingCenter.y, odol.aimingCenter.z);
        model.color          = odol.color;
        model.colorTop       = odol.colorTop;
        model.viewDensity    = odol.viewDensity;
        model.remarksFlags   = odol.remarks;
        model.centerOfMass   = Vector3(odol.centerOfMass.x, odol.centerOfMass.y, odol.centerOfMass.z);
        memcpy(model.invInertia, odol.invInertia, sizeof(model.invInertia));
        model.andHints          = static_cast<RenderHints>(odol.andHints);
        model.orHints           = static_cast<RenderHints>(odol.orHints);
        model.canOcclude        = odol.canOcclude ? 1 : 0;
        model.canBeOccluded     = odol.canBeOccluded ? 1 : 0;
        model.allowAnimation    = odol.allowAnimation ? 1 : 0;
        model.lockAutoCenter    = odol.lockAutoCenter ? 1 : 0;
        model.autoCenterEnabled = odol.autoCenter ? 1 : 0;
        model.mapType           = odol.mapType;

        model.massArray.assign(odol.massArray.begin(), odol.massArray.end());
        model.mass     = odol.mass;
        model.invMass  = odol.invMass;
        model.armor    = odol.armor;
        model.invArmor = odol.invArmor;

        model.memoryIdx                = odol.memory;
        model.geometryIdx              = odol.geometry;
        model.geometryFireIdx          = odol.geometryFire;
        model.geometryViewIdx          = odol.geometryView;
        model.geometryViewPilotIdx     = odol.geometryViewPilot;
        model.geometryViewGunnerIdx    = odol.geometryViewGunner;
        model.geometryViewCommanderIdx = odol.geometryViewCommander;
        model.geometryViewCargoIdx     = odol.geometryViewCargo;
        model.landContactIdx           = odol.landContact;
        model.roadwayIdx               = odol.roadway;
        model.pathsIdx                 = odol.paths;
        model.hitpointsIdx             = odol.hitpoints;

        return model;
    }

    static LODLevel convertLOD(const P3D::CompleteLOD& odolLOD, float resolution)
    {
        LODLevel lod;
        lod.resolution = resolution;
        lod.purpose    = Poseidon::Model::ClassifyLodResolution(resolution);
        lod.mesh       = convertMesh(odolLOD);
        return lod;
    }

    static Mesh convertMesh(const P3D::CompleteLOD& odolLOD)
    {
        Mesh mesh;
        convertGeometry(odolLOD, mesh);
        convertMaterials(odolLOD, mesh);
        convertSections(odolLOD, mesh);
        convertSelections(odolLOD, mesh);
        convertProperties(odolLOD, mesh);
        convertProxies(odolLOD, mesh);
        convertEdges(odolLOD, mesh);
        convertFrames(odolLOD, mesh);
        convertBounds(odolLOD, mesh);
        mesh.iconColor     = odolLOD.endData.colorTop;
        mesh.selectedColor = odolLOD.endData.color;
        mesh.special       = static_cast<Poseidon::Model::SpecialFlags>(odolLOD.endData.special);
        return mesh;
    }

    static void convertGeometry(const P3D::CompleteLOD& odolLOD, Mesh& mesh)
    {
        const auto& vtable = odolLOD.vertexTable;
        mesh.vertices.reserve(vtable.points.size());
        for (size_t i = 0; i < vtable.points.size(); ++i)
        {
            Vertex      v;
            const auto& bisPos = vtable.points[i];
            v.position = Vector3(bisPos.x, bisPos.y, bisPos.z);
            if (i < vtable.normals.size())
            {
                const auto& bisNorm = vtable.normals[i];
                v.normal = Vector3(bisNorm.x, bisNorm.y, bisNorm.z);
            }
            else
            {
                v.normal = Vector3(0, 0, 1);
            }
            if (i < vtable.uvCoords.size())
            {
                const auto& bisUV = vtable.uvCoords[i];
                v.uv = Vector2(bisUV.u, bisUV.v);
            }
            else
            {
                v.uv = Vector2(0, 0);
            }
            v.flags = i < vtable.clipFlags.size() ? static_cast<VertexFlags>(vtable.clipFlags[i]) : VertexFlags::None;
            mesh.vertices.push_back(v);
        }

        size_t faceIdx = 0;
        for (const auto& face : odolLOD.faces.faces)
        {
            if (face.vertexCount == 3)
            {
                Triangle tri;
                tri.indices[0]   = face.vertexIndices[0];
                tri.indices[1]   = face.vertexIndices[1];
                tri.indices[2]   = face.vertexIndices[2];
                tri.materialIndex = 0;
                tri.flags        = static_cast<FaceFlags>(face.flags);
                tri.originalIndex = static_cast<uint32_t>(faceIdx);
                mesh.triangles.push_back(tri);
            }
            else if (face.vertexCount == 4)
            {
                Quad quad;
                quad.indices[0]   = face.vertexIndices[0];
                quad.indices[1]   = face.vertexIndices[1];
                quad.indices[2]   = face.vertexIndices[2];
                quad.indices[3]   = face.vertexIndices[3];
                quad.materialIndex = 0;
                quad.flags        = static_cast<FaceFlags>(face.flags);
                quad.originalIndex = static_cast<uint32_t>(faceIdx);
                mesh.quads.push_back(quad);
            }
            faceIdx++;
        }
    }

    static void convertMaterials(const P3D::CompleteLOD& odolLOD, Mesh& mesh)
    {
        mesh.materials.reserve(odolLOD.textures.texturePaths.size());
        for (const auto& texPath : odolLOD.textures.texturePaths)
        {
            Material mat;
            mat.name        = texPath;
            mat.texturePath = texPath;
            mesh.materials.push_back(mat);
        }

        size_t triIdx = 0, quadIdx = 0;
        for (const auto& face : odolLOD.faces.faces)
        {
            uint32_t matIdx = face.textureIndex >= 0 ? static_cast<uint32_t>(face.textureIndex) : UINT32_MAX;
            if (face.vertexCount == 3)
            {
                if (triIdx < mesh.triangles.size())
                    mesh.triangles[triIdx].materialIndex = matIdx;
                ++triIdx;
            }
            else if (face.vertexCount == 4)
            {
                if (quadIdx < mesh.quads.size())
                    mesh.quads[quadIdx].materialIndex = matIdx;
                ++quadIdx;
            }
        }
    }

    static void convertSections(const P3D::CompleteLOD& odolLOD, Mesh& mesh)
    {
        mesh.sections.reserve(odolLOD.sections.sections.size());
        for (const auto& sec : odolLOD.sections.sections)
        {
            Section section;
            section.materialIndex   = sec.textureIndex >= 0 ? static_cast<uint32_t>(sec.textureIndex) : UINT32_MAX;
            section.specialMaterial = sec.material;
            // Byte offsets, not face indices; ShapeAdapter uses FindSections instead
            section.startTriangle   = sec.faceIndexLowerBound;
            section.triangleCount   = sec.faceIndexUpperBound - sec.faceIndexLowerBound;
            section.hints           = static_cast<RenderHints>(sec.special);
            mesh.sections.push_back(section);
        }
    }

    static void convertSelections(const P3D::CompleteLOD& odolLOD, Mesh& mesh)
    {
        mesh.selections.reserve(odolLOD.namedSelections.selections.size());
        for (const auto& sel : odolLOD.namedSelections.selections)
        {
            NamedSelection selection;
            selection.name = sel.name;
            selection.vertexIndices.reserve(sel.vertexIndices.size());
            for (uint16_t idx : sel.vertexIndices)
                selection.vertexIndices.push_back(static_cast<uint32_t>(idx));
            selection.vertexWeights = sel.vertexWeights;
            // ODOL's array is already the linear 0-255 weight ShapeAdapter uses,
            // so the raw and decoded views coincide here. Filled anyway so a
            // consumer reading the source view is not told MLOD has weights and
            // ODOL has none.
            selection.sourceVertexWeights = sel.vertexWeights;
            selection.triangleIndices.reserve(sel.faceIndices.size());
            for (uint16_t idx : sel.faceIndices)
                selection.triangleIndices.push_back(static_cast<uint32_t>(idx));
            selection.faceSelectionOffsets = sel.faceSelectionIndices;
            selection.sectionIndices.reserve(sel.faceSelectionIndices2.size());
            for (uint32_t idx : sel.faceSelectionIndices2)
                selection.sectionIndices.push_back(idx);
            selection.needsSections = sel.needSelection;
            mesh.selections.push_back(selection);
        }
    }

    static void convertProperties(const P3D::CompleteLOD& odolLOD, Mesh& mesh)
    {
        mesh.properties.reserve(odolLOD.namedProperties.properties.size());
        for (const auto& prop : odolLOD.namedProperties.properties)
        {
            NamedProperty property;
            property.name  = prop.property;
            property.value = prop.value;
            mesh.properties.push_back(property);
        }
    }

    static void convertProxies(const P3D::CompleteLOD& odolLOD, Mesh& mesh)
    {
        mesh.proxies.reserve(odolLOD.proxies.proxies.size());
        for (const auto& p : odolLOD.proxies.proxies)
        {
            Proxy proxy;
            proxy.name           = p.name;
            proxy.selectionIndex = p.namedSelectionIndex;
            proxy.id             = static_cast<int32_t>(p.sequenceId);
            // ODOL stores [right, up, forward, position]
            proxy.transform.Set(
                p.transform[0].x, p.transform[0].y, p.transform[0].z, p.transform[3].x,
                p.transform[1].x, p.transform[1].y, p.transform[1].z, p.transform[3].y,
                p.transform[2].x, p.transform[2].y, p.transform[2].z, p.transform[3].z);
            mesh.proxies.push_back(proxy);
        }
    }

    static void convertEdges(const P3D::CompleteLOD& odolLOD, Mesh& mesh)
    {
        mesh.edges.mlodIndices   = odolLOD.edges.mlodIndex.edges;
        mesh.edges.vertexIndices = odolLOD.edges.vertexIndex.edges;
    }

    static void convertFrames(const P3D::CompleteLOD& odolLOD, Mesh& mesh)
    {
        mesh.frames.reserve(odolLOD.frames.frames.size());
        for (const auto& sourceFrame : odolLOD.frames.frames)
        {
            std::vector<Vector3> positions;
            positions.reserve(sourceFrame.bonePositions.size());
            for (const auto& sourcePos : sourceFrame.bonePositions)
                positions.emplace_back(sourcePos.x, sourcePos.y, sourcePos.z);
            mesh.frames.emplace_back(sourceFrame.frameTime, std::move(positions));
        }
    }

    static void convertBounds(const P3D::CompleteLOD& odolLOD, Mesh& mesh)
    {
        const auto& bounds    = odolLOD.bounds;
        mesh.boundingBox.min  = Vector3(bounds.minPos.x, bounds.minPos.y, bounds.minPos.z);
        mesh.boundingBox.max  = Vector3(bounds.maxPos.x, bounds.maxPos.y, bounds.maxPos.z);
        mesh.bCenter          = Vector3(bounds.bCenter.x, bounds.bCenter.y, bounds.bCenter.z);
        mesh.bRadius          = bounds.bRadius;
        mesh.orHints          = static_cast<RenderHints>(bounds.orHints);
        mesh.andHints         = static_cast<RenderHints>(bounds.andHints);
    }
};

} // namespace Poseidon::Asset::Formats
