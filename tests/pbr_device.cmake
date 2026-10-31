if(WIN32)
    find_path(MC3_PBR_DEVICE_TOML_INCLUDE toml++/toml.hpp HINTS
        "${CMAKE_CURRENT_LIST_DIR}/../templates/game/out/native/_deps/tomlplusplus-src/include"
        "${CMAKE_CURRENT_LIST_DIR}/../templates/game/out/build/release/_deps/tomlplusplus-src/include")
    if(NOT MC3_PBR_DEVICE_TOML_INCLUDE)
        message(FATAL_ERROR "PBR device CPU tests require the pinned offline tomlplusplus headers")
    endif()
    add_executable(pbr_device_test
        "${CMAKE_CURRENT_LIST_DIR}/pbr_device_test.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../runtime/gpu/pbr_device.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../runtime/gpu/shader_cache.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../runtime/gpu/pbr_pipeline.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../runtime/gpu/pbr_geometry.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../runtime/gpu/pbr_material.cpp")
    target_include_directories(pbr_device_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../runtime" "${MC3_PBR_DEVICE_TOML_INCLUDE}")
    target_compile_features(pbr_device_test PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(pbr_device_test PRIVATE /O2 /UNDEBUG /W4)
    else()
        target_compile_options(pbr_device_test PRIVATE -O2 -UNDEBUG -Wall -Wextra)
    endif()
    target_link_libraries(pbr_device_test PRIVATE d3d12 d3dcompiler bcrypt windowscodecs ole32 uuid)
    add_test(NAME pbr_device COMMAND pbr_device_test)
endif()
