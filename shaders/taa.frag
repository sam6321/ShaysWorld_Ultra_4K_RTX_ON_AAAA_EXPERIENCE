#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D currentColor;
layout(set = 0, binding = 1) uniform sampler2D historyColor;
layout(set = 0, binding = 2) uniform sampler2D depthMap;

layout(push_constant) uniform Push {
  mat4 reprojection; // prevViewProj * inverse(currViewProj)
  vec4 params;       // x=enabled, y=historyValid, z=historyBlend, w=unused
} pc;

vec3 clipAabb(vec3 aabbMin, vec3 aabbMax, vec3 hist) {
  vec3 c = 0.5 * (aabbMin + aabbMax);
  vec3 e = max(0.5 * (aabbMax - aabbMin), vec3(1e-4));
  vec3 o = hist - c;
  vec3 t = e / max(abs(o), vec3(1e-4));
  float m = min(min(t.x, t.y), t.z);
  return (m < 1.0) ? (c + o * m) : hist;
}

void main() {
  vec3 current = texture(currentColor, vUV).rgb;
  if (pc.params.x < 0.5 || pc.params.y < 0.5) {
    outColor = vec4(current, 1.0);
    return;
  }

  float depth = texture(depthMap, vUV).r;
  // Sky is drawn without depth writes — buffer stays cleared at 1.0. Don't reproject it.
  bool sky = depth >= 0.9995;

  vec2 prevUV = vUV;
  bool offscreen = false;
  if (!sky) {
    vec4 clip = vec4(vUV * 2.0 - 1.0, depth, 1.0);
    vec4 prevClip = pc.reprojection * clip;
    float invW = 1.0 / max(abs(prevClip.w), 1e-5);
    prevUV = prevClip.xy * invW * 0.5 + 0.5;
    offscreen = prevUV.x < 0.0 || prevUV.x > 1.0 || prevUV.y < 0.0 || prevUV.y > 1.0 ||
                prevClip.w <= 0.0;
  }

  vec3 history = offscreen ? current
                           : texture(historyColor, clamp(prevUV, vec2(0.001), vec2(0.999))).rgb;

  // Tight RGB neighborhood clamp — rejects ghosts without chroma explosions.
  vec2 texel = 1.0 / vec2(textureSize(currentColor, 0));
  vec3 nearMin = current;
  vec3 nearMax = current;
  for (int y = -1; y <= 1; ++y) {
    for (int x = -1; x <= 1; ++x) {
      if (x == 0 && y == 0) {
        continue;
      }
      vec3 n = texture(currentColor, vUV + vec2(float(x), float(y)) * texel).rgb;
      nearMin = min(nearMin, n);
      nearMax = max(nearMax, n);
    }
  }
  vec3 extent = max(nearMax - nearMin, vec3(0.002));
  history = clipAabb(nearMin - extent * 0.1, nearMax + extent * 0.1, history);

  float blend = pc.params.z;
  if (sky) {
    // Sky is procedural and drawn without depth — UV history smears stars into streaks.
    // Skip temporal accumulation; rely on stable unjittered sky rays instead.
    outColor = vec4(current, 1.0);
    return;
  }
  float speed = length((vUV - prevUV) * vec2(textureSize(currentColor, 0)));
  blend *= exp(-speed * 0.05);
  if (offscreen) {
    blend = 0.0;
  }
  blend = clamp(blend, 0.0, 0.92);

  outColor = vec4(mix(current, history, blend), 1.0);
}
