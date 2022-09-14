// Copyright 2020 Samsung Electronics Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef EMBEDDER_TIZEN_CONTEXT_VULKAN_H_
#define EMBEDDER_TIZEN_CONTEXT_VULKAN_H_

#include <vector>

#include <vulkan/vulkan.h>
#include "flutter/shell/platform/embedder/embedder.h"

namespace flutter {

class TizenContextVulkan {
 public:
  explicit TizenContextVulkan();
  virtual ~TizenContextVulkan();

  bool CreateSurface(void* render_target,
                     void* render_target_display,
                     int32_t width,
                     int32_t height);

  void CreatePhysicalDevice();
  void CreateLogicalDeviceAndQueue();
  void CreateCommandPool();
  void CreateSwapChain(uint32_t w, uint32_t h);
  void OnResize(uint32_t w, uint32_t h);

  bool IsValid() const { return is_valid_; }
  uint32_t GetAPIVersion() const { return VK_MAKE_VERSION(1, 0, 0); }
  const VkInstance& GetInstance() const { return instance_; }
  const VkPhysicalDevice& GetPhysicalDevice() const { return physical_device_; }
  const VkDevice& GetLogicalDevice() const { return logical_device_; }
  uint32_t GetGraphicsQueueFamilyIndex() const {
    return graphics_queue_family_index_;
  }
  const VkQueue& GetGraphicsQueue() const { return graphics_queue_; }
  size_t GetInstanceExtensionCount() const {
    return instance_extensions_.size();
  }
  const char** GetInstanceExtension() { return instance_extensions_.data(); }
  size_t GetDeviceExtensionCount() const {
    return enabled_device_extensions_.size();
  }
  const char** GetDeviceExtension() {
    return enabled_device_extensions_.data();
  }
  FlutterVulkanImage GetNextImageCallback(const FlutterFrameInfo* frame_info);
  bool PresentCallback(const FlutterVulkanImage* image);

  std::function<void*(VkInstance, const char*)> GetInstanceProcAddr = nullptr;

  void RecreateSwapChain();

 private:
  VkInstance instance_;
  std::vector<const char*> instance_extensions_;

  VkPhysicalDevice physical_device_;
  std::vector<const char*> enabled_device_extensions_;
  VkDevice logical_device_;
  uint32_t graphics_queue_family_index_;
  uint32_t present_queue_family_index_;
  VkQueue graphics_queue_;

  VkSurfaceKHR surface_;
  VkSurfaceFormatKHR surface_format_;

  std::vector<VkSemaphore> present_transition_semaphore_;
  std::vector<VkFence> image_ready_fence_;
  std::vector<VkFence> submit_done_fence_;

  VkCommandPool swapchain_command_pool_;
  std::vector<VkCommandBuffer> present_transition_buffers_;

  VkSwapchainKHR swapchain_;
  std::vector<VkImage> swapchain_images_;

  uint32_t image_index_;
  uint32_t frame_index_;

  int32_t width_;
  int32_t height_;
  bool is_valid_ = false;
};

}  // namespace flutter

#endif  // EMBEDDER_TIZEN_CONTEXT_VULKAN_H_
