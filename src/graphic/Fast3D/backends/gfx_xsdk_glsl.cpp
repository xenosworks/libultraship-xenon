#include "gfx_xsdk_glsl.h"

#include <prism/processor.h>

#include "../interpreter.h"

namespace Fast {
// Raised whenever the C++ below writes different GLSL from the same templates, so programs kept on the disk from before
// aren't read back.
static const char* const GENERATOR_REVISION = "gfx_xsdk_glsl 1";

// gfx_opengl's default.shader.vs, its attributes after the position at locations from 2, which xost takes as TEXCOORD 0
// on (xenos-toolchain/docs/xsc.md, "Interface conventions"), and each texture's coordinates and clamps in one.
static const char* const VERTEX_TEMPLATE =
    R"glsl(@prism(type='vertex', name='Fast3D xsdk vertex shader', version='1.0.0', description='gfx_opengl default.shader.vs for xost', author='Emill & Prism Team')

#version 450

layout(location = 0) in vec4 aVtxPos;
@for(i in 0..2)
    @if(o_textures[i])
layout(location = @{l_tex[i] + 2}) in vec@{n_tex[i]} aTex@{i};
layout(location = @{l_tex[i]}) out vec@{n_tex[i]} vTex@{i};
    @end
@end
@if(o_fog)
layout(location = @{l_fog + 2}) in vec4 aFog;
layout(location = @{l_fog}) out vec4 vFog;
@end
@if(o_grayscale)
layout(location = @{l_gray + 2}) in vec4 aGrayscaleColor;
layout(location = @{l_gray}) out vec4 vGrayscaleColor;
@end
@for(i in 0..o_inputs)
    @if(o_alpha)
layout(location = @{l_input + i + 2}) in vec4 aInput@{i + 1};
layout(location = @{l_input + i}) out vec4 vInput@{i + 1};
    @else
layout(location = @{l_input + i + 2}) in vec3 aInput@{i + 1};
layout(location = @{l_input + i}) out vec3 vInput@{i + 1};
    @end
@end

void main() {
@for(i in 0..2)
    @if(o_textures[i])
    vTex@{i} = aTex@{i};
    @end
@end
@if(o_fog)
    vFog = aFog;
@end
@if(o_grayscale)
    vGrayscaleColor = aGrayscaleColor;
@end
@for(i in 0..o_inputs)
    vInput@{i + 1} = aInput@{i + 1};
@end
    gl_Position = aVtxPos;
}
)glsl";

// gfx_opengl's default.shader.fs. Its functions are macros, as xost takes no calls, and its uniforms are in one block.
static const char* const FRAGMENT_TEMPLATE =
    R"glsl(@prism(type='fragment', name='Fast3D xsdk fragment shader', version='1.0.0', description='gfx_opengl default.shader.fs for xost', author='Emill & Prism Team')

#version 450

@for(i in 0..2)
    @if(o_textures[i])
layout(location = @{l_tex[i]}) in vec@{n_tex[i]} vTex@{i};
    @end
@end
@if(o_fog)
layout(location = @{l_fog}) in vec4 vFog;
@end
@if(o_grayscale)
layout(location = @{l_gray}) in vec4 vGrayscaleColor;
@end
@for(i in 0..o_inputs)
    @if(o_alpha)
layout(location = @{l_input + i}) in vec4 vInput@{i + 1};
    @else
layout(location = @{l_input + i}) in vec3 vInput@{i + 1};
    @end
@end

layout(location = 0) out vec4 vOutColor;

layout(set = 0, binding = 0, std140) uniform Fast3DUniforms {
    // Texture i's width and height, then its mask's.
    vec4 texture_size[2];
    // Whether textures 0 and 1 take three-point filtering, 1 or 0, then frame_count and noise_scale.
    vec4 frame;
};

@if(o_textures[0])
layout(set = 0, binding = 1) uniform sampler2D uTex0;
@end
@if(o_textures[1])
layout(set = 0, binding = 2) uniform sampler2D uTex1;
@end
@if(o_masks[0])
layout(set = 0, binding = 3) uniform sampler2D uTexMask0;
@end
@if(o_masks[1])
layout(set = 0, binding = 4) uniform sampler2D uTexMask1;
@end
@if(o_blend[0])
layout(set = 0, binding = 5) uniform sampler2D uTexBlend0;
@end
@if(o_blend[1])
layout(set = 0, binding = 6) uniform sampler2D uTexBlend1;
@end

