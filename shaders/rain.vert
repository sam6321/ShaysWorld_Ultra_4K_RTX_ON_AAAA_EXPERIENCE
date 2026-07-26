#version 450

layout(location = 0) in vec2 inCorner;   // quad corner in [-0.5,0.5]
layout(location = 1) in vec4 inParticle; // xyz world position (drop head), w = length 0..1

layout(push_constant) uniform Push {
  mat4 viewProj;
  vec4 camPos;
  vec4 camRight;
  vec4 params;
} pc;

layout(location = 0) out float vFade;
layout(location = 1) out float vAlong;

void main() {
  vec3 world = inParticle.xyz;
  float len = mix(0.4, 1.1, clamp(inParticle.w, 0.0, 1.0));
  float width = 0.022;

  vec3 across = pc.camRight.xyz;
  float acrossLen = length(across);
  across = (acrossLen > 1e-4) ? (across / acrossLen) : vec3(1.0, 0.0, 0.0);

  // Head at particle pos; streak extends upward (rain falls down through the head).
  float along = inCorner.y + 0.5; // 0 at bottom head, 1 at top tail
  vec3 pos = world + across * (inCorner.x * width) + vec3(0.0, along * len, 0.0);

  gl_Position = pc.viewProj * vec4(pos, 1.0);
  vAlong = along;
  float dist = length(world - pc.camPos.xyz);
  vFade = smoothstep(30.0, 5.0, dist);
}
