#include "render/quality_renderer.h"

#include "render/local_lights.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool ok, const char* msg) {
  if (!ok) throw std::runtime_error(msg);
}
void checkVk(VkResult r, const char* msg) { check(r == VK_SUCCESS, msg); }

struct SkyPush {
  glm::mat4 invViewProj;
  glm::vec4 cameraPos;
  glm::vec4 sunDir;
  glm::vec4 sunColor;
  glm::vec4 params;
};
struct AoPush {
  glm::mat4 proj;
  glm::mat4 invProj;
  glm::vec4 params;
};
struct PostPush {
  glm::vec4 params;
  glm::vec4 params2;
  glm::vec4 sunScreen;
  glm::vec4 fogColor;
};
struct PostMatricesUBO {
  glm::mat4 invViewProj{1.0f};
  glm::mat4 viewProj{1.0f};
  glm::vec4 sunDirIntensity{0.0f, -1.0f, 0.0f, 1.0f}; // xyz dir (toward ground), w intensity
  glm::vec4 sunColor{1.0f};
};
struct TaaPush {
  glm::mat4 reprojection;
  glm::vec4 params;
};
struct BlitPush {
  glm::vec4 params;  // x=ca y=sharpen z=bloom w=unused
  glm::vec4 params2;
};
struct RainPush {
  glm::mat4 viewProj;
  glm::vec4 camPos;
  glm::vec4 camRight;
  glm::vec4 params;
};

VkImageView makeView(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspect,
                     uint32_t layers, uint32_t baseLayer = 0) {
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = image;
  view.viewType = layers > 1 && baseLayer == 0 && layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                             : VK_IMAGE_VIEW_TYPE_2D;
  if (layers == 1) view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  if (baseLayer == 0 && layers > 1) view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  if (layers == 1) {
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  }
  view.format = format;
  view.subresourceRange.aspectMask = aspect;
  view.subresourceRange.baseArrayLayer = baseLayer;
  view.subresourceRange.layerCount = layers;
  view.subresourceRange.levelCount = 1;
  VkImageView out = VK_NULL_HANDLE;
  checkVk(vkCreateImageView(device, &view, nullptr, &out), "view");
  return out;
}

} // namespace

VkShaderModule QualityRenderer::loadShader(VkDevice device, const std::string& path) {
  std::ifstream file(path, std::ios::ate | std::ios::binary);
  check(static_cast<bool>(file), path.c_str());
  size_t size = static_cast<size_t>(file.tellg());
  std::vector<char> buffer(size);
  file.seekg(0);
  file.read(buffer.data(), static_cast<std::streamsize>(size));
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = buffer.size();
  info.pCode = reinterpret_cast<const uint32_t*>(buffer.data());
  VkShaderModule module = VK_NULL_HANDLE;
  checkVk(vkCreateShaderModule(device, &info, nullptr, &module), "shader");
  return module;
}

bool QualityRenderer::loadFsVertFrag(VulkanContext& vk, const char* fragName, VkShaderModule& vert,
                                     VkShaderModule& frag) {
  const char* bases[] = {SHAYS_SHADER_DIR "/", "shaders/"};
  for (const char* base : bases) {
    std::string vp = std::string(base) + "fullscreen.vert.spv";
    std::ifstream t(vp, std::ios::binary);
    if (!t) continue;
    vert = loadShader(vk.device(), vp);
    frag = loadShader(vk.device(), std::string(base) + fragName);
    return true;
  }
  return false;
}

bool QualityRenderer::init(VulkanContext& vk) {
  extent_ = vk.swapchainExtent();
  return true;
}

void QualityRenderer::shutdown(VulkanContext& vk) { destroyResources(vk); }

void QualityRenderer::setEnabled(VulkanContext& vk, MeshPass& meshes, VkRenderPass swapchainPass,
                                 bool enabled) {
  if (enabled == enabled_) {
    if (enabled && (extent_.width != vk.swapchainExtent().width ||
                    extent_.height != vk.swapchainExtent().height)) {
      resize(vk, meshes, swapchainPass);
    }
    return;
  }
  vkDeviceWaitIdle(vk.device());
  if (!enabled) {
    destroyResources(vk);
    meshes.setShadowMap(vk, VK_NULL_HANDLE, VK_NULL_HANDLE);
    meshes.setLampShadowAtlas(vk, VK_NULL_HANDLE, VK_NULL_HANDLE);
    enabled_ = false;
    prevCamPosValid_ = false;
    return;
  }
  extent_ = vk.swapchainExtent();
  check(createResources(vk, meshes, swapchainPass), "quality resources");
  enabled_ = true;
}

void QualityRenderer::resize(VulkanContext& vk, MeshPass& meshes, VkRenderPass swapchainPass) {
  if (!enabled_) return;
  vkDeviceWaitIdle(vk.device());
  destroyResources(vk);
  extent_ = vk.swapchainExtent();
  check(createResources(vk, meshes, swapchainPass), "quality resize");
}

