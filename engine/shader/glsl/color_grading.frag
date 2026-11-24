#version 310 es

#extension GL_GOOGLE_include_directive : enable

#include "constants.h"

layout(input_attachment_index = 0, set = 0, binding = 0) uniform highp subpassInput in_color;

layout(set = 0, binding = 1) uniform sampler2D color_grading_lut_texture_sampler;

layout(location = 0) out highp vec4 out_color;

void main()
{
    highp ivec2 lut_tex_size = textureSize(color_grading_lut_texture_sampler, 0);
    highp float _COLORS      = float(lut_tex_size.y);

    highp vec4 color = subpassLoad(in_color).rgba;

    // The LUT is a 2D atlas of N×N tiles laid out horizontally: width = N*N, height = N.
    // Each tile represents one blue-channel slice.  Red indexes within a tile horizontally,
    // green indexes vertically, and blue selects which tile to read.
    // The Linear sampler handles bilinear interpolation inside each tile (r and g axes).
    // We manually lerp between the two adjacent blue tiles to get trilinear quality.

    highp float b_scaled = color.b * (_COLORS - 1.0);
    highp float b_floor  = floor(b_scaled);
    highp float b_frac   = b_scaled - b_floor;

    // Half-texel offset so we sample at texel centres and avoid clamping artefacts
    // at the first and last slice.
    highp float r_uv = (color.r * (_COLORS - 1.0) + 0.5) / (_COLORS * _COLORS);
    highp float g_uv = (color.g * (_COLORS - 1.0) + 0.5) / _COLORS;

    highp float tile_width = 1.0 / _COLORS;   // each tile occupies 1/_COLORS of total width

    highp vec2 uv0 = vec2(b_floor * tile_width + r_uv, g_uv);
    highp vec2 uv1 = vec2((b_floor + 1.0) * tile_width + r_uv, g_uv);

    highp vec4 lut_color = mix(texture(color_grading_lut_texture_sampler, uv0),
                               texture(color_grading_lut_texture_sampler, uv1),
                               b_frac);

    out_color = lut_color;
}
