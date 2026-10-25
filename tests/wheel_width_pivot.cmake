# Opt-in until coordinated with the owner of tests/CMakeLists.txt.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../templates/game/generated/game/x86_recomp.14.cpp" AND
   EXISTS "${CMAKE_CURRENT_LIST_DIR}/../templates/game/generated/game/x86_recomp.27.cpp")
    add_executable(wheel_width_pivot_test "${CMAKE_CURRENT_LIST_DIR}/wheel_width_pivot_fixture.cpp")
    target_include_directories(wheel_width_pivot_test PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../runtime" "${CMAKE_CURRENT_LIST_DIR}/../templates/game/generated/game")
    target_compile_features(wheel_width_pivot_test PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(wheel_width_pivot_test PRIVATE /O2 /UNDEBUG)
    else()
        target_compile_options(wheel_width_pivot_test PRIVATE -O2 -UNDEBUG)
    endif()
    add_test(NAME wheel_width_pivot COMMAND wheel_width_pivot_test)
    # Production ApplyRender consumes this original result in tuning_test's ownership/refresh regression.
    if(TARGET tuning_test)
        target_sources(tuning_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/tuning_width_reader_fixture.cpp")
        target_compile_definitions(tuning_test PRIVATE MC3_TEST_REAL_WIDTH_READER=1)
    endif()
endif()
