#pragma once

#include "render/mesh_pass.h"
#include "settings/graphics_settings.h"
#include "vk/context.h"

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class QualityRenderer {
public:
  struct CameraFrame {
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
    glm::mat4 invViewProj{1.0f};
    glm::vec3 position{0.0f};
    float nearPlane = 0.1f;
    float farPlane = 2000.0f;
  };

  bool init(VulkanContext& vk);
  void shutdown(VulkanContext& vk);

  void setEnabled(VulkanContext& vk, MeshPass& meshes, VkRenderPass swapchainPass, bool enabled);
  bool enabled() const { return enabled_; }
  void resize(VulkanContext& vk, MeshPass& meshes, VkRenderPass swapchainPass);

  void render(VulkanContext& vk, MeshPass& meshes, VkCommandBuffer cmd, uint32_t swapImageIndex,
              const GraphicsSettings& gfx, const CameraFrame& cam, FrameLighting lighting);

  // Begin ImGui-compatible UI pass that loads the post-processed swapchain color.
  void beginUiPass(VkCommandBuffer cmd, uint32_t swapImageIndex);
  void endUiPass(VkCommandBuffer cmd);
  VkRenderPass uiRenderPass() const { return uiPass_; }

private:
  bool createResources(VulkanContext& vk, MeshPass& meshes, VkRenderPass swapchainPass);
  void destroyResources(VulkanContext& vk);
  void updateCascades(const CameraFrame& cam, const glm::vec3& sunDir, FrameLighting& lighting);
  VkShaderModule loadShader(VkDevice device, const std::string& path);
  bool loadFsVertFrag(VulkanContext& vk, const char* fragName, VkShaderModule& vert,
                      VkShaderModule& frag);
  static glm::vec2 halton(uint32_t index);

  bool enabled_ = false;
  VkExtent2D extent_{};

  VkRenderPass hdrPass_ = VK_NULL_HANDLE;
  VkImage hdrColor_ = VK_NULL_HANDLE;
  VmaAllocation hdrColorAlloc_ = VK_NULL_HANDLE;
  VkImageView hdrColorView_ = VK_NULL_HANDLE;
  VkImage hdrDepth_ = VK_NULL_HANDLE;
  VmaAllocation hdrDepthAlloc_ = VK_NULL_HANDLE;
  VkImageView hdrDepthView_ = VK_NULL_HANDLE;
  VkFramebuffer hdrFb_ = VK_NULL_HANDLE;

  static constexpr uint32_t kCascades = 4;
  static constexpr uint32_t kShadowSize = 4096;
  VkRenderPass shadowPass_ = VK_NULL_HANDLE;
  VkImage shadowImage_ = VK_NULL_HANDLE;
  VmaAllocation shadowAlloc_ = VK_NULL_HANDLE;
  VkImageView shadowArrayView_ = VK_NULL_HANDLE;
  std::array<VkImageView, kCascades> shadowLayerViews_{};
  std::array<VkFramebuffer, kCascades> shadowFbs_{};
  VkSampler shadowSampler_ = VK_NULL_HANDLE;

  VkRenderPass aoPass_ = VK_NULL_HANDLE;
  VkImage aoImage_ = VK_NULL_HANDLE;
  VmaAllocation aoAlloc_ = VK_NULL_HANDLE;
  VkImageView aoView_ = VK_NULL_HANDLE;
  VkFramebuffer aoFb_ = VK_NULL_HANDLE;
  VkImage aoBlurImage_ = VK_NULL_HANDLE;
  VmaAllocation aoBlurAlloc_ = VK_NULL_HANDLE;
  VkImageView aoBlurView_ = VK_NULL_HANDLE;
  VkFramebuffer aoBlurFb_ = VK_NULL_HANDLE;

  // Tonemap/bloom/CA/sharpen into LDR offscreen, then TAA, then blit to swapchain.
  VkRenderPass ldrPass_ = VK_NULL_HANDLE;
  VkImage ldrColor_ = VK_NULL_HANDLE;
  VmaAllocation ldrColorAlloc_ = VK_NULL_HANDLE;
  VkImageView ldrColorView_ = VK_NULL_HANDLE;
  VkFramebuffer ldrFb_ = VK_NULL_HANDLE;

  VkRenderPass taaPass_ = VK_NULL_HANDLE;
  std::array<VkImage, 2> historyColor_{};
  std::array<VmaAllocation, 2> historyAlloc_{};
  std::array<VkImageView, 2> historyView_{};
  std::array<VkFramebuffer, 2> historyFb_{};
  uint32_t historyWrite_ = 0;
  bool historyValid_ = false;
  uint32_t jitterIndex_ = 0;
  glm::mat4 prevViewProj_{1.0f};

  VkSampler linearSampler_ = VK_NULL_HANDLE;
  VkDescriptorPool fsPool_ = VK_NULL_HANDLE;

  VkPipelineLayout skyLayout_ = VK_NULL_HANDLE;
  VkPipeline skyPipeline_ = VK_NULL_HANDLE;

  VkDescriptorSetLayout aoSetLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout aoLayout_ = VK_NULL_HANDLE;
  VkPipeline aoPipeline_ = VK_NULL_HANDLE;
  VkDescriptorSet aoSet_ = VK_NULL_HANDLE;

  VkDescriptorSetLayout aoBlurSetLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout aoBlurLayout_ = VK_NULL_HANDLE;
  VkPipeline aoBlurPipeline_ = VK_NULL_HANDLE;
  VkDescriptorSet aoBlurSet_ = VK_NULL_HANDLE;

  VkDescriptorSetLayout postSetLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout postLayout_ = VK_NULL_HANDLE;
  VkPipeline postPipeline_ = VK_NULL_HANDLE;
  VkDescriptorSet postSet_ = VK_NULL_HANDLE;
  VkBuffer postUbo_ = VK_NULL_HANDLE;
  VmaAllocation postUboAlloc_ = VK_NULL_HANDLE;
  void* postUboMapped_ = nullptr;

  VkDescriptorSetLayout taaSetLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout taaLayout_ = VK_NULL_HANDLE;
  VkPipeline taaPipeline_ = VK_NULL_HANDLE;
  VkDescriptorSet taaSet_ = VK_NULL_HANDLE;

  VkRenderPass presentPass_ = VK_NULL_HANDLE; // blit → swapchain
  VkDescriptorSetLayout blitSetLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout blitLayout_ = VK_NULL_HANDLE;
  VkPipeline blitPipeline_ = VK_NULL_HANDLE;
  VkDescriptorSet blitSet_ = VK_NULL_HANDLE;
  std::vector<VkFramebuffer> presentFramebuffers_;

  VkRenderPass uiPass_ = VK_NULL_HANDLE;
  std::vector<VkFramebuffer> uiFramebuffers_;

  VkImage dummyAo_ = VK_NULL_HANDLE;
  VmaAllocation dummyAoAlloc_ = VK_NULL_HANDLE;
  VkImageView dummyAoView_ = VK_NULL_HANDLE;

  // Walkway lamp spot-shadow atlas (4x2 tiles of 1024)
  static constexpr uint32_t kLampTile = 1024;
  static constexpr uint32_t kLampAtlasW = 4096;
  static constexpr uint32_t kLampAtlasH = 2048;
  VkImage lampAtlas_ = VK_NULL_HANDLE;
  VmaAllocation lampAtlasAlloc_ = VK_NULL_HANDLE;
  VkImageView lampAtlasView_ = VK_NULL_HANDLE;
  VkFramebuffer lampAtlasFb_ = VK_NULL_HANDLE;
  VkSampler lampAtlasSampler_ = VK_NULL_HANDLE;

  float exposure_ = 1.0f;
  float rainTime_ = 0.0f;
  glm::vec3 prevCamPos_{0.0f};
  bool prevCamPosValid_ = false;

  // Camera-local rain particles (world-space billboards).
  static constexpr uint32_t kRainParticles = 2800;
  std::vector<glm::vec4> rainParticles_; // xyz world, w = length seed
  VkBuffer rainQuadBuf_ = VK_NULL_HANDLE;
  VmaAllocation rainQuadAlloc_ = VK_NULL_HANDLE;
  VkBuffer rainInstanceBuf_ = VK_NULL_HANDLE;
  VmaAllocation rainInstanceAlloc_ = VK_NULL_HANDLE;
  void* rainInstanceMapped_ = nullptr;
  VkPipelineLayout rainLayout_ = VK_NULL_HANDLE;
  VkPipeline rainPipeline_ = VK_NULL_HANDLE;
};
