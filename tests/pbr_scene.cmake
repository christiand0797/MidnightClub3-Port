if(WIN32)
    add_executable(pbr_scene_test
        "${CMAKE_CURRENT_LIST_DIR}/pbr_scene_test.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../runtime/gpu/pbr_scene.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../runtime/gpu/pbr_pipeline.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../runtime/gpu/pbr_geometry.cpp")
    target_include_directories(pbr_scene_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../runtime")
    target_compile_features(pbr_scene_test PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(pbr_scene_test PRIVATE /O2 /UNDEBUG)
    else()
        target_compile_options(pbr_scene_test PRIVATE -O2 -UNDEBUG -Wall -Wextra)
    endif()
    add_test(NAME pbr_scene COMMAND pbr_scene_test)
endif()
