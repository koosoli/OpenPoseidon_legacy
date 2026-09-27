#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageClodDiskCodec.hpp>
#include <memory>

namespace Poseidon::GeometryPages
{
// Original controlled pilot only: same actual Shape packing as the private
// renderer fixture. Explicit Tools/tests operation, no global startup/frame work.
// The upstream bake is synchronous and not cancellable or scratch/time bounded.
inline constexpr uint32_t ControlledClodPilotVersion=1;
struct ControlledClodSource { ShapeExport original;std::string helperSha256; };
struct ControlledClodCache { ClodRamPackage selected;std::string helperSha256;uint32_t groups=0,clusters=0,originalFineVertices=0,originalFineTriangles=0,authoredFallbackTriangles=0; };
enum class ControlledClodStatus { Built, WrongOwner, Failed, AllocationFailed };
namespace ControlledClodDetail
{
inline Shape* Box(int subdivisions)
{
    auto shape=std::make_unique<Shape>();
    const Vector3 corners[8]={Vector3(-2,0,-2),Vector3(2,0,-2),Vector3(2,0,2),Vector3(-2,0,2),
        Vector3(-2,3,-2),Vector3(2,3,-2),Vector3(2,3,2),Vector3(-2,3,2)};
    const int faces[6][4]={{0,1,2,3},{4,7,6,5},{0,4,5,1},{1,5,6,2},{2,6,7,3},{3,7,4,0}};
    const Vector3 normals[6]={Vector3(0,-1,0),Vector3(0,1,0),Vector3(0,0,-1),Vector3(1,0,0),Vector3(0,0,1),Vector3(-1,0,0)};
    for(int f=0;f<6;++f) for(int v=0;v<subdivisions;++v) for(int u=0;u<subdivisions;++u)
    {
        const auto point=[&](float a,float b) {
            return corners[faces[f][0]]*(1-a)*(1-b)+corners[faces[f][1]]*a*(1-b)+
                corners[faces[f][2]]*a*b+corners[faces[f][3]]*(1-a)*b; };
        const int first=shape->NVertex();
        const float a=float(u)/subdivisions,b=float(v)/subdivisions,c=float(u+1)/subdivisions,d=float(v+1)/subdivisions;
        shape->AddVertexFast(point(a,b),normals[f],0,a,b); shape->AddVertexFast(point(c,b),normals[f],0,c,b);
        shape->AddVertexFast(point(c,d),normals[f],0,c,d); shape->AddVertexFast(point(a,d),normals[f],0,a,d);
        Poly face; face.Init(); face.SetN(4);
        for(int i=0;i<4;++i) face.Set(i,first+i);
        shape->AddFace(face);
    }
    ShapeSection section; section.properties.Init(); section.material=0;
    section.beg=shape->BeginFaces(); section.end=shape->EndFaces(); shape->AddSection(section);
    return shape.release();
}
// Original connected curved grid. Shared vertex IDs allow real simplification;
// the box's per-quad seam copies deliberately do not serve as a CLOD pilot.
inline Shape* Grid(int subdivisions)
{
    auto shape=std::make_unique<Shape>();
    for(int v=0;v<=subdivisions;++v) for(int u=0;u<=subdivisions;++u) {
        const float a=float(u)/subdivisions,b=float(v)/subdivisions;
        const float x=a*4-2,z=b*4-2;
        const float y=.9f*(1-x*x/4)*(1-z*z/4);
        shape->AddVertexFast(Vector3(x,y,z),V3Up,0,a,b);
    }
    for(int v=0;v<subdivisions;++v) for(int u=0;u<subdivisions;++u) {
        const int a=v*(subdivisions+1)+u,b=a+1,c=a+subdivisions+1,d=c+1;
        Poly face; face.Init(); face.SetN(4);
        face.Set(0,a); face.Set(1,c); face.Set(2,d); face.Set(3,b); shape->AddFace(face);
    }
    ShapeSection section; section.properties.Init(); section.material=0;
    section.beg=shape->BeginFaces(); section.end=shape->EndFaces(); shape->AddSection(section);
    return shape.release();
}
inline std::string HelperHash(const LODShape& shape)
{
    Foundation::Sha256 hash;
    for(int level=2;level<5;++level) {
        const Shape& geometry=*shape.Level(level);
        for(int i=0;i<geometry.NVertex();++i) {
            const float p[3]={geometry.Pos(i).X(),geometry.Pos(i).Y(),geometry.Pos(i).Z()}; hash.Update(p,sizeof(p));
        }
        for(Offset at=geometry.BeginFaces();at<geometry.EndFaces();geometry.NextFace(at)) {
            const auto& face=geometry.Face(at); const int n=face.N(); hash.Update(&n,sizeof(n));
            for(int i=0;i<n;++i) { const auto index=face.GetVertex(i); hash.Update(&index,sizeof(index)); }
        }
    }
    return hash.Hex();
}
}
inline ControlledClodStatus BuildControlledClodSource(ControlledClodSource& destination)
{
    if(!Foundation::IsMainThread()) return ControlledClodStatus::WrongOwner;
    try {
        LODShape authored;authored.AddShape(ControlledClodDetail::Grid(1),10);authored.AddShape(ControlledClodDetail::Grid(16),1);
        authored.AddShape(ControlledClodDetail::Box(1),1e13f);authored.AddShape(ControlledClodDetail::Box(1),6e15f);authored.AddShape(ControlledClodDetail::Box(1),7e15f);
        if(authored.FindGeometryLevel()!=2 || authored.FindViewGeometryLevel()!=3 || authored.FindFireGeometryLevel()!=4) return ControlledClodStatus::Failed;
        const auto helpers=ControlledClodDetail::HelperHash(authored);
        ShapeExportSelection selection;selection.controlledAuthoredRigid=true;selection.coarseLevel=0;selection.fineLevel=1;
        selection.source.coarseRepresentation=0;selection.source.fineRepresentation=1;
        selection.source.vertexLayout=sizeof(SVertex);selection.source.materialMapping=1;selection.source.sourceSha256[0]=1;
        ControlledClodSource result;
        if(ExportShapePair(authored,selection,result.original)!=ExportStatus::Exported) return ControlledClodStatus::Failed;
        Foundation::Sha256 hash;hash.Update(std::string("OpenPoseidon-private-curved-clod-v1"));
        for(const auto* mesh:{&result.original.coarse,&result.original.fine}) {
            hash.Update(mesh->vertices.data(),mesh->vertices.size()*sizeof(SVertex));
            hash.Update(mesh->indices.data(),mesh->indices.size()*sizeof(uint32_t));hash.Update(mesh->materials.data(),mesh->materials.size()*sizeof(uint32_t));
        }
        result.original.source.sourceSha256=ClodDiskDetail::HashBytes(hash);
        if(helpers!=ControlledClodDetail::HelperHash(authored)) return ControlledClodStatus::Failed;
        result.helperSha256=helpers;destination=std::move(result);return ControlledClodStatus::Built;
    } catch(const std::bad_alloc&) {return ControlledClodStatus::AllocationFailed;}
}
inline ControlledClodStatus BakeControlledClodCache(ControlledClodCache& destination)
{
    ControlledClodSource source;const auto built=BuildControlledClodSource(source);
    if(built!=ControlledClodStatus::Built) return built;
    try {
        ClodBake bake;ClodBakeLimits limits;limits.vertices=289;limits.triangles=512;limits.groups=128;
        limits.clusters=64;limits.indexEntries=32768;limits.storedBytes=262144;
        if(BakeClodPilot(source.original,true,bake,limits)!=ClodBakeStatus::Baked) return ControlledClodStatus::Failed;
        float threshold=0;for(const auto& group:bake.groups) if(group.simplified.error!=FLT_MAX) threshold=std::max(threshold,group.simplified.error);
        if(!(threshold>0) || !std::isfinite(threshold)) return ControlledClodStatus::Failed;
        threshold=std::nextafter(threshold,std::numeric_limits<float>::infinity());
        if(!std::isfinite(threshold) || threshold==FLT_MAX) return ControlledClodStatus::Failed;
        ClodCut coarse,fine;ClodDecodedCut coarseDecoded,fineDecoded;
        ClodCutDecodeLimits cutLimits;cutLimits.clusters=64;cutLimits.vertexRecords=4096;cutLimits.indexEntries=8192;cutLimits.knownPayloadBytes=262144;
        if(!SelectClodCut(bake,threshold,coarse) || !SelectClodCut(bake,0,fine) ||
           DecodeClodCut(bake,threshold,coarse,coarseDecoded,cutLimits)!=ClodCutDecodeStatus::Decoded ||
           DecodeClodCut(bake,0,fine,fineDecoded,cutLimits)!=ClodCutDecodeStatus::Decoded) return ControlledClodStatus::Failed;
        ControlledClodCache result;
        if(BuildClodRamPackage(bake,threshold,0,coarseDecoded,fineDecoded,result.selected)!=ClodRamStatus::Built) return ControlledClodStatus::Failed;
        result.originalFineVertices=uint32_t(source.original.fine.vertices.size());
        result.originalFineTriangles=uint32_t(source.original.fine.indices.size()/3);
        result.authoredFallbackTriangles=uint32_t(source.original.coarse.indices.size()/3);
        result.groups=uint32_t(bake.groups.size());result.clusters=uint32_t(bake.clusters.size());result.helperSha256=source.helperSha256;
        destination=std::move(result);return ControlledClodStatus::Built;
    } catch(const std::bad_alloc&) {return ControlledClodStatus::AllocationFailed;}
}
}
