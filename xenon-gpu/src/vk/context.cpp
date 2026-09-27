#include <xgpu/vk.h>

#include <cstdio>
#include <vector>

namespace xgpu::vk {

Context::~Context() {
  if (device_) {
    vkDeviceWaitIdle(device_);
    if (fence_) vkDestroyFence(device_, fence_, nullptr);
    if (command_pool_) vkDestroyCommandPool(device_, command_pool_, nullptr);
    if (owns_device_) vkDestroyDevice(device_, nullptr);
  }
  if (instance_ && owns_device_) vkDestroyInstance(instance_, nullptr);
}

bool Context::InitializeExternal(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
                                 uint32_t queue_family, QueueAccess with_queue, std::string* error) {
  owns_device_ = false;
  instance_ = instance;
  physical_device_ = physical_device;
  device_ = device;
  queue_family_ = queue_family;
  with_queue_ = std::move(with_queue);
  VkPhysicalDeviceProperties properties;
  vkGetPhysicalDeviceProperties(physical_device_, &properties);
  device_name_ = properties.deviceName;
  vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties_);
  return InitializeCommon(error);
}

bool Context::InitializeCommon(std::string* error) {
  VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = queue_family_;
  if (vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_) != VK_SUCCESS) {
    *error = "vkCreateCommandPool failed";
    return false;
  }
  VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (vkCreateFence(device_, &fence_info, nullptr, &fence_) != VK_SUCCESS) {
    *error = "vkCreateFence failed";
    return false;
  }
  VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = command_pool_;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  vkAllocateCommandBuffers(device_, &alloc, &frame_cmd_);
  return true;
}

VkResult Context::SubmitAndWait(VkCommandBuffer cmd) {
  VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  VkResult result = VK_SUCCESS;
  if (with_queue_) {
    with_queue_([&](VkQueue queue) { result = vkQueueSubmit(queue, 1, &submit, fence_); });
  } else {
    result = vkQueueSubmit(queue_, 1, &submit, fence_);
  }
  if (result == VK_SUCCESS) result = vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
  vkResetFences(device_, 1, &fence_);
  if (result != VK_SUCCESS) std::fprintf(stderr, "xgpu: queue submission failed (VkResult %d)\n", int(result));
  return result;
}

VkCommandBuffer Context::frame_command_buffer() {
  if (!frame_open_) {
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(frame_cmd_, &begin);
    frame_open_ = true;
  }
  return frame_cmd_;
}

bool Context::FlushFrame() {
  if (!frame_open_) return true;
  frame_open_ = false;
  vkEndCommandBuffer(frame_cmd_);
  const bool ok = SubmitAndWait(frame_cmd_) == VK_SUCCESS;
  vkResetCommandBuffer(frame_cmd_, 0);
  return ok;
}

bool Context::Initialize(std::string* error) {
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "xenon-gpu";
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  instance_info.pApplicationInfo = &app;
  if (vkCreateInstance(&instance_info, nullptr, &instance_) != VK_SUCCESS) {
    *error = "vkCreateInstance failed";
    return false;
  }

  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance_, &count, nullptr);
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance_, &count, devices.data());
  for (VkPhysicalDevice device : devices) {
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(device, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_3) continue;
    if (!physical_device_ || properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
      physical_device_ = device;
      device_name_ = properties.deviceName;
      if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) break;
    }
  }
  if (!physical_device_) {
    *error = "no Vulkan 1.3 device";
    return false;
  }
  vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties_);

  vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &count, families.data());
  for (uint32_t i = 0; i < count; ++i) {
    if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      queue_family_ = i;
      break;
    }
  }

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue_info.queueFamilyIndex = queue_family_;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;
  // Features the translated guest shaders and the native renderer use (only
  // those the device supports are requested).
  VkPhysicalDeviceVulkan13Features supported13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan12Features supported12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  supported12.pNext = &supported13;
  VkPhysicalDeviceFeatures2 supported = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  supported.pNext = &supported12;
  vkGetPhysicalDeviceFeatures2(physical_device_, &supported);
  const VkPhysicalDeviceFeatures& s = supported.features;
  VkPhysicalDeviceVulkan13Features features13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  features13.dynamicRendering = VK_TRUE;
  features13.synchronization2 = VK_TRUE;
  features13.shaderDemoteToHelperInvocation = supported13.shaderDemoteToHelperInvocation;
  VkPhysicalDeviceVulkan12Features features12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  features12.pNext = &features13;
  features12.storageBuffer8BitAccess = supported12.storageBuffer8BitAccess;
  features12.uniformAndStorageBuffer8BitAccess = supported12.uniformAndStorageBuffer8BitAccess;
  features12.shaderFloat16 = supported12.shaderFloat16;
  features12.shaderInt8 = supported12.shaderInt8;
  features12.uniformBufferStandardLayout = supported12.uniformBufferStandardLayout;
  features12.scalarBlockLayout = supported12.scalarBlockLayout;
  VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  features.pNext = &features12;
  VkPhysicalDeviceFeatures& f = features.features;
  f.independentBlend = s.independentBlend;
  f.depthClamp = s.depthClamp;
  f.depthBiasClamp = s.depthBiasClamp;
  f.fillModeNonSolid = s.fillModeNonSolid;
  f.samplerAnisotropy = s.samplerAnisotropy;
  f.textureCompressionBC = s.textureCompressionBC;
  f.imageCubeArray = s.imageCubeArray;
  f.fullDrawIndexUint32 = s.fullDrawIndexUint32;
  f.vertexPipelineStoresAndAtomics = s.vertexPipelineStoresAndAtomics;
  f.fragmentStoresAndAtomics = s.fragmentStoresAndAtomics;
  f.shaderClipDistance = s.shaderClipDistance;
  f.shaderCullDistance = s.shaderCullDistance;
  f.sampleRateShading = s.sampleRateShading;
  f.shaderImageGatherExtended = s.shaderImageGatherExtended;
  f.shaderStorageImageWriteWithoutFormat = s.shaderStorageImageWriteWithoutFormat;
  VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  device_info.pNext = &features;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  if (vkCreateDevice(physical_device_, &device_info, nullptr, &device_) != VK_SUCCESS) {
    *error = "vkCreateDevice failed";
    return false;
  }
  vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
  return InitializeCommon(error);
}

