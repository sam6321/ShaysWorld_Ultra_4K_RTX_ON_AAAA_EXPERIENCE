#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdrColor;
layout(set = 0, binding = 1) uniform sampler2D aoMap;
layout(set = 0, binding = 2) uniform sampler2D depthMap;
layout(set = 0, binding = 3) uniform PostMatrices {
  mat4 invViewProj;
  mat4 viewProj;
  vec4 sunDirIntensity; // xyz = sunDir (as in lighting), w = intensity
  vec4 sunColor;
} mats;

layout(push_constant) uniform Push {
  // x=aoOn y=exposure z=fogDensity w=godrayIntensity
  vec4 params;
  // x=fogOn y=godrayOn z=contactOn w=rainSSR
  vec4 params2;
  // xy = sunScreenUV, zw = camera.xz
  vec4 sunScreen;
  // xyz fog color, w = camera.y
  vec4 fogColor;
} pc;

vec3 aces(vec3 x) {
  const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
  return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 worldFromDepth(vec2 uv, float depth) {
  vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
  vec4 w = mats.invViewProj * clip;
  return w.xyz / w.w;
}

float contactShadow(vec2 uv, float depth) {
  if (pc.params2.z < 0.5 || depth >= 0.9995) return 1.0;
  vec2 sunUV = pc.sunScreen.xy;
  vec2 dir = sunUV - uv;
  float len = length(dir);
  if (len < 1e-4) return 1.0;
  dir /= len;
  const int STEPS = 12;
  float maxDist = min(len, 0.08);
  float shadow = 1.0;
  for (int i = 1; i <= STEPS; ++i) {
    float t = float(i) / float(STEPS);
    vec2 uvS = uv + dir * maxDist * t;
    if (any(lessThan(uvS, vec2(0.0))) || any(greaterThan(uvS, vec2(1.0)))) break;
    float dS = texture(depthMap, uvS).r;
    if (dS >= 0.9995) continue;
    float expect = mix(depth, texture(depthMap, sunUV).r, t * 0.35);
    if (dS < depth - 0.0008 * float(i) && dS < expect + 0.002) {
      shadow = mix(0.35, 1.0, t);
      break;
    }
  }
  return shadow;
}

vec3 godRays(vec2 uv) {
  if (pc.params.w < 0.001 || pc.params2.y < 0.5) return vec3(0.0);
  vec2 sunUV = pc.sunScreen.xy;
  if (any(lessThan(sunUV, vec2(-0.2))) || any(greaterThan(sunUV, vec2(1.2))))
    return vec3(0.0);
  vec2 delta = (uv - sunUV) / 16.0;
  float occ = 0.0;
  vec2 coord = uv;
  for (int i = 0; i < 16; ++i) {
    coord -= delta;
    float d = texture(depthMap, clamp(coord, 0.0, 1.0)).r;
    occ += (d >= 0.9990) ? 1.0 : 0.0;
  }
  occ /= 16.0;
  float fall = 1.0 - smoothstep(0.0, 0.85, length(uv - sunUV));
  return vec3(1.0, 0.95, 0.85) * occ * fall * pc.params.w;
}

vec3 distanceFog(vec3 color, vec2 uv, float depth) {
  if (pc.params2.x < 0.5 || pc.params.z < 1e-6) return color;
  if (depth >= 0.9995) {
    return mix(color, pc.fogColor.rgb, 0.15);
  }
  vec3 world = worldFromDepth(uv, depth);
  float dist = length(world - vec3(pc.sunScreen.z, pc.fogColor.w, pc.sunScreen.w));
  float heightFog = exp(-max(world.y - 100.0, 0.0) * 0.008);
  float f = 1.0 - exp(-pc.params.z * dist * heightFog);
  f = clamp(f, 0.0, 0.85);
  return mix(color, pc.fogColor.rgb, f);
}

float hash21(vec2 p) {
  return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float valueNoise(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  float a = hash21(i);
  float b = hash21(i + vec2(1.0, 0.0));
  float c = hash21(i + vec2(0.0, 1.0));
  float d = hash21(i + vec2(1.0, 1.0));
  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float fbm2(vec2 p) {
  float v = 0.0;
  float a = 0.5;
  mat2 m = mat2(0.80, 0.60, -0.60, 0.80);
  for (int i = 0; i < 4; ++i) {
    v += a * valueNoise(p);
    p = m * p * 2.05 + vec2(17.1, 9.2);
    a *= 0.5;
  }
  return v;
}

vec3 ssrPuddles(vec3 color, vec2 uv, float depth) {
  if (pc.params2.w < 0.5 || depth >= 0.9995) return color;

  vec3 world = worldFromDepth(uv, depth);
  vec3 camPos = vec3(pc.sunScreen.z, pc.fogColor.w, pc.sunScreen.w);
  vec3 V = normalize(camPos - world);
  float viewDist = length(camPos - world);

  vec2 texel = 1.0 / vec2(textureSize(depthMap, 0));
  float d1 = texture(depthMap, uv + vec2(texel.x, 0.0)).r;
  float d2 = texture(depthMap, uv + vec2(0.0, texel.y)).r;
  if (d1 >= 0.9995 || d2 >= 0.9995) return color;
  vec3 p1 = worldFromDepth(uv + vec2(texel.x, 0.0), d1);
  vec3 p2 = worldFromDepth(uv + vec2(0.0, texel.y), d2);
  vec3 N = normalize(cross(p1 - world, p2 - world));
  if (dot(N, V) < 0.0) N = -N;
  if (N.y < 0.65) return color;
  N = vec3(0.0, 1.0, 0.0);

  // Organic puddle mask — no screen-space color fetches (SSR was tiling pillars).
  vec2 wp = world.xz * 0.038;
  wp += 0.45 * vec2(fbm2(wp + vec2(2.1, 0.7)), fbm2(wp + vec2(0.3, 4.4)));
  float n = fbm2(wp);
  float edge = fbm2(world.xz * 0.12 + vec2(3.7, 11.2));
  float puddle = smoothstep(0.38, 0.58, n + edge * 0.14);
  puddle *= smoothstep(70.0, 18.0, viewDist);
  if (puddle < 0.05) return color;

  vec3 R = reflect(-V, N);
  float fresnel = pow(1.0 - clamp(dot(N, V), 0.0, 1.0), 2.8);
  fresnel = mix(0.12, 0.82, fresnel);

  // Analytic environment along the reflection ray (sky gradient + sun glint).
  float skyT = clamp(R.y * 0.5 + 0.5, 0.0, 1.0);
  vec3 groundCol = pc.fogColor.rgb * 0.28;
  vec3 skyCol = pc.fogColor.rgb * 1.15;
  vec3 env = mix(groundCol, skyCol, skyT * skyT);

  vec3 Lsun = normalize(-mats.sunDirIntensity.xyz);
  float sunAmt = mats.sunDirIntensity.w;
  if (sunAmt > 0.05 && R.y > -0.05) {
    float spec = pow(max(dot(R, Lsun), 0.0), 96.0);
    env += mats.sunColor.rgb * sunAmt * spec * 1.8;
    // Soft broader lobe so wet patches catch light without mirroring geometry.
    float gloss = pow(max(dot(R, Lsun), 0.0), 12.0);
    env += mats.sunColor.rgb * sunAmt * gloss * 0.12;
  }

  // Tiny high-frequency sparkle from FBM so puddles aren't flat paint.
  float sparkle = pow(fbm2(world.xz * 2.5 + V.xz * 4.0), 4.0);
  env += vec3(0.55, 0.6, 0.65) * sparkle * fresnel * 0.25;

  vec3 wet = mix(color * 0.68, env, fresnel * 0.72);
  return mix(color, wet, puddle * pc.params2.w);
}

void main() {
  float depth = texture(depthMap, vUV).r;
  vec3 hdr = texture(hdrColor, vUV).rgb;

  if (pc.params.x > 0.5) {
    float ao = texture(aoMap, vUV).r;
    hdr *= mix(1.0, ao, 0.8);
  }

  float cs = contactShadow(vUV, depth);
  hdr *= mix(1.0, cs, 0.55);

  hdr = ssrPuddles(hdr, vUV, depth);
  hdr += godRays(vUV);
  hdr = distanceFog(hdr, vUV, depth);

  hdr *= max(pc.params.y, 0.05);
  outColor = vec4(aces(hdr), 1.0);
}
