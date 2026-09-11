#version 450

// Texture bindings (Set 1)
layout(set = 1, binding = 0) uniform sampler   u_sampler;
layout(set = 1, binding = 1) uniform texture2D u_tex_y;
layout(set = 1, binding = 2) uniform texture2D u_tex_uv;

// Dawn's injected metadata uniform
layout(set = 1, binding = 3) uniform ExternalTextureParams {
    mat4 colorConversionMatrix;
    vec2 uvMin;
    vec2 uvMax;
    // ... Additional padding managed by Dawn
} u_params;

// Uniforms (Set 0)
layout(std140, set = 0, binding = 1) uniform Uniforms {
    vec2  tex_size;     
    int   color_range;  
    int   colorspace;   
} uf;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    // Clamp UVs to visible rect provided by Dawn (prevents video border artifacting)
    vec2 clampedUV = clamp(v_uv, u_params.uvMin, u_params.uvMax);

    // Sample Y (Luma) and UV (Chroma)
    float y = texture(sampler2D(u_tex_y, u_sampler), clampedUV).r;
    vec2 uv = texture(sampler2D(u_tex_uv, u_sampler), clampedUV).rg;

    // Offset chroma channels by -0.5 (YUV range normalization)
    vec4 yuv = vec4(y, uv.x - 0.5, uv.y - 0.5, 1.0);

    // Transform YUV -> RGB
    vec3 rgb = (u_params.colorConversionMatrix * yuv).rgb;

    // Force Alpha to 1.0 (Matrices often leave alpha as 0.0 or unchanged)
    o_color = vec4(rgb, 1.0);
}