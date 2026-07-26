#include "app.h"
#include "util/paths.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void checkVk(VkResult r, const char* what) {
  if (r != VK_SUCCESS) {
    throw std::runtime_error(what);
  }
}

// OG main.cpp DisplayNoExit regions (Shay units) * WORLD_SCALE 0.01.
bool inNoExitZone(float x, float z) {
  return (x > 355.0f && z < 253.44f) || (x > 341.0f && z > 411.27f);
}
} // namespace

void App::onFramebufferResize(GLFWwindow* window, int, int) {
  auto* app = static_cast<App*>(glfwGetWindowUserPointer(window));
  if (app) {
    app->framebufferResized_ = true;
  }
}

bool App::init() {
  if (!glfwInit()) {
    return false;
  }
  // Portable layout: assets/ and shaders/ sit next to the exe.
  chdirToExeDirectory();

  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
  window_ = glfwCreateWindow(1600, 900, "Shays World VK — Performance", nullptr, nullptr);
  if (!window_) {
    glfwTerminate();
    return false;
  }
  glfwSetWindowUserPointer(window_, this);
  glfwSetFramebufferSizeCallback(window_, onFramebufferResize);
  glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
  glfwGetCursorPos(window_, &lastMouseX_, &lastMouseY_);

  graphics_.applyPreset(GraphicsPreset::Performance);
  lastPreset_ = graphics_.preset;
  assetsDir_ = resolveAssetsDirectory();
  std::fprintf(stderr, "Assets: %s\n", assetsDir_.c_str());

  if (!vk_.init(window_)) {
    return false;
  }

  createMainRenderPass();
  createDepthResources();
  createFramebuffers();
  initImGui(mainPass_);
  quality_.init(vk_);

  if (!loadCampus()) {
    std::fprintf(stderr, "Warning: campus scene failed to load (empty world)\n");
  }
  if (!loadNoExitSign()) {
    std::fprintf(stderr, "Warning: no_exit sign missing — zone warning disabled\n");
  }

  lastTitleUpdate_ = std::chrono::steady_clock::now();
  return true;
}

bool App::loadCampus() {
  const std::string bin = assetsDir_ + "/scene.bin";
  if (!scene::loadScene(bin, assetsDir_, scene_)) {
    return false;
  }
  const std::string col = assetsDir_ + "/collision.json";
  if (collision_.load(col)) {
    camera_.setCollisionWorld(&collision_);
    camera_.freeFly = false;
    // Snap spawn to plains immediately.
    camera_.update(0.0f, false, false, false, false, false, false, 0.0f, 0.0f);
  } else {
    std::fprintf(stderr, "Warning: %s missing — free-fly only\n", col.c_str());
    camera_.freeFly = true;
  }
  if (!stepAudio_.load(assetsDir_ + "/sounds/step.wav")) {
    std::fprintf(stderr, "Warning: step.wav missing — no footsteps\n");
  }
  return meshPass_.init(vk_, mainPass_, scene_, assetsDir_);
}

void App::createMainRenderPass() {
  VkAttachmentDescription attachments[2]{};
  attachments[0].format = vk_.swapchainFormat();
  attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
  attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  attachments[1].format = VK_FORMAT_D32_SFLOAT;
  attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
  attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &colorRef;
  subpass.pDepthStencilAttachment = &depthRef;

  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dep.dstStageMask =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dep.dstAccessMask =
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

  VkRenderPassCreateInfo rpInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpInfo.attachmentCount = 2;
  rpInfo.pAttachments = attachments;
  rpInfo.subpassCount = 1;
  rpInfo.pSubpasses = &subpass;
  rpInfo.dependencyCount = 1;
  rpInfo.pDependencies = &dep;
  checkVk(vkCreateRenderPass(vk_.device(), &rpInfo, nullptr, &mainPass_), "render pass");
}

