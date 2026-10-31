if(WIN32)
    add_executable(pbr_pipeline_test
        "${CMAKE_CURRENT_LIST_DIR}/pbr_pipeline_test.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../runtime/gpu/pbr_pipeline.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../runtime/gpu/pbr_geometry.cpp")
    target_include_directories(pbr_pipeline_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../runtime")
    target_compile_features(pbr_pipeline_test PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(pbr_pipeline_test PRIVATE /O2 /UNDEBUG /W4)
    else()
        target_compile_options(pbr_pipeline_test PRIVATE -O2 -UNDEBUG -Wall -Wextra)
    endif()
    target_link_libraries(pbr_pipeline_test PRIVATE d3dcompiler d3d12)
    add_test(NAME pbr_pipeline COMMAND pbr_pipeline_test)
endif()
