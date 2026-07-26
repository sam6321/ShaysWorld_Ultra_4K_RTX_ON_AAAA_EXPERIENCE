#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
  mat4 invViewProj;
  vec4 cameraPos;
  vec4 sunDir;
  vec4 sunColor;
  vec4 params; // x = timeHours, y = cloudsOn
} pc;

float hash(vec2 p) {
  return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float hash3(vec3 p) {
  return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453);
}

float noise(vec2 p) {
  vec2 i = floor(p);
  vec2 f = fract(p);
  float a = hash(i);
  float b = hash(i + vec2(1.0, 0.0));
  float c = hash(i + vec2(0.0, 1.0));
  float d = hash(i + vec2(1.0, 1.0));
  vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
  return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float fbm(vec2 p) {
  float v = 0.0;
  float a = 0.5;
  mat2 rot = mat2(0.80, -0.60, 0.60, 0.80);
  for (int i = 0; i < 6; ++i) {
    v += a * noise(p);
    p = rot * p * 2.02;
    a *= 0.5;
  }
  return v;
}

float starField(vec3 rd) {
  float stars = 0.0;
  for (int layer = 0; layer < 3; ++layer) {
    float scale = 48.0 + float(layer) * 36.0;
    vec3 p = rd * scale;
    vec3 cell = floor(p);
    vec3 f = fract(p) - 0.5;
    float rnd = hash3(cell + float(layer) * 19.7);
    // Denser field so dusk/night actually shows stars
    if (rnd > 0.93) {
      vec3 offs = vec3(hash3(cell + 1.1), hash3(cell + 2.3), hash3(cell + 3.7)) - 0.5;
      float d = length(f - offs * 0.28);
      float mag = (rnd - 0.93) / 0.07;
      stars += smoothstep(0.085, 0.0, d) * (0.45 + 0.55 * mag);
    }
  }
  return clamp(stars, 0.0, 2.0);
}

float cloudDensity(vec3 rd, float timeHours) {
  // Stereographic from +Y — continuous over the dome, no horizon divide blow-up.
  float denom = 1.0 + max(rd.y, 0.0);
  vec2 uv = (rd.xz / denom) * 2.2;
  uv += vec2(timeHours * 0.012, timeHours * 0.007);

  vec2 w1 = vec2(fbm(uv * 0.7 + 1.7), fbm(uv * 0.7 + 9.3));
  uv += (w1 - 0.5) * 0.65;

  float low = fbm(uv * 0.85);
  float mid = fbm(uv * 2.4 + w1);
  float hi = fbm(uv * 5.5 - w1 * 0.5);

  float shape = low * 0.6 + mid * 0.3 + hi * 0.1;
  // Very soft coverage — avoid shard-like thresholds
  float dens = smoothstep(0.32, 0.78, shape);
  dens *= mix(0.75, 1.0, smoothstep(0.25, 0.7, mid));
  dens = dens * dens * (3.0 - 2.0 * dens); // smoothstep-ish polish

  dens *= smoothstep(-0.02, 0.12, rd.y);
  dens *= 1.0 - 0.25 * smoothstep(0.7, 1.0, rd.y);
  return clamp(dens, 0.0, 1.0);
}

void main() {
  vec2 ndc = vUV * 2.0 - 1.0;
  vec4 nearH = pc.invViewProj * vec4(ndc, 0.0, 1.0);
  vec4 farH = pc.invViewProj * vec4(ndc, 1.0, 1.0);
  vec3 rd = normalize(farH.xyz / farH.w - nearH.xyz / nearH.w);

  vec3 sun = normalize(-pc.sunDir.xyz);
  float sunHeight = clamp(sun.y, -0.25, 1.0);
  float day = smoothstep(-0.05, 0.15, sunHeight);
  // Stars ramp in through dusk, not only full night
  float starVis = smoothstep(0.35, -0.05, sunHeight);

  float elev = rd.y * 0.5 + 0.5;
  vec3 zenith = mix(vec3(0.01, 0.015, 0.05), vec3(0.15, 0.35, 0.85), day);
  vec3 horizon = mix(vec3(0.03, 0.03, 0.06), vec3(0.65, 0.75, 0.95), day);
  vec3 sunset = vec3(1.0, 0.45, 0.15);
  float sunsetAmt = (1.0 - abs(sunHeight)) * day * smoothstep(0.0, 0.35, 1.0 - abs(rd.y));
  vec3 sky = mix(horizon, zenith, pow(clamp(elev, 0.0, 1.0), 1.15));
  sky = mix(sky, sunset, sunsetAmt * 0.55);

  float sunDot = max(dot(rd, sun), 0.0);
  sky += pc.sunColor.rgb * (pow(sunDot, 400.0) * 2.0 + pow(sunDot, 6.0) * 0.5) * day *
         pc.sunDir.w * 0.12;

  if (starVis > 0.01) {
    sky += vec3(0.9, 0.93, 1.0) * starField(rd) * starVis * 0.85;
  }

  if (pc.params.y > 0.5) {
    float dens = cloudDensity(rd, pc.params.x);
    vec3 cloudDay = vec3(0.94, 0.95, 0.98);
    vec3 cloudNight = vec3(0.08, 0.09, 0.14);
    vec3 cloudCol = mix(cloudNight, cloudDay, day);
    cloudCol = mix(cloudCol, sunset, sunsetAmt * 0.4);
    float lit = pow(sunDot, 3.0);
    cloudCol += pc.sunColor.rgb * lit * dens * day * 0.2;
    float opacity = dens * mix(0.45, 0.75, day);
    sky = mix(sky, cloudCol, opacity);
  }

  outColor = vec4(sky, 1.0);
}