uint32_t Context::FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags flags) const {
  for (uint32_t i = 0; i < memory_properties_.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) && (memory_properties_.memoryTypes[i].propertyFlags & flags) == flags) {
      return i;
    }
  }
  return UINT32_MAX;
}

bool Context::CreateImage(Image& out, VkFormat format, uint32_t width, uint32_t height,
                          VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageCreateFlags flags) {
  const bool depth = format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                     format == VK_FORMAT_D32_SFLOAT;
  VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.flags = flags;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = samples;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(device_, &info, nullptr, &out.image) != VK_SUCCESS) return false;
  VkMemoryRequirements requirements;
  vkGetImageMemoryRequirements(device_, out.image, &requirements);
  VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = requirements.size;
  alloc.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(device_, &alloc, nullptr, &out.memory) != VK_SUCCESS) return false;
  vkBindImageMemory(device_, out.image, out.memory, 0);
  out.format = format;
  out.aspect = depth ? (format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                        : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                     : VK_IMAGE_ASPECT_COLOR_BIT;
  out.width = width;
  out.height = height;
  out.samples = samples;
  out.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  static uint64_t next_image_id = 1;
  out.id = next_image_id++;
  if (!(usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) {
    return true;  // transfer-only: no view
  }
  VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = out.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.subresourceRange = {out.aspect, 0, 1, 0, 1};
  return vkCreateImageView(device_, &view, nullptr, &out.view) == VK_SUCCESS;
}

void Context::DestroyImage(Image& image) {
  if (image.view) vkDestroyImageView(device_, image.view, nullptr);
  if (image.image) vkDestroyImage(device_, image.image, nullptr);
  if (image.memory) vkFreeMemory(device_, image.memory, nullptr);
  image = Image();
}

bool Context::CreateBuffer(Buffer& out, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible) {
  VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = usage;
  if (vkCreateBuffer(device_, &info, nullptr, &out.buffer) != VK_SUCCESS) return false;
  VkMemoryRequirements requirements;
  vkGetBufferMemoryRequirements(device_, out.buffer, &requirements);
  VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = requirements.size;
  alloc.memoryTypeIndex = FindMemoryType(
      requirements.memoryTypeBits,
      host_visible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                   : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(device_, &alloc, nullptr, &out.memory) != VK_SUCCESS) return false;
  vkBindBufferMemory(device_, out.buffer, out.memory, 0);
  out.size = size;
  if (host_visible) vkMapMemory(device_, out.memory, 0, size, 0, &out.mapped);
  return true;
}

void Context::DestroyBuffer(Buffer& buffer) {
  if (buffer.buffer) vkDestroyBuffer(device_, buffer.buffer, nullptr);
  if (buffer.memory) vkFreeMemory(device_, buffer.memory, nullptr);
  buffer = Buffer();
}

bool Context::Submit(const std::function<void(VkCommandBuffer)>& record) {
  VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = command_pool_;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  VkCommandBuffer cmd;
  if (vkAllocateCommandBuffers(device_, &alloc, &cmd) != VK_SUCCESS) return false;
  VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);
  record(cmd);
  vkEndCommandBuffer(cmd);
  const bool ok = SubmitAndWait(cmd) == VK_SUCCESS;
  vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
  return ok;
}

void Context::Transition(VkCommandBuffer cmd, Image& image, VkImageLayout layout) {
  if (image.layout == layout) return;
  VkImageMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  barrier.oldLayout = image.layout;
  barrier.newLayout = layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image.image;
  barrier.subresourceRange = {image.aspect, 0, 1, 0, 1};
  VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd, &dependency);
  image.layout = layout;
}

bool Context::WritePPM(Image& source, const char* path) {
  Image rgba;
  if (!CreateImage(rgba, VK_FORMAT_R8G8B8A8_UNORM, source.width, source.height, VK_SAMPLE_COUNT_1_BIT,
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
    return false;
  }
  Buffer staging;
  CreateBuffer(staging, VkDeviceSize(source.width) * source.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
  Submit([&](VkCommandBuffer cmd) {
    Transition(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(cmd, rgba, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.srcOffsets[1] = {int32_t(source.width), int32_t(source.height), 1};
    blit.dstOffsets[1] = blit.srcOffsets[1];
    vkCmdBlitImage(cmd, source.image, source.layout, rgba.image, rgba.layout, 1, &blit, VK_FILTER_NEAREST);
    Transition(cmd, rgba, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {source.width, source.height, 1};
    vkCmdCopyImageToBuffer(cmd, rgba.image, rgba.layout, staging.buffer, 1, &copy);
  });
  bool ok = false;
  if (FILE* f = std::fopen(path, "wb")) {
    std::fprintf(f, "P6\n%u %u\n255\n", source.width, source.height);
    const uint8_t* pixels = static_cast<const uint8_t*>(staging.mapped);
    for (uint32_t i = 0; i < source.width * source.height; ++i) std::fwrite(pixels + i * 4, 1, 3, f);
    ok = std::fclose(f) == 0;
  }
  DestroyBuffer(staging);
  DestroyImage(rgba);
  return ok;
}

}  // namespace xgpu::vk
