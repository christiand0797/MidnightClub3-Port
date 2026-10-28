add_executable(pbr_geometry_test
    "${CMAKE_CURRENT_LIST_DIR}/pbr_geometry_test.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../runtime/gpu/pbr_geometry.cpp")
target_include_directories(pbr_geometry_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../runtime")
target_compile_features(pbr_geometry_test PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(pbr_geometry_test PRIVATE /O2 /UNDEBUG)
else()
    target_compile_options(pbr_geometry_test PRIVATE -O2 -UNDEBUG)
endif()
if(WIN32)
    target_link_libraries(pbr_geometry_test PRIVATE d3dcompiler)
endif()
add_test(NAME pbr_geometry COMMAND pbr_geometry_test)