void QualityRenderer::destroyResources(VulkanContext& vk) {
  auto wipeImg = [&](VkImage& i, VmaAllocation& a, VkImageView& v) {
    if (v) vkDestroyImageView(vk.device(), v, nullptr);
    if (i) vmaDestroyImage(vk.allocator(), i, a);
    i = VK_NULL_HANDLE;
    a = VK_NULL_HANDLE;
    v = VK_NULL_HANDLE;
  };

  if (hdrFb_) vkDestroyFramebuffer(vk.device(), hdrFb_, nullptr);
  hdrFb_ = VK_NULL_HANDLE;
  wipeImg(hdrColor_, hdrColorAlloc_, hdrColorView_);
  wipeImg(hdrDepth_, hdrDepthAlloc_, hdrDepthView_);
  if (hdrPass_) vkDestroyRenderPass(vk.device(), hdrPass_, nullptr);
  hdrPass_ = VK_NULL_HANDLE;

  for (auto& fb : shadowFbs_) {
    if (fb) vkDestroyFramebuffer(vk.device(), fb, nullptr);
    fb = VK_NULL_HANDLE;
  }
  for (auto& v : shadowLayerViews_) {
    if (v) vkDestroyImageView(vk.device(), v, nullptr);
    v = VK_NULL_HANDLE;
  }
  if (shadowArrayView_) vkDestroyImageView(vk.device(), shadowArrayView_, nullptr);
  shadowArrayView_ = VK_NULL_HANDLE;
  if (shadowImage_) vmaDestroyImage(vk.allocator(), shadowImage_, shadowAlloc_);
  shadowImage_ = VK_NULL_HANDLE;
  shadowAlloc_ = VK_NULL_HANDLE;
  if (shadowSampler_) vkDestroySampler(vk.device(), shadowSampler_, nullptr);
  shadowSampler_ = VK_NULL_HANDLE;
  if (shadowPass_) vkDestroyRenderPass(vk.device(), shadowPass_, nullptr);
  shadowPass_ = VK_NULL_HANDLE;

  if (aoFb_) vkDestroyFramebuffer(vk.device(), aoFb_, nullptr);
  if (aoBlurFb_) vkDestroyFramebuffer(vk.device(), aoBlurFb_, nullptr);
  aoFb_ = aoBlurFb_ = VK_NULL_HANDLE;
  wipeImg(aoImage_, aoAlloc_, aoView_);
  wipeImg(aoBlurImage_, aoBlurAlloc_, aoBlurView_);
  wipeImg(dummyAo_, dummyAoAlloc_, dummyAoView_);
  if (aoPass_) vkDestroyRenderPass(vk.device(), aoPass_, nullptr);
  aoPass_ = VK_NULL_HANDLE;

  if (skyPipeline_) vkDestroyPipeline(vk.device(), skyPipeline_, nullptr);
  if (skyLayout_) vkDestroyPipelineLayout(vk.device(), skyLayout_, nullptr);
  skyPipeline_ = VK_NULL_HANDLE;
  skyLayout_ = VK_NULL_HANDLE;

  if (aoPipeline_) vkDestroyPipeline(vk.device(), aoPipeline_, nullptr);
  if (aoLayout_) vkDestroyPipelineLayout(vk.device(), aoLayout_, nullptr);
  if (aoSetLayout_) vkDestroyDescriptorSetLayout(vk.device(), aoSetLayout_, nullptr);
  aoPipeline_ = VK_NULL_HANDLE;
  aoLayout_ = VK_NULL_HANDLE;
  aoSetLayout_ = VK_NULL_HANDLE;

  if (aoBlurPipeline_) vkDestroyPipeline(vk.device(), aoBlurPipeline_, nullptr);
  if (aoBlurLayout_) vkDestroyPipelineLayout(vk.device(), aoBlurLayout_, nullptr);
  if (aoBlurSetLayout_) vkDestroyDescriptorSetLayout(vk.device(), aoBlurSetLayout_, nullptr);
  aoBlurPipeline_ = VK_NULL_HANDLE;
  aoBlurLayout_ = VK_NULL_HANDLE;
  aoBlurSetLayout_ = VK_NULL_HANDLE;

  if (postPipeline_) vkDestroyPipeline(vk.device(), postPipeline_, nullptr);
  if (postLayout_) vkDestroyPipelineLayout(vk.device(), postLayout_, nullptr);
  if (postSetLayout_) vkDestroyDescriptorSetLayout(vk.device(), postSetLayout_, nullptr);
  postPipeline_ = VK_NULL_HANDLE;
  postLayout_ = VK_NULL_HANDLE;
  postSetLayout_ = VK_NULL_HANDLE;
  if (postUbo_) {
    vmaDestroyBuffer(vk.allocator(), postUbo_, postUboAlloc_);
    postUbo_ = VK_NULL_HANDLE;
    postUboAlloc_ = VK_NULL_HANDLE;
    postUboMapped_ = nullptr;
  }

  if (taaPipeline_) vkDestroyPipeline(vk.device(), taaPipeline_, nullptr);
  if (taaLayout_) vkDestroyPipelineLayout(vk.device(), taaLayout_, nullptr);
  if (taaSetLayout_) vkDestroyDescriptorSetLayout(vk.device(), taaSetLayout_, nullptr);
  taaPipeline_ = VK_NULL_HANDLE;
  taaLayout_ = VK_NULL_HANDLE;
  taaSetLayout_ = VK_NULL_HANDLE;

  if (blitPipeline_) vkDestroyPipeline(vk.device(), blitPipeline_, nullptr);
  if (blitLayout_) vkDestroyPipelineLayout(vk.device(), blitLayout_, nullptr);
  if (blitSetLayout_) vkDestroyDescriptorSetLayout(vk.device(), blitSetLayout_, nullptr);
  blitPipeline_ = VK_NULL_HANDLE;
  blitLayout_ = VK_NULL_HANDLE;
  blitSetLayout_ = VK_NULL_HANDLE;

  if (rainPipeline_) vkDestroyPipeline(vk.device(), rainPipeline_, nullptr);
  if (rainLayout_) vkDestroyPipelineLayout(vk.device(), rainLayout_, nullptr);
  rainPipeline_ = VK_NULL_HANDLE;
  rainLayout_ = VK_NULL_HANDLE;
  if (rainQuadBuf_) {
    vmaDestroyBuffer(vk.allocator(), rainQuadBuf_, rainQuadAlloc_);
    rainQuadBuf_ = VK_NULL_HANDLE;
    rainQuadAlloc_ = VK_NULL_HANDLE;
  }
  if (rainInstanceBuf_) {
    vmaDestroyBuffer(vk.allocator(), rainInstanceBuf_, rainInstanceAlloc_);
    rainInstanceBuf_ = VK_NULL_HANDLE;
    rainInstanceAlloc_ = VK_NULL_HANDLE;
    rainInstanceMapped_ = nullptr;
  }
  rainParticles_.clear();

  if (ldrFb_) vkDestroyFramebuffer(vk.device(), ldrFb_, nullptr);
  ldrFb_ = VK_NULL_HANDLE;
  wipeImg(ldrColor_, ldrColorAlloc_, ldrColorView_);
  if (ldrPass_) vkDestroyRenderPass(vk.device(), ldrPass_, nullptr);
  ldrPass_ = VK_NULL_HANDLE;

  for (int i = 0; i < 2; ++i) {
    if (historyFb_[i]) vkDestroyFramebuffer(vk.device(), historyFb_[i], nullptr);
    historyFb_[i] = VK_NULL_HANDLE;
    wipeImg(historyColor_[i], historyAlloc_[i], historyView_[i]);
  }
  if (taaPass_) vkDestroyRenderPass(vk.device(), taaPass_, nullptr);
  taaPass_ = VK_NULL_HANDLE;
  historyWrite_ = 0;
  historyValid_ = false;
  jitterIndex_ = 0;

  for (auto fb : presentFramebuffers_) {
    if (fb) vkDestroyFramebuffer(vk.device(), fb, nullptr);
  }
  presentFramebuffers_.clear();
  if (presentPass_) vkDestroyRenderPass(vk.device(), presentPass_, nullptr);
  presentPass_ = VK_NULL_HANDLE;

  if (lampAtlasFb_) vkDestroyFramebuffer(vk.device(), lampAtlasFb_, nullptr);
  lampAtlasFb_ = VK_NULL_HANDLE;
  if (lampAtlasView_) vkDestroyImageView(vk.device(), lampAtlasView_, nullptr);
  lampAtlasView_ = VK_NULL_HANDLE;
  if (lampAtlas_) vmaDestroyImage(vk.allocator(), lampAtlas_, lampAtlasAlloc_);
  lampAtlas_ = VK_NULL_HANDLE;
  lampAtlasAlloc_ = VK_NULL_HANDLE;
  if (lampAtlasSampler_) vkDestroySampler(vk.device(), lampAtlasSampler_, nullptr);
  lampAtlasSampler_ = VK_NULL_HANDLE;

  for (auto fb : uiFramebuffers_) {
    if (fb) vkDestroyFramebuffer(vk.device(), fb, nullptr);
  }
  uiFramebuffers_.clear();
  if (uiPass_) vkDestroyRenderPass(vk.device(), uiPass_, nullptr);
  uiPass_ = VK_NULL_HANDLE;

  if (fsPool_) vkDestroyDescriptorPool(vk.device(), fsPool_, nullptr);
  fsPool_ = VK_NULL_HANDLE;
  if (linearSampler_) vkDestroySampler(vk.device(), linearSampler_, nullptr);
  linearSampler_ = VK_NULL_HANDLE;
  aoSet_ = aoBlurSet_ = postSet_ = taaSet_ = blitSet_ = VK_NULL_HANDLE;
}

