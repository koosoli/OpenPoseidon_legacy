# Explicit opt-in integration: no FetchContent/network or runtime registration.
# Pinned upstream source and licence: thirdparty/meshoptimizer-clod/PIN.json.
if(TARGET openposeidon_meshoptimizer_clod)
    return()
endif()

set(_op_clod_root "${CMAKE_CURRENT_LIST_DIR}/../thirdparty/meshoptimizer-clod")
add_library(openposeidon_meshoptimizer_clod STATIC EXCLUDE_FROM_ALL
    "${_op_clod_root}/clusterlod.cpp"
    "${_op_clod_root}/src/allocator.cpp"
    "${_op_clod_root}/src/clusterizer.cpp"
    "${_op_clod_root}/src/indexgenerator.cpp"
    "${_op_clod_root}/src/meshletutils.cpp"
    "${_op_clod_root}/src/partition.cpp"
    "${_op_clod_root}/src/simplifier.cpp"
    "${_op_clod_root}/src/spatialorder.cpp"
)
add_library(OpenPoseidon::MeshoptimizerClod ALIAS openposeidon_meshoptimizer_clod)
target_include_directories(openposeidon_meshoptimizer_clod SYSTEM PUBLIC
    "${_op_clod_root}/src"
    "${_op_clod_root}/demo"
)
target_compile_features(openposeidon_meshoptimizer_clod PUBLIC cxx_std_11)
unset(_op_clod_root)
