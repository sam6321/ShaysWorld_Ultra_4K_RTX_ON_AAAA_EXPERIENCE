#include "render/mesh_pass.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {

void check(bool ok, const char* msg) {
  if (!ok) {
    throw std::runtime_error(msg);
  }
}

VkCommandBuffer beginOneTime(VkDevice device, VkCommandPool pool) {
  VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandPool = pool;
  alloc.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(device, &alloc, &cmd);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);
  return cmd;
}

void endOneTime(VkDevice device, VkQueue queue, VkCommandPool pool, VkCommandBuffer cmd) {
  vkEndCommandBuffer(cmd);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
  vkQueueWaitIdle(queue);
  vkFreeCommandBuffers(device, pool, 1, &cmd);
}

struct ShadowPush {
  glm::mat4 lightViewProj;
};

} // namespace

VkShaderModule MeshPass::loadShader(VkDevice device, const std::string& path) {
  std::ifstream file(path, std::ios::ate | std::ios::binary);
  check(static_cast<bool>(file), path.c_str());
  const size_t size = static_cast<size_t>(file.tellg());
  std::vector<char> buffer(size);
  file.seekg(0);
  file.read(buffer.data(), static_cast<std::streamsize>(size));

  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = buffer.size();
  info.pCode = reinterpret_cast<const uint32_t*>(buffer.data());
  VkShaderModule module = VK_NULL_HANDLE;
  check(vkCreateShaderModule(device, &info, nullptr, &module) == VK_SUCCESS, "shader module");
  return module;
}

bool MeshPass::createDummyShadow(VulkanContext& vk) {
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = VK_FORMAT_D32_SFLOAT;
  ii.extent = {1, 1, 1};
  ii.mipLevels = 1;
  ii.arrayLayers = 4;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VmaAllocationCreateInfo ai{};
  ai.usage = VMA_MEMORY_USAGE_AUTO;
  check(vmaCreateImage(vk.allocator(), &ii, &ai, &dummyShadow_.image, &dummyShadow_.allocation,
                       nullptr) == VK_SUCCESS,
        "dummy shadow");

  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = dummyShadow_.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  view.format = VK_FORMAT_D32_SFLOAT;
  view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  view.subresourceRange.levelCount = 1;
  view.subresourceRange.layerCount = 4;
  check(vkCreateImageView(vk.device(), &view, nullptr, &dummyShadow_.view) == VK_SUCCESS,
        "dummy shadow view");

  VkSamplerCreateInfo samp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  samp.magFilter = VK_FILTER_LINEAR;
  samp.minFilter = VK_FILTER_LINEAR;
  samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  samp.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
  samp.compareEnable = VK_FALSE;
  check(vkCreateSampler(vk.device(), &samp, nullptr, &shadowSampler_) == VK_SUCCESS,
        "shadow sampler");

  // Transition to shader read
  VkCommandPoolCreateInfo poolCi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  poolCi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  poolCi.queueFamilyIndex = vk.graphicsQueueFamily();
  VkCommandPool pool = VK_NULL_HANDLE;
  vkCreateCommandPool(vk.device(), &poolCi, nullptr, &pool);
  VkCommandBuffer cmd = beginOneTime(vk.device(), pool);
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = dummyShadow_.image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 4;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &barrier);
  endOneTime(vk.device(), vk.graphicsQueue(), pool, cmd);
  vkDestroyCommandPool(vk.device(), pool, nullptr);
  return true;
}

