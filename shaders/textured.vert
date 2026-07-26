#version 450
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in uint inMat;

layout(set = 1, binding = 0) uniform FrameUBO {
  vec4 cameraPos;
  vec4 sunDir;
  vec4 sunColor;
  vec4 ambientSky;
  vec4 ambientGround;
  vec4 params;
  vec4 params2;
  mat4 lightVP0;
  mat4 lightVP1;
  mat4 lightVP2;
  mat4 lightVP3;
  vec4 cascadeSplits;
  vec4 lights[36];
  mat4 lampVP[8];
  vec4 lampTile[8];
  vec4 lampSlot;
  mat4 viewProj;
} frame;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
layout(location = 3) flat out uint vMat;

void main() {
  vWorldPos = inPos;
  vNormal = inNormal;
  vUV = inUV;
  vMat = inMat;
  gl_Position = frame.viewProj * vec4(inPos, 1.0);
}
