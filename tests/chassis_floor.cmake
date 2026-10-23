# Synthetic shape inputs, actual generated classifier/writer/polygon-rebuild instructions. No game launch.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../templates/game/generated/game/x86_recomp.78.cpp")
    add_executable(chassis_floor_test "${CMAKE_CURRENT_LIST_DIR}/chassis_floor_fixture.cpp")
    target_include_directories(chassis_floor_test PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../runtime"
        "${CMAKE_CURRENT_LIST_DIR}/../templates/game/generated/game")
    target_compile_features(chassis_floor_test PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(chassis_floor_test PRIVATE /O2 /UNDEBUG)
    else()
        target_compile_options(chassis_floor_test PRIVATE -O2 -UNDEBUG)
    endif()
    add_test(NAME chassis_floor COMMAND chassis_floor_test)
endif()
