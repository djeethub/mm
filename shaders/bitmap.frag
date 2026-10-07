#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(set = 0, binding = 0) uniform sampler2D uTexture[];

layout(location = 0) in vec2 inUV;
layout(location = 1) flat in uint inId;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(uTexture[nonuniformEXT(inId)], inUV);
}
