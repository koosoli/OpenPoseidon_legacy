# Deliberately outside the shipping overlay directory. Selecting this recipe
# requires an explicit VCPKG_OVERLAY_PORTS override in a separate build tree.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO erincatto/box3d
    REF 954cf879e717334eb96da9a5c255788fdc9f5f87
    SHA512 73e8da5429a14b99401a7bb2bcb6e781446f3f9c6db7dfd9b9caeb27cebbec1825513c8dfaa41b8cca6da819f7a8afe812090c788f74d56478f0f3d48f563125
    PATCHES msvc-runtime.diff x86-msvc-popcount.diff
)

# The upstream API version still says 0.1.0, although three APIs used by our
# private backend were renamed. Export an exact-revision feature definition;
# never infer these names from the unchanged upstream version number.
file(APPEND "${SOURCE_PATH}/src/CMakeLists.txt"
    "\ntarget_compile_definitions(box3d INTERFACE POSEIDON_BOX3D_PINNED_954CF87=1)\n")

if(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm")
    list(APPEND OPTIONS -DBOX3D_DISABLE_SIMD=ON)
endif()
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS ${OPTIONS}
        -DBOX3D_SAMPLES=OFF
        -DBOX3D_BENCHMARKS=OFF
        -DBOX3D_DOCS=OFF
        -DBOX3D_PROFILE=OFF
        -DBOX3D_VALIDATE=OFF
        -DBOX3D_UNIT_TESTS=OFF
        -DBOX3D_COMPILE_WARNING_AS_ERROR=OFF
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/box3d)
if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/box3d/base.h" "defined(BOX3D_DLL)" "1")
endif()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