void App::createDepthResources() {
  destroyDepthResources();
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = VK_FORMAT_D32_SFLOAT;
  info.extent = {vk_.swapchainExtent().width, vk_.swapchainExtent().height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VmaAllocationCreateInfo alloc{};
  alloc.usage = VMA_MEMORY_USAGE_AUTO;
  checkVk(vmaCreateImage(vk_.allocator(), &info, &alloc, &depthImage_, &depthAlloc_, nullptr),
          "depth image");

  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = depthImage_;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = VK_FORMAT_D32_SFLOAT;
  view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  view.subresourceRange.levelCount = 1;
  view.subresourceRange.layerCount = 1;
  checkVk(vkCreateImageView(vk_.device(), &view, nullptr, &depthView_), "depth view");
}

void App::destroyDepthResources() {
  if (depthView_) {
    vkDestroyImageView(vk_.device(), depthView_, nullptr);
    depthView_ = VK_NULL_HANDLE;
  }
  if (depthImage_) {
    vmaDestroyImage(vk_.allocator(), depthImage_, depthAlloc_);
    depthImage_ = VK_NULL_HANDLE;
    depthAlloc_ = VK_NULL_HANDLE;
  }
}

void App::createFramebuffers() {
  destroyFramebuffers();
  framebuffers_.resize(vk_.swapchainImageViews().size());
  for (size_t i = 0; i < framebuffers_.size(); ++i) {
    VkImageView attachments[] = {vk_.swapchainImageViews()[i], depthView_};
    VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbInfo.renderPass = mainPass_;
    fbInfo.attachmentCount = 2;
    fbInfo.pAttachments = attachments;
    fbInfo.width = vk_.swapchainExtent().width;
    fbInfo.height = vk_.swapchainExtent().height;
    fbInfo.layers = 1;
    checkVk(vkCreateFramebuffer(vk_.device(), &fbInfo, nullptr, &framebuffers_[i]), "framebuffer");
  }
}

void App::destroyFramebuffers() {
  for (auto fb : framebuffers_) {
    vkDestroyFramebuffer(vk_.device(), fb, nullptr);
  }
  framebuffers_.clear();
}

void App::initImGui(VkRenderPass renderPass) {
  if (!imguiPool_) {
    VkDescriptorPoolSize poolSizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
        {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000},
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 1000;
    poolInfo.poolSizeCount = static_cast<uint32_t>(std::size(poolSizes));
    poolInfo.pPoolSizes = poolSizes;
    checkVk(vkCreateDescriptorPool(vk_.device(), &poolInfo, nullptr, &imguiPool_), "imgui pool");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForVulkan(window_, true);
  } else {
    if (noExitDs_) {
      ImGui_ImplVulkan_RemoveTexture(noExitDs_);
      noExitDs_ = VK_NULL_HANDLE;
    }
    ImGui_ImplVulkan_Shutdown();
  }

  ImGui_ImplVulkan_InitInfo initInfo{};
  initInfo.Instance = vk_.instance();
  initInfo.PhysicalDevice = vk_.physicalDevice();
  initInfo.Device = vk_.device();
  initInfo.QueueFamily = vk_.graphicsQueueFamily();
  initInfo.Queue = vk_.graphicsQueue();
  initInfo.DescriptorPool = imguiPool_;
  initInfo.MinImageCount = 2;
  initInfo.ImageCount = static_cast<uint32_t>(vk_.swapchainImages().size());
  initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  initInfo.RenderPass = renderPass;
  ImGui_ImplVulkan_Init(&initInfo);
  bindNoExitImGui();
}

void App::shutdownImGui() {
  if (vk_.device() != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(vk_.device());
  }
  if (noExitDs_) {
    ImGui_ImplVulkan_RemoveTexture(noExitDs_);
    noExitDs_ = VK_NULL_HANDLE;
  }
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  if (imguiPool_) {
    vkDestroyDescriptorPool(vk_.device(), imguiPool_, nullptr);
    imguiPool_ = VK_NULL_HANDLE;
  }
}

void App::bindNoExitImGui() {
  if (!noExitView_ || !noExitSampler_) {
    return;
  }
  noExitDs_ = ImGui_ImplVulkan_AddTexture(noExitSampler_, noExitView_,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

bool App::loadNoExitSign() {
  const std::string path = assetsDir_ + "/textures/no_exit.rgb";
  constexpr uint32_t kW = 256;
  constexpr uint32_t kH = 64;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::vector<uint8_t> rgb(static_cast<size_t>(kW) * kH * 3);
  in.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
  if (!in) {
    return false;
  }

  std::vector<uint8_t> rgba(static_cast<size_t>(kW) * kH * 4);
  for (uint32_t p = 0; p < kW * kH; ++p) {
    rgba[p * 4 + 0] = rgb[p * 3 + 0];
    rgba[p * 4 + 1] = rgb[p * 3 + 1];
    rgba[p * 4 + 2] = rgb[p * 3 + 2];
    rgba[p * 4 + 3] = 255;
  }

  VkBuffer staging = VK_NULL_HANDLE;
  VmaAllocation stagingAlloc = VK_NULL_HANDLE;
  VkBufferCreateInfo sbi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  sbi.size = rgba.size();
  sbi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  VmaAllocationCreateInfo sai{};
  sai.usage = VMA_MEMORY_USAGE_AUTO;
  sai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
              VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VmaAllocationInfo sinfo{};
  checkVk(vmaCreateBuffer(vk_.allocator(), &sbi, &sai, &staging, &stagingAlloc, &sinfo),
          "noexit staging");
  std::memcpy(sinfo.pMappedData, rgba.data(), rgba.size());

  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = VK_FORMAT_R8G8B8A8_SRGB;
  ii.extent = {kW, kH, 1};
  ii.mipLevels = 1;
  ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VmaAllocationCreateInfo iai{};
  iai.usage = VMA_MEMORY_USAGE_AUTO;
  checkVk(vmaCreateImage(vk_.allocator(), &ii, &iai, &noExitImage_, &noExitAlloc_, nullptr),
          "noexit image");

  VkCommandPoolCreateInfo poolCi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  poolCi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  poolCi.queueFamilyIndex = vk_.graphicsQueueFamily();
  VkCommandPool uploadPool = VK_NULL_HANDLE;
  checkVk(vkCreateCommandPool(vk_.device(), &poolCi, nullptr, &uploadPool), "noexit pool");

  VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocInfo.commandPool = uploadPool;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  checkVk(vkAllocateCommandBuffers(vk_.device(), &allocInfo, &cmd), "noexit cmd");

  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);

  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = noExitImage_;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &barrier);

  VkBufferImageCopy copy{};
  copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copy.imageSubresource.layerCount = 1;
  copy.imageExtent = {kW, kH, 1};
  vkCmdCopyBufferToImage(cmd, staging, noExitImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                         &copy);

  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &barrier);
  vkEndCommandBuffer(cmd);

  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  vkQueueSubmit(vk_.graphicsQueue(), 1, &submit, VK_NULL_HANDLE);
  vkQueueWaitIdle(vk_.graphicsQueue());

  vkDestroyCommandPool(vk_.device(), uploadPool, nullptr);
  vmaDestroyBuffer(vk_.allocator(), staging, stagingAlloc);

  VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  viewInfo.image = noExitImage_;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
  viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  viewInfo.subresourceRange.levelCount = 1;
  viewInfo.subresourceRange.layerCount = 1;
  checkVk(vkCreateImageView(vk_.device(), &viewInfo, nullptr, &noExitView_), "noexit view");

  VkSamplerCreateInfo samp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  samp.magFilter = VK_FILTER_LINEAR;
  samp.minFilter = VK_FILTER_LINEAR;
  samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  checkVk(vkCreateSampler(vk_.device(), &samp, nullptr, &noExitSampler_), "noexit sampler");

  bindNoExitImGui();
  return true;
}

