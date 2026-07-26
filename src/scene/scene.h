#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace scene {

struct Vertex {
  float px, py, pz;
  float nx, ny, nz;
  float u, v;
};

struct Submesh {
  uint32_t material = 0;
  uint32_t firstIndex = 0;
  uint32_t indexCount = 0;
};

struct Material {
  std::string name;
  std::string texturePath;
  uint32_t width = 1;
  uint32_t height = 1;
  float metallic = 0.0f;
  float roughness = 0.6f;
  std::vector<uint8_t> rgb; // tightly packed RGB8, may be empty
};

struct Scene {
  float worldScale = 0.01f;
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<Submesh> submeshes;
  std::vector<Material> materials;
};

bool loadScene(const std::string& binPath, const std::string& assetsRoot, Scene& out);

} // namespace scene