bool MeshPass::createDummyLampAtlas(VulkanContext& vk) {
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = VK_FORMAT_D32_SFLOAT;
  ii.extent = {4, 4, 1};
  ii.mipLevels = 1;
  ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  VmaAllocationCreateInfo ai{};
  ai.usage = VMA_MEMORY_USAGE_AUTO;
  check(vmaCreateImage(vk.allocator(), &ii, &ai, &dummyLampAtlas_.image, &dummyLampAtlas_.allocation,
                       nullptr) == VK_SUCCESS,
        "dummy lamp atlas");
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = dummyLampAtlas_.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = VK_FORMAT_D32_SFLOAT;
  view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  view.subresourceRange.levelCount = 1;
  view.subresourceRange.layerCount = 1;
  check(vkCreateImageView(vk.device(), &view, nullptr, &dummyLampAtlas_.view) == VK_SUCCESS,
        "dummy lamp view");
  VkSamplerCreateInfo samp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  samp.magFilter = VK_FILTER_NEAREST;
  samp.minFilter = VK_FILTER_NEAREST;
  samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  samp.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
  check(vkCreateSampler(vk.device(), &samp, nullptr, &lampAtlasSampler_) == VK_SUCCESS,
        "lamp atlas sampler");

  VkCommandPoolCreateInfo poolCi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  poolCi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  poolCi.queueFamilyIndex = vk.graphicsQueueFamily();
  VkCommandPool pool = VK_NULL_HANDLE;
  vkCreateCommandPool(vk.device(), &poolCi, nullptr, &pool);
  VkCommandBuffer cmd = beginOneTime(vk.device(), pool);
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = dummyLampAtlas_.image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &barrier);
  endOneTime(vk.device(), vk.graphicsQueue(), pool, cmd);
  vkDestroyCommandPool(vk.device(), pool, nullptr);
  return true;
}

bool MeshPass::init(VulkanContext& vk, VkRenderPass swapchainPass, const scene::Scene& scene,
                    const std::string&) {
  indexCount_ = static_cast<uint32_t>(scene.indices.size());
  materialCount_ = static_cast<uint32_t>(scene.materials.size());

  VkSamplerCreateInfo samp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  samp.magFilter = VK_FILTER_LINEAR;
  samp.minFilter = VK_FILTER_LINEAR;
  samp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samp.maxLod = 1.0f;
  check(vkCreateSampler(vk.device(), &samp, nullptr, &sampler_) == VK_SUCCESS, "sampler");

  check(createDummyShadow(vk), "dummy shadow");
  check(createDummyLampAtlas(vk), "dummy lamp atlas");
  check(createTextures(vk, scene), "textures");
  check(createFrameUbo(vk), "frame ubo");
  check(uploadScene(vk, scene), "upload");
  check(createPipeline(vk, swapchainPass, pipelineLdr_), "ldr pipeline");
  setShadowMap(vk, dummyShadow_.view, shadowSampler_);
  setLampShadowAtlas(vk, dummyLampAtlas_.view, lampAtlasSampler_);
  return true;
}

bool MeshPass::createFrameUbo(VulkanContext& vk) {
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = sizeof(GpuFrameUbo);
  bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  VmaAllocationCreateInfo ai{};
  ai.usage = VMA_MEMORY_USAGE_AUTO;
  ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
             VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VmaAllocationInfo info{};
  check(vmaCreateBuffer(vk.allocator(), &bi, &ai, &frameUbo_, &frameUboAlloc_, &info) == VK_SUCCESS,
        "frame ubo");
  frameUboMapped_ = info.pMappedData;

  VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocInfo.descriptorPool = pool_;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts = &frameSetLayout_;
  check(vkAllocateDescriptorSets(vk.device(), &allocInfo, &frameSet_) == VK_SUCCESS, "frame set");

  VkDescriptorBufferInfo bufferInfo{};
  bufferInfo.buffer = frameUbo_;
  bufferInfo.range = sizeof(GpuFrameUbo);
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = frameSet_;
  write.dstBinding = 0;
  write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  write.descriptorCount = 1;
  write.pBufferInfo = &bufferInfo;
  vkUpdateDescriptorSets(vk.device(), 1, &write, 0, nullptr);
  return true;
}

void MeshPass::setShadowMap(VulkanContext& vk, VkImageView shadowView, VkSampler shadowSamp) {
  VkDescriptorImageInfo imageInfo{};
  imageInfo.sampler = shadowSamp ? shadowSamp : shadowSampler_;
  imageInfo.imageView = shadowView ? shadowView : dummyShadow_.view;
  imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = frameSet_;
  write.dstBinding = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.descriptorCount = 1;
  write.pImageInfo = &imageInfo;
  vkUpdateDescriptorSets(vk.device(), 1, &write, 0, nullptr);
}

void MeshPass::setLampShadowAtlas(VulkanContext& vk, VkImageView atlasView, VkSampler atlasSamp) {
  VkDescriptorImageInfo imageInfo{};
  imageInfo.sampler = atlasSamp ? atlasSamp : lampAtlasSampler_;
  imageInfo.imageView = atlasView ? atlasView : dummyLampAtlas_.view;
  imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = frameSet_;
  write.dstBinding = 2;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.descriptorCount = 1;
  write.pImageInfo = &imageInfo;
  vkUpdateDescriptorSets(vk.device(), 1, &write, 0, nullptr);
}

