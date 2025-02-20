/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#ifndef WSI_COMMON_H
#define WSI_COMMON_H

#include <stdint.h>
#include <stdbool.h>

#include "vk_alloc.h"
#include "vk_dispatch_table.h"
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef WSI_ENTRYPOINTS_H
extern const struct vk_instance_entrypoint_table wsi_instance_entrypoints;
extern const struct vk_physical_device_entrypoint_table wsi_physical_device_entrypoints;
extern const struct vk_device_entrypoint_table wsi_device_entrypoints;
#endif

#include <util/list.h>

/* This is guaranteed to not collide with anything because it's in the
 * VK_KHR_swapchain namespace but not actually used by the extension.
 */
#define VK_STRUCTURE_TYPE_WSI_IMAGE_CREATE_INFO_MESA (VkStructureType)1000001002
#define VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA (VkStructureType)1000001003
#define VK_STRUCTURE_TYPE_WSI_SURFACE_SUPPORTED_COUNTERS_MESA (VkStructureType)1000001005
#define VK_STRUCTURE_TYPE_WSI_MEMORY_SIGNAL_SUBMIT_INFO_MESA (VkStructureType)1000001006
#define VK_STRUCTURE_TYPE_WSI_IMAGE_CREATE_INFO2_MESA (VkStructureType)1000001007
#define VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO2_MESA (VkStructureType)1000001008

/* This is always chained to VkImageCreateInfo when a wsi image is created.
 * It indicates that the image can be transitioned to/from
 * VK_IMAGE_LAYOUT_PRESENT_SRC_KHR.
 */
struct wsi_image_create_info {
    VkStructureType sType;
    const void *pNext;
    bool scanout;

    /* if true, the image is a buffer blit source */
    bool buffer_blit_src;
};

struct wsi_memory_allocate_info {
    VkStructureType sType;
    const void *pNext;
    bool implicit_sync;
};

/* To be chained into VkSurfaceCapabilities2KHR */
struct wsi_surface_supported_counters {
   VkStructureType sType;
   const void *pNext;

   VkSurfaceCounterFlagsEXT supported_surface_counters;

};

/* To be chained into VkSubmitInfo */
struct wsi_memory_signal_submit_info {
    VkStructureType sType;
    const void *pNext;
    VkDeviceMemory memory;
};

struct wsi_image_create_info2 {
    VkStructureType sType;
    const void *pNext;
    int display_fd;
};

struct wsi_memory_allocate_info2 {
    VkStructureType sType;
    const void *pNext;
    int display_fd;
};

struct wsi_interface;

struct driOptionCache;

#define VK_ICD_WSI_PLATFORM_MAX (VK_ICD_WSI_PLATFORM_DISPLAY + 1)

struct wsi_device {
   /* Allocator for the instance */
   VkAllocationCallbacks instance_alloc;

   VkPhysicalDevice pdevice;
   VkPhysicalDeviceMemoryProperties memory_props;
   uint32_t queue_family_count;

   VkPhysicalDeviceDrmPropertiesEXT drm_info;

#if defined(VULKAN_WSI_USE_PCI_BUS_INFO)
   VkPhysicalDevicePCIBusInfoPropertiesEXT pci_bus_info;
#endif

   VkExternalSemaphoreHandleTypeFlags semaphore_export_handle_types;

   bool has_import_memory_host;

   /** Indicates if wsi_image_create_info::scanout is supported
    *
    * If false, WSI will always use either modifiers or the prime blit path.
    */
   bool supports_scanout;
   bool supports_modifiers;
   uint32_t maxImageDimension2D;
   uint32_t optimalBufferCopyRowPitchAlignment;
   VkPresentModeKHR override_present_mode;
   bool force_bgra8_unorm_first;

   /* Whether to enable adaptive sync for a swapchain if implemented and
    * available. Not all window systems might support this. */
   bool enable_adaptive_sync;

   /* Handles, such as VKDevice, cannot be converted to Mesa data
    * structures using VK_FROM_HANDLE. */
   bool opaque_vk_handles;

   /* List of fences to signal when hotplug event happens. */
   struct list_head hotplug_fences;

   struct {
      /* Override the minimum number of images on the swapchain.
       * 0 = no override */
      uint32_t override_minImageCount;

      /* Forces strict number of image on the swapchain using application
       * provided VkSwapchainCreateInfoKH::RminImageCount.
       */
      bool strict_imageCount;

      /* Ensures to create at least the number of image specified by the
       * driver in VkSurfaceCapabilitiesKHR::minImageCount.
       */
      bool ensure_minImageCount;

