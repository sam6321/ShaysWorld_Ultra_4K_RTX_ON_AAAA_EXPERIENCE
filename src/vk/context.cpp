#include "vk/context.h"

#include <GLFW/glfw3.h>

#include <cstdio>
#include <stdexcept>

namespace {
void check(bool ok, const char* what) {
  if (!ok) {
    throw std::runtime_error(what);
  }
}
} // namespace

bool VulkanContext::init(GLFWwindow* window) {
  window_ = window;

  vkb::InstanceBuilder builder;
  builder.set_app_name("Shays World VK").require_api_version(1, 2, 0);
#ifndef NDEBUG
  builder.request_validation_layers(true).use_default_debug_messenger();
#endif
  auto instRet = builder.build();
  check(instRet.has_value(), "Failed to create Vulkan instance");
  vkbInstance_ = instRet.value();
  instance_ = vkbInstance_.instance;

  check(glfwCreateWindowSurface(instance_, window_, nullptr, &surface_) == VK_SUCCESS,
        "Failed to create window surface");

  // Bindless albedo array (one draw for the whole campus).
  VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  v12.descriptorIndexing = VK_TRUE;
  v12.runtimeDescriptorArray = VK_TRUE;
  v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
  v12.descriptorBindingPartiallyBound = VK_TRUE;
  v12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;

  vkb::PhysicalDeviceSelector selector{vkbInstance_};
  auto physRet = selector.set_surface(surface_)
                     .set_minimum_version(1, 2)
                     .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
                     .set_required_features_12(v12)
                     .select();
  check(physRet.has_value(), "Failed to select physical device");

  vkb::DeviceBuilder deviceBuilder{physRet.value()};
  auto devRet = deviceBuilder.build();
  check(devRet.has_value(), "Failed to create logical device");
  vkbDevice_ = devRet.value();
  device_ = vkbDevice_.device;
  physicalDevice_ = vkbDevice_.physical_device.physical_device;
  graphicsQueue_ = vkbDevice_.get_queue(vkb::QueueType::graphics).value();
  graphicsQueueFamily_ = vkbDevice_.get_queue_index(vkb::QueueType::graphics).value();

  check(createAllocator(), "Failed to create VMA allocator");
  check(createSwapchain(), "Failed to create swapchain");
  check(createSwapchainSync(), "Failed to create swapchain sync objects");
  check(createFrameData(), "Failed to create frame data");
  return true;
}

void VulkanContext::shutdown() {
  if (device_ != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(device_);
  }
  destroyFrameData();
  destroySwapchainSync();
  destroySwapchain();
  if (allocator_ != VK_NULL_HANDLE) {
    vmaDestroyAllocator(allocator_);
    allocator_ = VK_NULL_HANDLE;
  }
  if (device_ != VK_NULL_HANDLE) {
    vkb::destroy_device(vkbDevice_);
    device_ = VK_NULL_HANDLE;
  }
  if (surface_ != VK_NULL_HANDLE) {
    vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
  }
  if (instance_ != VK_NULL_HANDLE) {
    vkb::destroy_instance(vkbInstance_);
    instance_ = VK_NULL_HANDLE;
  }
}

bool VulkanContext::recreateSwapchain() {
  int width = 0, height = 0;
  glfwGetFramebufferSize(window_, &width, &height);
  while (width == 0 || height == 0) {
    glfwGetFramebufferSize(window_, &width, &height);
    glfwWaitEvents();
  }

  vkDeviceWaitIdle(device_);
  destroySwapchainSync();
  destroySwapchain();
  if (!createSwapchain()) {
    return false;
  }
  return createSwapchainSync();
}

bool VulkanContext::createAllocator() {
  VmaAllocatorCreateInfo info{};
  info.physicalDevice = physicalDevice_;
  info.device = device_;
  info.instance = instance_;
  info.vulkanApiVersion = VK_API_VERSION_1_2;
  return vmaCreateAllocator(&info, &allocator_) == VK_SUCCESS;
}

