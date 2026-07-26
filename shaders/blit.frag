#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1) uniform sampler2D hdr;

layout(push_constant) uniform Push {
  // x=ca y=sharpen z=bloom w=unused (rain is world particles now)
  vec4 params;
  vec4 params2;
} pc;

vec3 sampleBloom(vec2 uv) {
  vec2 texel = 1.0 / vec2(textureSize(hdr, 0));
  vec3 acc = vec3(0.0);
  float wsum = 0.0;
  const float thresh = 0.9;
  const float knee = 0.5;
  for (int y = -5; y <= 5; ++y) {
    for (int x = -5; x <= 5; ++x) {
      float dist = length(vec2(float(x), float(y)));
      if (dist > 5.1) continue;
      vec3 c = texture(hdr, uv + vec2(x, y) * texel * 3.25).rgb;
      float lum = max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 1e-4);
      float soft = clamp(lum - thresh + knee, 0.0, 2.0 * knee);
      soft = (soft * soft) / (4.0 * knee + 1e-4);
      float contrib = max(lum - thresh, 0.0) + soft;
      float g = exp(-dist * dist * 0.12);
      acc += c * (contrib / lum) * g;
      wsum += g;
    }
  }
  if (wsum < 1e-5) return vec3(0.0);
  acc /= wsum;
  return acc / (acc + vec3(1.0));
}

vec3 sharpenLuma(vec3 color, vec2 uv, float sharpness) {
  float amount = clamp(sharpness, 0.0, 1.0);
  amount = amount * amount * 2.25;
  if (amount < 1e-4) return color;
  vec2 texel = 1.0 / vec2(textureSize(src, 0));
  vec3 blur =
      (texture(src, uv + vec2(0.0, -texel.y)).rgb + texture(src, uv + vec2(0.0, texel.y)).rgb +
       texture(src, uv + vec2(-texel.x, 0.0)).rgb + texture(src, uv + vec2(texel.x, 0.0)).rgb +
       texture(src, uv + vec2(-texel.x, -texel.y)).rgb +
       texture(src, uv + vec2(texel.x, -texel.y)).rgb +
       texture(src, uv + vec2(-texel.x, texel.y)).rgb +
       texture(src, uv + vec2(texel.x, texel.y)).rgb) *
      0.125;
  float highY = dot(color - blur, vec3(0.299, 0.587, 0.114));
  return clamp(color + vec3(highY) * amount, 0.0, 1.0);
}

vec3 applyCA(vec3 color, vec2 uv, float strength) {
  float s = clamp(strength, 0.0, 1.0);
  if (s < 1e-4) return color;
  vec2 texel = 1.0 / vec2(textureSize(src, 0));
  vec2 centered = uv - 0.5;
  vec2 aspect = vec2(float(textureSize(src, 0).x) / float(textureSize(src, 0).y), 1.0);
  vec2 c = centered * aspect;
  float r = length(c);
  vec2 dir = (r > 1e-5) ? (c / r) : vec2(1.0, 0.0);
  vec2 offset = dir * mix(r, r * r, 0.35) * (s * 18.0) * texel;
  return vec3(texture(src, uv + offset).r, color.g, texture(src, uv - offset).b);
}

void main() {
  vec3 color = texture(src, vUV).rgb;
  float bloomStr = clamp(pc.params.z, 0.0, 1.0);
  if (bloomStr > 0.001) {
    vec3 b = sampleBloom(vUV) * bloomStr;
    color = color + b * (1.15 - color * 0.5);
  }
  if (pc.params.y > 0.001) color = sharpenLuma(color, vUV, pc.params.y);
  if (pc.params.x > 0.001) color = applyCA(color, vUV, pc.params.x);
  outColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
