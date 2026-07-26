#version 450

layout(location = 0) in float vFade;
layout(location = 1) in float vAlong;
layout(location = 0) out vec4 outColor;

void main() {
  // Soft head near the bottom of the streak, fade at the tail.
  float body = smoothstep(0.0, 0.15, vAlong) * smoothstep(1.0, 0.35, vAlong);
  float alpha = body * vFade * 0.55;
  if (alpha < 0.01) discard;
  outColor = vec4(0.75, 0.82, 0.9, alpha);
}
