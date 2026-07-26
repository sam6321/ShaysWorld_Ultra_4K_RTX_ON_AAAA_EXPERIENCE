#pragma once

#include "render/local_lights.h"
#include "scene/scene.h"
#include "vk/context.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct GpuTexture {
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
};

struct FrameLighting {
  glm::vec3 cameraPos{0.0f};
  glm::vec3 sunDir{0.35f, -0.9f, 0.25f};
  glm::vec3 sunColor{1.0f, 0.96f, 0.90f};
  float sunIntensity = 3.5f;
  glm::vec3 ambientSky{0.45f, 0.55f, 0.75f};
  glm::vec3 ambientGround{0.18f, 0.16f, 0.12f};
  bool qualityMode = false;
  bool linearHdr = false;
  bool shadowsEnabled = false;
  std::array<glm::mat4, 4> lightViewProj{};
  glm::vec4 cascadeSplits{0.0f};

  // Local walkway lamps
  float localLightFade = 0.0f;
  int localLightCount = 0;
  bool lampShadowsEnabled = false;
  float rainWet = 0.0f;
  std::array<LocalLight, kWalkwayLampCount> localLights{};
  // Shadowed lights are packed first; count in lampShadowCount
  int lampShadowCount = 0;
  std::array<glm::mat4, kShadowedLampSlots> lampViewProj{};
  std::array<glm::vec4, kShadowedLampSlots> lampTileScaleBias{}; // xy scale, zw offset
};

class MeshPass {
public:
  bool init(VulkanContext& vk, VkRenderPass swapchainPass, const scene::Scene& scene,
            const std::string& assetsRoot);
  void shutdown(VulkanContext& vk);

  bool ensurePipelines(VulkanContext& vk, VkRenderPass swapchainPass, VkRenderPass hdrPass);
  bool ensureShadowPipeline(VulkanContext& vk, VkRenderPass shadowPass);

  void setShadowMap(VulkanContext& vk, VkImageView shadowView, VkSampler shadowSampler);
  void setLampShadowAtlas(VulkanContext& vk, VkImageView atlasView, VkSampler atlasSampler);
  void updateFrame(VulkanContext& vk, const FrameLighting& lighting, const glm::mat4& viewProj);
  void draw(VkCommandBuffer cmd, VkExtent2D extent, bool useHdrPipeline);
  void drawDepth(VkCommandBuffer cmd, const glm::mat4& lightViewProj, VkExtent2D extent,
                 VkOffset2D offset = {0, 0});

  VkBuffer vertexBuffer() const { return vertexBuffer_; }
  VkBuffer indexBuffer() const { return indexBuffer_; }
  uint32_t indexCount() const { return indexCount_; }
  uint32_t drawCount() const { return indexCount_ > 0 ? 1u : 0u; }

private:
  struct GpuVertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    uint32_t material;
  };

  bool createPipeline(VulkanContext& vk, VkRenderPass renderPass, VkPipeline& outPipeline);
  bool createShadowPipeline(VulkanContext& vk, VkRenderPass renderPass);
  bool uploadScene(VulkanContext& vk, const scene::Scene& scene);
  bool createTextures(VulkanContext& vk, const scene::Scene& scene);
  bool createFrameUbo(VulkanContext& vk);
  bool createDummyShadow(VulkanContext& vk);
  bool createDummyLampAtlas(VulkanContext& vk);
  VkShaderModule loadShader(VkDevice device, const std::string& path);

  struct GpuFrameUbo {
    glm::vec4 cameraPos{};
    glm::vec4 sunDir{};
    glm::vec4 sunColor{};
    glm::vec4 ambientSky{};
    glm::vec4 ambientGround{};
    glm::vec4 params{};
    glm::vec4 params2{};
    glm::mat4 lightVP0{1.0f};
    glm::mat4 lightVP1{1.0f};
    glm::mat4 lightVP2{1.0f};
    glm::mat4 lightVP3{1.0f};
    glm::vec4 cascadeSplits{};
    glm::vec4 lights[36]{};
    glm::mat4 lampVP[8]{};
    glm::vec4 lampTile[8]{};
    glm::vec4 lampSlot{};
    glm::mat4 viewProj{1.0f};
  };

  VkPipeline pipelineLdr_ = VK_NULL_HANDLE;
  VkPipeline pipelineHdr_ = VK_NULL_HANDLE;
  VkPipeline pipelineShadow_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipelineLayout shadowLayout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout materialSetLayout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout frameSetLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;

  VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
  VmaAllocation vertexAlloc_ = VK_NULL_HANDLE;
  VkBuffer indexBuffer_ = VK_NULL_HANDLE;
  VmaAllocation indexAlloc_ = VK_NULL_HANDLE;
  VkBuffer frameUbo_ = VK_NULL_HANDLE;
  VmaAllocation frameUboAlloc_ = VK_NULL_HANDLE;
  void* frameUboMapped_ = nullptr;
  VkDescriptorSet frameSet_ = VK_NULL_HANDLE;

  VkBuffer matParamsBuf_ = VK_NULL_HANDLE;
  VmaAllocation matParamsAlloc_ = VK_NULL_HANDLE;
  VkDescriptorSet materialSet_ = VK_NULL_HANDLE;
  uint32_t materialCount_ = 0;

  GpuTexture dummyShadow_{};
  GpuTexture dummyLampAtlas_{};
  VkSampler shadowSampler_ = VK_NULL_HANDLE;
  VkSampler lampAtlasSampler_ = VK_NULL_HANDLE;

  std::vector<GpuTexture> textures_;
  uint32_t indexCount_ = 0;
  VkRenderPass currentHdrPass_ = VK_NULL_HANDLE;
  VkRenderPass currentShadowPass_ = VK_NULL_HANDLE;
};
