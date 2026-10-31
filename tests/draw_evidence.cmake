add_executable(draw_evidence_test
    "${CMAKE_CURRENT_LIST_DIR}/draw_evidence_test.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../runtime/gpu/draw_evidence.cpp")
target_include_directories(draw_evidence_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../runtime")
target_compile_features(draw_evidence_test PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(draw_evidence_test PRIVATE /O2 /UNDEBUG /W4)
else()
    target_compile_options(draw_evidence_test PRIVATE -O2 -UNDEBUG -Wall -Wextra)
endif()
target_link_libraries(draw_evidence_test PRIVATE bcrypt)
add_test(NAME draw_evidence COMMAND draw_evidence_test)
