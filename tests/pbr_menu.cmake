if(WIN32)
    find_path(MC3_PBR_IMGUI_INCLUDE imgui.h HINTS
        "${CMAKE_CURRENT_LIST_DIR}/../templates/game/out/native/_deps/imgui-src"
        "${CMAKE_CURRENT_LIST_DIR}/../templates/game/out/build/release/_deps/imgui-src")
    if(MC3_PBR_IMGUI_INCLUDE)
        add_executable(pbr_menu_test "${CMAKE_CURRENT_LIST_DIR}/pbr_menu_test.cpp"
            "${MC3_PBR_IMGUI_INCLUDE}/imgui.cpp" "${MC3_PBR_IMGUI_INCLUDE}/imgui_draw.cpp"
            "${MC3_PBR_IMGUI_INCLUDE}/imgui_tables.cpp" "${MC3_PBR_IMGUI_INCLUDE}/imgui_widgets.cpp")
        target_include_directories(pbr_menu_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../runtime" "${MC3_PBR_IMGUI_INCLUDE}")
        target_compile_features(pbr_menu_test PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(pbr_menu_test PRIVATE /O2 /UNDEBUG)
        else()
            target_compile_options(pbr_menu_test PRIVATE -O2 -UNDEBUG)
        endif()
        target_link_libraries(pbr_menu_test PRIVATE user32 imm32)
        add_test(NAME pbr_menu COMMAND pbr_menu_test)
    endif()
endif()
