#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageProjectedSurfaceBound.hpp>
#include <Poseidon/Graphics/Core/MatrixConversion.hpp>
#include <cstring>

namespace PS=Poseidon::GeometryPages::ProjectedSurface;
namespace {
void CopyMatrix(std::array<float,16>& out,const Poseidon::GfxMatrix& value){
    static_assert(sizeof(value)==16*sizeof(float));std::memcpy(out.data(),&value,sizeof(value));
}
PS::Input ProjectionInput(){
    PS::Input input;auto& b=input.binding;b.originalSource.sourceSha256[0]=1;
    b.originalSource.vertexLayout=68;b.originalSource.materialMapping=1;b.originalSource.fineRepresentation=1;
    b.selectedKey.source=b.originalSource;b.selectedKey.source.sourceSha256[0]=2;b.selectedKey.packing={64,128,65536};
    b.coarsePackedSha256[0]=3;b.finePackedSha256[0]=4;b.certificateAlgorithmVersion=1;
    b.coarseCut[0]=3;b.fineCut[0]=0;b.fineCut[1]=1;b.coarseCutCount=1;b.fineCutCount=2;
    b.coarseThresholdBits=std::bit_cast<uint32_t>(.5f);input.cut={5,17,9};
    input.minimum={-1,-1,-1};input.maximum={1,1,1};input.surfaceDistanceUpper=.125;
    b.fineToCoarseUpperBits=std::bit_cast<uint64_t>(.125);b.coarseToFineUpperBits=std::bit_cast<uint64_t>(.1);
    b.hausdorffUpperBits=std::bit_cast<uint64_t>(input.surfaceDistanceUpper);
    input.viewportWidth=1280;input.viewportHeight=720;input.clipNear=.25f;
    // Execute the actual engine conversion functions, NOT a guessed FOV matrix.
    Matrix4 model=MIdentity;model.SetPosition(Vector3(0,0,30));Poseidon::GfxMatrix matrix;
    Poseidon::ConvertMatrix(matrix,model);CopyMatrix(input.model,matrix);
    Poseidon::ConvertMatrix(matrix,MIdentity);matrix._41=matrix._42=matrix._43=0;CopyMatrix(input.view,matrix);
    Matrix4 projection=MZero;projection(0,0)=1.25f;projection(1,1)=1.75f;
    projection(2,2)=1.01f;projection.SetPosition(Vector3(0,0,-1.01f*input.clipNear));
    Poseidon::ConvertProjectionMatrix(matrix,projection,0);
    // Exact current PushSceneCamera WGPU infinite-far overrides; x/y/w unchanged.
    matrix._33=1;matrix._43=-input.clipNear;CopyMatrix(input.projection,matrix);return input;
}
std::array<long double,2> Project(const PS::Input& input,const std::array<long double,3>& point){
    std::array<long double,4> value{point[0],point[1],point[2],1};
    for(const auto* matrix:{&input.model,&input.view,&input.projection}){
        std::array<long double,4> next{};
        for(unsigned j=0;j<4;++j)for(unsigned i=0;i<4;++i)next[j]+=value[i]*(*matrix)[i*4+j];value=next;}
    REQUIRE(value[3]>0);
    return {(value[0]/value[3]+1)*input.viewportWidth/2+input.viewportOriginX,
            (1-value[1]/value[3])*input.viewportHeight/2+input.viewportOriginY};
}
void IndependentPairs(const PS::Input& input,const PS::Result& result){
    // Both test surfaces can consist of closed small triangles around these
    // points. The mathematical test pairs are inside the authoritative union box
    // and have local Euclidean separation <= E; no producer-binding claim here.
    const long double d=input.surfaceDistanceUpper/2;
    for(int x=-1;x<=1;++x)for(int y=-1;y<=1;++y)for(int z=-1;z<=1;++z){
        const std::array<long double,3> p{x*.5L,y*.5L,z*.5L};
        for(unsigned axis=0;axis<3;++axis)for(int sign:{-1,1}){auto q=p;q[axis]+=sign*d;
            const auto a=Project(input,p),b=Project(input,q);const auto dx=std::abs(a[0]-b[0]),dy=std::abs(a[1]-b[1]);
            CHECK(dx<=result.pixelXUpper);CHECK(dy<=result.pixelYUpper);
            CHECK(std::sqrt(dx*dx+dy*dy)<=result.euclideanPixelUpper);}
    }
}
}
TEST_CASE("Projected selected-surface bound uses actual row-memory camera conversion and viewport scale", "[geometry-page-projected-surface-bound]")
{
    auto input=ProjectionInput();PS::Result result;
    REQUIRE(PS::Build(input,input.binding,input.cut,result)==PS::Status::Bounded);
    CHECK(result.depthMinimum<=29);CHECK(result.depthMinimum>28.99);
    CHECK(result.pixelXUpper>0);CHECK(result.pixelYUpper>0);
    CHECK(result.euclideanPixelUpper>=result.pixelXUpper+result.pixelYUpper);
    CHECK(result.tuple.cut==input.cut);CHECK(result.tuple.binding==input.binding);
    CHECK(std::memcmp(result.tuple.model.data(),input.model.data(),sizeof(input.model))==0);
    CHECK(result.tuple.viewportWidth==1280);IndependentPairs(input,result);
    auto resized=input;resized.viewportWidth*=2;resized.viewportHeight*=2;resized.viewportOriginX=31;
    ++resized.cut.cameraGeneration;PS::Result bigger;
    REQUIRE(PS::Build(resized,resized.binding,resized.cut,bigger)==PS::Status::Bounded);
    CHECK(bigger.pixelXUpper>=result.pixelXUpper*2);CHECK(bigger.pixelYUpper>=result.pixelYUpper*2);
    IndependentPairs(resized,bigger);
    const auto center=Project(input,{0,0,0});CHECK(center[0]==640);CHECK(center[1]==360);
    const auto side=Project(input,{1,0,0});CHECK(side[0]>center[0]);
}

