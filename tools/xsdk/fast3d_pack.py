#!/usr/bin/env python3
"""Writes a game's shader pack for gfx_xsdk (src/graphic/Fast3D/backends/gfx_xsdk_pack.h): the programs gfx_xsdk
compiled on the console and kept, built into the game as C, so a first run needn't compile them.

    fast3d_pack.py <a copy of the console's game:\\shaders\\fast3d\\<folder>> <the C file to write>

The folder's name, which names the templates and compilers its programs came from, goes into the pack: gfx_xsdk uses
the pack only while the game keeps its programs in a folder of that name. Add the game's C file to its xsdk build.
"""

import sys
from pathlib import Path

# A kept file's first word, "F3DX" (gfx_xsdk_shader.cpp, CACHE_MAGIC).
MAGIC = b"F3DX"
BYTES_A_LINE = 16


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    folder = Path(sys.argv[1])
    output = Path(sys.argv[2])
    files = sorted(path for path in folder.glob("*.f3dx") if path.read_bytes()[:4] == MAGIC)
    if not files:
        sys.exit(f"fast3d_pack: no kept programs in {folder}")
    lines = [
        "// gfx_xsdk's shader pack, written by libultraship-xenon's tools/xsdk/fast3d_pack.py from the programs",
        f"// kept in shaders/fast3d/{folder.name}: {len(files)} of them.",
        '#include "graphic/Fast3D/backends/gfx_xsdk_pack.h"',
        "",
    ]
    for i, path in enumerate(files):
        data = path.read_bytes()
        lines.append(f"static const uint8_t program_{i}[{len(data)}] = {{")
        for at in range(0, len(data), BYTES_A_LINE):
            lines.append("    " + " ".join(f"0x{b:02X}," for b in data[at:at + BYTES_A_LINE]))
        lines.append("};")
    lines.append("")
    lines.append("static const XsdkShaderPackProgram programs[] = {")
    for i, path in enumerate(files):
        lines.append(f'    {{ "{path.stem}", program_{i}, sizeof(program_{i}) }},')
    lines.append("};")
    lines.append("")
    lines.append(f'const XsdkShaderPack xsdk_fast3d_pack = {{ "{folder.name}", programs, {len(files)} }};')
    output.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"fast3d_pack: {len(files)} programs, {sum(p.stat().st_size for p in files)} bytes, for {folder.name}")


main()
