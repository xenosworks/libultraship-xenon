# fast3d_check, gfx_xsdk's checks on the console (fast3d_check.cpp), linked against libultraship itself, so it calls
# the backend and its combiner decoding as a game does. Built only when asked for, in a game's build for xsdk:
#   ninja -C <build> fast3d_check
# breaks/build.sh builds it broken on purpose.
add_executable(fast3d_check EXCLUDE_FROM_ALL fast3d_check.cpp)
target_link_libraries(fast3d_check PRIVATE libultraship)
target_compile_options(fast3d_check PRIVATE -Wall -Wextra)
xsdk_add_xex(fast3d_check TITLE_ID 0x58534633 NAME "fast3d_check")