void App::destroyNoExitSign() {
  if (noExitDs_) {
    ImGui_ImplVulkan_RemoveTexture(noExitDs_);
    noExitDs_ = VK_NULL_HANDLE;
  }
  if (noExitSampler_) {
    vkDestroySampler(vk_.device(), noExitSampler_, nullptr);
    noExitSampler_ = VK_NULL_HANDLE;
  }
  if (noExitView_) {
    vkDestroyImageView(vk_.device(), noExitView_, nullptr);
    noExitView_ = VK_NULL_HANDLE;
  }
  if (noExitImage_) {
    vmaDestroyImage(vk_.allocator(), noExitImage_, noExitAlloc_);
    noExitImage_ = VK_NULL_HANDLE;
    noExitAlloc_ = VK_NULL_HANDLE;
  }
}

void App::onPresetChanged() {
  vkDeviceWaitIdle(vk_.device());
  const bool quality = graphics_.preset == GraphicsPreset::Quality;
  quality_.setEnabled(vk_, meshPass_, mainPass_, quality);
  initImGui(quality ? quality_.uiRenderPass() : mainPass_);
  lastPreset_ = graphics_.preset;
}

void App::handleResize() {
  vk_.recreateSwapchain();
  createDepthResources();
  createFramebuffers();
  if (quality_.enabled()) {
    quality_.resize(vk_, meshPass_, mainPass_);
    initImGui(quality_.uiRenderPass());
  }
}

