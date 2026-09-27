// xenon-gpu: minimal Vulkan context for the native renderer (headless for now).
#pragma once

// Include this before any ReXGlue header: ReXGlue's Vulkan API header
// disables the global prototypes (it loads functions itself), while xenon-gpu
// calls the loader directly. Beta extensions match ReXGlue's configuration so
// its vulkan.hpp users still compile.
#ifdef VK_NO_PROTOTYPES
#error "xgpu/vk.h must be included before ReXGlue's Vulkan headers"
#endif
#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif
#ifndef VULKAN_HPP_NO_TO_STRING
#define VULKAN_HPP_NO_TO_STRING
#endif
#if defined(__linux__)
#ifndef VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_XCB_KHR
#endif
#ifndef VK_USE_PLATFORM_WAYLAND_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif
#include <wayland-client.h>
#endif
#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <string>

namespace xgpu::vk {

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageAspectFlags aspect = 0;
  uint32_t width = 0, height = 0;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  uint64_t id = 0;  // unique per created image (handles may be reused)
};

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
  void* mapped = nullptr;  // host-visible buffers only
};

class Context {
 public:
  ~Context();
  // Creates an instance and device on the first discrete GPU (else any GPU).
  bool Initialize(std::string* error);

  // Uses a device owned by someone else (the ReXGlue presenter's). Queue
  // access is externally synchronized: `with_queue` must call its argument
  // with the queue while holding the owner's queue lock.
  using QueueAccess = std::function<void(const std::function<void(VkQueue)>&)>;
  bool InitializeExternal(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
                          uint32_t queue_family, QueueAccess with_queue, std::string* error);

  // Per-frame command buffer: begun on first use, submitted and awaited by
  // FlushFrame().
  VkCommandBuffer frame_command_buffer();
  bool FlushFrame();
  bool frame_open() const { return frame_open_; }

  VkDevice device() const { return device_; }
  VkPhysicalDevice physical_device() const { return physical_device_; }
  const std::string& device_name() const { return device_name_; }

  bool CreateImage(Image& out, VkFormat format, uint32_t width, uint32_t height,
                   VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageCreateFlags flags = 0);
  void DestroyImage(Image& image);
  bool CreateBuffer(Buffer& out, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible);
  void DestroyBuffer(Buffer& buffer);

  // Records into a command buffer, submits it and waits.
  bool Submit(const std::function<void(VkCommandBuffer)>& record);

  // Debug: converts a single-sampled color image to RGB8 and writes a PPM.
  // Submits and waits by itself (call outside a frame).
  bool WritePPM(Image& image, const char* path);

  // Records a layout transition (full barrier; correctness over speed for now).
  static void Transition(VkCommandBuffer cmd, Image& image, VkImageLayout layout);

  uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags flags) const;
  uint32_t queue_family() const { return queue_family_; }

 private:
  bool InitializeCommon(std::string* error);
  VkResult SubmitAndWait(VkCommandBuffer cmd);

  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memory_properties_{};
  std::string device_name_;
  bool owns_device_ = true;
  QueueAccess with_queue_;
  VkFence fence_ = VK_NULL_HANDLE;
  VkCommandBuffer frame_cmd_ = VK_NULL_HANDLE;
  bool frame_open_ = false;
};

}  // namespace xgpu::vk
