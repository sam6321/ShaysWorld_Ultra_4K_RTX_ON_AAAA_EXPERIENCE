#include "game/camera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace {
constexpr float kPi = 3.14159265358979323846f;
float toRad(float deg) { return deg * kPi / 180.0f; }
} // namespace

glm::vec3 Camera::forward() const {
  const float yaw = toRad(yawDeg);
  const float pitch = toRad(pitchDeg);
  return glm::normalize(glm::vec3{
      std::sin(yaw) * std::cos(pitch),
      std::sin(pitch),
      std::cos(yaw) * std::cos(pitch),
  });
}

glm::vec3 Camera::forwardXZ() const {
  const float yaw = toRad(yawDeg);
  return glm::normalize(glm::vec3{std::sin(yaw), 0.0f, std::cos(yaw)});
}

bool Camera::update(float dt, bool forward, bool back, bool left, bool right, bool up, bool down,
                    float mouseDx, float mouseDy) {
  yawDeg -= mouseDx * lookSpeed;
  pitchDeg = std::clamp(pitchDeg - mouseDy * lookSpeed, -89.0f, 89.0f);

  bool step = false;
  const glm::vec3 f = this->forward();
  const glm::vec3 worldUp{0.0f, 1.0f, 0.0f};
  const glm::vec3 r = glm::normalize(glm::cross(f, worldUp));

  if (freeFly || world_ == nullptr) {
    glm::vec3 wish{0.0f};
    if (forward) wish += f;
    if (back) wish -= f;
    if (right) wish += r;
    if (left) wish -= r;
    if (up) wish += worldUp;
    if (down) wish -= worldUp;
    if (glm::length(wish) > 0.0f) {
      position += glm::normalize(wish) * moveSpeed * dt;
    }
    return false;
  }

  // Gameplay: horizontal wish on XZ, plains set Y, AABB blocks walls.
  glm::vec3 wish{0.0f};
  const glm::vec3 fXZ = forwardXZ();
  const glm::vec3 rXZ = glm::normalize(glm::cross(fXZ, worldUp));
  if (forward) wish += fXZ;
  if (back) wish -= fXZ;
  if (right) wish += rXZ;
  if (left) wish -= rXZ;

  if (glm::length(wish) > 1e-4f) {
    wish = glm::normalize(wish) * moveSpeed * dt;
    const float prevX = position.x;
    const float prevZ = position.z;

    // Try full move, then axis slide (better than OG hard reject).
    auto tryMove = [&](float dx, float dz, float lookahead) {
      if (world_->blocksMove(position.x, position.z, dx, dz, lookahead)) return false;
      // Also reject if destination itself is inside a box.
      if (world_->collidesXZ(position.x + dx, position.z + dz)) return false;
      position.x += dx;
      position.z += dz;
      return true;
    };

    if (!tryMove(wish.x, wish.z, 5.0f)) {
      tryMove(wish.x, 0.0f, 1.0f);
      tryMove(0.0f, wish.z, 1.0f);
    }

    const float moveX = position.x - prevX;
    const float moveZ = position.z - prevZ;
    FootingResult foot =
        world_->sampleFooting(position.x, position.z, prevX, prevZ, position.y, moveX, moveZ,
                              plainIndex_, plainHeight_);
    if (foot.onPlain) {
      position.y = foot.eyeY;
      if (foot.flatHeightChanged) step = true;
      plainIndex_ = foot.plainIndex;
      if (foot.plainIndex >= 0 &&
          world_->plains()[static_cast<size_t>(foot.plainIndex)].type == PlainType::Flat) {
        plainHeight_ = foot.eyeY;
      }
    }
  } else {
    // Still snap to footing when standing still (e.g. after load).
    FootingResult foot = world_->sampleFooting(position.x, position.z, position.x, position.z,
                                               position.y, 0.0f, 0.0f, plainIndex_, plainHeight_);
    if (foot.onPlain) {
      position.y = foot.eyeY;
      plainIndex_ = foot.plainIndex;
    }
  }

  return step;
}

glm::mat4 Camera::viewMatrix() const {
  return glm::lookAt(position, position + forward(), glm::vec3{0.0f, 1.0f, 0.0f});
}