TEST_CASE("Projection admits known conservative surface algorithms with exact version binding", "[geometry-page-projected-surface-bound]")
{
    auto input=ProjectionInput();PS::Result result;
    input.binding.certificateAlgorithmVersion=2;
    REQUIRE(PS::Build(input,input.binding,input.cut,result)==PS::Status::Bounded);
    auto different=input.binding;different.certificateAlgorithmVersion=1;
    CHECK(PS::Build(input,different,input.cut,result)==PS::Status::IdentityMismatch);
    input.binding.certificateAlgorithmVersion=3;
    REQUIRE(PS::Build(input,input.binding,input.cut,result)==PS::Status::Bounded);
    different=input.binding;different.certificateAlgorithmVersion=2;
    CHECK(PS::Build(input,different,input.cut,result)==PS::Status::IdentityMismatch);
    input.binding.certificateAlgorithmVersion=4;
    CHECK(PS::Build(input,input.binding,input.cut,result)==PS::Status::Invalid);
}
TEST_CASE("Off-axis perspective depth sensitivity affine scale and rotations remain conservatively bounded", "[geometry-page-projected-surface-bound]")
{
    auto centered=ProjectionInput();PS::Result base;
    REQUIRE(PS::Build(centered,centered.binding,centered.cut,base)==PS::Status::Bounded);
    auto input=centered;
    SECTION("lateral translated object increases depth-coupled horizontal bound") {
        input.model[12]=8;PS::Result result;REQUIRE(PS::Build(input,input.binding,input.cut,result)==PS::Status::Bounded);
        CHECK(result.pixelXUpper>base.pixelXUpper);IndependentPairs(input,result);
    }
    SECTION("nonuniform affine model scale is supported without rigid-length assumptions") {
        input.model[0]=2;input.model[5]=.5f;input.model[10]=3;
        PS::Result result;REQUIRE(PS::Build(input,input.binding,input.cut,result)==PS::Status::Bounded);
        CHECK(result.depthMinimum<=27);CHECK(result.pixelXUpper>base.pixelXUpper);IndependentPairs(input,result);
    }
    SECTION("exact rigid model quarter-turn and translation-free view rotation") {
        input.model[0]=0;input.model[2]=-1;input.model[8]=1;input.model[10]=0;
        input.view[0]=0;input.view[1]=1;input.view[4]=-1;input.view[5]=0;
        PS::Result result;REQUIRE(PS::Build(input,input.binding,input.cut,result)==PS::Status::Bounded);IndependentPairs(input,result);
    }
    SECTION("wider FOV reduces mathematical bound without changing depth") {
        input.projection[0]*=.5f;input.projection[5]*=.5f;
        PS::Result result;REQUIRE(PS::Build(input,input.binding,input.cut,result)==PS::Status::Bounded);
        CHECK(result.pixelXUpper<base.pixelXUpper);CHECK(result.pixelYUpper<base.pixelYUpper);IndependentPairs(input,result);
    }
}
TEST_CASE("Near behind clipped malformed and mismatched immutable tuples refuse transactionally", "[geometry-page-projected-surface-bound]")
{
    auto input=ProjectionInput();auto expected=input.binding;auto cut=input.cut;
    PS::Result kept;kept.euclideanPixelUpper=77;kept.tuple.cut.modelBirth=99;PS::Status status=PS::Status::Unsupported;
    SECTION("union box intersects near plane"){input.model[14]=1;status=PS::Status::NearIntersection;}
    SECTION("whole object behind camera"){input.model[14]=-30;status=PS::Status::NearIntersection;}
    SECTION("near plane equality is not certified"){input.model[14]=1.25f;status=PS::Status::NearIntersection;}
    SECTION("lateral clipping changes projected set metric"){input.model[12]=40;status=PS::Status::OutsideViewport;}
    SECTION("live camera generation paired with old tuple"){++cut.cameraGeneration;status=PS::Status::IdentityMismatch;}
    SECTION("reused model identity"){++cut.modelBirth;status=PS::Status::IdentityMismatch;}
    SECTION("certificate generation mismatch"){++cut.certificateGeneration;status=PS::Status::IdentityMismatch;}
    SECTION("full selected key mismatch"){++expected.selectedKey.packing.pageBytes;status=PS::Status::IdentityMismatch;}
    SECTION("packed cut hash mismatch"){expected.finePackedSha256[0]^=1;status=PS::Status::IdentityMismatch;}
    SECTION("actual cut IDs mismatch"){++expected.coarseCut[0];status=PS::Status::IdentityMismatch;}
    SECTION("threshold bit mismatch"){++expected.coarseThresholdBits;status=PS::Status::IdentityMismatch;}
    SECTION("zero generation"){input.cut.cameraGeneration=0;cut=input.cut;status=PS::Status::Invalid;}
    SECTION("missing certificate binding"){input.binding.certificateAlgorithmVersion=0;expected=input.binding;status=PS::Status::Invalid;}
    SECTION("empty viewport"){input.viewportWidth=0;status=PS::Status::Invalid;}
    SECTION("bounded viewport cap"){input.viewportHeight=16385;status=PS::Status::Invalid;}
    SECTION("unknown perspective convention"){input.projection[8]=.1f;}
    SECTION("translated view violates actual camera-relative convention"){input.view[12]=1;}
    SECTION("nonfinite transform"){input.model[0]=INFINITY;}
    SECTION("projective model unsupported"){input.model[3]=1;}
    SECTION("subnormal float requires unknown DAZ semantics"){input.model[0]=std::bit_cast<float>(uint32_t(1));}
    SECTION("nonfinite certified error"){input.surfaceDistanceUpper=INFINITY;input.binding.hausdorffUpperBits=std::bit_cast<uint64_t>(input.surfaceDistanceUpper);expected=input.binding;status=PS::Status::Invalid;}
    SECTION("negative certified error"){input.surfaceDistanceUpper=-1;input.binding.hausdorffUpperBits=std::bit_cast<uint64_t>(input.surfaceDistanceUpper);expected=input.binding;status=PS::Status::Invalid;}
    SECTION("numeric error changed without matching certified bits"){input.surfaceDistanceUpper=0;status=PS::Status::IdentityMismatch;}
    SECTION("invalid union bounds"){input.minimum[0]=2;status=PS::Status::Invalid;}
    REQUIRE(PS::Build(input,expected,cut,kept)==status);
    CHECK(kept.euclideanPixelUpper==77);CHECK(kept.tuple.cut.modelBirth==99);
}
TEST_CASE("Zero discrepancy and uncertain underflow overflow arithmetic do not invent projection confidence", "[geometry-page-projected-surface-bound]")
{
    auto input=ProjectionInput();input.surfaceDistanceUpper=0;input.binding.fineToCoarseUpperBits=0;
    input.binding.coarseToFineUpperBits=0;input.binding.hausdorffUpperBits=0;PS::Result result;
    REQUIRE(PS::Build(input,input.binding,input.cut,result)==PS::Status::Bounded);
    CHECK(result.pixelXUpper==0);CHECK(result.pixelYUpper==0);CHECK(result.euclideanPixelUpper==0);
    double value=1;
    CHECK_FALSE(PS::Detail::Mul(1e-200,1e-200,true,value));
    CHECK_FALSE(PS::Detail::Mul(std::numeric_limits<double>::max(),2,true,value));
    CHECK_FALSE(PS::Detail::Div(1e-200,1e200,true,value));
    CHECK_FALSE(PS::Detail::Rounded(std::numeric_limits<double>::min(),false,false,value));
    CHECK(PS::Detail::Add(1,-1,true,value));CHECK(value==0);
    CHECK(PS::Detail::Mul(0,std::numeric_limits<double>::max(),true,value));CHECK(value==0);
}