void MeshPass::updateFrame(VulkanContext&, const FrameLighting& lighting,
                           const glm::mat4& viewProj) {
  if (!frameUboMapped_) {
    return;
  }
  GpuFrameUbo ubo{};
  ubo.cameraPos = glm::vec4(lighting.cameraPos, 1.0f);
  ubo.sunDir = glm::vec4(glm::normalize(lighting.sunDir), lighting.sunIntensity);
  ubo.sunColor = glm::vec4(lighting.sunColor, 1.0f);
  ubo.ambientSky = glm::vec4(lighting.ambientSky, 1.0f);
  ubo.ambientGround = glm::vec4(lighting.ambientGround, 1.0f);
  ubo.params = glm::vec4(lighting.qualityMode ? 1.0f : 0.0f, lighting.linearHdr ? 1.0f : 0.0f,
                         lighting.shadowsEnabled ? 1.0f : 0.0f, lighting.localLightFade);
  ubo.params2 = glm::vec4(static_cast<float>(lighting.localLightCount),
                          lighting.lampShadowsEnabled ? 1.0f : 0.0f, lighting.rainWet, 0.0f);
  ubo.lightVP0 = lighting.lightViewProj[0];
  ubo.lightVP1 = lighting.lightViewProj[1];
  ubo.lightVP2 = lighting.lightViewProj[2];
  ubo.lightVP3 = lighting.lightViewProj[3];
  ubo.cascadeSplits = lighting.cascadeSplits;
  for (int i = 0; i < lighting.localLightCount && i < static_cast<int>(kWalkwayLampCount); ++i) {
    const LocalLight& L = lighting.localLights[static_cast<size_t>(i)];
    ubo.lights[i * 3 + 0] = glm::vec4(L.position, L.range);
    ubo.lights[i * 3 + 1] = glm::vec4(L.color, L.intensity);
    ubo.lights[i * 3 + 2] = glm::vec4(L.direction, L.cosOuter);
  }
  for (int i = 0; i < lighting.lampShadowCount && i < static_cast<int>(kShadowedLampSlots); ++i) {
    ubo.lampVP[i] = lighting.lampViewProj[static_cast<size_t>(i)];
    ubo.lampTile[i] = lighting.lampTileScaleBias[static_cast<size_t>(i)];
  }
  ubo.lampSlot = glm::vec4(static_cast<float>(lighting.lampShadowCount), 0, 0, 0);
  ubo.viewProj = viewProj;
  std::memcpy(frameUboMapped_, &ubo, sizeof(ubo));
}

bool MeshPass::uploadScene(VulkanContext& vk, const scene::Scene& scene) {
  std::vector<GpuVertex> verts(scene.vertices.size());
  for (size_t i = 0; i < scene.vertices.size(); ++i) {
    const auto& v = scene.vertices[i];
    verts[i] = {v.px, v.py, v.pz, v.nx, v.ny, v.nz, v.u, v.v, 0u};
  }
  for (const auto& sm : scene.submeshes) {
    for (uint32_t i = 0; i < sm.indexCount; ++i) {
      const uint32_t idx = scene.indices[sm.firstIndex + i];
      if (idx < verts.size()) {
        verts[idx].material = sm.material;
      }
    }
  }

  const VkDeviceSize vBytes = verts.size() * sizeof(GpuVertex);
  const VkDeviceSize iBytes = scene.indices.size() * sizeof(uint32_t);

  auto createBuf = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf,
                       VmaAllocation& alloc, const void* data) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
               VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    check(vmaCreateBuffer(vk.allocator(), &bi, &ai, &buf, &alloc, &info) == VK_SUCCESS, "buffer");
    std::memcpy(info.pMappedData, data, static_cast<size_t>(size));
  };

  createBuf(vBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertexBuffer_, vertexAlloc_, verts.data());
  createBuf(iBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indexBuffer_, indexAlloc_,
            scene.indices.data());
  return true;
}

