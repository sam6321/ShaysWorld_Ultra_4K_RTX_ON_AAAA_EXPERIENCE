#include "mesh_gen.h"

namespace bake {
namespace {

void pushQuad(Mesh& mesh,
              float x0, float y0, float z0,
              float x1, float y1, float z1,
              float x2, float y2, float z2,
              float x3, float y3, float z3,
              float u0, float v0, float u1, float v1, float u2, float v2, float u3, float v3,
              float nx, float ny, float nz) {
  const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
  mesh.vertices.push_back({x0, y0, z0, nx, ny, nz, u0, v0});
  mesh.vertices.push_back({x1, y1, z1, nx, ny, nz, u1, v1});
  mesh.vertices.push_back({x2, y2, z2, nx, ny, nz, u2, v2});
  mesh.vertices.push_back({x3, y3, z3, nx, ny, nz, u3, v3});
  // Two triangles (same winding as Shay's GL_QUADS path).
  mesh.indices.push_back(base + 0);
  mesh.indices.push_back(base + 1);
  mesh.indices.push_back(base + 2);
  mesh.indices.push_back(base + 0);
  mesh.indices.push_back(base + 2);
  mesh.indices.push_back(base + 3);
}

void createXtoZ(Mesh& mesh, float xImgSize, float zImgSize, float xStart, float yStart,
                float zStart, float xTimes, float zTimes) {
  const float x1 = xStart;
  const float z1 = zStart;
  const float x2 = xStart + (xImgSize * xTimes);
  const float z2 = zStart + (zImgSize * zTimes);
  pushQuad(mesh,
           x1, yStart, z1,
           x1, yStart, z2,
           x2, yStart, z2,
           x2, yStart, z1,
           0.0f, 0.0f, 0.0f, zTimes, xTimes, zTimes, xTimes, 0.0f,
           0.0f, 1.0f, 0.0f);
}

void createXtoY(Mesh& mesh, float xImgSize, float yImgSize, float xStart, float yStart,
                float zStart, float xTimes, float yTimes, bool flip) {
  float flipX = 0.0f;
  float tempX = xTimes;
  if (flip) {
    flipX = xTimes;
    tempX = 0.0f;
  }
  const float x2 = xStart + (xImgSize * xTimes);
  const float y2 = yStart + (yImgSize * yTimes);
  pushQuad(mesh,
           xStart, yStart, zStart,
           xStart, y2, zStart,
           x2, y2, zStart,
           x2, yStart, zStart,
           flipX, 0.0f, flipX, yTimes, tempX, yTimes, tempX, 0.0f,
           0.0f, 0.0f, 1.0f);
}

void createYtoZ(Mesh& mesh, float yImgSize, float zImgSize, float xStart, float yStart,
                float zStart, float yTimes, float zTimes, bool flip) {
  float flipZ = 0.0f;
  float tempZ = zTimes;
  if (flip) {
    flipZ = zTimes;
    tempZ = 0.0f;
  }
  const float y2 = yStart + (yImgSize * yTimes);
  const float z2 = zStart + (zImgSize * zTimes);
  pushQuad(mesh,
           xStart, yStart, zStart,
           xStart, yStart, z2,
           xStart, y2, z2,
           xStart, y2, zStart,
           0.0f, flipZ, 0.0f, tempZ, yTimes, tempZ, yTimes, flipZ,
           1.0f, 0.0f, 0.0f);
}

} // namespace

Mesh createAxisAlignedQuad(AxisMode mode, float xImgSize, float zImgSize, float xStart,
                           float yStart, float zStart, float xTimes, float zTimes) {
  Mesh mesh;
  switch (mode) {
  case AxisMode::XY:
    createXtoY(mesh, xImgSize, zImgSize, xStart, yStart, zStart, xTimes, zTimes, false);
    break;
  case AxisMode::XZ:
    createXtoZ(mesh, xImgSize, zImgSize, xStart, yStart, zStart, xTimes, zTimes);
    break;
  case AxisMode::YZ:
    createYtoZ(mesh, xImgSize, zImgSize, xStart, yStart, zStart, xTimes, zTimes, false);
    break;
  case AxisMode::YZ_Flip:
    createYtoZ(mesh, xImgSize, zImgSize, xStart, yStart, zStart, xTimes, zTimes, true);
    break;
  case AxisMode::XY_Flip:
    createXtoY(mesh, xImgSize, zImgSize, xStart, yStart, zStart, xTimes, zTimes, true);
    break;
  }
  return mesh;
}

void applyWorldScale(Mesh& mesh, float scale) {
  for (auto& v : mesh.vertices) {
    v.px *= scale;
    v.py *= scale;
    v.pz *= scale;
  }
}

void applyWorldScale(std::vector<Mesh>& meshes, float scale) {
  for (auto& m : meshes) {
    applyWorldScale(m, scale);
  }
}

} // namespace bake
