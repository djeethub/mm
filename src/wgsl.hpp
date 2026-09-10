#pragma once

static const char __shader_vert_wgsl[] = R"(
struct TransformBlock {
    u_position : vec2<f32>,
    u_size     : vec2<f32>,
}

@group(0) @binding(0) 
var<uniform> transform : TransformBlock;

struct VertexOutput {
    @builtin(position) position : vec4<f32>,
    @location(0)       outUV    : vec2<f32>,
}

const unit_quad = array<vec2<f32>, 4>(
    vec2<f32>(-0.5, -0.5), // Bottom-Left
    vec2<f32>( 0.5, -0.5), // Bottom-Right
    vec2<f32>(-0.5,  0.5), // Top-Left
    vec2<f32>( 0.5,  0.5)  // Top-Right
);

const uvs = array<vec2<f32>, 4>(
    vec2<f32>(0.0, 1.0),
    vec2<f32>(1.0, 1.0),
    vec2<f32>(0.0, 0.0),
    vec2<f32>(1.0, 0.0)
);

@vertex
fn main(@builtin(vertex_index) vertex_index : u32) -> VertexOutput {
    var output : VertexOutput;
    
    // Scale the unit quad and offset by position
    let finalPosition = (unit_quad[vertex_index] * transform.u_size) + transform.u_position;
    
    output.position = vec4<f32>(finalPosition, 0.0, 1.0);
    output.outUV = uvs[vertex_index];
    
    return output;
}
)";

static const char __yuv_frag_wgsl[] = R"(
struct Uniforms {
    tex_size    : vec2<f32>,
    color_range : i32, // 2 = jpeg, 1 = mpeg
    colorspace  : i32, // 2 = BT.601, 1 = BT.709, 9/10 = BT.2020
}

// Uniform binding
@group(3) @binding(0) var<uniform> uf : Uniforms;

// Textures & Samplers (Split from sampler2D)
@group(2) @binding(0) var u_tex_y : texture_2d<f32>;
@group(2) @binding(1) var u_tex_u : texture_2d<f32>;
@group(2) @binding(2) var u_tex_v : texture_2d<f32>;
@group(2) @binding(3) var u_sampler : sampler;

struct FragmentInput {
    @location(0) v_uv : vec2<f32>,
}

// Replaced #include "utils.glsl" YUV to RGB conversion function
fn to_rgb(yuv_in : vec3<f32>, color_range : i32, colorspace : i32) -> vec3<f32> {
    var yuv = yuv_in;

    // Range expansion (MPEG video range vs JPEG full range)
    if (color_range == 1) { // MPEG / Limited Range (16..235 for Y, 16..240 for UV)
        yuv.x = (yuv.x - (16.0 / 255.0)) * (255.0 / 219.0);
        yuv.y = (yuv.y - (128.0 / 255.0)) * (255.0 / 224.0);
        yuv.z = (yuv.z - (128.0 / 255.0)) * (255.0 / 224.0);
    } else { // JPEG / Full Range (0..255)
        yuv.y -= 0.5;
        yuv.z -= 0.5;
    }

    var rgb : vec3<f32>;

    // Matrix conversions based on Colorspace
    if (colorspace == 1) { 
        // BT.709
        rgb.r = yuv.x + 1.5748 * yuv.z;
        rgb.g = yuv.x - 0.1873 * yuv.y - 0.4681 * yuv.z;
        rgb.b = yuv.x + 1.8556 * yuv.y;
    } else if (colorspace == 9 || colorspace == 10) { 
        // BT.2020
        rgb.r = yuv.x + 1.4746 * yuv.z;
        rgb.g = yuv.x - 0.16455 * yuv.y - 0.57135 * yuv.z;
        rgb.b = yuv.x + 1.8814 * yuv.y;
    } else { 
        // BT.601 (Default fallback)
        rgb.r = yuv.x + 1.402 * yuv.z;
        rgb.g = yuv.x - 0.344136 * yuv.y - 0.714136 * yuv.z;
        rgb.b = yuv.x + 1.772 * yuv.y;
    }

    return clamp(rgb, vec3<f32>(0.0), vec3<f32>(1.0));
}

