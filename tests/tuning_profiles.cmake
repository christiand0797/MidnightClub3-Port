target_sources(tuning_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/tuning_profile_bank_fixture.cpp")
if(TARGET tuning_refresh_test)
    target_sources(tuning_refresh_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/tuning_profile_bank_fixture.cpp")
endif()
if(WIN32)
    find_path(MC3_PROFILE_TOML_INCLUDE toml++/toml.hpp HINTS
        "${CMAKE_CURRENT_LIST_DIR}/../templates/game/out/native/_deps/tomlplusplus-src/include"
        "${CMAKE_CURRENT_LIST_DIR}/../templates/game/out/build/release/_deps/tomlplusplus-src/include")
    if(MC3_PROFILE_TOML_INCLUDE)
        add_executable(tuning_profiles_store_test "${CMAKE_CURRENT_LIST_DIR}/tuning_profiles_store_test.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/../runtime/app/tuning_profiles.cpp")
        target_include_directories(tuning_profiles_store_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../runtime" "${MC3_PROFILE_TOML_INCLUDE}")
        target_compile_features(tuning_profiles_store_test PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(tuning_profiles_store_test PRIVATE /O2 /UNDEBUG)
        else()
            target_compile_options(tuning_profiles_store_test PRIVATE -O2 -UNDEBUG)
        endif()
        add_test(NAME tuning_profiles_store COMMAND tuning_profiles_store_test)
        add_executable(tuning_profiles_game_test "${CMAKE_CURRENT_LIST_DIR}/tuning_profiles_game_test.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/tuning_profiles_original_fixture.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/../runtime/app/tuning_profiles.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/../templates/game/src/game/tuning_profiles.cpp")
        target_include_directories(tuning_profiles_game_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../runtime"
            "${CMAKE_CURRENT_LIST_DIR}/../templates/game/generated/game" "${MC3_PROFILE_TOML_INCLUDE}")
        target_compile_features(tuning_profiles_game_test PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(tuning_profiles_game_test PRIVATE /O2 /UNDEBUG)
        else()
            target_compile_options(tuning_profiles_game_test PRIVATE -O2 -UNDEBUG)
        endif()
        add_test(NAME tuning_profiles_game COMMAND tuning_profiles_game_test)
    endif()
endif()
