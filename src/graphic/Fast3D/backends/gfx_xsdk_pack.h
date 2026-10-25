#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A game's shader pack for gfx_xsdk: programs it compiled on the console and kept, built into the game by
// tools/xsdk/fast3d_pack.py so a first run needn't compile them. A program's bytes are its kept file's
// (gfx_xsdk_shader.cpp), its name the file's without .f3dx, and folder the name of the folder kept programs were in,
// which names the templates and compilers they came from (XsdkShaderCacheOpen).
typedef struct {
    const char* name;
    const uint8_t* bytes;
    uint32_t size;
} XsdkShaderPackProgram;

typedef struct {
    const char* folder;
    const XsdkShaderPackProgram* programs;
    uint32_t count;
} XsdkShaderPack;

// The game's pack; a game without one gets gfx_xsdk's empty one.
extern const XsdkShaderPack xsdk_fast3d_pack;

#ifdef __cplusplus
}
#endif
