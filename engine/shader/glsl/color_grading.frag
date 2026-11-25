#version 310 es

#extension GL_GOOGLE_include_directive : enable

#include "constants.h"

layout(input_attachment_index = 0, set = 0, binding = 0) uniform highp subpassInput in_color;

layout(set = 0, binding = 1) uniform sampler2D color_grading_lut_texture_sampler;

layout(location = 0) out highp vec4 out_color;

// Sample one texel from the LUT atlas using the exact pixel-centre UV.
// We always pass pixel-centre coordinates, so no bilinear bleeding.
highp vec4 sampleLUT(highp float tile_index, highp float r, highp float g)
{
    highp ivec2 lut_size = textureSize(color_grading_lut_texture_sampler, 0);
    highp float W        = float(lut_size.x); // 256
    highp float H        = float(lut_size.y); // 16  = N

    // Pixel-centre of the texel at (r_idx, g_idx) inside the tile
    highp float u = (tile_index * H + r * (H - 1.0) + 0.5) / W;
    highp float v = (              g * (H - 1.0) + 0.5) / H;

    return texture(color_grading_lut_texture_sampler, vec2(u, v));
}

void main()
{
    highp ivec2 lut_size = textureSize(color_grading_lut_texture_sampler, 0);
    highp float N        = float(lut_size.y); // 16

    highp vec4  color = subpassLoad(in_color).rgba;
    highp float r     = clamp(color.r, 0.0, 1.0);
    highp float g     = clamp(color.g, 0.0, 1.0);
    highp float b     = clamp(color.b, 0.0, 1.0);

    // B selects between two adjacent tiles
    highp float b_scaled = b * (N - 1.0);
    highp float b0       = floor(b_scaled);
    highp float b1       = min(b0 + 1.0, N - 1.0);
    highp float b_frac   = b_scaled - b0;

    // R and G are interpolated manually inside each tile
    highp float r_lo = floor(r * (N - 1.0)) / (N - 1.0);
    highp float r_hi = min(r_lo + 1.0 / (N - 1.0), 1.0);
    highp float r_f  = r * (N - 1.0) - floor(r * (N - 1.0));

    highp float g_lo = floor(g * (N - 1.0)) / (N - 1.0);
    highp float g_hi = min(g_lo + 1.0 / (N - 1.0), 1.0);
    highp float g_f  = g * (N - 1.0) - floor(g * (N - 1.0));

    // 8-sample trilinear fetch (2 tiles × 2×2 corners)
    highp vec4 c000 = sampleLUT(b0, r_lo, g_lo);
    highp vec4 c100 = sampleLUT(b0, r_hi, g_lo);
    highp vec4 c010 = sampleLUT(b0, r_lo, g_hi);
    highp vec4 c110 = sampleLUT(b0, r_hi, g_hi);
    highp vec4 c001 = sampleLUT(b1, r_lo, g_lo);
    highp vec4 c101 = sampleLUT(b1, r_hi, g_lo);
    highp vec4 c011 = sampleLUT(b1, r_lo, g_hi);
    highp vec4 c111 = sampleLUT(b1, r_hi, g_hi);

    highp vec4 result = mix(
        mix(mix(c000, c100, r_f), mix(c010, c110, r_f), g_f),
        mix(mix(c001, c101, r_f), mix(c011, c111, r_f), g_f),
        b_frac);

    out_color = result;
}