bool MeshPass::createTextures(VulkanContext& vk, const scene::Scene& scene) {
  textures_.resize(scene.materials.size());
  const uint32_t matCount = static_cast<uint32_t>(std::max<size_t>(1, scene.materials.size()));

  VkDescriptorSetLayoutBinding matBindings[2]{};
  matBindings[0].binding = 0;
  matBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  matBindings[0].descriptorCount = matCount;
  matBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  matBindings[1].binding = 1;
  matBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  matBindings[1].descriptorCount = 1;
  matBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorBindingFlags bindFlags[2] = {
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
      0,
  };
  VkDescriptorSetLayoutBindingFlagsCreateInfo bindFlagsInfo{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
  bindFlagsInfo.bindingCount = 2;
  bindFlagsInfo.pBindingFlags = bindFlags;

  VkDescriptorSetLayoutCreateInfo matLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  matLayoutInfo.pNext = &bindFlagsInfo;
  matLayoutInfo.bindingCount = 2;
  matLayoutInfo.pBindings = matBindings;
  check(vkCreateDescriptorSetLayout(vk.device(), &matLayoutInfo, nullptr, &materialSetLayout_) ==
            VK_SUCCESS,
        "material set layout");

  VkDescriptorSetLayoutBinding frameBindings[3]{};
  frameBindings[0].binding = 0;
  frameBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  frameBindings[0].descriptorCount = 1;
  frameBindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  frameBindings[1].binding = 1;
  frameBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  frameBindings[1].descriptorCount = 1;
  frameBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  frameBindings[2].binding = 2;
  frameBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  frameBindings[2].descriptorCount = 1;
  frameBindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo frameLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  frameLayoutInfo.bindingCount = 3;
  frameLayoutInfo.pBindings = frameBindings;
  check(vkCreateDescriptorSetLayout(vk.device(), &frameLayoutInfo, nullptr, &frameSetLayout_) ==
            VK_SUCCESS,
        "frame set layout");

  VkDescriptorPoolSize poolSizes[3] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, matCount + 2},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
  };
  VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  poolInfo.maxSets = 2;
  poolInfo.poolSizeCount = 3;
  poolInfo.pPoolSizes = poolSizes;
  check(vkCreateDescriptorPool(vk.device(), &poolInfo, nullptr, &pool_) == VK_SUCCESS, "pool");

  VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocInfo.descriptorPool = pool_;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts = &materialSetLayout_;
  check(vkAllocateDescriptorSets(vk.device(), &allocInfo, &materialSet_) == VK_SUCCESS,
        "material set");

  // Material params SSBO (metallic/roughness/emissive).
  std::vector<glm::vec4> matParams(matCount, glm::vec4(0.0f, 0.6f, 0.0f, 0.0f));
  for (size_t i = 0; i < scene.materials.size(); ++i) {
    const auto& mat = scene.materials[i];
    float emissive = (mat.name == "LIGHT") ? 4.5f : 0.0f;
    matParams[i] = glm::vec4(mat.metallic, mat.roughness, emissive, 0.0f);
  }
  {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = sizeof(glm::vec4) * matCount;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
               VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    check(vmaCreateBuffer(vk.allocator(), &bi, &ai, &matParamsBuf_, &matParamsAlloc_, &info) ==
              VK_SUCCESS,
          "mat params");
    std::memcpy(info.pMappedData, matParams.data(), sizeof(glm::vec4) * matCount);
  }

  VkCommandPoolCreateInfo poolCi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  poolCi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  poolCi.queueFamilyIndex = vk.graphicsQueueFamily();
  VkCommandPool uploadPool = VK_NULL_HANDLE;
  vkCreateCommandPool(vk.device(), &poolCi, nullptr, &uploadPool);

  std::vector<VkDescriptorImageInfo> imageInfos(matCount);
  for (size_t i = 0; i < scene.materials.size(); ++i) {
    const auto& mat = scene.materials[i];
    const uint32_t w = mat.width;
    const uint32_t h = mat.height;

    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    for (uint32_t p = 0; p < w * h; ++p) {
      rgba[p * 4 + 0] = mat.rgb[p * 3 + 0];
      rgba[p * 4 + 1] = mat.rgb[p * 3 + 1];
      rgba[p * 4 + 2] = mat.rgb[p * 3 + 2];
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
    check(vmaCreateBuffer(vk.allocator(), &sbi, &sai, &staging, &stagingAlloc, &sinfo) ==
              VK_SUCCESS,
          "staging");
    std::memcpy(sinfo.pMappedData, rgba.data(), rgba.size());

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_SRGB;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo iai{};
    iai.usage = VMA_MEMORY_USAGE_AUTO;
    check(vmaCreateImage(vk.allocator(), &ii, &iai, &textures_[i].image, &textures_[i].allocation,
                         nullptr) == VK_SUCCESS,
          "image");

    VkCommandBuffer cmd = beginOneTime(vk.device(), uploadPool);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = textures_[i].image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, staging, textures_[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &copy);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
    endOneTime(vk.device(), vk.graphicsQueue(), uploadPool, cmd);
    vmaDestroyBuffer(vk.allocator(), staging, stagingAlloc);

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = textures_[i].image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    check(vkCreateImageView(vk.device(), &viewInfo, nullptr, &textures_[i].view) == VK_SUCCESS,
          "view");

    imageInfos[i].sampler = sampler_;
    imageInfos[i].imageView = textures_[i].view;
    imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }

  if (!scene.materials.empty()) {
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = materialSet_;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = static_cast<uint32_t>(scene.materials.size());
    writes[0].pImageInfo = imageInfos.data();

    VkDescriptorBufferInfo matBufInfo{};
    matBufInfo.buffer = matParamsBuf_;
    matBufInfo.range = VK_WHOLE_SIZE;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = materialSet_;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].descriptorCount = 1;
    writes[1].pBufferInfo = &matBufInfo;
    vkUpdateDescriptorSets(vk.device(), 2, writes, 0, nullptr);
  }

  vkDestroyCommandPool(vk.device(), uploadPool, nullptr);
  return true;
}