#define WRAP(x, low, high) mod((x) - (low), (high) - (low)) + (low)
// xost's sin is close only from -pi to pi (xenos-toolchain/docs/xsc.md, "Approximate builtins").
#define SIN(x) sin((x) - 6.2831853 * floor((x) * 0.15915494 + 0.5))
#define RANDOM(value) fract(SIN(dot(SIN(value), vec3(12.9898, 78.233, 37.719))) * 143758.5453)
#define FILTER_3POINT(result, tex, uv, size) { \
    vec2 fraction = fract((uv) * (size) - vec2(0.5)); \
    vec2 o = fraction - step(1.0, fraction.x + fraction.y); \
    vec4 c0 = texture(tex, (uv) - o / (size)); \
    vec4 c1 = texture(tex, (uv) - vec2(o.x - sign(o.x), o.y) / (size)); \
    vec4 c2 = texture(tex, (uv) - vec2(o.x, o.y - sign(o.y)) / (size)); \
    result = c0 + abs(o.x) * (c1 - c0) + abs(o.y) * (c2 - c0); \
}
@if(o_three_point_filtering)
#define SAMPLE(result, id, tex, uv, size) if (frame[id] > 0.5) FILTER_3POINT(result, tex, uv, size) else { result = texture(tex, uv); }
@else
#define SAMPLE(result, id, tex, uv, size) result = texture(tex, uv);
@end

void main() {
@if(o_frag_coord)
    // Read once, first: read inside an expression the unroller takes, xost refuses it as gl_FragCoord.zw.
    vec2 fragCoord = gl_FragCoord.xy;
@end
@for(i in 0..2)
    @if(o_textures[i])
        @{s = o_clamp[i][0]}
        @{t = o_clamp[i][1]}
    vec2 texSize@{i} = texture_size[@{i}].xy;
        @if(!s && !t)
    vec2 uv@{i} = vTex@{i}.xy;
        @else
            @if(s && t)
    vec2 uv@{i} = min(max(vTex@{i}.xy, 0.5 / texSize@{i}), vTex@{i}.zw);
            @elseif(s)
    vec2 uv@{i} = vec2(min(max(vTex@{i}.x, 0.5 / texSize@{i}.x), vTex@{i}.z), vTex@{i}.y);
            @else
    vec2 uv@{i} = vec2(vTex@{i}.x, min(max(vTex@{i}.y, 0.5 / texSize@{i}.y), vTex@{i}.z));
            @end
        @end
    vec4 texSample@{i};
    SAMPLE(texSample@{i}, @{i}, uTex@{i}, uv@{i}, texSize@{i})
        @if(o_masks[i])
    vec4 maskVal@{i};
    SAMPLE(maskVal@{i}, @{i}, uTexMask@{i}, uv@{i}, texture_size[@{i}].zw)
            @if(o_blend[i])
    vec4 blendVal@{i};
    SAMPLE(blendVal@{i}, @{i}, uTexBlend@{i}, uv@{i}, texSize@{i})
            @else
    vec4 blendVal@{i} = vec4(0.0, 0.0, 0.0, 0.0);
            @end
    vec4 texVal@{i} = mix(texSample@{i}, blendVal@{i}, maskVal@{i}.a);
        @else
    vec4 texVal@{i} = texSample@{i};
        @end
    @end
@end

@if(o_alpha)
    vec4 texel;
@else
    vec3 texel;
@end

@if(o_2cyc)
    @{f_range = 2}
@else
    @{f_range = 1}
@end

@for(c in 0..f_range)
    @if(c == 1)
        @if(o_alpha)
            @if(o_c[c][1][2] == SHADER_COMBINED)
    texel.a = WRAP(texel.a, -1.01, 1.01);
            @else
    texel.a = WRAP(texel.a, -0.51, 1.51);
            @end
        @end

        @if(o_c[c][0][2] == SHADER_COMBINED)
    texel.rgb = WRAP(texel.rgb, -1.01, 1.01);
        @else
    texel.rgb = WRAP(texel.rgb, -0.51, 1.51);
        @end
    @end

    @if(!o_color_alpha_same[c] && o_alpha)
    texel = vec4(@{
        append_formula(o_c[c], o_do_single[c][0],
                       o_do_multiply[c][0], o_do_mix[c][0], false, false, true, c == 0)
        }, @{append_formula(o_c[c], o_do_single[c][1],
                       o_do_multiply[c][1], o_do_mix[c][1], true, true, true, c == 0)
        });
    @else
    texel = @{append_formula(o_c[c], o_do_single[c][0],
                       o_do_multiply[c][0], o_do_mix[c][0], o_alpha, false,
                       o_alpha, c == 0)};
    @end
@end

    texel = WRAP(texel, -0.51, 1.51);
    texel = clamp(texel, 0.0, 1.0);
@if(o_fog)
    @if(o_alpha)
    texel = vec4(mix(texel.rgb, vFog.rgb, vFog.a), texel.a);
    @else
    texel = mix(texel, vFog.rgb, vFog.a);
    @end
@end

@if(o_texture_edge && o_alpha)
    if (!(texel.a > 0.19)) discard;
    texel.a = 1.0;
@end

@if(o_alpha && o_noise)
    texel.a *= floor(clamp(RANDOM(vec3(floor(fragCoord * frame.w), frame.z)) + texel.a, 0.0, 1.0));
@end

@if(o_grayscale)
    float intensity = (texel.r + texel.g + texel.b) / 3.0;
    vec3 new_texel = vGrayscaleColor.rgb * intensity;
    texel.rgb = mix(texel.rgb, new_texel, vGrayscaleColor.a);
@end

@if(o_alpha)
    @if(o_alpha_threshold)
    if (texel.a < 8.0 / 256.0) discard;
    @end
    @if(o_invisible)
    texel.a = 0.0;
    @end
    vec4 color = texel;
@else
    vec4 color = vec4(texel, 1.0);
@end

@if(srgb_mode)
    vec3 higher = vec3(1.055) * pow(color.rgb, vec3(1.0 / 2.4)) - vec3(0.055);
    vOutColor = vec4(mix(higher, color.rgb * vec3(12.92), lessThan(color.rgb, vec3(0.0031308))), color.a);
@else
    vOutColor = color;
@end
}
)glsl";

