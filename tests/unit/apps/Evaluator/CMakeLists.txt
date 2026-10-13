set(EVALUATOR_TEST_SOURCES
    test_stubs.cpp
    test_expressions.cpp
    test_stubs_output.cpp
    test_format.cpp
    test_errors.cpp
    test_mock_objects.cpp
    test_sqs_runner.cpp
    test_sqs_integration.cpp
    test_loop_exit.cpp
    test_validate.cpp
    test_integration.cpp
    test_host_api.cpp
    test_parity_fixtures.cpp
)

add_executable(PoseidonEvaluatorTests ${EVALUATOR_TEST_SOURCES})

target_link_libraries(PoseidonEvaluatorTests PRIVATE
    Poseidon
    Catch2::Catch2
)

target_include_directories(PoseidonEvaluatorTests PRIVATE
    ${CMAKE_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}
)

set_target_properties(PoseidonEvaluatorTests PROPERTIES
    OUTPUT_NAME "PoseidonEvaluatorTests"
)

include(${CMAKE_SOURCE_DIR}/cmake/CatchWindowsSafe.cmake)
ofpr_catch_discover_tests(PoseidonEvaluatorTests
    PROPERTIES
        ENVIRONMENT "UBSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/ubsan-suppressions.txt;LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/lsan-suppressions.txt"
)

# Copy test fixtures next to the test binary so tests are independent of CWD
add_custom_command(TARGET PoseidonEvaluatorTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/tests/fixtures/evaluator
        $<TARGET_FILE_DIR:PoseidonEvaluatorTests>/fixtures
    COMMENT "Copying Evaluator test fixtures"
)
