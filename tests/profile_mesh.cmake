# Profile mesh resources must be covered independently of the numeric diameter readers.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/profile_mesh_fixture.cpp" AND
   EXISTS "${CMAKE_CURRENT_LIST_DIR}/../templates/game/generated/game/x86_recomp.26.cpp" AND
   EXISTS "${CMAKE_CURRENT_LIST_DIR}/../templates/game/generated/game/x86_recomp.27.cpp" AND
   EXISTS "${CMAKE_CURRENT_LIST_DIR}/../templates/game/generated/game/x86_recomp.72.cpp")
    add_executable(profile_mesh_test
        "${CMAKE_CURRENT_LIST_DIR}/profile_mesh_fixture.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/garage_profiles_fixture.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../templates/game/src/game/garage.cpp")
    target_include_directories(profile_mesh_test PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../runtime"
        "${CMAKE_CURRENT_LIST_DIR}/../templates/game/generated/game")
    target_compile_features(profile_mesh_test PRIVATE cxx_std_20)
    target_compile_definitions(profile_mesh_test PRIVATE MC3_TEST_PROFILE_MESH_FIXTURE=1 MC3_TEST_REAL_PROFILE_READERS=1)
    if(MSVC)
        target_compile_options(profile_mesh_test PRIVATE /O2 /UNDEBUG)
    else()
        target_compile_options(profile_mesh_test PRIVATE -O2 -UNDEBUG)
    endif()
    add_test(NAME profile_mesh COMMAND profile_mesh_test)
endif()