bool VulkanContext::createSwapchain() {
  vkb::SwapchainBuilder swapBuilder{vkbDevice_, surface_};
  auto swapRet = swapBuilder
                     .set_desired_format({VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                     .set_desired_present_mode(VK_PRESENT_MODE_IMMEDIATE_KHR) // uncapped FPS chase
                     .add_fallback_present_mode(VK_PRESENT_MODE_MAILBOX_KHR)
                     .add_fallback_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                     .set_desired_extent(0, 0)
                     .build();
  if (!swapRet) {
    std::fprintf(stderr, "Swapchain error: %s\n", swapRet.error().message().c_str());
    return false;
  }
  auto vkbSwap = swapRet.value();
  swapchain_ = vkbSwap.swapchain;
  swapchainFormat_ = vkbSwap.image_format;
  swapchainExtent_ = vkbSwap.extent;
  swapchainImages_ = vkbSwap.get_images().value();
  swapchainImageViews_ = vkbSwap.get_image_views().value();
  return true;
}

void VulkanContext::destroySwapchain() {
  for (auto view : swapchainImageViews_) {
    vkDestroyImageView(device_, view, nullptr);
  }
  swapchainImageViews_.clear();
  swapchainImages_.clear();
  if (swapchain_ != VK_NULL_HANDLE) {
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
  }
}

bool VulkanContext::createSwapchainSync() {
  destroySwapchainSync();
  const size_t imageCount = swapchainImages_.size();
  if (imageCount == 0) {
    return false;
  }

  renderFinished_.resize(imageCount, VK_NULL_HANDLE);
  imagesInFlight_.assign(imageCount, VK_NULL_HANDLE);

  VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (size_t i = 0; i < imageCount; ++i) {
    if (vkCreateSemaphore(device_, &semInfo, nullptr, &renderFinished_[i]) != VK_SUCCESS) {
      destroySwapchainSync();
      return false;
    }
  }
  return true;
}

void VulkanContext::destroySwapchainSync() {
  for (VkSemaphore sem : renderFinished_) {
    if (sem) {
      vkDestroySemaphore(device_, sem, nullptr);
    }
  }
  renderFinished_.clear();
  imagesInFlight_.clear();
}

bool VulkanContext::createFrameData() {
  for (int i = 0; i < kFramesInFlight; ++i) {
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = graphicsQueueFamily_;
    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &frames_[i].commandPool) != VK_SUCCESS) {
      return false;
    }

    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = frames_[i].commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &allocInfo, &frames_[i].commandBuffer) != VK_SUCCESS) {
      return false;
    }

    VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateSemaphore(device_, &semInfo, nullptr, &frames_[i].imageAvailable) != VK_SUCCESS ||
        vkCreateFence(device_, &fenceInfo, nullptr, &frames_[i].inFlight) != VK_SUCCESS) {
      return false;
    }
  }
  return true;
}

void VulkanContext::destroyFrameData() {
  for (int i = 0; i < kFramesInFlight; ++i) {
    auto& f = frames_[i];
    if (f.inFlight) vkDestroyFence(device_, f.inFlight, nullptr);
    if (f.imageAvailable) vkDestroySemaphore(device_, f.imageAvailable, nullptr);
    if (f.commandPool) vkDestroyCommandPool(device_, f.commandPool, nullptr);
    f = {};
  }
}

VkCommandBuffer VulkanContext::beginFrame(uint32_t& imageIndex, bool& outOfDate) {
  outOfDate = false;
  FrameData& f = frames_[frameIndex_];
  vkWaitForFences(device_, 1, &f.inFlight, VK_TRUE, UINT64_MAX);

  VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.imageAvailable,
                                          VK_NULL_HANDLE, &imageIndex);
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    outOfDate = true;
    return VK_NULL_HANDLE;
  }
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    throw std::runtime_error("Failed to acquire swapchain image");
  }

  if (imageIndex >= imagesInFlight_.size()) {
    throw std::runtime_error("Swapchain image index out of range");
  }
  if (imagesInFlight_[imageIndex] != VK_NULL_HANDLE) {
    vkWaitForFences(device_, 1, &imagesInFlight_[imageIndex], VK_TRUE, UINT64_MAX);
  }
  imagesInFlight_[imageIndex] = f.inFlight;

  vkResetFences(device_, 1, &f.inFlight);
  vkResetCommandBuffer(f.commandBuffer, 0);

  VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(f.commandBuffer, &beginInfo);
  return f.commandBuffer;
}

void VulkanContext::endFrame(uint32_t imageIndex, bool& outOfDate) {
  outOfDate = false;
  FrameData& f = frames_[frameIndex_];
  vkEndCommandBuffer(f.commandBuffer);

  if (imageIndex >= renderFinished_.size()) {
    throw std::runtime_error("Swapchain image index out of range");
  }
  VkSemaphore renderFinished = renderFinished_[imageIndex];

  VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = &f.imageAvailable;
  submit.pWaitDstStageMask = &waitStage;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &f.commandBuffer;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &renderFinished;
  vkQueueSubmit(graphicsQueue_, 1, &submit, f.inFlight);

  VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &renderFinished;
  present.swapchainCount = 1;
  present.pSwapchains = &swapchain_;
  present.pImageIndices = &imageIndex;
  VkResult result = vkQueuePresentKHR(graphicsQueue_, &present);
  if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
    outOfDate = true;
  } else if (result != VK_SUCCESS) {
    throw std::runtime_error("Failed to present");
  }

  advanceFrame();
}