const char* const XSDK_GLSL_CLEAR_VERTEX = R"glsl(#version 450
layout(location = 0) in vec4 aPos;
void main() {
    gl_Position = aPos;
}
)glsl";

const char* const XSDK_GLSL_CLEAR_FRAGMENT = R"glsl(#version 450
layout(location = 0) out vec4 vOutColor;
void main() {
    vOutColor = vec4(0.0, 0.0, 0.0, 1.0);
}
)glsl";

// gfx_opengl's RAND_NOISE, through the macro above.
#define RAND_NOISE "((RANDOM(vec3(floor(fragCoord * frame.w), frame.z)) + 1.0) / 2.0)"

// gfx_opengl's shader_item_to_str (gfx_opengl.cpp:93-166), with RAND_NOISE above.
static const char* ShaderItemToStr(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha,
                                   bool first_cycle, bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            case SHADER_0:
                return with_alpha ? "vec4(0.0, 0.0, 0.0, 0.0)" : "vec3(0.0, 0.0, 0.0)";
            case SHADER_1:
                return with_alpha ? "vec4(1.0, 1.0, 1.0, 1.0)" : "vec3(1.0, 1.0, 1.0)";
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "vInput1" : "vInput1.rgb";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "vInput2" : "vInput2.rgb";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "vInput3" : "vInput3.rgb";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "vInput4" : "vInput4.rgb";
            case SHADER_TEXEL0:
                return first_cycle ? (with_alpha ? "texVal0" : "texVal0.rgb")
                                   : (with_alpha ? "texVal1" : "texVal1.rgb");
            case SHADER_TEXEL0A:
                return first_cycle
                           ? (hint_single_element ? "texVal0.a"
                                                  : (with_alpha ? "vec4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)"
                                                                : "vec3(texVal0.a, texVal0.a, texVal0.a)"))
                           : (hint_single_element ? "texVal1.a"
                                                  : (with_alpha ? "vec4(texVal1.a, texVal1.a, texVal1.a, texVal1.a)"
                                                                : "vec3(texVal1.a, texVal1.a, texVal1.a)"));
            case SHADER_TEXEL1A:
                return first_cycle
                           ? (hint_single_element ? "texVal1.a"
                                                  : (with_alpha ? "vec4(texVal1.a, texVal1.a, texVal1.a, texVal1.a)"
                                                                : "vec3(texVal1.a, texVal1.a, texVal1.a)"))
                           : (hint_single_element ? "texVal0.a"
                                                  : (with_alpha ? "vec4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)"
                                                                : "vec3(texVal0.a, texVal0.a, texVal0.a)"));
            case SHADER_TEXEL1:
                return first_cycle ? (with_alpha ? "texVal1" : "texVal1.rgb")
                                   : (with_alpha ? "texVal0" : "texVal0.rgb");
            case SHADER_COMBINED:
                return with_alpha ? "texel" : "texel.rgb";
            case SHADER_NOISE:
                return with_alpha ? "vec4(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")"
                                  : "vec3(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")";
        }
    } else {
        switch (item) {
            case SHADER_0:
                return "0.0";
            case SHADER_1:
                return "1.0";
            case SHADER_INPUT_1:
                return "vInput1.a";
            case SHADER_INPUT_2:
                return "vInput2.a";
            case SHADER_INPUT_3:
                return "vInput3.a";
            case SHADER_INPUT_4:
                return "vInput4.a";
            case SHADER_TEXEL0:
                return first_cycle ? "texVal0.a" : "texVal1.a";
            case SHADER_TEXEL0A:
                return first_cycle ? "texVal0.a" : "texVal1.a";
            case SHADER_TEXEL1A:
                return first_cycle ? "texVal1.a" : "texVal0.a";
            case SHADER_TEXEL1:
                return first_cycle ? "texVal1.a" : "texVal0.a";
            case SHADER_COMBINED:
                return "texel.a";
            case SHADER_NOISE:
                return RAND_NOISE;
        }
    }
    return "";
}

