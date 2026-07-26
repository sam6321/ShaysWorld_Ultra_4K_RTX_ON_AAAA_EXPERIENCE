#version 450
layout(location = 0) in vec3 inPos;

layout(push_constant) uniform Push {
  mat4 lightViewProj;
} pc;

void main() {
  gl_Position = pc.lightViewProj * vec4(inPos, 1.0);
}