      /* Wait for fences before submitting buffers to Xwayland. Defaults to
       * true.
       */
      bool xwaylandWaitReady;
   } x11;

   bool sw;

   /* Set to true if the implementation is ok with linear WSI images. */
   bool wants_linear;

   /* Signals the semaphore such that any wait on the semaphore will wait on
    * any reads or writes on the give memory object.  This is used to
    * implement the semaphore signal operation in vkAcquireNextImage.  This
    * requires the driver to implement vk_device::create_sync_for_memory.
    */
   bool signal_semaphore_with_memory;

   /* Signals the fence such that any wait on the fence will wait on any reads
    * or writes on the give memory object.  This is used to implement the
    * semaphore signal operation in vkAcquireNextImage.  This requires the
    * driver to implement vk_device::create_sync_for_memory.  The resulting
    * vk_sync must support CPU waits.
    */
   bool signal_fence_with_memory;

   /*
    * This sets the ownership for a WSI memory object:
    *
    * The ownership is true if and only if the application is allowed to submit
    * command buffers that reference the buffer.
    *
    * This can be used to prune BO lists without too many adverse affects on
    * implicit sync.
    *
    * Side note: care needs to be taken for internally delayed submissions wrt
    * timeline semaphores.
    */
   void (*set_memory_ownership)(VkDevice device,
                                VkDeviceMemory memory,
                                VkBool32 ownership);

   /*
    * If this is set, the WSI device will call it to let the driver backend
    * decide if it can present images directly on the given device fd.
    */
   bool (*can_present_on_device)(VkPhysicalDevice pdevice, int fd);

   /*
    * A driver can implement this callback to return a special queue to execute
    * buffer blits.
    */
   VkQueue (*get_buffer_blit_queue)(VkDevice device);

#define WSI_CB(cb) PFN_vk##cb cb
   WSI_CB(AllocateMemory);
   WSI_CB(AllocateCommandBuffers);
   WSI_CB(BindBufferMemory);
   WSI_CB(BindImageMemory);
   WSI_CB(BeginCommandBuffer);
   WSI_CB(CmdPipelineBarrier);
   WSI_CB(CmdCopyImageToBuffer);
   WSI_CB(CreateBuffer);
   WSI_CB(CreateCommandPool);
   WSI_CB(CreateFence);
   WSI_CB(CreateImage);
   WSI_CB(CreateSemaphore);
   WSI_CB(DestroyBuffer);
   WSI_CB(DestroyCommandPool);
   WSI_CB(DestroyFence);
   WSI_CB(DestroyImage);
   WSI_CB(DestroySemaphore);
   WSI_CB(EndCommandBuffer);
   WSI_CB(FreeMemory);
   WSI_CB(FreeCommandBuffers);
   WSI_CB(GetBufferMemoryRequirements);
   WSI_CB(GetImageDrmFormatModifierPropertiesEXT);
   WSI_CB(GetImageMemoryRequirements);
   WSI_CB(GetImageSubresourceLayout);
   WSI_CB(GetMemoryFdKHR);
   WSI_CB(GetPhysicalDeviceFormatProperties);
   WSI_CB(GetPhysicalDeviceFormatProperties2KHR);
   WSI_CB(GetPhysicalDeviceImageFormatProperties2);
   WSI_CB(GetSemaphoreFdKHR);
   WSI_CB(ResetFences);
   WSI_CB(QueueSubmit);
   WSI_CB(WaitForFences);
   WSI_CB(MapMemory);
   WSI_CB(UnmapMemory);
#undef WSI_CB

    struct wsi_interface *                  wsi[VK_ICD_WSI_PLATFORM_MAX];
};

typedef PFN_vkVoidFunction (VKAPI_PTR *WSI_FN_GetPhysicalDeviceProcAddr)(VkPhysicalDevice physicalDevice, const char* pName);

VkResult
wsi_device_init2(struct wsi_device *wsi,
                 VkPhysicalDevice pdevice,
                 WSI_FN_GetPhysicalDeviceProcAddr proc_addr,
                 const VkAllocationCallbacks *alloc,
                 int display_fd,
                 const struct driOptionCache *dri_options,
                 bool sw_device,
                 bool opaque_vk_handles,
                 const struct vk_device_extension_table *device_extensions);

VkResult
wsi_device_init(struct wsi_device *wsi,
                VkPhysicalDevice pdevice,
                WSI_FN_GetPhysicalDeviceProcAddr proc_addr,
                const VkAllocationCallbacks *alloc,
                int display_fd,
                const struct driOptionCache *dri_options,
                bool sw_device);

void
wsi_device_finish(struct wsi_device *wsi,
                  const VkAllocationCallbacks *alloc);