bool QualityRenderer::createResources(VulkanContext& vk, MeshPass& meshes,
                                      VkRenderPass swapchainPass) {
  const uint32_t w = extent_.width;
  const uint32_t h = extent_.height;
  check(w > 0 && h > 0, "bad extent");

  VmaAllocationCreateInfo imgAlloc{};
  imgAlloc.usage = VMA_MEMORY_USAGE_AUTO;

  // --- HDR render pass ---
  {
    VkAttachmentDescription atts[2]{};
    atts[0].format = VK_FORMAT_R16G16B16A16_SFLOAT;
    atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    atts[1].format = VK_FORMAT_D32_SFLOAT;
    atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[1].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;
    sub.pDepthStencilAttachment = &depthRef;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep.dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dep.dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkSubpassDependency dep2{};
    dep2.srcSubpass = 0;
    dep2.dstSubpass = VK_SUBPASS_EXTERNAL;
    dep2.srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep2.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep2.srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkSubpassDependency deps[] = {dep, dep2};
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 2;
    rp.pAttachments = atts;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies = deps;
    checkVk(vkCreateRenderPass(vk.device(), &rp, nullptr, &hdrPass_), "hdr pass");
  }

  auto createColor = [&](VkFormat fmt, VkImageUsageFlags usage, VkImage& img, VmaAllocation& alloc,
                         VkImageView& view, uint32_t ww, uint32_t hh) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {ww, hh, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    checkVk(vmaCreateImage(vk.allocator(), &ii, &imgAlloc, &img, &alloc, nullptr), "img");
    VkImageAspectFlags aspect =
        (fmt == VK_FORMAT_D32_SFLOAT) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    checkVk(vkCreateImageView(vk.device(), &vi, nullptr, &view), "img view");
  };

  createColor(VK_FORMAT_R16G16B16A16_SFLOAT,
              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, hdrColor_,
              hdrColorAlloc_, hdrColorView_, w, h);
  createColor(VK_FORMAT_D32_SFLOAT,
              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, hdrDepth_,
              hdrDepthAlloc_, hdrDepthView_, w, h);

  {
    VkImageView atts[] = {hdrColorView_, hdrDepthView_};
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = hdrPass_;
    fb.attachmentCount = 2;
    fb.pAttachments = atts;
    fb.width = w;
    fb.height = h;
    fb.layers = 1;
    checkVk(vkCreateFramebuffer(vk.device(), &fb, nullptr, &hdrFb_), "hdr fb");
  }

  // --- Shadow pass ---
  {
    VkAttachmentDescription att{};
    att.format = VK_FORMAT_D32_SFLOAT;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference depthRef{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.pDepthStencilAttachment = &depthRef;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    checkVk(vkCreateRenderPass(vk.device(), &rp, nullptr, &shadowPass_), "shadow pass");

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_D32_SFLOAT;
    ii.extent = {kShadowSize, kShadowSize, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = kCascades;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    checkVk(vmaCreateImage(vk.allocator(), &ii, &imgAlloc, &shadowImage_, &shadowAlloc_, nullptr),
            "shadow img");

    VkImageViewCreateInfo arrayView{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    arrayView.image = shadowImage_;
    arrayView.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    arrayView.format = VK_FORMAT_D32_SFLOAT;
    arrayView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    arrayView.subresourceRange.levelCount = 1;
    arrayView.subresourceRange.layerCount = kCascades;
    checkVk(vkCreateImageView(vk.device(), &arrayView, nullptr, &shadowArrayView_), "shadow array");

    for (uint32_t i = 0; i < kCascades; ++i) {
      VkImageViewCreateInfo lv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      lv.image = shadowImage_;
      lv.viewType = VK_IMAGE_VIEW_TYPE_2D;
      lv.format = VK_FORMAT_D32_SFLOAT;
      lv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
      lv.subresourceRange.baseArrayLayer = i;
      lv.subresourceRange.layerCount = 1;
      lv.subresourceRange.levelCount = 1;
      checkVk(vkCreateImageView(vk.device(), &lv, nullptr, &shadowLayerViews_[i]), "shadow layer");
      VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fb.renderPass = shadowPass_;
      fb.attachmentCount = 1;
      fb.pAttachments = &shadowLayerViews_[i];
      fb.width = kShadowSize;
      fb.height = kShadowSize;
      fb.layers = 1;
      checkVk(vkCreateFramebuffer(vk.device(), &fb, nullptr, &shadowFbs_[i]), "shadow fb");
    }

    // NEAREST: manual PCF compares raw depths. LINEAR depth filtering breaks compares.
    VkSamplerCreateInfo samp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samp.magFilter = VK_FILTER_NEAREST;
    samp.minFilter = VK_FILTER_NEAREST;
    samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samp.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    checkVk(vkCreateSampler(vk.device(), &samp, nullptr, &shadowSampler_), "shadow samp");
  }

  // --- Lamp spot-shadow atlas (4x2 of 1024) ---
  {
    createColor(VK_FORMAT_D32_SFLOAT,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, lampAtlas_,
                lampAtlasAlloc_, lampAtlasView_, kLampAtlasW, kLampAtlasH);
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = shadowPass_;
    fb.attachmentCount = 1;
    fb.pAttachments = &lampAtlasView_;
    fb.width = kLampAtlasW;
    fb.height = kLampAtlasH;
    fb.layers = 1;
    checkVk(vkCreateFramebuffer(vk.device(), &fb, nullptr, &lampAtlasFb_), "lamp atlas fb");
    VkSamplerCreateInfo samp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samp.magFilter = VK_FILTER_NEAREST;
    samp.minFilter = VK_FILTER_NEAREST;
    samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samp.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    checkVk(vkCreateSampler(vk.device(), &samp, nullptr, &lampAtlasSampler_), "lamp atlas samp");
  }

  // --- AO pass (R8) ---
  {
    VkAttachmentDescription att{};
    att.format = VK_FORMAT_R8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    checkVk(vkCreateRenderPass(vk.device(), &rp, nullptr, &aoPass_), "ao pass");

    const uint32_t aw = std::max(1u, w / 2);
    const uint32_t ah = std::max(1u, h / 2);
    createColor(VK_FORMAT_R8_UNORM,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, aoImage_, aoAlloc_,
                aoView_, aw, ah);
    createColor(VK_FORMAT_R8_UNORM,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, aoBlurImage_,
                aoBlurAlloc_, aoBlurView_, aw, ah);
    createColor(VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                dummyAo_, dummyAoAlloc_, dummyAoView_, 1, 1);

    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = aoPass_;
    fb.attachmentCount = 1;
    fb.pAttachments = &aoView_;
    fb.width = aw;
    fb.height = ah;
    fb.layers = 1;
    checkVk(vkCreateFramebuffer(vk.device(), &fb, nullptr, &aoFb_), "ao fb");
    fb.pAttachments = &aoBlurView_;
    checkVk(vkCreateFramebuffer(vk.device(), &fb, nullptr, &aoBlurFb_), "ao blur fb");
  }

  // --- LDR tonemap pass (offscreen) ---
  {
    VkAttachmentDescription att{};
    att.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies = deps;
    checkVk(vkCreateRenderPass(vk.device(), &rp, nullptr, &ldrPass_), "ldr pass");
    checkVk(vkCreateRenderPass(vk.device(), &rp, nullptr, &taaPass_), "taa pass");

    createColor(VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, ldrColor_,
                ldrColorAlloc_, ldrColorView_, w, h);
    for (int i = 0; i < 2; ++i) {
      createColor(VK_FORMAT_R16G16B16A16_SFLOAT,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, historyColor_[i],
                  historyAlloc_[i], historyView_[i], w, h);
      VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fb.renderPass = taaPass_;
      fb.attachmentCount = 1;
      fb.pAttachments = &historyView_[i];
      fb.width = w;
      fb.height = h;
      fb.layers = 1;
      checkVk(vkCreateFramebuffer(vk.device(), &fb, nullptr, &historyFb_[i]), "history fb");
    }
    {
      VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fb.renderPass = ldrPass_;
      fb.attachmentCount = 1;
      fb.pAttachments = &ldrColorView_;
      fb.width = w;
      fb.height = h;
      fb.layers = 1;
      checkVk(vkCreateFramebuffer(vk.device(), &fb, nullptr, &ldrFb_), "ldr fb");
    }
    historyValid_ = false;
    historyWrite_ = 0;
    jitterIndex_ = 0;
  }

  // --- Present blit (swapchain format) ---
  {
    VkAttachmentDescription att{};
    att.format = vk.swapchainFormat();
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    checkVk(vkCreateRenderPass(vk.device(), &rp, nullptr, &presentPass_), "present pass");

    presentFramebuffers_.resize(vk.swapchainImageViews().size());
    for (size_t i = 0; i < presentFramebuffers_.size(); ++i) {
      VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fb.renderPass = presentPass_;
      fb.attachmentCount = 1;
      fb.pAttachments = &vk.swapchainImageViews()[i];
      fb.width = w;
      fb.height = h;
      fb.layers = 1;
      checkVk(vkCreateFramebuffer(vk.device(), &fb, nullptr, &presentFramebuffers_[i]), "present fb");
    }
  }

  // --- UI pass (load color for ImGui) ---
  {
    VkAttachmentDescription att{};
    att.format = vk.swapchainFormat();
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    checkVk(vkCreateRenderPass(vk.device(), &rp, nullptr, &uiPass_), "ui pass");

    uiFramebuffers_.resize(vk.swapchainImageViews().size());
    for (size_t i = 0; i < uiFramebuffers_.size(); ++i) {
      VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fb.renderPass = uiPass_;
      fb.attachmentCount = 1;
      fb.pAttachments = &vk.swapchainImageViews()[i];
      fb.width = w;
      fb.height = h;
      fb.layers = 1;
      checkVk(vkCreateFramebuffer(vk.device(), &fb, nullptr, &uiFramebuffers_[i]), "ui fb");
    }
  }

  VkSamplerCreateInfo samp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  samp.magFilter = VK_FILTER_LINEAR;
  samp.minFilter = VK_FILTER_LINEAR;
  samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  checkVk(vkCreateSampler(vk.device(), &samp, nullptr, &linearSampler_), "linear samp");

  // Descriptor pool for fullscreen sets
  VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 48},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4},
  };
  VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  poolInfo.maxSets = 24;
  poolInfo.poolSizeCount = 2;
  poolInfo.pPoolSizes = sizes;
  checkVk(vkCreateDescriptorPool(vk.device(), &poolInfo, nullptr, &fsPool_), "fs pool");

  auto makeFsPipeline = [&](const char* fragSpv, VkRenderPass pass, uint32_t pushBytes,
                            int samplers, VkDescriptorSetLayout& setLayout, VkPipelineLayout& layout,
                            VkPipeline& pipeline, bool depthTest, bool depthWrite) {
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    check(loadFsVertFrag(vk, fragSpv, vert, frag), fragSpv);

    if (samplers > 0) {
      std::vector<VkDescriptorSetLayoutBinding> binds(samplers);
      for (int i = 0; i < samplers; ++i) {
        binds[i].binding = static_cast<uint32_t>(i);
        binds[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
      }
      VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      li.bindingCount = static_cast<uint32_t>(binds.size());
      li.pBindings = binds.data();
      checkVk(vkCreateDescriptorSetLayout(vk.device(), &li, nullptr, &setLayout), "fs set");
    }

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = pushBytes;
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (samplers > 0) {
      pli.setLayoutCount = 1;
      pli.pSetLayouts = &setLayout;
    }
    if (pushBytes > 0) {
      pli.pushConstantRangeCount = 1;
      pli.pPushConstantRanges = &push;
    }
    checkVk(vkCreatePipelineLayout(vk.device(), &pli, nullptr, &layout), "fs layout");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = depthTest ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = depthWrite ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = layout;
    gp.renderPass = pass;
    checkVk(vkCreateGraphicsPipelines(vk.device(), VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline),
            "fs pipe");
    vkDestroyShaderModule(vk.device(), vert, nullptr);
    vkDestroyShaderModule(vk.device(), frag, nullptr);
  };

  VkDescriptorSetLayout unused = VK_NULL_HANDLE;
  makeFsPipeline("sky.frag.spv", hdrPass_, sizeof(SkyPush), 0, unused, skyLayout_, skyPipeline_,
                 false, false);
  makeFsPipeline("ssao.frag.spv", aoPass_, sizeof(AoPush), 1, aoSetLayout_, aoLayout_, aoPipeline_,
                 false, false);
  makeFsPipeline("ssao_blur.frag.spv", aoPass_, 0, 1, aoBlurSetLayout_, aoBlurLayout_,
                 aoBlurPipeline_, false, false);
  // Post pass: 3 samplers + matrices UBO (created below, not via makeFsPipeline).
  makeFsPipeline("taa.frag.spv", taaPass_, sizeof(TaaPush), 3, taaSetLayout_, taaLayout_,
                 taaPipeline_, false, false);
  makeFsPipeline("blit.frag.spv", presentPass_, sizeof(BlitPush), 2, blitSetLayout_, blitLayout_,
                 blitPipeline_, false, false);

  // --- Post pipeline (samplers + UBO) ---
  {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = sizeof(PostMatricesUBO);
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
               VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo ainfo{};
    checkVk(vmaCreateBuffer(vk.allocator(), &bi, &ai, &postUbo_, &postUboAlloc_, &ainfo),
            "post ubo");
    postUboMapped_ = ainfo.pMappedData;

    VkDescriptorSetLayoutBinding binds[4]{};
    for (int i = 0; i < 3; ++i) {
      binds[i].binding = static_cast<uint32_t>(i);
      binds[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      binds[i].descriptorCount = 1;
      binds[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    binds[3].binding = 3;
    binds[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binds[3].descriptorCount = 1;
    binds[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 4;
    li.pBindings = binds;
    checkVk(vkCreateDescriptorSetLayout(vk.device(), &li, nullptr, &postSetLayout_), "post set");

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(PostPush);
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &postSetLayout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    checkVk(vkCreatePipelineLayout(vk.device(), &pli, nullptr, &postLayout_), "post layout");

    // Reuse makeFsPipeline body by calling it would fight set layout — build pipe via temp helper.
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    check(loadFsVertFrag(vk, "post.frag.spv", vert, frag), "post.frag.spv");
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &att;
    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = postLayout_;
    gp.renderPass = ldrPass_;
    checkVk(vkCreateGraphicsPipelines(vk.device(), VK_NULL_HANDLE, 1, &gp, nullptr, &postPipeline_),
            "post pipe");
    vkDestroyShaderModule(vk.device(), vert, nullptr);
    vkDestroyShaderModule(vk.device(), frag, nullptr);
  }

  // --- Rain particle billboards (HDR transparent) ---
  {
    const char* bases[] = {SHAYS_SHADER_DIR "/", "shaders/"};
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    bool loaded = false;
    for (const char* base : bases) {
      std::ifstream t(std::string(base) + "rain.vert.spv", std::ios::binary);
      if (!t) continue;
      vert = loadShader(vk.device(), std::string(base) + "rain.vert.spv");
      frag = loadShader(vk.device(), std::string(base) + "rain.frag.spv");
      loaded = true;
      break;
    }
    check(loaded, "rain shaders");

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.size = sizeof(RainPush);
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    checkVk(vkCreatePipelineLayout(vk.device(), &pli, nullptr, &rainLayout_), "rain layout");

    VkVertexInputBindingDescription binds[2]{};
    binds[0].binding = 0;
    binds[0].stride = sizeof(glm::vec2);
    binds[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    binds[1].binding = 1;
    binds[1].stride = sizeof(glm::vec4);
    binds[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = 0;
    attrs[1].location = 1;
    attrs[1].binding = 1;
    attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[1].offset = 0;
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 2;
    vi.pVertexBindingDescriptions = binds;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = rainLayout_;
    gp.renderPass = hdrPass_;
    checkVk(vkCreateGraphicsPipelines(vk.device(), VK_NULL_HANDLE, 1, &gp, nullptr, &rainPipeline_),
            "rain pipe");
    vkDestroyShaderModule(vk.device(), vert, nullptr);
    vkDestroyShaderModule(vk.device(), frag, nullptr);

    const glm::vec2 quad[6] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f},
                               {-0.5f, -0.5f}, {0.5f, 0.5f},  {-0.5f, 0.5f}};
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = sizeof(quad);
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
               VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo ainfo{};
    checkVk(vmaCreateBuffer(vk.allocator(), &bi, &ai, &rainQuadBuf_, &rainQuadAlloc_, &ainfo),
            "rain quad");
    std::memcpy(ainfo.pMappedData, quad, sizeof(quad));

    bi.size = sizeof(glm::vec4) * kRainParticles;
    checkVk(vmaCreateBuffer(vk.allocator(), &bi, &ai, &rainInstanceBuf_, &rainInstanceAlloc_, &ainfo),
            "rain instances");
    rainInstanceMapped_ = ainfo.pMappedData;

    rainParticles_.resize(kRainParticles);
    auto hash01 = [](uint32_t x) {
      x ^= x >> 16;
      x *= 0x7feb352du;
      x ^= x >> 15;
      x *= 0x846ca68bu;
      x ^= x >> 16;
      return static_cast<float>(x) / static_cast<float>(0xffffffffu);
    };
    for (uint32_t i = 0; i < kRainParticles; ++i) {
      const float u = hash01(i * 3u + 1u);
      const float v = hash01(i * 7u + 3u);
      const float w = hash01(i * 11u + 5u);
      rainParticles_[i] = glm::vec4((u - 0.5f) * 36.0f, v * 24.0f - 2.0f, (w - 0.5f) * 36.0f,
                                    hash01(i * 13u + 9u));
    }
  }

  // Allocate descriptor sets
  {
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = fsPool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &aoSetLayout_;
    checkVk(vkAllocateDescriptorSets(vk.device(), &alloc, &aoSet_), "ao set");
    alloc.pSetLayouts = &aoBlurSetLayout_;
    checkVk(vkAllocateDescriptorSets(vk.device(), &alloc, &aoBlurSet_), "ao blur set");
    alloc.pSetLayouts = &postSetLayout_;
    checkVk(vkAllocateDescriptorSets(vk.device(), &alloc, &postSet_), "post set");
    alloc.pSetLayouts = &taaSetLayout_;
    checkVk(vkAllocateDescriptorSets(vk.device(), &alloc, &taaSet_), "taa set");
    alloc.pSetLayouts = &blitSetLayout_;
    checkVk(vkAllocateDescriptorSets(vk.device(), &alloc, &blitSet_), "blit set");
  }

  auto writeSampled = [&](VkDescriptorSet set, uint32_t binding, VkImageView view) {
    VkDescriptorImageInfo ii{};
    ii.sampler = linearSampler_;
    ii.imageView = view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = binding;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.descriptorCount = 1;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(vk.device(), 1, &w, 0, nullptr);
  };
  writeSampled(aoSet_, 0, hdrDepthView_);
  writeSampled(aoBlurSet_, 0, aoView_);
  writeSampled(postSet_, 0, hdrColorView_);
  writeSampled(postSet_, 1, aoBlurView_);
  writeSampled(postSet_, 2, hdrDepthView_);
  {
    VkDescriptorBufferInfo bi{};
    bi.buffer = postUbo_;
    bi.offset = 0;
    bi.range = sizeof(PostMatricesUBO);
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = postSet_;
    w.dstBinding = 3;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w.descriptorCount = 1;
    w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(vk.device(), 1, &w, 0, nullptr);
  }

  // Clear dummy AO to white
  {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = vk.graphicsQueueFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(vk.device(), &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(vk.device(), &ai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = dummyAo_;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);
    VkClearColorValue white{};
    white.float32[0] = 1.0f;
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    vkCmdClearColorImage(cmd, dummyAo_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(vk.graphicsQueue(), 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(vk.graphicsQueue());
    vkDestroyCommandPool(vk.device(), pool, nullptr);
  }

  meshes.ensureShadowPipeline(vk, shadowPass_);
  meshes.ensurePipelines(vk, swapchainPass, hdrPass_);
  meshes.setShadowMap(vk, shadowArrayView_, shadowSampler_);
  return true;
}

void QualityRenderer::beginUiPass(VkCommandBuffer cmd, uint32_t swapImageIndex) {
  if (!uiPass_ || swapImageIndex >= uiFramebuffers_.size()) return;
  VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rp.renderPass = uiPass_;
  rp.framebuffer = uiFramebuffers_[swapImageIndex];
  rp.renderArea.extent = extent_;
  vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
}

void QualityRenderer::endUiPass(VkCommandBuffer cmd) {
  vkCmdEndRenderPass(cmd);
}

void QualityRenderer::updateCascades(const CameraFrame& cam, const glm::vec3& sunDir,
                                     FrameLighting& lighting) {
  const float nearP = cam.nearPlane;
  const float farP = std::min(cam.farPlane, 600.0f);
  const float lambda = 0.65f;
  float splits[5]{};
  splits[0] = nearP;
  for (int i = 1; i < 5; ++i) {
    float p = static_cast<float>(i) / 4.0f;
    float logS = nearP * std::pow(farP / nearP, p);
    float uniS = nearP + (farP - nearP) * p;
    splits[i] = lambda * logS + (1.0f - lambda) * uniS;
  }
  lighting.cascadeSplits =
      glm::vec4(splits[1], splits[2], splits[3], splits[4]);

  glm::vec3 lightDir = glm::normalize(sunDir);
  if (std::abs(lightDir.y) < 0.05f) {
    lightDir = glm::normalize(glm::vec3(0.35f, -0.9f, 0.25f));
  }

  const float fov = glm::radians(70.0f);
  // Recover aspect from proj
  float aspect = 1.0f;
  if (std::abs(cam.proj[0][0]) > 1e-6f) {
    aspect = std::abs(cam.proj[1][1] / cam.proj[0][0]); // after Y flip both negative
  }

  glm::mat4 invView = glm::inverse(cam.view);

  for (int c = 0; c < 4; ++c) {
    float cn = splits[c];
    float cf = splits[c + 1];
    glm::mat4 cascadeProj = glm::perspective(fov, aspect, cn, cf);
    cascadeProj[1][1] *= -1.0f;
    glm::mat4 invCascade = glm::inverse(cascadeProj * cam.view);

    std::array<glm::vec3, 8> corners{};
    int idx = 0;
    for (int z = 0; z < 2; ++z) {
      for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
          glm::vec4 ndc(x ? 1.f : -1.f, y ? 1.f : -1.f, z ? 1.f : 0.f, 1.f);
          glm::vec4 world = invCascade * ndc;
          corners[idx++] = glm::vec3(world) / world.w;
        }
      }
    }

    glm::vec3 center(0.0f);
    for (auto& p : corners) center += p;
    center /= 8.0f;

    float radius = 0.0f;
    for (auto& p : corners) radius = std::max(radius, glm::length(p - center));
    radius = std::ceil(radius * 16.0f) / 16.0f;

    glm::vec3 up(0, 1, 0);
    if (std::abs(glm::dot(up, -lightDir)) > 0.99f) up = glm::vec3(0, 0, 1);
    glm::mat4 lightView = glm::lookAt(center - lightDir * radius * 2.0f, center, up);

    // Stabilize cascades by snapping the light-view origin to shadow-map texels.
    const float worldUnitsPerTexel = (radius * 2.0f) / static_cast<float>(kShadowSize);
    glm::vec3 shadowOrigin = glm::vec3(lightView * glm::vec4(center, 1.0f));
    shadowOrigin.x = std::floor(shadowOrigin.x / worldUnitsPerTexel) * worldUnitsPerTexel;
    shadowOrigin.y = std::floor(shadowOrigin.y / worldUnitsPerTexel) * worldUnitsPerTexel;
    glm::mat4 invLightView = glm::inverse(lightView);
    center = glm::vec3(invLightView * glm::vec4(shadowOrigin, 1.0f));
    lightView = glm::lookAt(center - lightDir * radius * 2.0f, center, up);

    glm::mat4 lightProj =
        glm::ortho(-radius, radius, -radius, radius, -radius * 4.0f, radius * 4.0f);
    lightProj[1][1] *= -1.0f;
    lighting.lightViewProj[c] = lightProj * lightView;
  }
}

void QualityRenderer::render(VulkanContext& vk, MeshPass& meshes, VkCommandBuffer cmd,
                             uint32_t swapImageIndex, const GraphicsSettings& gfx,
                             const CameraFrame& camIn, FrameLighting lighting) {
  if (!enabled_) return;

  CameraFrame cam = camIn;
  const glm::mat4 skyInvViewProj = camIn.invViewProj; // unjittered — keeps stars/clouds stable
  const bool useTaa = gfx.antiAliasing;
  glm::vec2 jitterPx(0.0f);
  if (useTaa) {
    jitterPx = halton(jitterIndex_++);
    // ~half-pixel jitter — enough for AA, less visible crawling.
    jitterPx = (jitterPx * 2.0f - 1.0f) * 0.5f;
    cam.proj[2][0] += jitterPx.x / static_cast<float>(extent_.width);
    cam.proj[2][1] -= jitterPx.y / static_cast<float>(extent_.height);
    cam.invViewProj = glm::inverse(cam.proj * cam.view);
  }

  lighting.linearHdr = true;
  lighting.shadowsEnabled = gfx.shadows;
  lighting.qualityMode = true;
  lighting.rainWet = gfx.rain ? 1.0f : 0.0f;
  updateCascades(cam, lighting.sunDir, lighting);

  // --- Local walkway lamps + spot atlas ---
  lighting.localLightCount = 0;
  lighting.lampShadowCount = 0;
  lighting.lampShadowsEnabled = false;
  lighting.localLightFade = 0.0f;
  if (gfx.localLights) {
    auto lamps = makeWalkwayLamps();
    const float sunY = -glm::normalize(lighting.sunDir).y; // height above horizon-ish
    float night = 1.0f - std::clamp((sunY + 0.05f) / 0.35f, 0.0f, 1.0f);
    if (gfx.lightsForcedOn) night = 1.0f;
    lighting.localLightFade = night;

    std::array<int, kWalkwayLampCount> order{};
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
      return glm::length(lamps[static_cast<size_t>(a)].position - cam.position) <
             glm::length(lamps[static_cast<size_t>(b)].position - cam.position);
    });

    lighting.localLightCount = static_cast<int>(kWalkwayLampCount);
    for (uint32_t i = 0; i < kWalkwayLampCount; ++i) {
      lighting.localLights[i] = lamps[static_cast<size_t>(order[i])];
    }

    const int shadowN =
        (gfx.shadows && night > 0.05f) ? static_cast<int>(kShadowedLampSlots) : 0;
    lighting.lampShadowCount = shadowN;
    lighting.lampShadowsEnabled = shadowN > 0;
    for (int i = 0; i < shadowN; ++i) {
      const LocalLight& L = lighting.localLights[static_cast<size_t>(i)];
      glm::vec3 up(0, 0, 1);
      if (std::abs(glm::dot(L.direction, up)) > 0.95f) up = glm::vec3(1, 0, 0);
      glm::mat4 view = glm::lookAt(L.position, L.position + L.direction * 5.0f, up);
      // Match shadow FOV to the outer spot cone (was 75° — much tighter than the light).
      const float halfAngle = std::acos(std::clamp(L.cosOuter, 0.05f, 0.95f));
      const float fovY = std::min(halfAngle * 2.0f + 0.12f, glm::radians(145.0f));
      glm::mat4 proj = glm::perspective(fovY, 1.0f, 0.55f, L.range);
      proj[1][1] *= -1.0f;
      lighting.lampViewProj[static_cast<size_t>(i)] = proj * view;
      const uint32_t tx = static_cast<uint32_t>(i) % 4u;
      const uint32_t ty = static_cast<uint32_t>(i) / 4u;
      lighting.lampTileScaleBias[static_cast<size_t>(i)] = glm::vec4(
          static_cast<float>(kLampTile) / static_cast<float>(kLampAtlasW),
          static_cast<float>(kLampTile) / static_cast<float>(kLampAtlasH),
          static_cast<float>(tx * kLampTile) / static_cast<float>(kLampAtlasW),
          static_cast<float>(ty * kLampTile) / static_cast<float>(kLampAtlasH));
    }
  }

  // Auto-exposure — keep night boost mild; walkway lamps already lift the path.
  {
    const float sunH = std::clamp(-glm::normalize(lighting.sunDir).y, -0.2f, 1.0f);
    float target = (sunH > 0.05f) ? 1.0f : glm::mix(1.35f, 1.05f, std::clamp((sunH + 0.05f) / 0.25f, 0.0f, 1.0f));
    target *= std::exp2(gfx.exposureBias);
    if (gfx.autoExposure) {
      exposure_ = glm::mix(exposure_, target, 0.08f);
    } else {
      exposure_ = std::exp2(gfx.exposureBias);
    }
  }
  rainTime_ += 1.0f / 60.0f;

  const glm::mat4 viewProj = cam.proj * cam.view;
  meshes.updateFrame(vk, lighting, viewProj);
  if (gfx.shadows) {
    meshes.setShadowMap(vk, shadowArrayView_, shadowSampler_);
  }
  if (lampAtlasView_) {
    meshes.setLampShadowAtlas(vk, lampAtlasView_, lampAtlasSampler_);
  }

  // 1) Shadow cascades
  if (gfx.shadows) {
    for (uint32_t i = 0; i < kCascades; ++i) {
      VkClearValue clear{};
      clear.depthStencil = {1.0f, 0};
      VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      rp.renderPass = shadowPass_;
      rp.framebuffer = shadowFbs_[i];
      rp.renderArea.extent = {kShadowSize, kShadowSize};
      rp.clearValueCount = 1;
      rp.pClearValues = &clear;
      vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
      meshes.drawDepth(cmd, lighting.lightViewProj[i], {kShadowSize, kShadowSize});
      vkCmdEndRenderPass(cmd);
    }
  }

  // 1b) Lamp spot shadows into atlas
  if (lighting.lampShadowsEnabled && lampAtlasFb_ && shadowPass_) {
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = shadowPass_;
    rp.framebuffer = lampAtlasFb_;
    rp.renderArea.extent = {kLampAtlasW, kLampAtlasH};
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    for (int i = 0; i < lighting.lampShadowCount; ++i) {
      const uint32_t tx = static_cast<uint32_t>(i) % 4u;
      const uint32_t ty = static_cast<uint32_t>(i) / 4u;
      meshes.drawDepth(cmd, lighting.lampViewProj[static_cast<size_t>(i)], {kLampTile, kLampTile},
                       {static_cast<int32_t>(tx * kLampTile), static_cast<int32_t>(ty * kLampTile)});
    }
    vkCmdEndRenderPass(cmd);
  }

  // 2) HDR scene: sky + meshes
  {
    VkClearValue clears[2]{};
    clears[0].color = {{0.02f, 0.03f, 0.06f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = hdrPass_;
    rp.framebuffer = hdrFb_;
    rp.renderArea.extent = extent_;
    rp.clearValueCount = 2;
    rp.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    if (gfx.volumetricSky) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipeline_);
      VkViewport vp{};
      vp.width = static_cast<float>(extent_.width);
      vp.height = static_cast<float>(extent_.height);
      vp.maxDepth = 1.0f;
      vkCmdSetViewport(cmd, 0, 1, &vp);
      VkRect2D sc{{0, 0}, extent_};
      vkCmdSetScissor(cmd, 0, 1, &sc);
      SkyPush push{};
      push.invViewProj = skyInvViewProj;
      push.cameraPos = glm::vec4(cam.position, 1.0f);
      push.sunDir = glm::vec4(glm::normalize(lighting.sunDir), lighting.sunIntensity);
      push.sunColor = glm::vec4(lighting.sunColor, 1.0f);
      push.params = glm::vec4(gfx.timeOfDayHours, gfx.clouds ? 1.0f : 0.0f, 0.0f, 0.0f);
      vkCmdPushConstants(cmd, skyLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
      vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    meshes.draw(cmd, extent_, true);

    if (gfx.rain && rainPipeline_ && rainInstanceMapped_) {
      const float dt = 1.0f / 60.0f;
      const float halfXZ = 18.0f;
      const float yMin = -2.0f;
      const float yMax = 22.0f;
      glm::vec3 camDelta(0.0f);
      if (prevCamPosValid_) {
        camDelta = cam.position - prevCamPos_;
      }
      for (uint32_t i = 0; i < kRainParticles; ++i) {
        glm::vec4& p = rainParticles_[i];
        // Keep particles camera-local by subtracting camera motion, then fall.
        p.x -= camDelta.x;
        p.z -= camDelta.z;
        p.y -= camDelta.y;
        const float speed = 14.0f + p.w * 10.0f;
        p.y -= speed * dt;

        auto wrap = [](float v, float half) {
          while (v > half) v -= half * 2.0f;
          while (v < -half) v += half * 2.0f;
          return v;
        };
        p.x = wrap(p.x, halfXZ);
        p.z = wrap(p.z, halfXZ);
        if (p.y < yMin) {
          while (p.y < yMin) p.y += (yMax - yMin);
        }
      }
      // Upload as world positions (camera-local offset + camera).
      auto* dst = static_cast<glm::vec4*>(rainInstanceMapped_);
      for (uint32_t i = 0; i < kRainParticles; ++i) {
        const glm::vec4& lp = rainParticles_[i];
        dst[i] = glm::vec4(cam.position + glm::vec3(lp), lp.w);
      }

      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rainPipeline_);
      VkViewport vp{};
      vp.width = static_cast<float>(extent_.width);
      vp.height = static_cast<float>(extent_.height);
      vp.maxDepth = 1.0f;
      vkCmdSetViewport(cmd, 0, 1, &vp);
      VkRect2D sc{{0, 0}, extent_};
      vkCmdSetScissor(cmd, 0, 1, &sc);
      RainPush push{};
      push.viewProj = viewProj;
      push.camPos = glm::vec4(cam.position, 1.0f);
      const glm::mat4& V = cam.view;
      push.camRight = glm::vec4(V[0][0], V[1][0], V[2][0], 0.0f);
      push.params = glm::vec4(1.0f);
      vkCmdPushConstants(cmd, rainLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
      VkBuffer bufs[] = {rainQuadBuf_, rainInstanceBuf_};
      VkDeviceSize offsets[] = {0, 0};
      vkCmdBindVertexBuffers(cmd, 0, 2, bufs, offsets);
      vkCmdDraw(cmd, 6, kRainParticles, 0, 0);
    }

    vkCmdEndRenderPass(cmd);
  }

  prevCamPos_ = cam.position;
  prevCamPosValid_ = true;

  // 3) SSAO
  if (gfx.ambientOcclusion) {
    {
      VkDescriptorImageInfo ii{};
      ii.sampler = linearSampler_;
      ii.imageView = hdrDepthView_;
      ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = aoSet_;
      w.dstBinding = 0;
      w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      w.descriptorCount = 1;
      w.pImageInfo = &ii;
      vkUpdateDescriptorSets(vk.device(), 1, &w, 0, nullptr);
    }

    const uint32_t aw = std::max(1u, extent_.width / 2);
    const uint32_t ah = std::max(1u, extent_.height / 2);
    VkClearValue clear{};
    clear.color = {{1.0f, 1.0f, 1.0f, 1.0f}};
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = aoPass_;
    rp.framebuffer = aoFb_;
    rp.renderArea.extent = {aw, ah};
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, aoPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, aoLayout_, 0, 1, &aoSet_, 0,
                            nullptr);
    VkViewport vp{};
    vp.width = static_cast<float>(aw);
    vp.height = static_cast<float>(ah);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{{0, 0}, {aw, ah}};
    vkCmdSetScissor(cmd, 0, 1, &sc);
    AoPush push{};
    push.proj = cam.proj;
    push.invProj = glm::inverse(cam.proj);
    push.params = glm::vec4(2.0f, 0.02f, 1.15f, 0.0f);
    vkCmdPushConstants(cmd, aoLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    {
      VkDescriptorImageInfo ii{};
      ii.sampler = linearSampler_;
      ii.imageView = aoView_;
      ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = aoBlurSet_;
      w.dstBinding = 0;
      w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      w.descriptorCount = 1;
      w.pImageInfo = &ii;
      vkUpdateDescriptorSets(vk.device(), 1, &w, 0, nullptr);
    }
    rp.framebuffer = aoBlurFb_;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, aoBlurPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, aoBlurLayout_, 0, 1, &aoBlurSet_,
                            0, nullptr);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
  }

  // 4) Atmosphere tonemap → LDR (fog/godrays/contact/SSR/AE)
  {
    VkImageView aoForPost = gfx.ambientOcclusion ? aoBlurView_ : dummyAoView_;
    VkDescriptorImageInfo imgs[3]{};
    imgs[0] = {linearSampler_, hdrColorView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imgs[1] = {linearSampler_, aoForPost, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imgs[2] = {linearSampler_, hdrDepthView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[3]{};
    for (int i = 0; i < 3; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = postSet_;
      writes[i].dstBinding = static_cast<uint32_t>(i);
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[i].descriptorCount = 1;
      writes[i].pImageInfo = &imgs[i];
    }
    vkUpdateDescriptorSets(vk.device(), 3, writes, 0, nullptr);

    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = ldrPass_;
    rp.framebuffer = ldrFb_;
    rp.renderArea.extent = extent_;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout_, 0, 1, &postSet_, 0,
                            nullptr);
    VkViewport vp{};
    vp.width = static_cast<float>(extent_.width);
    vp.height = static_cast<float>(extent_.height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{{0, 0}, extent_};
    vkCmdSetScissor(cmd, 0, 1, &sc);

    // Project sun direction to screen UV
    glm::vec3 sunDir = glm::normalize(-lighting.sunDir);
    glm::vec4 sunClip = viewProj * glm::vec4(cam.position + sunDir * 500.0f, 1.0f);
    glm::vec2 sunUV(0.5f);
    if (std::abs(sunClip.w) > 1e-4f) {
      sunUV = glm::vec2(sunClip) / sunClip.w;
      sunUV = sunUV * 0.5f + 0.5f;
    }

    PostPush push{};
    push.params =
        glm::vec4(gfx.ambientOcclusion ? 1.0f : 0.0f, exposure_,
                  gfx.distanceFog ? gfx.fogDensity : 0.0f,
                  gfx.volumetrics ? gfx.godrayIntensity * std::max(0.0f, -lighting.sunDir.y) : 0.0f);
    push.params2 =
        glm::vec4(gfx.distanceFog ? 1.0f : 0.0f, gfx.volumetrics ? 1.0f : 0.0f,
                  gfx.contactShadows ? 1.0f : 0.0f, gfx.rain ? 1.0f : 0.0f);
    push.sunScreen = glm::vec4(sunUV.x, sunUV.y, cam.position.x, cam.position.z);
    push.fogColor = glm::vec4(lighting.ambientSky, cam.position.y);
    if (postUboMapped_) {
      PostMatricesUBO mats{};
      mats.invViewProj = cam.invViewProj;
      mats.viewProj = viewProj;
      mats.sunDirIntensity = glm::vec4(glm::normalize(lighting.sunDir), lighting.sunIntensity);
      mats.sunColor = glm::vec4(lighting.sunColor, 1.0f);
      std::memcpy(postUboMapped_, &mats, sizeof(mats));
    }
    vkCmdPushConstants(cmd, postLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
  }

  VkImageView presentSrc = ldrColorView_;

  // 5) TAA resolve into history ping-pong
  if (useTaa) {
    const uint32_t write = historyWrite_;
    const uint32_t read = 1u - write;
    VkDescriptorImageInfo imgs[3]{};
    imgs[0] = {linearSampler_, ldrColorView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imgs[1] = {linearSampler_, historyValid_ ? historyView_[read] : ldrColorView_,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imgs[2] = {linearSampler_, hdrDepthView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[3]{};
    for (int i = 0; i < 3; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = taaSet_;
      writes[i].dstBinding = static_cast<uint32_t>(i);
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[i].descriptorCount = 1;
      writes[i].pImageInfo = &imgs[i];
    }
    vkUpdateDescriptorSets(vk.device(), 3, writes, 0, nullptr);

    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = taaPass_;
    rp.framebuffer = historyFb_[write];
    rp.renderArea.extent = extent_;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, taaPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, taaLayout_, 0, 1, &taaSet_, 0,
                            nullptr);
    VkViewport vp{};
    vp.width = static_cast<float>(extent_.width);
    vp.height = static_cast<float>(extent_.height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{{0, 0}, extent_};
    vkCmdSetScissor(cmd, 0, 1, &sc);

    TaaPush push{};
    push.reprojection = prevViewProj_ * cam.invViewProj;
    // Milder history weight — less ghosting, still softens aliasing.
    push.params = glm::vec4(1.0f, historyValid_ ? 1.0f : 0.0f, 0.9f, 0.0f);
    vkCmdPushConstants(cmd, taaLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    presentSrc = historyView_[write];
    historyWrite_ = read;
    historyValid_ = true;
    prevViewProj_ = viewProj;
  } else {
    historyValid_ = false;
  }

  // 6) Blit resolved color → swapchain (ImGui continues in ui pass).
  if (swapImageIndex >= presentFramebuffers_.size() || !presentFramebuffers_[swapImageIndex]) {
    return;
  }
  {
    VkDescriptorImageInfo imgs[2]{};
    imgs[0] = {linearSampler_, presentSrc, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    imgs[1] = {linearSampler_, hdrColorView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[2]{};
    for (int i = 0; i < 2; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = blitSet_;
      writes[i].dstBinding = static_cast<uint32_t>(i);
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[i].descriptorCount = 1;
      writes[i].pImageInfo = &imgs[i];
    }
    vkUpdateDescriptorSets(vk.device(), 2, writes, 0, nullptr);

    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = presentPass_;
    rp.framebuffer = presentFramebuffers_[swapImageIndex];
    rp.renderArea.extent = extent_;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blitPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blitLayout_, 0, 1, &blitSet_, 0,
                            nullptr);
    VkViewport vp{};
    vp.width = static_cast<float>(extent_.width);
    vp.height = static_cast<float>(extent_.height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{{0, 0}, extent_};
    vkCmdSetScissor(cmd, 0, 1, &sc);
    BlitPush blitPush{};
    blitPush.params =
        glm::vec4(gfx.chromaticAberration ? gfx.chromaticAberrationStrength : 0.0f,
                  gfx.sharpening ? gfx.sharpenStrength : 0.0f,
                  gfx.bloom ? std::clamp(gfx.bloomStrength, 0.0f, 1.0f) : 0.0f, 0.0f);
    blitPush.params2 = glm::vec4(0.0f);
    vkCmdPushConstants(cmd, blitLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(blitPush),
                       &blitPush);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
  }
}

glm::vec2 QualityRenderer::halton(uint32_t index) {
  auto radicalInverse = [](uint32_t i, uint32_t base) {
    float f = 1.0f;
    float r = 0.0f;
    while (i > 0) {
      f /= static_cast<float>(base);
      r += f * static_cast<float>(i % base);
      i /= base;
    }
    return r;
  };
  // Skip first sample (0,0); cycle 0..15
  const uint32_t i = (index % 16u) + 1u;
  return {radicalInverse(i, 2), radicalInverse(i, 3)};
}
