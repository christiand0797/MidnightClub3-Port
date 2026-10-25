if(WIN32)
    add_executable(vertex_constants_test "${CMAKE_CURRENT_LIST_DIR}/vertex_constants_test.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/vertex_constants_original_fixture.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../runtime/gpu/vertex.cpp")
    target_include_directories(vertex_constants_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../runtime"
        "${CMAKE_CURRENT_LIST_DIR}/../templates/game/generated/game")
    target_compile_features(vertex_constants_test PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(vertex_constants_test PRIVATE /O2 /UNDEBUG)
    else()
        target_compile_options(vertex_constants_test PRIVATE -O2 -UNDEBUG)
    endif()
    add_test(NAME vertex_constants COMMAND vertex_constants_test)
endif()
