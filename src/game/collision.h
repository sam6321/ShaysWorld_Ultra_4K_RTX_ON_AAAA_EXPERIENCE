#pragma once

#include <glm/vec3.hpp>

#include <cstdint>
#include <string>
#include <vector>

enum class PlainType : uint8_t { Flat = 0, XY = 1, ZY = 2 };

struct Plain {
  PlainType type = PlainType::Flat;
  float minX = 0, maxX = 0;
  float minY = 0, maxY = 0;
  float minZ = 0, maxZ = 0;
};

struct AabbXZ {
  float minX = 0, maxX = 0;
  float minZ = 0, maxZ = 0;
};

struct FootingResult {
  bool onPlain = false;
  float eyeY = 0.0f;
  int plainIndex = -1;
  bool flatHeightChanged = false; // OG footstep trigger
};

class CollisionWorld {
public:
  bool load(const std::string& path);

  [[nodiscard]] bool collidesXZ(float x, float z) const;
  // Probe ahead like OG: returns true if blocked.
  [[nodiscard]] bool blocksMove(float x, float z, float dx, float dz, float lookahead) const;

  // Apply plains footing. prevX/prevZ used for slope direction (OG parity).
  FootingResult sampleFooting(float x, float z, float prevX, float prevZ, float curY, float moveX,
                              float moveZ, int prevPlainIndex, float prevFlatHeight) const;

  [[nodiscard]] const std::vector<Plain>& plains() const { return plains_; }
  [[nodiscard]] const std::vector<AabbXZ>& aabbs() const { return aabbs_; }

private:
  std::vector<Plain> plains_;
  std::vector<AabbXZ> aabbs_;
};