bool MeshPass::createPipeline(VulkanContext& vk, VkRenderPass renderPass, VkPipeline& outPipeline) {
  const char* candidates[] = {
      SHAYS_SHADER_DIR "/textured.vert.spv",
      "shaders/textured.vert.spv",
  };
  VkShaderModule vert = VK_NULL_HANDLE;
  VkShaderModule frag = VK_NULL_HANDLE;
  for (const char* c : candidates) {
    std::ifstream test(c, std::ios::binary);
    if (!test) continue;
    std::string vp = c;
    std::string fp = vp;
    auto pos = fp.find("textured.vert.spv");
    fp.replace(pos, std::strlen("textured.vert.spv"), "textured.frag.spv");
    vert = loadShader(vk.device(), vp);
    frag = loadShader(vk.device(), fp);
    break;
  }
  check(vert && frag, "Missing SPIR-V textured shaders");

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";

  VkVertexInputBindingDescription binding{};
  binding.binding = 0;
  binding.stride = sizeof(GpuVertex);
  binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription attrs[4]{};
  attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, px)};
  attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, nx)};
  attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GpuVertex, u)};
  attrs[3] = {3, 0, VK_FORMAT_R32_UINT, offsetof(GpuVertex, material)};
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = 1;
  vi.pVertexBindingDescriptions = &binding;
  vi.vertexAttributeDescriptionCount = 4;
  vi.pVertexAttributeDescriptions = attrs;

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
  ds.depthWriteEnable = VK_TRUE;
  ds.depthCompareOp = VK_COMPARE_OP_LESS;
  VkPipelineColorBlendAttachmentState blendAtt{};
  blendAtt.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &blendAtt;
  VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dynStates;

  if (!layout_) {
    VkDescriptorSetLayout setLayouts[] = {materialSetLayout_, frameSetLayout_};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = setLayouts;
    check(vkCreatePipelineLayout(vk.device(), &layoutInfo, nullptr, &layout_) == VK_SUCCESS,
          "layout");
  }

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
  gp.layout = layout_;
  gp.renderPass = renderPass;
  check(vkCreateGraphicsPipelines(vk.device(), VK_NULL_HANDLE, 1, &gp, nullptr, &outPipeline) ==
            VK_SUCCESS,
        "pipeline");

  vkDestroyShaderModule(vk.device(), vert, nullptr);
  vkDestroyShaderModule(vk.device(), frag, nullptr);
  return true;
}

