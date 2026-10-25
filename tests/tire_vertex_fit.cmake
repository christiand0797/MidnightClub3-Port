# Actual assembler + CPU recording render thread; no game/GPU launch or media worker.
if(WIN32)
    foreach(path raw converted)
        add_executable(tire_vertex_fit_${path}_test
            "${CMAKE_CURRENT_LIST_DIR}/tire_vertex_fit_test.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/../runtime/gpu/vertex.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/../runtime/gpu/render_thread.cpp")
        target_include_directories(tire_vertex_fit_${path}_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../runtime")
        target_compile_features(tire_vertex_fit_${path}_test PRIVATE cxx_std_20)
        if(path STREQUAL "raw")
            target_compile_definitions(tire_vertex_fit_${path}_test PRIVATE XBE_RAW_VERTICES=1)
        else()
            target_compile_definitions(tire_vertex_fit_${path}_test PRIVATE XBE_RAW_VERTICES=0)
        endif()
        if(MSVC)
            target_compile_options(tire_vertex_fit_${path}_test PRIVATE /O2 /UNDEBUG)
        else()
            target_compile_options(tire_vertex_fit_${path}_test PRIVATE -O2 -UNDEBUG -march=x86-64-v2)
        endif()
        add_test(NAME tire_vertex_fit_${path} COMMAND tire_vertex_fit_${path}_test)
    endforeach()
endif()
