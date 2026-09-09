#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

TEST_CASE("WGPU instance translation storage belongs exclusively to the consumer", "[wgpu][instance-queue]")
{
    const auto root = std::filesystem::path(TESTS_ROOT_DIR).parent_path();
    std::ifstream file(root / "engine/WgpuRenderer/EngineWgpu.cpp");
    REQUIRE(file.good());
    std::ostringstream contents;
    contents << file.rdbuf();
    const std::string source = contents.str();
    const auto producerStart = source.find("uint32_t EngineWgpu::EnqueueInstanceAdd(");
    const auto consumerStart = source.find("void EngineWgpu::DrainInstanceOps(");
    const auto consumerEnd = source.find("void EngineWgpu::RunConsumerBlock(", consumerStart);
    REQUIRE(producerStart != std::string::npos);
    REQUIRE(consumerStart != std::string::npos);
    REQUIRE(consumerEnd != std::string::npos);
    REQUIRE(producerStart < consumerStart);
    const auto producer = source.substr(producerStart, consumerStart - producerStart);
    const auto consumer = source.substr(consumerStart, consumerEnd - consumerStart);

    // Source ownership guard, not a substitute for a thread sanitizer run.
    REQUIRE(producer.find("_instSlotOf") == std::string::npos);
    REQUIRE(producer.find("_nextInstanceHandle++") != std::string::npos);
    REQUIRE(consumer.find("_instSlotOf.resize") != std::string::npos);
    REQUIRE(consumer.find("_nextInstanceHandle") == std::string::npos);
    REQUIRE(consumer.find("op.handle >= _instSlotOf.size() || _instSlotOf[op.handle] == InvalidInstanceSlot")
            != std::string::npos);
    const auto invalidation = consumer.find("_instSlotOf[op.handle] = InvalidInstanceSlot;");
    REQUIRE(invalidation < consumer.find("_modelIdOf.find"));
    REQUIRE(consumer.find("_instSlotOf[op.handle] = InvalidInstanceSlot;", invalidation + 1)
            > consumer.find("wgr_instance_remove"));
}
