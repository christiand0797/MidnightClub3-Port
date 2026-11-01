include_guard(GLOBAL)
# NVIDIA DLSS Super Resolution (NGX SDK 310.9.16, DLSS 4): fetched at configure time, each file checked against its
# SHA-256, and never committed (the SDK license forbids placing it under this repository's open-source license).
# XBE_DLSS=OFF, or a failed download, builds without it: runtime/gpu/dlss.cpp then reports DLSS as unavailable.
option(XBE_DLSS "Build with NVIDIA DLSS Super Resolution (downloads the NGX SDK)" ON)
set(XBE_DLSS_DIR ${CMAKE_BINARY_DIR}/_deps/dlss-310.9.16)
set(XBE_DLSS_FOUND OFF)
if(XBE_DLSS)
    set(base https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16)
    set(files
        "LICENSE.txt|LICENSE.txt|-"
        "include/nvsdk_ngx.h|include/nvsdk_ngx.h|dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848"
        "include/nvsdk_ngx_defs.h|include/nvsdk_ngx_defs.h|ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e"
        "include/nvsdk_ngx_helpers.h|include/nvsdk_ngx_helpers.h|5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471"
        "include/nvsdk_ngx_helpers_d3d.h|include/nvsdk_ngx_helpers_d3d.h|ec75224f36ed6580baaf250fa83620405ad81a1d98c2c436654a0c9cf6a6b8ba"
        "include/nvsdk_ngx_params.h|include/nvsdk_ngx_params.h|943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc"
        "include/nvsdk_ngx_loader.h|include/nvsdk_ngx_loader.h|abdf5f66a692620538357d29fdf78e3e2a677ee8140b7a638962ecb766585e75"
        "include/nvsdk_ngx_helpers_cuda.h|include/nvsdk_ngx_helpers_cuda.h|f2851a76107bdf4fbcb7b261f3c35573c312d8ecb4bc18fb02d9e2cec7705042"
        "lib/Windows_x86_64/x64/nvsdk_ngx_d.lib|lib/nvsdk_ngx_d.lib|bc43df9ec032004fde681d3e3afd01cde239e03a0125b822e88674c84287f99c"
        "lib/Windows_x86_64/rel/nvngx_dlss.dll|lib/nvngx_dlss.dll|d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e")
    set(ok ON)
    foreach(entry IN LISTS files)
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 remote)
        list(GET parts 1 local)
        list(GET parts 2 sha)
        set(dest ${XBE_DLSS_DIR}/${local})
        if(local STREQUAL "LICENSE.txt")
            # the license text is informational: no pinned hash, kept beside the DLL in a release
            if(NOT EXISTS ${dest})
                file(DOWNLOAD ${base}/${remote} ${dest} STATUS st)
            endif()
            continue()
        endif()
        if(EXISTS ${dest})
            file(SHA256 ${dest} have)
            if(have STREQUAL sha)
                continue()
            endif()
        endif()
        message(STATUS "DLSS: downloading ${remote}")
        file(DOWNLOAD ${base}/${remote} ${dest} EXPECTED_HASH SHA256=${sha} STATUS st)
        list(GET st 0 code)
        if(NOT code EQUAL 0)
            message(WARNING "DLSS: ${remote} could not be downloaded (${st}); building without DLSS")
            set(ok OFF)
            break()
        endif()
    endforeach()
    if(ok)
        set(XBE_DLSS_FOUND ON)
        message(STATUS "DLSS: NGX SDK 310.9.16 in ${XBE_DLSS_DIR}")
    endif()
endif()

# DLSS for an executable target: headers, the NGX loader library and nvngx_dlss.dll beside the executable
function(xbe_enable_dlss target)
    if(NOT XBE_DLSS_FOUND)
        return()
    endif()
    target_compile_definitions(${target} PRIVATE XBE_DLSS=1)
    target_include_directories(${target} SYSTEM PRIVATE ${XBE_DLSS_DIR}/include)
    target_link_libraries(${target} PRIVATE ${XBE_DLSS_DIR}/lib/nvsdk_ngx_d.lib)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${XBE_DLSS_DIR}/lib/nvngx_dlss.dll $<TARGET_FILE_DIR:${target}>/nvngx_dlss.dll
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${XBE_DLSS_DIR}/LICENSE.txt $<TARGET_FILE_DIR:${target}>/NVIDIA_DLSS_LICENSE.txt)
endfunction()
