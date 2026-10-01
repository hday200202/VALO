#version 450

/*
    No vertex input: the positions are in the shader, so VtxLayout is empty and
    nothing is bound. The push constant is the only data crossing over.

    Wound counter-clockwise in VALO clip space, which is Y-up: the software backend
    flips at the viewport transform and the Vulkan backend uses a negative-height
    viewport to match. Reverse two of these and the default cullmode discards it.
*/
layout(push_constant) uniform Push {
    mat4 mvp;
} pc;

layout(location = 0) out vec3 vColor;

vec2 positions[3] = vec2[](vec2(0.0, 0.5), vec2(-0.5, -0.5), vec2(0.5, -0.5));
vec3 colors[3]    = vec3[](vec3(1, 0, 0), vec3(0, 1, 0), vec3(0, 0, 1));

void main() {
    gl_Position = pc.mvp * vec4(positions[gl_VertexIndex], 0.5, 1.0);
    vColor = colors[gl_VertexIndex];
}