void App::drawUi(float fps) {
  const bool showNoExit =
      noExitDs_ && inNoExitZone(camera_.position.x, camera_.position.z);
  if (!showHud_ && !showNoExit) {
    skipImGui_ = true;
    return;
  }
  skipImGui_ = false;

  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();

  if (showHud_) {
    ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 320), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Shays World VK")) {
      ImGui::Text("FPS: %.1f (%.2f ms)", fps, fps > 0.0f ? 1000.0f / fps : 0.0f);
      ImGui::Text("Pos: %.1f, %.1f, %.1f", camera_.position.x, camera_.position.y,
                  camera_.position.z);
      ImGui::Text("Scene: %zu verts / %u draws (bindless)", scene_.vertices.size(),
                  meshPass_.drawCount());
      ImGui::Separator();
      ImGui::Text("Preset: %s", graphics_.presetName().data());
      if (ImGui::Button("Toggle Performance / Quality (F1)")) {
        graphics_.togglePreset();
      }
      ImGui::TextUnformatted(
          "WASD walk | Mouse look | F2 cursor | F3 free-fly | F1 Quality | F4 HUD");
      ImGui::Text("Mode: %s | Collision: %s", camera_.freeFly ? "free-fly" : "walk",
                  collision_.aabbs().empty() ? "off" : "plains+AABB");
      ImGui::Text("Lighting: %s", graphics_.pbrIbl ? "PBR + analytic IBL" : "Lambert (fast)");
      if (graphics_.dayNightCycle || graphics_.pbrIbl) {
        ImGui::SliderFloat("Time of day (h)", &graphics_.timeOfDayHours, 0.0f, 24.0f);
        ImGui::Checkbox("Auto progress time", &graphics_.timeOfDayAuto);
        if (graphics_.timeOfDayAuto) {
          ImGui::SliderFloat("Day speed (h/s)", &graphics_.timeOfDaySpeed, 0.01f, 0.5f, "%.3f");
        }
      }
      if (graphics_.preset == GraphicsPreset::Quality) {
        ImGui::SliderFloat("Bloom", &graphics_.bloomStrength, 0.0f, 1.0f);
        ImGui::SliderFloat("CA", &graphics_.chromaticAberrationStrength, 0.0f, 1.0f);
        ImGui::SliderFloat("Sharpen", &graphics_.sharpenStrength, 0.0f, 1.0f);
        ImGui::SliderFloat("Fog density", &graphics_.fogDensity, 0.0f, 0.002f, "%.5f");
        ImGui::SliderFloat("God rays", &graphics_.godrayIntensity, 0.0f, 1.0f);
        ImGui::SliderFloat("Exposure bias", &graphics_.exposureBias, -2.0f, 2.0f);
        ImGui::Checkbox("Rain (R)", &graphics_.rain);
        ImGui::Checkbox("Force lamps (L)", &graphics_.lightsForcedOn);
      }
      ImGui::Separator();
      ImGui::Text("FX:");
      ImGui::BulletText("PBR/IBL: %s", graphics_.pbrIbl ? "ON" : "off");
      ImGui::BulletText("Shadows (CSM): %s", graphics_.shadows ? "ON" : "off");
      ImGui::BulletText("Sky/Clouds: %s", graphics_.volumetricSky ? "ON" : "off");
      ImGui::BulletText("GTAO: %s", graphics_.ambientOcclusion ? "ON" : "off");
      ImGui::BulletText("God rays/Fog/AE: %s", graphics_.volumetrics ? "ON" : "off");
      ImGui::BulletText("Contact shadows: %s", graphics_.contactShadows ? "ON" : "off");
      ImGui::BulletText("Local lamps: %s", graphics_.localLights ? "ON" : "off");
      ImGui::BulletText("Rain/wet puddles: %s", graphics_.rain ? "ON" : "off");
      ImGui::BulletText("TAA/CA/Sharpen/Bloom: %s", graphics_.bloom ? "ON" : "off");
    }
    ImGui::End();
  }

  // OG DisplayNoExit — centred "area not modelled" banner.
  if (showNoExit) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // OG DisplayNoExit used display list 454 at native 256×64.
    constexpr float kBannerW = 256.0f;
    constexpr float kBannerH = 64.0f;
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##NoExit", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoInputs)) {
      ImGui::Image(reinterpret_cast<ImTextureID>(noExitDs_), ImVec2(kBannerW, kBannerH));
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
  }

  ImGui::Render();
}

