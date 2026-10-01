#version 450

layout(push_constant) uniform Push { mat4 mvp; } pc;
layout(set = 0, binding = 3) uniform Tint { vec4 color; } tint;

layout(location = 0) out vec3 vColor;

vec2 positions[3] = vec2[](vec2(0.0, 0.5), vec2(-0.5, -0.5), vec2(0.5, -0.5));

void main() {
    gl_Position = pc.mvp * vec4(positions[gl_VertexIndex], 0.5, 1.0);
    vColor = tint.color.rgb;
}
