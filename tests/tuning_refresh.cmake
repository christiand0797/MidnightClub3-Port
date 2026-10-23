# Include from tests/CMakeLists.txt when the generated game is available. This deliberately lives separately
# from tuning_test.cpp so the ownership and refresh regressions can be integrated independently.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../templates/game/generated/game/x86_recomp.72.cpp")
    add_executable(tuning_refresh_test
        "${CMAKE_CURRENT_LIST_DIR}/tuning_refresh_fixture.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../templates/game/src/game/tuning.cpp")
    target_include_directories(tuning_refresh_test PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../runtime"
        "${CMAKE_CURRENT_LIST_DIR}/../templates/game/generated/game")
    target_compile_features(tuning_refresh_test PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(tuning_refresh_test PRIVATE /O2 /UNDEBUG)
    else()
        target_compile_options(tuning_refresh_test PRIVATE -O2 -UNDEBUG)
    endif()
    add_test(NAME tuning_refresh COMMAND tuning_refresh_test)
endif()
