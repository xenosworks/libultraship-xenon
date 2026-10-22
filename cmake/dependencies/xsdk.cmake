#=================== ImGui ===================
# SDL2 is sdl2-compat on SDL-xenon; xsdk has no OpenGL.
find_package(SDL2 REQUIRED)
target_link_libraries(ImGui PUBLIC SDL2::SDL2)
# ImGui opens links with fork and execvp on a POSIX system, which the console has no way to do.
target_compile_definitions(ImGui PRIVATE IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