void App::recordFrame(VkCommandBuffer cmd, uint32_t imageIndex) {
  const float aspect = vk_.swapchainExtent().height > 0
                           ? static_cast<float>(vk_.swapchainExtent().width) /
                                 static_cast<float>(vk_.swapchainExtent().height)
                           : 1.0f;
  const glm::mat4 proj = glm::perspective(glm::radians(70.0f), aspect, 0.1f, 2000.0f);
  glm::mat4 vulkanProj = proj;
  vulkanProj[1][1] *= -1.0f;
  const glm::mat4 view = camera_.viewMatrix();
  const glm::mat4 viewProj = vulkanProj * view;

  FrameLighting lighting{};
  lighting.cameraPos = camera_.position;
  lighting.qualityMode = graphics_.pbrIbl;

  const float hours = graphics_.pbrIbl ? graphics_.timeOfDayHours : 14.0f;
  const float dayT = hours / 24.0f;
  const float sunAngle = (dayT - 0.25f) * 6.2831853f;
  lighting.sunDir = glm::normalize(glm::vec3(std::cos(sunAngle) * 0.6f, -std::sin(sunAngle),
                                             std::sin(sunAngle * 0.35f) * 0.4f));
  if (lighting.sunDir.y > -0.05f) {
    lighting.sunIntensity = graphics_.pbrIbl ? 0.35f : 2.0f;
    lighting.sunColor = {0.55f, 0.65f, 0.95f};
    lighting.ambientSky = {0.05f, 0.07f, 0.12f};
    lighting.ambientGround = {0.02f, 0.02f, 0.03f};
  } else {
    const float height = std::clamp(-lighting.sunDir.y, 0.0f, 1.0f);
    lighting.sunIntensity = graphics_.pbrIbl ? (1.5f + 3.0f * height) : 2.5f;
    lighting.sunColor = glm::mix(glm::vec3(1.0f, 0.55f, 0.25f), glm::vec3(1.0f, 0.96f, 0.90f), height);
    lighting.ambientSky =
        glm::mix(glm::vec3(0.55f, 0.35f, 0.25f), glm::vec3(0.45f, 0.58f, 0.85f), height);
    lighting.ambientGround = {0.16f, 0.14f, 0.10f};
  }

  if (graphics_.preset == GraphicsPreset::Quality && quality_.enabled()) {
    QualityRenderer::CameraFrame cam{};
    cam.view = view;
    cam.proj = vulkanProj;
    cam.invViewProj = glm::inverse(viewProj);
    cam.position = camera_.position;
    cam.nearPlane = 0.1f;
    cam.farPlane = 2000.0f;
    quality_.render(vk_, meshPass_, cmd, imageIndex, graphics_, cam, lighting);
    quality_.beginUiPass(cmd, imageIndex);
    if (!skipImGui_) {
      ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    }
    quality_.endUiPass(cmd);
    return;
  }

  // Performance path — direct swapchain Lambert
  VkClearValue clears[2]{};
  clears[0].color = {{97.0f / 255.0f, 140.0f / 255.0f, 185.0f / 255.0f, 1.0f}};
  clears[1].depthStencil = {1.0f, 0};

  VkRenderPassBeginInfo rpBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rpBegin.renderPass = mainPass_;
  rpBegin.framebuffer = framebuffers_[imageIndex];
  rpBegin.renderArea.extent = vk_.swapchainExtent();
  rpBegin.clearValueCount = 2;
  rpBegin.pClearValues = clears;

  vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
  lighting.linearHdr = false;
  lighting.shadowsEnabled = false;
  meshPass_.updateFrame(vk_, lighting, viewProj);
  meshPass_.draw(cmd, vk_.swapchainExtent(), false);
  if (!skipImGui_) {
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
  }
  vkCmdEndRenderPass(cmd);
}

