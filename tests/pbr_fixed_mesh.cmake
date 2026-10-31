if(WIN32)
    foreach(path raw converted)
        add_executable(pbr_fixed_mesh_${path}_test
            "${CMAKE_CURRENT_LIST_DIR}/pbr_fixed_mesh_test.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/../runtime/gpu/pbr_fixed_mesh.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/../runtime/gpu/pbr_geometry.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/../runtime/gpu/pbr_pipeline.cpp")
        target_include_directories(pbr_fixed_mesh_${path}_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../runtime")
        target_compile_features(pbr_fixed_mesh_${path}_test PRIVATE cxx_std_20)
        if(path STREQUAL "raw")
            target_compile_definitions(pbr_fixed_mesh_${path}_test PRIVATE XBE_RAW_VERTICES=1)
        else()
            target_compile_definitions(pbr_fixed_mesh_${path}_test PRIVATE XBE_RAW_VERTICES=0)
        endif()
        if(MSVC)
            target_compile_options(pbr_fixed_mesh_${path}_test PRIVATE /O2 /UNDEBUG)
        else()
            target_compile_options(pbr_fixed_mesh_${path}_test PRIVATE -O2 -UNDEBUG -Wall -Wextra)
        endif()
        target_link_libraries(pbr_fixed_mesh_${path}_test PRIVATE d3d12)
        add_test(NAME pbr_fixed_mesh_${path} COMMAND pbr_fixed_mesh_${path}_test)
    endforeach()
endif()
