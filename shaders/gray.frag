#version 450

layout(push_constant, std430) uniform pc {
    layout(offset = 16) vec2  u_tex_size;
};

layout(set = 0, binding = 0) uniform sampler2D u_tex_y;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    float r_y;
    // Calculate how many source texels fit inside ONE screen pixel
    vec2 duv_dx = dFdx(v_uv);
    vec2 duv_dy = dFdy(v_uv);
    vec2 texelsPerPixel = vec2(length(duv_dx), length(duv_dy)) * u_tex_size;

    // --- DOWNSCALING PATH (Large image shrunk into small window) ---
    if (texelsPerPixel.x > 1.2 || texelsPerPixel.y > 1.2) {
        // Expand kernel radius based on downscaling factor to aggregate all pixels
        vec2 radius = max(texelsPerPixel * 0.5, vec2(1.0));
        float totalWeight = 0.0;
        r_y = 0.0;
        // Sample across the footprint covering this destination pixel
        for (float y = -radius.y; y <= radius.y; y += 1.0) {
            for (float x = -radius.x; x <= radius.x; x += 1.0) {
                vec2 sampleUV = v_uv + (vec2(x, y) / radius) * (duv_dx + duv_dy) * 0.5;
                
                // Gaussian-like weighting towards pixel center
                float w = exp(-2.0 * (x*x + y*y) / (radius.x * radius.x + radius.y * radius.y + 1e-5));
                r_y += texture(u_tex_y, sampleUV).r * w;
                totalWeight += w;
            }
        }
        r_y /= max(totalWeight, 1e-5);
    } else {
        r_y = texture(u_tex_y, v_uv).r;
    }

    o_color = vec4(r_y, r_y, r_y, 1.0);
}
