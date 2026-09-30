#include <catch2/catch_test_macros.hpp>
#include <Poseidon/IO/Streams/ModelCompressedSourceBirth.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace Poseidon;
namespace
{
struct ColdGate
{
    std::string previous;bool existed=false;
    explicit ColdGate(const char* value="1")
    {
        if(const char* old=std::getenv("WGR_GEOMETRY_PAGE_RETAIL_COLD_SOURCE")){existed=true;previous=old;}
        Set(value);
    }
    static void Set(const char* value)
    {
#ifdef _WIN32
        _putenv_s("WGR_GEOMETRY_PAGE_RETAIL_COLD_SOURCE",value?value:"");
#else
        if(value)setenv("WGR_GEOMETRY_PAGE_RETAIL_COLD_SOURCE",value,1);
        else unsetenv("WGR_GEOMETRY_PAGE_RETAIL_COLD_SOURCE");
#endif
    }
    ~ColdGate(){Set(existed?previous.c_str():nullptr);}
};
std::vector<char> Encoded(std::span<const char> bytes)
{
    QOStream out;SSCompress codec;codec.Encode(out,bytes.data(),long(bytes.size()));
    return {out.str(),out.str()+out.pcount()};
}
struct ColdArchive
{
    std::filesystem::path root,directory,file;QFBank bank;
    ColdArchive(const std::vector<char>& bytes,int codec=CompMagic,bool trailing=false,
        const char* member="skala_new.p3d",const char* archiveDirectory="DTA")
    {
        Foundation::CaptureMainThread();
        static std::atomic<uint32_t> serial{0};
        root=std::filesystem::canonical(std::filesystem::temp_directory_path());
        directory=root/("cold-model-birth-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+
            "-"+std::to_string(++serial));
        REQUIRE(std::filesystem::create_directory(directory));
        REQUIRE(std::filesystem::create_directory(directory/archiveDirectory));
        file=directory/archiveDirectory/"data3d.pbo";
        auto encoded=codec==CompMagic?Encoded(bytes):bytes;if(trailing)encoded.push_back('!');
        std::ofstream out(file,std::ios::binary);REQUIRE(bool(out));
        out.write(member,std::streamsize(std::strlen(member)+1));
        for(int value:{codec,int(bytes.size()),0,0,int(encoded.size())})out.write(reinterpret_cast<const char*>(&value),4);
        const char end=0;const int zero=0;out.write(&end,1);
        for(int i=0;i<5;++i)out.write(reinterpret_cast<const char*>(&zero),4);
        out.write(encoded.data(),std::streamsize(encoded.size()));out.close();REQUIRE(bool(out));
        REQUIRE(bank.open(RString((file.parent_path()/"data3d").string().c_str())));
        bank.SetPrefix("data3d\\");bank.Lock();
    }
    ~ColdArchive()
    {
        if(bank.IsLocked())bank.Unlock();bank.close();
        std::error_code error;const auto resolved=std::filesystem::canonical(directory,error);
        if(error||resolved!=directory||resolved.parent_path()!=root||
           resolved.filename().string().rfind("cold-model-birth-",0)!=0)return;
        std::filesystem::remove(file,error);error.clear();
        std::filesystem::remove(file.parent_path(),error);error.clear();
        std::filesystem::remove(directory,error);
    }
};
}

TEST_CASE("Cold Cprs source birth wraps the exact decoded buffer and is parser-claimed once",
          "[archive][model][compressed-birth]")
{
    ColdGate gate;const std::vector<char> bytes{'O','D','O','L',7,0,0,0,'x'};ColdArchive archive(bytes);
    ModelCompressedSourceBirth::ReadScope scope("DATA3D/skala_new.p3d");
    auto buffer=archive.bank.Read("skala_new.p3d");REQUIRE(buffer);REQUIRE(buffer->GetSize()==bytes.size());
    REQUIRE(std::memcmp(buffer->GetData(),bytes.data(),bytes.size())==0);
#ifdef _WIN32
    auto birth=buffer->GetCompressedModelSourceBirth();REQUIRE(birth);REQUIRE(birth->Valid());
    REQUIRE(birth->decodedBytes==bytes.size());REQUIRE(birth->logicalName=="data3d\\skala_new.p3d");
    REQUIRE(birth->readScopeToken==scope.Token());REQUIRE(birth->KnownCppBytes()<=64*1024);
    REQUIRE(archive.bank.MatchesMountedCompressedMember("skala_new.p3d",*birth->lease));
    REQUIRE_FALSE(buffer->GetArchiveSourceBinding()); // transformed model bytes are not raw PAC bytes
    std::shared_ptr<const ModelCompressedSourceBirth> wrongThread;
    std::thread other([&]{wrongThread=buffer->TakeCompressedModelSourceBirthForParse();});other.join();
    REQUIRE_FALSE(wrongThread);REQUIRE(buffer->TakeCompressedModelSourceBirthForParse()==birth);
    REQUIRE_FALSE(buffer->TakeCompressedModelSourceBirthForParse());
    REQUIRE(buffer->GetCompressedModelSourceBirth()==birth);
    std::vector<char> decoded;std::string hash;REQUIRE(birth->lease->ReadDecoded(decoded,hash));
    REQUIRE(decoded==bytes);REQUIRE(hash==Foundation::Sha256::Of(buffer->GetData(),buffer->GetSize()));
    constexpr char hex[]="0123456789abcdef";
    for(size_t i=0;i<birth->decodedSha256.size();++i) {
        REQUIRE(hex[birth->decodedSha256[i]>>4]==hash[i*2]);
        REQUIRE(hex[birth->decodedSha256[i]&15]==hash[i*2+1]);
    }
#else
    REQUIRE_FALSE(buffer->GetCompressedModelSourceBirth());
#endif
}

