#!/bin/sh
# fast3d_check built once for each patch here, broken on purpose, as <out>/fast3d_check_<break>.xex; each patch is
# taken back out after its build, even a failed one. Run in WSL, on a game's build for xsdk that has the check:
#   sh tests/xsdk/breaks/build.sh <the libultraship tree the build compiles> <the build directory> [<out>]
# out is build-xsdk-breaks in this repository by default. Each fails the check it names:
# - shared_block_offset: every texture in a block at its first page; fails textures_share_a_block, and the other
#   checks that draw two textures at once, combiner_mix_both_textures and combiner_two_cycles.
# - stale_texture_size: the block of constants kept while its register is, whatever its values; fails
#   texture_size_constant.
# - depth_clamp_off: the vertex template's depth clamp taken out; fails depth_clamp.
# - decal_ignored: decals drawn as plain draws; fails decal.
# - mirror_swapped: mirror and mirror-once swapped; fails wrap_mirror and wrap_mirror_once.
# - blend_ignored: nothing blends; fails alpha_blend.
# - fog_after_inputs: fog's attribute read after the combiner's inputs; fails fog.
# - framebuffer_texture_wrong: SelectTextureFb takes the framebuffer before the one asked for; fails
#   game_framebuffer.
# - mix_swapped: mix's first two arguments swapped; fails combiner_mix_both_textures.
# - second_cycle_dropped: a two-cycle combiner's second cycle not drawn; fails combiner_two_cycles.
# - one_as_zero: the combiner's 1 item as 0; fails combiner_general.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
tree=$(readlink -f "$1")
build=$(readlink -f "$2")
out=$(readlink -f "${3:-$here/../../../build-xsdk-breaks}")
mkdir -p "$out"
for name in shared_block_offset stale_texture_size depth_clamp_off decal_ignored mirror_swapped blend_ignored \
    fog_after_inputs framebuffer_texture_wrong mix_swapped second_cycle_dropped one_as_zero; do
    # Lines end as git keeps them, LF, in the patch and the files it touches: a Windows checkout's CRLF would refuse
    # one or the other.
    sed 's/\r$//' "$here/$name.patch" > "$out/$name.patch"
    for file in $(sed -n 's#^+++ b/##p' "$out/$name.patch"); do
        sed -i 's/\r$//' "$tree/$file"
    done
    patch -d "$tree" -p1 --forward --quiet --no-backup-if-mismatch < "$out/$name.patch"
    trap 'patch -d "$tree" -p1 -R --quiet --no-backup-if-mismatch < "$out/$name.patch"' EXIT INT TERM
    ninja -C "$build" fast3d_check > "$out/fast3d_check_$name.log" 2>&1
    patch -d "$tree" -p1 -R --quiet --no-backup-if-mismatch < "$out/$name.patch"
    trap - EXIT INT TERM
    # The check's binary directory is libultraship's, wherever the game puts it.
    cp "$(find "$build" "$tree" -name fast3d_check.xex -newer "$here/$name.patch" -print -quit)" \
        "$out/fast3d_check_$name.xex"
done
