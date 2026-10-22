#=================== ImGui ===================
# SDL2 is sdl2-compat on SDL-xenon; xsdk has no OpenGL.
find_package(SDL2 REQUIRED)
target_link_libraries(ImGui PUBLIC SDL2::SDL2)
# ImGui opens links with fork and execvp on a POSIX system, which the console has no way to do.
target_compile_definitions(ImGui PRIVATE IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)

# imgui-xenon's backend for xsdk's GPU layer and controller 1, from its repository, built against this ImGui
# (imgui-xenon:docs/README-xsdk.md).
set(IMGUI_XENON_DIR "" CACHE PATH "imgui-xenon's repository, whose backends/imgui_impl_xsdk.cpp draws ImGui")
if (NOT EXISTS "${IMGUI_XENON_DIR}/backends/imgui_impl_xsdk.cpp")
    message(FATAL_ERROR "IMGUI_XENON_DIR isn't imgui-xenon's repository: '${IMGUI_XENON_DIR}'")
endif()
target_sources(ImGui PRIVATE "${IMGUI_XENON_DIR}/backends/imgui_impl_xsdk.cpp")
target_include_directories(ImGui PUBLIC "${IMGUI_XENON_DIR}/backends")

#=================== gfx_xsdk ===================
option(XSDK_FAST3D_NULL "gfx_xsdk draws nothing, and counts and times what Fast3D asks of it" OFF)
if (NOT XSDK_FAST3D_NULL)
    # glslang, for the programs gfx_xsdk compiles on the console: GLSL to SPIR-V alone, as glslang-xenon's xsdk/
    # builds it.
    set(GLSLANG_XENON_DIR "" CACHE PATH "glslang-xenon's repository")
    if (NOT EXISTS "${GLSLANG_XENON_DIR}/glslang/Public/ShaderLang.h")
        message(FATAL_ERROR "GLSLANG_XENON_DIR isn't glslang-xenon's repository: '${GLSLANG_XENON_DIR}'")
    endif()
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory("${GLSLANG_XENON_DIR}" glslang EXCLUDE_FROM_ALL)
endif()