TEST_CASE("An earlier unclaimed FileCache buffer cannot become a new cold parser birth",
          "[archive][model][compressed-birth]")
{
    ColdGate gate;ColdArchive archive(std::vector<char>(512,'a'));Ref<IFileBuffer> earlier;
    {
        ModelCompressedSourceBirth::ReadScope first("data3d\\skala_new.p3d");
        earlier=archive.bank.Read("skala_new.p3d");REQUIRE(earlier);
    }
    REQUIRE_FALSE(earlier->TakeCompressedModelSourceBirthForParse());
    ModelCompressedSourceBirth::ReadScope later("data3d\\skala_new.p3d");
    REQUIRE_FALSE(earlier->TakeCompressedModelSourceBirthForParse());
    auto fresh=archive.bank.Read("skala_new.p3d");REQUIRE(fresh);
#ifdef _WIN32
    REQUIRE(earlier->GetCompressedModelSourceBirth());REQUIRE(fresh->GetCompressedModelSourceBirth());
    REQUIRE(earlier->GetCompressedModelSourceBirth()->readScopeToken!=fresh->GetCompressedModelSourceBirth()->readScopeToken);
    REQUIRE(fresh->TakeCompressedModelSourceBirthForParse());
    ColdArchive changed(std::vector<char>(512,'z'));
    REQUIRE_FALSE(changed.bank.MatchesMountedCompressedMember("skala_new.p3d",
        *earlier->GetCompressedModelSourceBirth()->lease));
    auto changedRead=changed.bank.Read("skala_new.p3d");REQUIRE(changedRead);
    REQUIRE(changedRead->GetCompressedModelSourceBirth());
    REQUIRE(changedRead->GetCompressedModelSourceBirth()->decodedSha256!=
        earlier->GetCompressedModelSourceBirth()->decodedSha256);
    REQUIRE(std::memcmp(earlier->GetData(),changedRead->GetData(),512)!=0);
#endif
}

TEST_CASE("Disabled, unscoped, foreign and unsupported reads stay unknown without changing decoded bytes",
          "[archive][model][compressed-birth]")
{
    ColdGate gate;const std::vector<char> bytes(512,'p');ColdArchive compressed(bytes);
    REQUIRE_FALSE(compressed.bank.Read("skala_new.p3d")->GetCompressedModelSourceBirth());
    {
        ColdGate disabled("0");ModelCompressedSourceBirth::ReadScope scope("data3d\\skala_new.p3d");
        REQUIRE_FALSE(scope.Active());REQUIRE_FALSE(compressed.bank.Read("skala_new.p3d")->GetCompressedModelSourceBirth());
    }
    {
        ModelCompressedSourceBirth::ReadScope foreign("data3d\\other.p3d");
        REQUIRE_FALSE(foreign.Active());REQUIRE_FALSE(compressed.bank.Read("skala_new.p3d")->GetCompressedModelSourceBirth());
    }
    ColdArchive raw(bytes,0),tail(bytes,CompMagic,true),oversized(std::vector<char>(128*1024+1,'b'));
    ColdArchive otherProfile(bytes,CompMagic,false,"skala2.p3d");
    ColdArchive wrongArchive(bytes,CompMagic,false,"skala_new.p3d","Other");
    ModelCompressedSourceBirth::ReadScope scope("data3d\\skala_new.p3d");
    for(auto* archive:{&raw,&tail,&oversized}) {
        auto read=archive->bank.Read("skala_new.p3d");REQUIRE(read);REQUIRE_FALSE(read->GetCompressedModelSourceBirth());
    }
    compressed.bank.SetPrefix("other\\");
    REQUIRE_FALSE(compressed.bank.Read("skala_new.p3d")->GetCompressedModelSourceBirth());
    auto crossed=otherProfile.bank.Read("skala2.p3d");REQUIRE(crossed);
    REQUIRE_FALSE(crossed->GetCompressedModelSourceBirth());
    auto wrongMount=wrongArchive.bank.Read("skala_new.p3d");REQUIRE(wrongMount);
    REQUIRE_FALSE(wrongMount->GetCompressedModelSourceBirth());
}

TEST_CASE("Cold compressed birth live metadata cap refuses proof and preserves ordinary IO",
          "[archive][model][compressed-birth]")
{
    ColdGate gate;ColdArchive archive(std::vector<char>(1024,'c'));
    ModelCompressedSourceBirth::ReadScope scope("data3d\\skala_new.p3d");
#ifdef _WIN32
    std::vector<Ref<IFileBuffer>> held;
    for(unsigned i=0;i<ModelCompressedSourceBirth::MaxLive;++i) {
        auto read=archive.bank.Read("skala_new.p3d");REQUIRE(read);REQUIRE(read->GetCompressedModelSourceBirth());
        held.push_back(read);
    }
    auto refused=archive.bank.Read("skala_new.p3d");REQUIRE(refused);REQUIRE(refused->GetSize()==1024);
    REQUIRE_FALSE(refused->GetCompressedModelSourceBirth());held.pop_back();
    auto resumed=archive.bank.Read("skala_new.p3d");REQUIRE(resumed);REQUIRE(resumed->GetCompressedModelSourceBirth());
#endif
}
