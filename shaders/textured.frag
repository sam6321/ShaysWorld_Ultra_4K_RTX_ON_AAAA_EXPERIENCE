#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(location = 3) flat in uint vMat;

layout(set = 0, binding = 0) uniform sampler2D albedoMaps[];
layout(set = 0, binding = 1) readonly buffer MatParams {
  vec4 params[]; // metallic, roughness, emissive, 0
} mats;

layout(set = 1, binding = 0) uniform FrameUBO {
  vec4 cameraPos;
  vec4 sunDir;
  vec4 sunColor;
  vec4 ambientSky;
  vec4 ambientGround;
  vec4 params;   // x=quality y=linearHdr z=csmOn w=localFade
  vec4 params2;  // x=lightCount y=lampShadowOn z=rainWet w=unused
  mat4 lightVP0;
  mat4 lightVP1;
  mat4 lightVP2;
  mat4 lightVP3;
  vec4 cascadeSplits;
  vec4 lights[36]; // 12 lights * 3 vec4 (posRange, colorInt, dirCosOuter)
  mat4 lampVP[8];
  vec4 lampTile[8]; // xy scale, zw offset — tile for shadowed lamp i
  vec4 lampSlot;    // x=shadowedCount
  mat4 viewProj;
} frame;

layout(set = 1, binding = 1) uniform sampler2DArray shadowMap;
layout(set = 1, binding = 2) uniform sampler2D lampShadowAtlas;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

float DistributionGGX(vec3 N, vec3 H, float roughness) {
  float a = roughness * roughness;
  float a2 = a * a;
  float NdotH = max(dot(N, H), 0.0);
  float denom = (NdotH * NdotH * (a2 - 1.0) + 1.0);
  return a2 / (PI * denom * denom);
}
float GeometrySchlickGGX(float NdotV, float roughness) {
  float r = roughness + 1.0;
  float k = (r * r) / 8.0;
  return NdotV / (NdotV * (1.0 - k) + k);
}
float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
  return GeometrySchlickGGX(max(dot(N, V), 0.0), roughness) *
         GeometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}
vec3 FresnelSchlick(float cosTheta, vec3 F0) {
  return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}
vec3 FresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
  return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float sampleCsm(vec3 worldPos, vec3 N, vec3 L, int cascade) {
  mat4 vp = cascade == 0 ? frame.lightVP0 :
            cascade == 1 ? frame.lightVP1 :
            cascade == 2 ? frame.lightVP2 : frame.lightVP3;
  float NdotL = clamp(dot(N, L), 0.0, 1.0);
  vec3 samplePos = worldPos + N * (0.04 + (1.0 - NdotL) * 0.08);
  vec4 lightClip = vp * vec4(samplePos, 1.0);
  vec3 proj = lightClip.xyz / lightClip.w;
  proj.xy = proj.xy * 0.5 + 0.5;
  if (any(lessThan(proj.xy, vec2(0.0))) || any(greaterThan(proj.xy, vec2(1.0))) || proj.z > 1.0)
    return 1.0;
  float bias = 0.0004 + (1.0 - NdotL) * 0.0012;
  float shadow = 0.0;
  vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0).xy);
  for (int x = -2; x <= 2; ++x)
    for (int y = -2; y <= 2; ++y) {
      float d = texture(shadowMap, vec3(proj.xy + vec2(x, y) * texel, float(cascade))).r;
      shadow += (proj.z - bias > d) ? 0.0 : 1.0;
    }
  return shadow / 25.0;
}

float computeCsm(vec3 worldPos, vec3 N, vec3 L, float viewDepth) {
  if (frame.params.z < 0.5) return 1.0;
  int cascade = 3;
  if (viewDepth < frame.cascadeSplits.x) cascade = 0;
  else if (viewDepth < frame.cascadeSplits.y) cascade = 1;
  else if (viewDepth < frame.cascadeSplits.z) cascade = 2;
  float s = sampleCsm(worldPos, N, L, cascade);
  float split = cascade == 0 ? frame.cascadeSplits.x :
                cascade == 1 ? frame.cascadeSplits.y :
                cascade == 2 ? frame.cascadeSplits.z : frame.cascadeSplits.w;
  float blend = smoothstep(split * 0.88, split, viewDepth);
  if (blend > 0.0 && cascade < 3)
    s = mix(s, sampleCsm(worldPos, N, L, cascade + 1), blend);
  return s;
}