bool MeshPass::createShadowPipeline(VulkanContext& vk, VkRenderPass renderPass) {
  if (pipelineShadow_ && currentShadowPass_ == renderPass) {
    return true;
  }
  if (pipelineShadow_) {
    vkDestroyPipeline(vk.device(), pipelineShadow_, nullptr);
    pipelineShadow_ = VK_NULL_HANDLE;
  }

  const char* candidates[] = {SHAYS_SHADER_DIR "/shadow.vert.spv", "shaders/shadow.vert.spv"};
  VkShaderModule vert = VK_NULL_HANDLE;
  VkShaderModule frag = VK_NULL_HANDLE;
  for (const char* c : candidates) {
    std::ifstream test(c, std::ios::binary);
    if (!test) continue;
    std::string vp = c;
    std::string fp = vp;
    auto pos = fp.find("shadow.vert.spv");
    fp.replace(pos, std::strlen("shadow.vert.spv"), "shadow.frag.spv");
    vert = loadShader(vk.device(), vp);
    frag = loadShader(vk.device(), fp);
    break;
  }
  check(vert && frag, "Missing shadow shaders");

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";

  VkVertexInputBindingDescription binding{};
  binding.binding = 0;
  binding.stride = sizeof(GpuVertex);
  binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription attrs[1]{};
  attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, px)};
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = 1;
  vi.pVertexBindingDescriptions = &binding;
  vi.vertexAttributeDescriptionCount = 1;
  vi.pVertexAttributeDescriptions = attrs;

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
  rs.depthBiasEnable = VK_TRUE;
  rs.depthBiasConstantFactor = 1.25f;
  rs.depthBiasSlopeFactor = 1.75f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = VK_TRUE;
  ds.depthWriteEnable = VK_TRUE;
  ds.depthCompareOp = VK_COMPARE_OP_LESS;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 0;
  VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dynStates;

  if (!shadowLayout_) {
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.size = sizeof(ShadowPush);
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(vk.device(), &layoutInfo, nullptr, &shadowLayout_) == VK_SUCCESS,
          "shadow layout");
  }

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
  gp.layout = shadowLayout_;
  gp.renderPass = renderPass;
  check(vkCreateGraphicsPipelines(vk.device(), VK_NULL_HANDLE, 1, &gp, nullptr, &pipelineShadow_) ==
            VK_SUCCESS,
        "shadow pipeline");
  currentShadowPass_ = renderPass;

  vkDestroyShaderModule(vk.device(), vert, nullptr);
  vkDestroyShaderModule(vk.device(), frag, nullptr);
  return true;
}

bool MeshPass::ensurePipelines(VulkanContext& vk, VkRenderPass swapchainPass, VkRenderPass hdrPass) {
  if (!pipelineLdr_ && swapchainPass != VK_NULL_HANDLE) {
    check(createPipeline(vk, swapchainPass, pipelineLdr_), "ldr");
  }
  if (hdrPass != VK_NULL_HANDLE) {
    if (pipelineHdr_ && currentHdrPass_ != hdrPass) {
      vkDestroyPipeline(vk.device(), pipelineHdr_, nullptr);
      pipelineHdr_ = VK_NULL_HANDLE;
    }
    if (!pipelineHdr_) {
      check(createPipeline(vk, hdrPass, pipelineHdr_), "hdr");
      currentHdrPass_ = hdrPass;
    }
  }
  return true;
}

bool MeshPass::ensureShadowPipeline(VulkanContext& vk, VkRenderPass shadowPass) {
  return createShadowPipeline(vk, shadowPass);
}

void MeshPass::draw(VkCommandBuffer cmd, VkExtent2D extent, bool useHdrPipeline) {
  VkPipeline pipe = useHdrPipeline ? pipelineHdr_ : pipelineLdr_;
  if (pipe == VK_NULL_HANDLE || indexCount_ == 0 || !materialSet_) {
    return;
  }
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  VkViewport viewport{};
  viewport.width = static_cast<float>(extent.width);
  viewport.height = static_cast<float>(extent.height);
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  VkRect2D scissor{};
  scissor.extent = extent;
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &materialSet_, 0,
                          nullptr);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 1, 1, &frameSet_, 0,
                          nullptr);
  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer_, &offset);
  vkCmdBindIndexBuffer(cmd, indexBuffer_, 0, VK_INDEX_TYPE_UINT32);
  vkCmdDrawIndexed(cmd, indexCount_, 1, 0, 0, 0);
}

