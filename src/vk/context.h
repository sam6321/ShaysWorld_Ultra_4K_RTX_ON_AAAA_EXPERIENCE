#pragma once

#include <vulkan/vulkan.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <functional>
#include <vector>

struct GLFWwindow;

struct FrameData {
  VkCommandPool commandPool = VK_NULL_HANDLE;
  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  VkSemaphore imageAvailable = VK_NULL_HANDLE;
  VkFence inFlight = VK_NULL_HANDLE;
};

class VulkanContext {
public:
  static constexpr int kFramesInFlight = 2;

  bool init(GLFWwindow* window);
  void shutdown();
  bool recreateSwapchain();

  VkInstance instance() const { return instance_; }
  VkDevice device() const { return device_; }
  VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
  VkQueue graphicsQueue() const { return graphicsQueue_; }
  uint32_t graphicsQueueFamily() const { return graphicsQueueFamily_; }
  VkSwapchainKHR swapchain() const { return swapchain_; }
  VkFormat swapchainFormat() const { return swapchainFormat_; }
  VkExtent2D swapchainExtent() const { return swapchainExtent_; }
  VmaAllocator allocator() const { return allocator_; }
  const std::vector<VkImageView>& swapchainImageViews() const { return swapchainImageViews_; }
  const std::vector<VkImage>& swapchainImages() const { return swapchainImages_; }
  FrameData& frame(int i) { return frames_[i]; }
  int frameIndex() const { return frameIndex_; }
  void advanceFrame() { frameIndex_ = (frameIndex_ + 1) % kFramesInFlight; }

  VkCommandBuffer beginFrame(uint32_t& imageIndex, bool& outOfDate);
  void endFrame(uint32_t imageIndex, bool& outOfDate);

  using ImGuiInitFn = std::function<void(VkRenderPass, uint32_t)>;

private:
  bool createAllocator();
  bool createSwapchain();
  bool createSwapchainSync();
  void destroySwapchainSync();
  bool createFrameData();
  void destroySwapchain();
  void destroyFrameData();

  GLFWwindow* window_ = nullptr;
  vkb::Instance vkbInstance_{};
  vkb::Device vkbDevice_{};
  VkInstance instance_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue graphicsQueue_ = VK_NULL_HANDLE;
  uint32_t graphicsQueueFamily_ = 0;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat swapchainFormat_ = VK_FORMAT_B8G8R8A8_SRGB;
  VkExtent2D swapchainExtent_{};
  std::vector<VkImage> swapchainImages_;
  std::vector<VkImageView> swapchainImageViews_;
  // One present/signal semaphore per swapchain image (not per frame-in-flight).
  // See https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html
  std::vector<VkSemaphore> renderFinished_;
  std::vector<VkFence> imagesInFlight_;
  VmaAllocator allocator_ = VK_NULL_HANDLE;
  FrameData frames_[kFramesInFlight]{};
  int frameIndex_ = 0;
};