void App::run() {
  using clock = std::chrono::steady_clock;
  auto last = clock::now();
  float fpsSmooth = 0.0f;

  while (!glfwWindowShouldClose(window_)) {
    glfwPollEvents();

    if (glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
      glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }

    static bool f1WasDown = false;
    const bool f1Down = glfwGetKey(window_, GLFW_KEY_F1) == GLFW_PRESS;
    if (f1Down && !f1WasDown) {
      graphics_.togglePreset();
    }
    f1WasDown = f1Down;

    static bool rWasDown = false;
    const bool rDown = glfwGetKey(window_, GLFW_KEY_R) == GLFW_PRESS;
    if (rDown && !rWasDown && graphics_.preset == GraphicsPreset::Quality) {
      graphics_.rain = !graphics_.rain;
    }
    rWasDown = rDown;

    static bool lWasDown = false;
    const bool lDown = glfwGetKey(window_, GLFW_KEY_L) == GLFW_PRESS;
    if (lDown && !lWasDown && graphics_.preset == GraphicsPreset::Quality) {
      graphics_.lightsForcedOn = !graphics_.lightsForcedOn;
    }
    lWasDown = lDown;

    if (graphics_.preset != lastPreset_) {
      onPresetChanged();
    }

    static bool f2WasDown = false;
    const bool f2Down = glfwGetKey(window_, GLFW_KEY_F2) == GLFW_PRESS;
    if (f2Down && !f2WasDown) {
      cursorCaptured_ = !cursorCaptured_;
      glfwSetInputMode(window_, GLFW_CURSOR,
                       cursorCaptured_ ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
      glfwGetCursorPos(window_, &lastMouseX_, &lastMouseY_);
    }
    f2WasDown = f2Down;

    static bool f3WasDown = false;
    const bool f3Down = glfwGetKey(window_, GLFW_KEY_F3) == GLFW_PRESS;
    if (f3Down && !f3WasDown) {
      camera_.freeFly = !camera_.freeFly;
    }
    f3WasDown = f3Down;

    static bool f4WasDown = false;
    const bool f4Down = glfwGetKey(window_, GLFW_KEY_F4) == GLFW_PRESS;
    if (f4Down && !f4WasDown) {
      showHud_ = !showHud_;
    }
    f4WasDown = f4Down;

    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(window_, &mx, &my);
    float mdx = 0.0f, mdy = 0.0f;
    if (cursorCaptured_) {
      mdx = static_cast<float>(mx - lastMouseX_);
      mdy = static_cast<float>(my - lastMouseY_);
    }
    lastMouseX_ = mx;
    lastMouseY_ = my;

    auto now = clock::now();
    float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    if (dt > 0.1f) dt = 0.1f;

    const bool stepped = camera_.update(
        dt, glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS,
        glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS,
        glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS,
        glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS,
        glfwGetKey(window_, GLFW_KEY_SPACE) == GLFW_PRESS,
        glfwGetKey(window_, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS, mdx, mdy);
    if (stepped) {
      stepAudio_.play();
    }

    if (graphics_.timeOfDayAuto &&
        (graphics_.dayNightCycle || graphics_.pbrIbl ||
         graphics_.preset == GraphicsPreset::Quality)) {
      graphics_.timeOfDayHours =
          std::fmod(graphics_.timeOfDayHours + dt * graphics_.timeOfDaySpeed, 24.0f);
      if (graphics_.timeOfDayHours < 0.0f) {
        graphics_.timeOfDayHours += 24.0f;
      }
    }

    if (dt > 0.0f) {
      fpsSmooth = fpsSmooth * 0.9f + (1.0f / dt) * 0.1f;
      fps_ = fpsSmooth;
    }

    if (std::chrono::duration<float>(now - lastTitleUpdate_).count() > 0.5f) {
      lastTitleUpdate_ = now;
      std::string title = "Shays World VK — ";
      title += graphics_.presetName();
      title += " — ";
      title += std::to_string(static_cast<int>(fps_ + 0.5f));
      title += " FPS";
      glfwSetWindowTitle(window_, title.c_str());
    }

    if (framebufferResized_) {
      framebufferResized_ = false;
      handleResize();
    }

    drawUi(fps_);

    uint32_t imageIndex = 0;
    bool outOfDate = false;
    VkCommandBuffer cmd = vk_.beginFrame(imageIndex, outOfDate);
    if (outOfDate || cmd == VK_NULL_HANDLE) {
      handleResize();
      continue;
    }

    recordFrame(cmd, imageIndex);
    vk_.endFrame(imageIndex, outOfDate);
    if (outOfDate) {
      handleResize();
    }
  }
}

void App::shutdown() {
  stepAudio_.shutdown();
  if (vk_.device() != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(vk_.device());
  }
  quality_.shutdown(vk_);
  meshPass_.shutdown(vk_);
  destroyNoExitSign();
  shutdownImGui();
  destroyFramebuffers();
  destroyDepthResources();
  if (mainPass_) {
    vkDestroyRenderPass(vk_.device(), mainPass_, nullptr);
    mainPass_ = VK_NULL_HANDLE;
  }
  vk_.shutdown();
  if (window_) {
    glfwDestroyWindow(window_);
    window_ = nullptr;
  }
  glfwTerminate();
}
