if(WIN32)
    find_path(MC3_PBR_TOML_INCLUDE toml++/toml.hpp HINTS
        "${CMAKE_CURRENT_LIST_DIR}/../templates/game/out/native/_deps/tomlplusplus-src/include"
        "${CMAKE_CURRENT_LIST_DIR}/../templates/game/out/build/release/_deps/tomlplusplus-src/include")
    if(MC3_PBR_TOML_INCLUDE)
        add_executable(pbr_material_test
            "${CMAKE_CURRENT_LIST_DIR}/pbr_material_test.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/../runtime/gpu/pbr_material.cpp")
        target_include_directories(pbr_material_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../runtime" "${MC3_PBR_TOML_INCLUDE}")
        target_compile_features(pbr_material_test PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(pbr_material_test PRIVATE /O2 /UNDEBUG)
        else()
            target_compile_options(pbr_material_test PRIVATE -O2 -UNDEBUG)
        endif()
        target_link_libraries(pbr_material_test PRIVATE bcrypt windowscodecs ole32 d3dcompiler uuid)
        add_test(NAME pbr_material COMMAND pbr_material_test)
    endif()
endif()