// The cycle's item k of its colour or alpha, as c.at(alpha, k) reads it: c is o_c[cycle], 2 by 4 ints.
static int Item(const prism::MTDArray<int>& c, bool alpha, int k) {
    return reinterpret_cast<const int*>(c.ptr)[(alpha ? 4 : 0) + k];
}

static bool GetBool(prism::ContextTypes* value) {
    return std::holds_alternative<int>(*value) && std::get<int>(*value) == 1;
}

// gfx_opengl's append_formula (gfx_opengl.cpp:175-213), which prism calls with its context first.
static prism::ContextTypes* AppendFormula(prism::ContextTypes* _, prism::ContextTypes* a_arg,
                                          prism::ContextTypes* a_single, prism::ContextTypes* a_mult,
                                          prism::ContextTypes* a_mix, prism::ContextTypes* a_with_alpha,
                                          prism::ContextTypes* a_only_alpha, prism::ContextTypes* a_alpha,
                                          prism::ContextTypes* a_first_cycle) {
    auto c = std::get<prism::MTDArray<int>>(*a_arg);
    bool do_single = GetBool(a_single);
    bool do_multiply = GetBool(a_mult);
    bool do_mix = GetBool(a_mix);
    bool with_alpha = GetBool(a_with_alpha);
    bool only_alpha = GetBool(a_only_alpha);
    bool opt_alpha = GetBool(a_alpha);
    bool first_cycle = GetBool(a_first_cycle);
    std::string out = "";
    if (do_single) {
        out += ShaderItemToStr(Item(c, only_alpha, 3), with_alpha, only_alpha, opt_alpha, first_cycle, false);
    } else if (do_multiply) {
        out += ShaderItemToStr(Item(c, only_alpha, 0), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += " * ";
        out += ShaderItemToStr(Item(c, only_alpha, 2), with_alpha, only_alpha, opt_alpha, first_cycle, true);
    } else if (do_mix) {
        out += "mix(";
        out += ShaderItemToStr(Item(c, only_alpha, 1), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += ", ";
        out += ShaderItemToStr(Item(c, only_alpha, 0), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += ", ";
        out += ShaderItemToStr(Item(c, only_alpha, 2), with_alpha, only_alpha, opt_alpha, first_cycle, true);
        out += ")";
    } else {
        out += "(";
        out += ShaderItemToStr(Item(c, only_alpha, 0), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += " - ";
        out += ShaderItemToStr(Item(c, only_alpha, 1), with_alpha, only_alpha, opt_alpha, first_cycle, false);
        out += ") * ";
        out += ShaderItemToStr(Item(c, only_alpha, 2), with_alpha, only_alpha, opt_alpha, first_cycle, true);
        out += " + ";
        out += ShaderItemToStr(Item(c, only_alpha, 3), with_alpha, only_alpha, opt_alpha, first_cycle, false);
    }
    return new prism::ContextTypes{ out };
}

static std::string Expand(const char* source, const prism::ContextItems& context) {
    prism::Processor processor;
    processor.populate(context);
    processor.load(source);
    return processor.process();
}

XsdkGlsl BuildXsdkGlsl(const CCFeatures& features, bool threePointFiltering, bool srgbMode) {
    XsdkGlsl glsl;
    // The interpreter's order (interpreter.cpp:1617-1762): each texture's coordinates and clamps, fog, grayscale, then
    // the combiner's inputs.
    int texLocation[2] = { 0, 0 };
    int texFloats[2] = { 0, 0 };
    uint32_t offset = 4;
    auto add = [&](uint32_t floats) {
        glsl.attributes.push_back({ offset, floats });
        offset += floats;
        return (int)glsl.attributes.size() - 1;
    };
    for (int i = 0; i < 2; i++) {
        if (features.usedTextures[i]) {
            texFloats[i] = 2 + features.clamp[i][0] + features.clamp[i][1];
            texLocation[i] = add(texFloats[i]);
        }
    }
    const int fogLocation = features.opt_fog ? add(4) : 0;
    const int grayLocation = features.opt_grayscale ? add(4) : 0;
    const int inputLocation = (int)glsl.attributes.size();
    for (int i = 0; i < features.numInputs; i++) {
        add(features.opt_alpha ? 4 : 3);
    }
    glsl.vertexFloats = offset;
    bool noiseItem = false;
    for (int i = 0; i < 2 * 2 * 4; i++) {
        noiseItem = noiseItem || (&features.c[0][0][0])[i] == SHADER_NOISE;
    }

    const prism::ContextItems context = {
        { "o_c", M_ARRAY(features.c, int, 2, 2, 4) },
        { "o_alpha", features.opt_alpha },
        { "o_fog", features.opt_fog },
        { "o_texture_edge", features.opt_texture_edge },
        { "o_noise", features.opt_noise },
        { "o_2cyc", features.opt_2cyc },
        { "o_alpha_threshold", features.opt_alpha_threshold },
        { "o_invisible", features.opt_invisible },
        { "o_grayscale", features.opt_grayscale },
        { "o_textures", M_ARRAY(features.usedTextures, bool, 2) },
        { "o_masks", M_ARRAY(features.used_masks, bool, 2) },
        { "o_blend", M_ARRAY(features.used_blend, bool, 2) },
        { "o_clamp", M_ARRAY(features.clamp, bool, 2, 2) },
        { "o_inputs", features.numInputs },
        { "o_do_mix", M_ARRAY(features.do_mix, bool, 2, 2) },
        { "o_do_single", M_ARRAY(features.do_single, bool, 2, 2) },
        { "o_do_multiply", M_ARRAY(features.do_multiply, bool, 2, 2) },
        { "o_color_alpha_same", M_ARRAY(features.color_alpha_same, bool, 2) },
        { "o_three_point_filtering", threePointFiltering },
        { "srgb_mode", srgbMode },
        { "o_frag_coord", noiseItem || (features.opt_alpha && features.opt_noise) },
        { "SHADER_COMBINED", SHADER_COMBINED },
        // Through void (*)(), which any function pointer casts to and from without a warning.
        { "append_formula", reinterpret_cast<InvokeFunc>(reinterpret_cast<void (*)()>(AppendFormula)) },
        { "l_tex", M_ARRAY(texLocation, int, 2) },
        { "n_tex", M_ARRAY(texFloats, int, 2) },
        { "l_fog", fogLocation },
        { "l_gray", grayLocation },
        { "l_input", inputLocation },
    };
    glsl.vertex = Expand(VERTEX_TEMPLATE, context);
    glsl.fragment = Expand(FRAGMENT_TEMPLATE, context);
    return glsl;
}

std::string XsdkGlslTemplates() {
    return std::string(GENERATOR_REVISION) + VERTEX_TEMPLATE + FRAGMENT_TEMPLATE + XSDK_GLSL_CLEAR_VERTEX +
           XSDK_GLSL_CLEAR_FRAGMENT;
}
} // namespace Fast