void MeshPass::drawDepth(VkCommandBuffer cmd, const glm::mat4& lightViewProj, VkExtent2D extent,
                         VkOffset2D offset) {
  if (pipelineShadow_ == VK_NULL_HANDLE || indexCount_ == 0) {
    return;
  }
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineShadow_);
  VkViewport viewport{};
  viewport.x = static_cast<float>(offset.x);
  viewport.y = static_cast<float>(offset.y);
  viewport.width = static_cast<float>(extent.width);
  viewport.height = static_cast<float>(extent.height);
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  VkRect2D scissor{};
  scissor.offset = offset;
  scissor.extent = extent;
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  ShadowPush push{};
  push.lightViewProj = lightViewProj;
  vkCmdPushConstants(cmd, shadowLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);

  VkDeviceSize vOff = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer_, &vOff);
  vkCmdBindIndexBuffer(cmd, indexBuffer_, 0, VK_INDEX_TYPE_UINT32);
  vkCmdDrawIndexed(cmd, indexCount_, 1, 0, 0, 0);
}

void MeshPass::shutdown(VulkanContext& vk) {
  vkDeviceWaitIdle(vk.device());
  if (pipelineLdr_) vkDestroyPipeline(vk.device(), pipelineLdr_, nullptr);
  if (pipelineHdr_) vkDestroyPipeline(vk.device(), pipelineHdr_, nullptr);
  if (pipelineShadow_) vkDestroyPipeline(vk.device(), pipelineShadow_, nullptr);
  if (layout_) vkDestroyPipelineLayout(vk.device(), layout_, nullptr);
  if (shadowLayout_) vkDestroyPipelineLayout(vk.device(), shadowLayout_, nullptr);
  if (pool_) vkDestroyDescriptorPool(vk.device(), pool_, nullptr);
  if (materialSetLayout_) vkDestroyDescriptorSetLayout(vk.device(), materialSetLayout_, nullptr);
  if (frameSetLayout_) vkDestroyDescriptorSetLayout(vk.device(), frameSetLayout_, nullptr);
  if (sampler_) vkDestroySampler(vk.device(), sampler_, nullptr);
  if (shadowSampler_) vkDestroySampler(vk.device(), shadowSampler_, nullptr);
  if (lampAtlasSampler_) vkDestroySampler(vk.device(), lampAtlasSampler_, nullptr);
  if (dummyShadow_.view) vkDestroyImageView(vk.device(), dummyShadow_.view, nullptr);
  if (dummyShadow_.image) vmaDestroyImage(vk.allocator(), dummyShadow_.image, dummyShadow_.allocation);
  if (dummyLampAtlas_.view) vkDestroyImageView(vk.device(), dummyLampAtlas_.view, nullptr);
  if (dummyLampAtlas_.image)
    vmaDestroyImage(vk.allocator(), dummyLampAtlas_.image, dummyLampAtlas_.allocation);
  for (auto& t : textures_) {
    if (t.view) vkDestroyImageView(vk.device(), t.view, nullptr);
    if (t.image) vmaDestroyImage(vk.allocator(), t.image, t.allocation);
  }
  textures_.clear();
  if (vertexBuffer_) vmaDestroyBuffer(vk.allocator(), vertexBuffer_, vertexAlloc_);
  if (indexBuffer_) vmaDestroyBuffer(vk.allocator(), indexBuffer_, indexAlloc_);
  if (frameUbo_) vmaDestroyBuffer(vk.allocator(), frameUbo_, frameUboAlloc_);
  if (matParamsBuf_) vmaDestroyBuffer(vk.allocator(), matParamsBuf_, matParamsAlloc_);
  pipelineLdr_ = pipelineHdr_ = pipelineShadow_ = VK_NULL_HANDLE;
  layout_ = shadowLayout_ = VK_NULL_HANDLE;
  pool_ = VK_NULL_HANDLE;
  materialSetLayout_ = frameSetLayout_ = VK_NULL_HANDLE;
  sampler_ = shadowSampler_ = VK_NULL_HANDLE;
  dummyShadow_ = {};
  vertexBuffer_ = indexBuffer_ = frameUbo_ = matParamsBuf_ = VK_NULL_HANDLE;
  matParamsAlloc_ = VK_NULL_HANDLE;
  frameUboMapped_ = nullptr;
  frameSet_ = materialSet_ = VK_NULL_HANDLE;
}
