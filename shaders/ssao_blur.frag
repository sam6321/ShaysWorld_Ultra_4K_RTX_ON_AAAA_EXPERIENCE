#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out float outAO;

layout(set = 0, binding = 0) uniform sampler2D aoMap;

void main() {
  vec2 texel = 1.0 / vec2(textureSize(aoMap, 0));
  float result = 0.0;
  for (int x = -2; x <= 2; ++x) {
    for (int y = -2; y <= 2; ++y) {
      result += texture(aoMap, vUV + vec2(float(x), float(y)) * texel).r;
    }
  }
  outAO = result / 25.0;
}
