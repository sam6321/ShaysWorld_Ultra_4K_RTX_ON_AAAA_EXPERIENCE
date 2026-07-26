#version 450
// GTAO-style horizon AO from depth (half-res).
layout(location = 0) in vec2 vUV;
layout(location = 0) out float outAO;

layout(set = 0, binding = 0) uniform sampler2D depthMap;

layout(push_constant) uniform Push {
  mat4 proj;
  mat4 invProj;
  vec4 params; // x=radius, y=thickness, z=intensity, w=unused
} pc;

vec3 viewPos(vec2 uv, float depth) {
  vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
  vec4 view = pc.invProj * clip;
  return view.xyz / view.w;
}

float hash(vec2 p) {
  return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

void main() {
  float depth = texture(depthMap, vUV).r;
  if (depth >= 0.9995) {
    outAO = 1.0;
    return;
  }

  vec3 origin = viewPos(vUV, depth);
  vec2 texel = 1.0 / vec2(textureSize(depthMap, 0));
  vec3 p1 = viewPos(vUV + vec2(texel.x, 0.0), texture(depthMap, vUV + vec2(texel.x, 0.0)).r);
  vec3 p2 = viewPos(vUV + vec2(0.0, texel.y), texture(depthMap, vUV + vec2(0.0, texel.y)).r);
  vec3 normal = normalize(cross(p1 - origin, p2 - origin));
  if (dot(normal, -normalize(origin)) < 0.0) normal = -normal;

  const int DIRECTIONS = 4;
  const int STEPS = 6;
  float radius = pc.params.x;
  float ao = 0.0;
  float rot = hash(vUV) * 6.2831853;

  for (int d = 0; d < DIRECTIONS; ++d) {
    float ang = rot + float(d) * (3.14159265 / float(DIRECTIONS));
    vec2 dir = vec2(cos(ang), sin(ang));
    float horizon = -1.0;
    for (int s = 1; s <= STEPS; ++s) {
      float t = (float(s) + hash(vUV + float(s))) / float(STEPS);
      vec2 uvS = vUV + dir * texel * (radius * 80.0) * t;
      if (any(lessThan(uvS, vec2(0.0))) || any(greaterThan(uvS, vec2(1.0)))) continue;
      float ds = texture(depthMap, uvS).r;
      if (ds >= 0.9995) continue;
      vec3 sampleP = viewPos(uvS, ds);
      vec3 delta = sampleP - origin;
      float dist = length(delta);
      float h = dot(normalize(delta), normal);
      float attn = 1.0 - clamp(dist / (radius * 8.0), 0.0, 1.0);
      horizon = max(horizon, h * attn);
    }
    ao += clamp(1.0 - horizon, 0.0, 1.0);
  }
  ao /= float(DIRECTIONS);
  outAO = pow(clamp(ao, 0.0, 1.0), pc.params.z);
}
