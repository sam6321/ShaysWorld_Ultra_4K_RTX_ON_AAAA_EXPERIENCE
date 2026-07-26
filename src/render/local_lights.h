#pragma once

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>

// OG DisplayLights fittings (WORLD_SCALE 0.01).
inline constexpr uint32_t kWalkwayLampCount = 12;
inline constexpr uint32_t kShadowedLampSlots = 8;

struct LocalLight {
  glm::vec3 position{0.0f};
  float range = 35.0f;
  glm::vec3 color{1.0f, 0.82f, 0.55f};
  float intensity = 18.0f;
  glm::vec3 direction{0.0f, -1.0f, 0.0f}; // spot axis
  float cosOuter = 0.55f;                   // cos(half-angle)
  float cosInner = 0.78f;
  float pad0 = 0.0f;
  float pad1 = 0.0f;
  float pad2 = 0.0f;
};

inline std::array<LocalLight, kWalkwayLampCount> makeWalkwayLamps() {
  std::array<LocalLight, kWalkwayLampCount> lights{};
  const float xs = 329.05f;
  // One emitter per fitting height, but keep intensity modest — three cones
  // stack on the same tiles and used to blow the HDR to white.
  const float ys[3] = {112.00f, 112.65f, 113.30f};
  const float zs[4] = {111.34f, 196.26f, 281.18f, 366.10f};
  uint32_t i = 0;
  for (float z : zs) {
    for (float y : ys) {
      LocalLight& L = lights[i++];
      // Origin well below the fitting so the cylinder stays out of the shadow frustum.
      L.position = {xs, y - 1.85f, z};
      L.direction = {0.0f, -1.0f, 0.0f};
      L.range = 36.0f;
      L.color = {1.0f, 0.78f, 0.48f};
      L.intensity = 3.2f;
      // Soft pool (~55° half-angle outer) — enough to light the path, not the grass.
      L.cosOuter = 0.57f;
      L.cosInner = 0.78f;
    }
  }
  return lights;
}

// GPU packing: 12 lights * 3 vec4 = 12*48 = 576 bytes (+ header)
struct GpuLocalLight {
  glm::vec4 posRange;    // xyz pos, w range
  glm::vec4 colorInt;    // rgb color, w intensity
  glm::vec4 dirCone;     // xyz dir, w = pack cosInner in high, use params for outer
};
