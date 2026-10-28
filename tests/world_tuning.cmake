add_executable(world_tuning_test "${CMAKE_CURRENT_LIST_DIR}/world_tuning_test.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/world_tuning_fixture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../templates/game/src/game/world_tuning.cpp")
target_include_directories(world_tuning_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../runtime"
    "${CMAKE_CURRENT_LIST_DIR}/../templates/game/src/game" "${CMAKE_CURRENT_LIST_DIR}/../templates/game/generated/game")
target_compile_features(world_tuning_test PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(world_tuning_test PRIVATE /O2 /UNDEBUG)
else()
    target_compile_options(world_tuning_test PRIVATE -O2 -UNDEBUG)
endif()
add_test(NAME world_tuning COMMAND world_tuning_test)
