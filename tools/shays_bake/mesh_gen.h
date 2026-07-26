#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bake {

constexpr float kWorldScale = 0.01f; // Shay coords / 100

enum class AxisMode : int {
  XY = 0,
  XZ = 1,
  YZ = 2,
  YZ_Flip = 3,
  XY_Flip = 4,
};

struct Vertex {
  float px, py, pz;
  float nx, ny, nz;
  float u, v;
};

struct Mesh {
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
  int materialId = 0;
};

// Mirrors TexturedPolygons::CreateDisplayList math (no GL).
Mesh createAxisAlignedQuad(AxisMode mode, float xImgSize, float zImgSize, float xStart,
                           float yStart, float zStart, float xTimes, float zTimes);

void applyWorldScale(Mesh& mesh, float scale = kWorldScale);
void applyWorldScale(std::vector<Mesh>& meshes, float scale = kWorldScale);

} // namespace bake