float sampleLampShadow(int slot, vec3 worldPos) {
  if (frame.params2.y < 0.5 || slot < 0 || slot >= int(frame.lampSlot.x)) return 1.0;
  vec4 clip = frame.lampVP[slot] * vec4(worldPos, 1.0);
  vec3 proj = clip.xyz / max(clip.w, 1e-4);
  proj.xy = proj.xy * 0.5 + 0.5;
  if (any(lessThan(proj, vec3(0.0))) || any(greaterThan(proj, vec3(1.0)))) return 1.0;
  vec2 uv = proj.xy * frame.lampTile[slot].xy + frame.lampTile[slot].zw;
  float bias = 0.0035;
  float shadow = 0.0;
  vec2 texel = 1.0 / vec2(textureSize(lampShadowAtlas, 0));
  for (int x = -1; x <= 1; ++x)
    for (int y = -1; y <= 1; ++y) {
      float d = texture(lampShadowAtlas, uv + vec2(x, y) * texel).r;
      shadow += (proj.z - bias > d) ? 0.0 : 1.0;
    }
  return shadow / 9.0;
}

vec3 evalLight(vec3 N, vec3 V, vec3 albedo, vec3 F0, float metallic, float roughness,
               vec3 L, vec3 radiance, float atten) {
  float NdotL = max(dot(N, L), 0.0);
  if (NdotL <= 0.0 || atten <= 0.0) return vec3(0.0);
  vec3 H = normalize(V + L);
  float NDF = DistributionGGX(N, H, roughness);
  float G = GeometrySmith(N, V, L, roughness);
  vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
  vec3 spec = (NDF * G * F) / (4.0 * max(dot(N, V), 0.0) * NdotL + 0.0001);
  vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
  return (kD * albedo / PI + spec) * radiance * NdotL * atten;
}

void main() {
  vec3 albedo = texture(albedoMaps[nonuniformEXT(vMat)], vUV).rgb;
  vec4 matP = mats.params[vMat];
  bool quality = frame.params.x > 0.5;

  vec3 N = normalize(vNormal);
  vec3 Lsun = normalize(-frame.sunDir.xyz);
  float NdotL = max(dot(N, Lsun), 0.0);

  // Performance: cheapest path — no shadows, no PBR, no local lights.
  if (!quality) {
    outColor = vec4(albedo * (0.35 + 0.65 * NdotL), 1.0);
    return;
  }

  if (frame.params2.z > 0.01) {
    albedo *= mix(1.0, 0.72, frame.params2.z);
  }
  vec3 V = normalize(frame.cameraPos.xyz - vWorldPos);
  if (dot(N, V) < 0.0) N = -N;

  float metallic = matP.x;
  float roughness = clamp(matP.y, 0.04, 1.0);
  if (frame.params2.z > 0.01) {
    roughness = mix(roughness, 0.12, frame.params2.z * 0.85);
  }

  float viewDepth = length(frame.cameraPos.xyz - vWorldPos);
  float shadow = computeCsm(vWorldPos, N, Lsun, viewDepth);

  vec3 F0 = mix(vec3(0.04), albedo, metallic);
  vec3 Lo = evalLight(N, V, albedo, F0, metallic, roughness, Lsun,
                      frame.sunColor.rgb * frame.sunDir.w, shadow);

  float fade = frame.params.w;
  int count = int(frame.params2.x + 0.5);
  int shadowed = int(frame.lampSlot.x + 0.5);
  for (int i = 0; i < 12; ++i) {
    if (i >= count || fade < 0.001) break;
    vec4 posRange = frame.lights[i * 3 + 0];
    vec4 colorInt = frame.lights[i * 3 + 1];
    vec4 dirCos = frame.lights[i * 3 + 2];
    vec3 toL = posRange.xyz - vWorldPos;
    float dist = length(toL);
    if (dist > posRange.w) continue;
    vec3 L = toL / max(dist, 1e-4);
    float cd = dot(-L, normalize(dirCos.xyz));
    float cosOuter = dirCos.w;
    float cosInner = mix(cosOuter, 1.0, 0.42);
    float spot = smoothstep(cosOuter, cosInner, cd);
    float atten = spot * fade * clamp(1.0 - dist / posRange.w, 0.0, 1.0);
    atten *= atten;
    float ls = 1.0;
    if (i < shadowed) ls = sampleLampShadow(i, vWorldPos + N * 0.05);
    Lo += evalLight(N, V, albedo, F0, metallic, roughness, L,
                    colorInt.rgb * colorInt.w, atten * ls);
  }

  float hemi = N.y * 0.5 + 0.5;
  vec3 irradiance = mix(frame.ambientGround.rgb, frame.ambientSky.rgb, hemi);
  vec3 Famb = FresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
  vec3 ambient = (1.0 - Famb) * (1.0 - metallic) * irradiance * albedo;
  vec3 R = reflect(-V, N);
  ambient += Famb * mix(frame.ambientGround.rgb, frame.ambientSky.rgb, R.y * 0.5 + 0.5) *
             pow(1.0 - roughness, 2.0) * 0.35;

  vec3 color = ambient + Lo;
  color += albedo * matP.z * fade;

  if (frame.params.y < 0.5) color = color / (color + vec3(1.0));
  outColor = vec4(color, 1.0);
}