@fragment
fn main(in : FragmentInput) -> @location(0) vec4<f32> {
    var yuv : vec3<f32>;

    // Calculate how many source texels fit inside ONE screen pixel
    let duv_dx = dpdx(in.v_uv);
    let duv_dy = dpdy(in.v_uv);
    let texelsPerPixel = vec2<f32>(length(duv_dx), length(duv_dy)) * uf.tex_size;

    // --- DOWNSCALING PATH (Large image shrunk into small window) ---
    if (texelsPerPixel.x > 1.2 || texelsPerPixel.y > 1.2) {
        // Expand kernel radius based on downscaling factor to aggregate all pixels
        let radius = max(texelsPerPixel * 0.5, vec2<f32>(1.0));
        var totalWeight : f32 = 0.0;
        yuv = vec3<f32>(0.0);

        // Sample across the footprint covering this destination pixel
        for (var y : f32 = -radius.y; y <= radius.y; y += 1.0) {
            for (var x : f32 = -radius.x; x <= radius.x; x += 1.0) {
                let sampleUV = in.v_uv + (vec2<f32>(x, y) / radius) * (duv_dx + duv_dy) * 0.5;

                // Gaussian-like weighting towards pixel center
                let w = exp(-2.0 * (x * x + y * y) / (radius.x * radius.x + radius.y * radius.y + 1e-5));
                
                let sampleY = textureSample(u_tex_y, u_sampler, sampleUV).r;
                let sampleU = textureSample(u_tex_u, u_sampler, sampleUV).r;
                let sampleV = textureSample(u_tex_v, u_sampler, sampleUV).r;

                yuv += vec3<f32>(sampleY, sampleU, sampleV) * w;
                totalWeight += w;
            }
        }
        yuv /= max(totalWeight, 1e-5);
    } else {
        let sampleY = textureSample(u_tex_y, u_sampler, in.v_uv).r;
        let sampleU = textureSample(u_tex_u, u_sampler, in.v_uv).r;
        let sampleV = textureSample(u_tex_v, u_sampler, in.v_uv).r;
        yuv = vec3<f32>(sampleY, sampleU, sampleV);
    }

    let rgb = to_rgb(yuv, uf.color_range, uf.colorspace);
    return vec4<f32>(rgb, 1.0);
}
)";
/*
static const char __nv12_ext_frag_wgsl[] = R"(
struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

struct Uniforms {
    tex_size    : vec2<f32>,
    color_range : i32, // 2 = jpeg, 1 = mpeg
    colorspace  : i32, // 2 = BT.601, 1 = BT.709, 9/10 = BT.2020
}

// Uniform binding
@group(0) @binding(1) var<uniform> uf : Uniforms;

// Textures & Samplers (Split from sampler2D)
@group(1) @binding(0) var myVideoTexture: texture_external;
@group(1) @binding(1) var mySampler: sampler; 

@fragment
fn main(in: VertexOutput) -> @location(0) vec4<f32> {
    return textureSampleBaseClampToEdge(myVideoTexture, mySampler, in.uv);
}
)";
*/
static const char __nv12_ext_frag_wgsl[] = R"(
struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

struct Uniforms {
    tex_size    : vec2<f32>,
    color_range : i32, // 2 = jpeg, 1 = mpeg
    colorspace  : i32, // 2 = BT.601, 1 = BT.709, 9/10 = BT.2020
}

// Uniform binding
@group(0) @binding(1) var<uniform> uf : Uniforms;

// Textures & Samplers (Split from sampler2D)
@group(1) @binding(0) var mySampler: sampler; 
@group(1) @binding(1) var myVideoTexture: texture_external;

@fragment
fn main(in: VertexOutput) -> @location(0) vec4<f32> {
    return textureSampleBaseClampToEdge(myVideoTexture, mySampler, in.uv);
}
)";

static const char __test_frag_wgsl[] = R"(
struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

struct Uniforms {
    tex_size    : vec2<f32>,
    color_range : i32, // 2 = jpeg, 1 = mpeg
    colorspace  : i32, // 2 = BT.601, 1 = BT.709, 9/10 = BT.2020
}

// Uniform binding
@group(0) @binding(1) var<uniform> uf : Uniforms;

// Textures & Samplers (Split from sampler2D)
@group(1) @binding(0) var mySampler: sampler; 
@group(1) @binding(1) var myVideoTexture: texture_external;

@fragment
fn main(in: VertexOutput) -> @location(0) vec4<f32> {
    return textureSampleBaseClampToEdge(myVideoTexture, mySampler, in.uv);
//    return vec4<f32>(in.uv.x, in.uv.y, 0.0, 1.0);
}
)";