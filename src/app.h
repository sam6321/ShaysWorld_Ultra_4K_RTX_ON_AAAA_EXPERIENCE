#pragma once

#include "game/camera.h"
#include "game/collision.h"
#include "game/audio.h"
#include "render/mesh_pass.h"
#include "render/quality_renderer.h"
#include "scene/scene.h"
#include "settings/graphics_settings.h"
#include "vk/context.h"

#include <chrono>
#include <string>
#include <vector>

struct GLFWwindow;

class App {
public:
  bool init();
  void run();
  void shutdown();

private:
  void initImGui(VkRenderPass renderPass);
  void shutdownImGui();
  void drawUi(float fps);
  void recordFrame(VkCommandBuffer cmd, uint32_t imageIndex);
  void createMainRenderPass();
  void createDepthResources();
  void destroyDepthResources();
  void createFramebuffers();
  void destroyFramebuffers();
  bool loadCampus();
  void onPresetChanged();
  void handleResize();
  bool loadNoExitSign();
  void destroyNoExitSign();
  void bindNoExitImGui();

  GLFWwindow* window_ = nullptr;
  VulkanContext vk_;
  GraphicsSettings graphics_{};
  Camera camera_{};
  CollisionWorld collision_{};
  StepAudio stepAudio_{};
  scene::Scene scene_{};
  MeshPass meshPass_{};
  QualityRenderer quality_{};
  GraphicsPreset lastPreset_ = GraphicsPreset::Performance;

  VkRenderPass mainPass_ = VK_NULL_HANDLE;
  VkDescriptorPool imguiPool_ = VK_NULL_HANDLE;
  std::vector<VkFramebuffer> framebuffers_;

  VkImage depthImage_ = VK_NULL_HANDLE;
  VmaAllocation depthAlloc_ = VK_NULL_HANDLE;
  VkImageView depthView_ = VK_NULL_HANDLE;

  // OG "area not modelled" HUD (data/noexit.raw → assets/textures/no_exit.rgb)
  VkImage noExitImage_ = VK_NULL_HANDLE;
  VmaAllocation noExitAlloc_ = VK_NULL_HANDLE;
  VkImageView noExitView_ = VK_NULL_HANDLE;
  VkSampler noExitSampler_ = VK_NULL_HANDLE;
  VkDescriptorSet noExitDs_ = VK_NULL_HANDLE;

  std::chrono::steady_clock::time_point lastTitleUpdate_{};
  float fps_ = 0.0f;
  bool framebufferResized_ = false;
  bool cursorCaptured_ = true;
  bool showHud_ = true;
  bool skipImGui_ = false;
  double lastMouseX_ = 0.0;
  double lastMouseY_ = 0.0;
  std::string assetsDir_;

  static void onFramebufferResize(GLFWwindow* window, int width, int height);
};