/* Setup file descriptor to be used with imported sync_fd's in wsi fences. */
void
wsi_device_setup_syncobj_fd(struct wsi_device *wsi_device,
                            int fd);

#define ICD_DEFINE_NONDISP_HANDLE_CASTS(__VkIcdType, __VkType)             \
                                                                           \
   static inline __VkIcdType *                                             \
   __VkIcdType ## _from_handle(__VkType _handle)                           \
   {                                                                       \
      return (__VkIcdType *)(uintptr_t) _handle;                           \
   }                                                                       \
                                                                           \
   static inline __VkType                                                  \
   __VkIcdType ## _to_handle(__VkIcdType *_obj)                            \
   {                                                                       \
      return (__VkType)(uintptr_t) _obj;                                   \
   }

#define ICD_FROM_HANDLE(__VkIcdType, __name, __handle) \
   __VkIcdType *__name = __VkIcdType ## _from_handle(__handle)

ICD_DEFINE_NONDISP_HANDLE_CASTS(VkIcdSurfaceBase, VkSurfaceKHR)

VkResult
wsi_common_get_surface_support(struct wsi_device *wsi_device,
                               uint32_t queueFamilyIndex,
                               VkSurfaceKHR surface,
                               VkBool32 *pSupported);

VkResult
wsi_common_get_surface_capabilities(struct wsi_device *wsi_device,
                                    VkSurfaceKHR surface,
                                    VkSurfaceCapabilitiesKHR *pSurfaceCapabilities);

VkResult
wsi_common_get_surface_capabilities2(struct wsi_device *wsi_device,
                                     const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
                                     VkSurfaceCapabilities2KHR *pSurfaceCapabilities);

VkResult
wsi_common_get_surface_formats(struct wsi_device *wsi_device,
                               VkSurfaceKHR surface,
                               uint32_t *pSurfaceFormatCount,
                               VkSurfaceFormatKHR *pSurfaceFormats);

VkResult
wsi_common_get_surface_formats2(struct wsi_device *wsi_device,
                                const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
                                uint32_t *pSurfaceFormatCount,
                                VkSurfaceFormat2KHR *pSurfaceFormats);

VkResult
wsi_common_get_surface_present_modes(struct wsi_device *wsi_device,
                                     VkSurfaceKHR surface,
                                     uint32_t *pPresentModeCount,
                                     VkPresentModeKHR *pPresentModes);

VkResult
wsi_common_get_present_rectangles(struct wsi_device *wsi_device,
                                  VkSurfaceKHR surface,
                                  uint32_t* pRectCount,
                                  VkRect2D* pRects);

VkResult
wsi_common_get_surface_capabilities2ext(struct wsi_device *wsi_device,
                                        VkSurfaceKHR surface,
                                        VkSurfaceCapabilities2EXT *pSurfaceCapabilities);

VkResult
wsi_common_get_images(VkSwapchainKHR _swapchain,
                      uint32_t *pSwapchainImageCount,
                      VkImage *pSwapchainImages);

VkImage
wsi_common_get_image(VkSwapchainKHR _swapchain, uint32_t index);

VkResult
wsi_common_acquire_next_image2(const struct wsi_device *wsi,
                               VkDevice device,
                               const VkAcquireNextImageInfoKHR *pAcquireInfo,
                               uint32_t *pImageIndex);

VkResult
wsi_common_create_swapchain(struct wsi_device *wsi,
                            VkDevice device,
                            const VkSwapchainCreateInfoKHR *pCreateInfo,
                            const VkAllocationCallbacks *pAllocator,
                            VkSwapchainKHR *pSwapchain);
void
wsi_common_destroy_swapchain(VkDevice device,
                             VkSwapchainKHR swapchain,
                             const VkAllocationCallbacks *pAllocator);

VkResult
wsi_common_queue_present(const struct wsi_device *wsi,
                         VkDevice device_h,
                         VkQueue queue_h,
                         int queue_family_index,
                         const VkPresentInfoKHR *pPresentInfo);

VkResult
wsi_common_create_swapchain_image(const struct wsi_device *wsi,
                                  const VkImageCreateInfo *pCreateInfo,
                                  VkSwapchainKHR _swapchain,
                                  VkImage *pImage);
VkResult
wsi_common_bind_swapchain_image(const struct wsi_device *wsi,
                                VkImage vk_image,
                                VkSwapchainKHR _swapchain,
                                uint32_t image_idx);

void
wsi_surface_destroy(VkSurfaceKHR _surface,
                    const VkAllocationCallbacks *pAllocator);

#ifdef __cplusplus
}
#endif

#endif