TEST_CASE("Projected model translation has a separate bounded world-unit domain", "[geometry-page-projected-surface-bound]") {
    auto input=ProjectionInput();PS::Result tenKm,hundredKm;
    input.minimum={-10,-10,-10};input.maximum={10,10,10};input.surfaceDistanceUpper=4;
    input.binding.fineToCoarseUpperBits=std::bit_cast<uint64_t>(4.);
    input.binding.coarseToFineUpperBits=std::bit_cast<uint64_t>(4.);
    input.binding.hausdorffUpperBits=std::bit_cast<uint64_t>(4.);
    // A valid narrow projection can still have >1 ideal pair discrepancy at10km.
    // This mathematical example is not the captured runtime camera packet.
    input.projection[0]=10;input.projection[5]=14;input.model[14]=10000;
    REQUIRE(PS::Build(input,input.binding,input.cut,tenKm)==PS::Status::Bounded);
    REQUIRE(tenKm.euclideanPixelUpper>1);IndependentPairs(input,tenKm);
    input.model[14]=100000;
    REQUIRE(PS::Build(input,input.binding,input.cut,hundredKm)==PS::Status::Bounded);
    REQUIRE(hundredKm.depthMinimum>99980);REQUIRE(hundredKm.euclideanPixelUpper<1);
    REQUIRE(hundredKm.euclideanPixelUpper<tenKm.euclideanPixelUpper/9);
    IndependentPairs(input,hundredKm);
    input.model[14]=1000000;PS::Result boundary;
    REQUIRE(PS::Build(input,input.binding,input.cut,boundary)==PS::Status::Bounded);
    IndependentPairs(input,boundary);
    auto bad=input;bad.model[14]=std::nextafter(1000000.f,std::numeric_limits<float>::infinity());
    PS::Result untouched=hundredKm;
    REQUIRE(PS::Build(bad,bad.binding,bad.cut,untouched)==PS::Status::Unsupported);
    CHECK(untouched.euclideanPixelUpper==hundredKm.euclideanPixelUpper);CHECK(untouched.tuple.model==hundredKm.tuple.model);
    for(unsigned axis=12;axis<=14;++axis){
        for(float value:{1000001.f,-1000001.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),std::numeric_limits<float>::denorm_min()}){
            bad=input;bad.model[axis]=value;
            REQUIRE(PS::Build(bad,bad.binding,bad.cut,untouched)==PS::Status::Unsupported);
            CHECK(untouched.tuple.model==hundredKm.tuple.model);
        }
    }
}
TEST_CASE("Large model translation does not widen basis view projection or geometry eligibility", "[geometry-page-projected-surface-bound]") {
    auto input=ProjectionInput();input.model[14]=100000;PS::Result result;
    REQUIRE(PS::Build(input,input.binding,input.cut,result)==PS::Status::Bounded);
    auto bad=input;bad.model[0]=10001;
    REQUIRE(PS::Build(bad,bad.binding,bad.cut,result)==PS::Status::Unsupported);
    bad=input;bad.view[0]=10001;
    REQUIRE(PS::Build(bad,bad.binding,bad.cut,result)==PS::Status::Unsupported);
    bad=input;bad.view[12]=1;
    REQUIRE(PS::Build(bad,bad.binding,bad.cut,result)==PS::Status::Unsupported);
    bad=input;bad.view[14]=100000;
    REQUIRE(PS::Build(bad,bad.binding,bad.cut,result)==PS::Status::Unsupported);
    bad=input;bad.projection[0]=10001;
    REQUIRE(PS::Build(bad,bad.binding,bad.cut,result)==PS::Status::Unsupported);
    bad=input;bad.maximum[0]=10001;
    REQUIRE(PS::Build(bad,bad.binding,bad.cut,result)==PS::Status::Invalid);
    bad=input;bad.model[3]=1;
    REQUIRE(PS::Build(bad,bad.binding,bad.cut,result)==PS::Status::Unsupported);
}
