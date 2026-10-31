if(WIN32)
    find_path(MC3_PBR_CONTROLS_TOML_INCLUDE toml++/toml.hpp HINTS
        "${CMAKE_CURRENT_LIST_DIR}/../templates/game/out/native/_deps/tomlplusplus-src/include"
        "${CMAKE_CURRENT_LIST_DIR}/../templates/game/out/build/release/_deps/tomlplusplus-src/include")
    if(MC3_PBR_CONTROLS_TOML_INCLUDE)
        add_executable(pbr_controls_test
            "${CMAKE_CURRENT_LIST_DIR}/pbr_controls_test.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/../runtime/gpu/pbr_controls.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/../runtime/gpu/pbr_material.cpp")
        target_include_directories(pbr_controls_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../runtime" "${MC3_PBR_CONTROLS_TOML_INCLUDE}")
        target_compile_features(pbr_controls_test PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(pbr_controls_test PRIVATE /O2 /UNDEBUG)
        else()
            target_compile_options(pbr_controls_test PRIVATE -O2 -UNDEBUG -Wall -Wextra)
        endif()
        target_link_libraries(pbr_controls_test PRIVATE bcrypt windowscodecs ole32 uuid)
        add_test(NAME pbr_controls COMMAND pbr_controls_test)
    endif()
endif()
