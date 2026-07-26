#pragma once

#include "game/collision.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

class Camera {
public:
  // Scaled Shay spawn — OG Y is already eye height (no +1.7).
  glm::vec3 position{327.20f, 95.36f, 48.00f};
  float yawDeg = 180.0f;
  float pitchDeg = 0.0f;
  // OG ~1400 Shay units/s * 0.01 scale.
  float moveSpeed = 14.0f;
  float lookSpeed = 0.12f;
  bool freeFly = false; // Space/Ctrl fly when true; gameplay walk when false

  void setCollisionWorld(const CollisionWorld* world) { world_ = world; }

  // Returns true if a footstep should play this frame.
  bool update(float dt, bool forward, bool back, bool left, bool right, bool up, bool down,
              float mouseDx, float mouseDy);

  [[nodiscard]] glm::mat4 viewMatrix() const;
  [[nodiscard]] glm::vec3 forward() const;
  [[nodiscard]] glm::vec3 forwardXZ() const;

private:
  const CollisionWorld* world_ = nullptr;
  int plainIndex_ = -1;
  float plainHeight_ = 0.0f;
};
