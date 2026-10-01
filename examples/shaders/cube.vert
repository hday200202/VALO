#version 450

layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 params;        // x = grid size, y = spacing, z = cube half-size
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;

layout(location = 0) out vec3 vColor;

void main() {
    float grid = pc.params.x;
    float i    = float(gl_InstanceIndex);

    float gx = mod(i, grid);
    float gy = mod(floor(i / grid), grid);
    float gz = floor(i / (grid * grid));

    float mid = (grid - 1.0) * 0.5;
    vec3 offset = (vec3(gx, gy, gz) - mid) * pc.params.y;

    gl_Position = pc.viewProj * vec4(inPos * pc.params.z + offset, 1.0);

    // no textures yet, so the uv and the grid cell stand in for one
    vColor = vec3(inUV, 0.5) * (0.4 + 0.6 * vec3(gx, gy, gz) / grid);
}
