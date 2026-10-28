add_executable(economy_test
    "${CMAKE_CURRENT_LIST_DIR}/economy_test.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../templates/game/src/game/economy.cpp")
target_include_directories(economy_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../runtime"
    "${CMAKE_CURRENT_LIST_DIR}/../templates/game/src/game"
    "${CMAKE_CURRENT_LIST_DIR}/../templates/game/generated/game")
target_compile_features(economy_test PRIVATE cxx_std_20)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../templates/game/generated/game/x86_recomp.1.cpp")
    target_sources(economy_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/economy_original_fixture.cpp")
    target_compile_definitions(economy_test PRIVATE MC3_TEST_ORIGINAL_SAVE_NEEDED=1)
endif()
if(MSVC)
    target_compile_options(economy_test PRIVATE /O2 /UNDEBUG)
else()
    target_compile_options(economy_test PRIVATE -O2 -UNDEBUG)
endif()
add_test(NAME economy COMMAND economy_test)
