if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../templates/game/generated/game/x86_recomp.49.cpp")
    add_executable(tire_draw_scope_test "${CMAKE_CURRENT_LIST_DIR}/tire_draw_scope_fixture.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../templates/game/src/game/garage.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../templates/game/src/game/tire_fit.cpp")
    target_include_directories(tire_draw_scope_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../runtime"
        "${CMAKE_CURRENT_LIST_DIR}/../templates/game/generated/game")
    target_compile_features(tire_draw_scope_test PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(tire_draw_scope_test PRIVATE /O2 /UNDEBUG)
    else()
        target_compile_options(tire_draw_scope_test PRIVATE -O2 -UNDEBUG)
    endif()
    add_test(NAME tire_draw_scope COMMAND tire_draw_scope_test)
endif()
