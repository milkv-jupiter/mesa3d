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

#include <X11/Xlib-xcb.h>
#include <X11/xshmfence.h>
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/shm.h>

#include "util/macros.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <xf86drm.h>
#include "drm-uapi/drm_fourcc.h"
#include "util/hash_table.h"
#include "util/os_file.h"
#include "util/os_time.h"
#include "util/u_debug.h"
#include "util/u_thread.h"
#include "util/xmlconfig.h"

#include "vk_instance.h"
#include "vk_physical_device.h"
#include "vk_util.h"
#include "vk_enum_to_str.h"
#include "wsi_common_entrypoints.h"
#include "wsi_common_private.h"
#include "wsi_common_x11.h"
#include "wsi_common_queue.h"

#ifdef HAVE_SYS_SHM_H
#include <sys/ipc.h>
#include <sys/shm.h>
#endif

struct wsi_x11_connection {
   bool has_dri3;
   bool has_dri3_modifiers;
   bool has_present;
   bool is_proprietary_x11;
   bool is_xwayland;
   bool has_mit_shm;
   bool has_xfixes;
};

struct wsi_x11 {
   struct wsi_interface base;

   pthread_mutex_t                              mutex;
   /* Hash table of xcb_connection -> wsi_x11_connection mappings */
   struct hash_table *connections;
};


/**
 * Wrapper around xcb_dri3_open. Returns the opened fd or -1 on error.
 */
static int
wsi_dri3_open(xcb_connection_t *conn,
	      xcb_window_t root,
	      uint32_t provider)
{
   xcb_dri3_open_cookie_t       cookie;
   xcb_dri3_open_reply_t        *reply;
   int                          fd;

   cookie = xcb_dri3_open(conn,
                          root,
                          provider);

   reply = xcb_dri3_open_reply(conn, cookie, NULL);
   if (!reply)
      return -1;

   /* According to DRI3 extension nfd must equal one. */
   if (reply->nfd != 1) {
      free(reply);
      return -1;
   }

   fd = xcb_dri3_open_reply_fds(conn, reply)[0];
   free(reply);
   fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);

   return fd;
}

/**
 * Checks compatibility of the device wsi_dev with the device the X server
 * provides via DRI3.
 *
 * This returns true when no device could be retrieved from the X server or when
 * the information for the X server device indicate that it is the same device.
 */
static bool
wsi_x11_check_dri3_compatible(const struct wsi_device *wsi_dev,
                              xcb_connection_t *conn)
{
   xcb_screen_iterator_t screen_iter =
      xcb_setup_roots_iterator(xcb_get_setup(conn));
   xcb_screen_t *screen = screen_iter.data;

   /* Open the DRI3 device from the X server. If we do not retrieve one we
    * assume our local device is compatible.
    */
   int dri3_fd = wsi_dri3_open(conn, screen->root, None);
   if (dri3_fd == -1)
      return true;

   bool match = wsi_device_matches_drm_fd(wsi_dev, dri3_fd);

   close(dri3_fd);

   return match;
}

static bool
wsi_x11_detect_xwayland(xcb_connection_t *conn,
                        xcb_query_extension_reply_t *randr_reply,
                        xcb_query_extension_reply_t *xwl_reply)
{
   /* Newer Xwayland exposes an X11 extension we can check for */
   if (xwl_reply && xwl_reply->present)
      return true;

   /* Older Xwayland uses the word "XWAYLAND" in the RandR output names */
   if (!randr_reply || !randr_reply->present)
      return false;

   xcb_randr_query_version_cookie_t ver_cookie =
      xcb_randr_query_version_unchecked(conn, 1, 3);
   xcb_randr_query_version_reply_t *ver_reply =
      xcb_randr_query_version_reply(conn, ver_cookie, NULL);
   bool has_randr_v1_3 = ver_reply && (ver_reply->major_version > 1 ||
                                       ver_reply->minor_version >= 3);
   free(ver_reply);

   if (!has_randr_v1_3)
      return false;

   const xcb_setup_t *setup = xcb_get_setup(conn);
   xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);

   xcb_randr_get_screen_resources_current_cookie_t gsr_cookie =
      xcb_randr_get_screen_resources_current_unchecked(conn, iter.data->root);
   xcb_randr_get_screen_resources_current_reply_t *gsr_reply =
      xcb_randr_get_screen_resources_current_reply(conn, gsr_cookie, NULL);

   if (!gsr_reply || gsr_reply->num_outputs == 0) {
      free(gsr_reply);
      return false;
   }

   xcb_randr_output_t *randr_outputs =
      xcb_randr_get_screen_resources_current_outputs(gsr_reply);
   xcb_randr_get_output_info_cookie_t goi_cookie =
      xcb_randr_get_output_info(conn, randr_outputs[0], gsr_reply->config_timestamp);
   free(gsr_reply);

   xcb_randr_get_output_info_reply_t *goi_reply =
      xcb_randr_get_output_info_reply(conn, goi_cookie, NULL);
   if (!goi_reply) {
      return false;
   }

   char *output_name = (char*)xcb_randr_get_output_info_name(goi_reply);
   bool is_xwayland = output_name && strncmp(output_name, "XWAYLAND", 8) == 0;
   free(goi_reply);

   return is_xwayland;
}

static struct wsi_x11_connection *
wsi_x11_connection_create(struct wsi_device *wsi_dev,
                          xcb_connection_t *conn)
{
   xcb_query_extension_cookie_t dri3_cookie, pres_cookie, randr_cookie,
                                amd_cookie, nv_cookie, shm_cookie, sync_cookie,
                                xfixes_cookie, xwl_cookie;
   xcb_query_extension_reply_t *dri3_reply, *pres_reply, *randr_reply,
                               *amd_reply, *nv_reply, *shm_reply = NULL,
                               *xfixes_reply, *xwl_reply;
   bool wants_shm = wsi_dev->sw && !(WSI_DEBUG & WSI_DEBUG_NOSHM) &&
                    wsi_dev->has_import_memory_host;
   bool has_dri3_v1_2 = false;
   bool has_present_v1_2 = false;

   struct wsi_x11_connection *wsi_conn =
      vk_alloc(&wsi_dev->instance_alloc, sizeof(*wsi_conn), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!wsi_conn)
      return NULL;

   sync_cookie = xcb_query_extension(conn, 4, "SYNC");
   dri3_cookie = xcb_query_extension(conn, 4, "DRI3");
   pres_cookie = xcb_query_extension(conn, 7, "Present");
   randr_cookie = xcb_query_extension(conn, 5, "RANDR");
   xfixes_cookie = xcb_query_extension(conn, 6, "XFIXES");
   xwl_cookie = xcb_query_extension(conn, 8, "XWAYLAND");

   if (wants_shm)
      shm_cookie = xcb_query_extension(conn, 7, "MIT-SHM");

   /* We try to be nice to users and emit a warning if they try to use a
    * Vulkan application on a system without DRI3 enabled.  However, this ends
    * up spewing the warning when a user has, for example, both Intel
    * integrated graphics and a discrete card with proprietary drivers and are
    * running on the discrete card with the proprietary DDX.  In this case, we
    * really don't want to print the warning because it just confuses users.
    * As a heuristic to detect this case, we check for a couple of proprietary
    * X11 extensions.
    */
   amd_cookie = xcb_query_extension(conn, 11, "ATIFGLRXDRI");
   nv_cookie = xcb_query_extension(conn, 10, "NV-CONTROL");

   xcb_discard_reply(conn, sync_cookie.sequence);
   dri3_reply = xcb_query_extension_reply(conn, dri3_cookie, NULL);
   pres_reply = xcb_query_extension_reply(conn, pres_cookie, NULL);
   randr_reply = xcb_query_extension_reply(conn, randr_cookie, NULL);
   amd_reply = xcb_query_extension_reply(conn, amd_cookie, NULL);
   nv_reply = xcb_query_extension_reply(conn, nv_cookie, NULL);
   xfixes_reply = xcb_query_extension_reply(conn, xfixes_cookie, NULL);
   xwl_reply = xcb_query_extension_reply(conn, xwl_cookie, NULL);
   if (wants_shm)
      shm_reply = xcb_query_extension_reply(conn, shm_cookie, NULL);
   if (!dri3_reply || !pres_reply || !xfixes_reply) {
      free(dri3_reply);
      free(pres_reply);
      free(xfixes_reply);
      free(xwl_reply);
      free(randr_reply);
      free(amd_reply);
      free(nv_reply);
      if (wants_shm)
         free(shm_reply);
      vk_free(&wsi_dev->instance_alloc, wsi_conn);
      return NULL;
   }

   wsi_conn->has_dri3 = dri3_reply->present != 0;
#ifdef HAVE_DRI3_MODIFIERS
   if (wsi_conn->has_dri3) {
      xcb_dri3_query_version_cookie_t ver_cookie;
      xcb_dri3_query_version_reply_t *ver_reply;

      ver_cookie = xcb_dri3_query_version(conn, 1, 2);
      ver_reply = xcb_dri3_query_version_reply(conn, ver_cookie, NULL);
      has_dri3_v1_2 = ver_reply != NULL &&
         (ver_reply->major_version > 1 || ver_reply->minor_version >= 2);
      free(ver_reply);
   }
#endif

   wsi_conn->has_present = pres_reply->present != 0;
#ifdef HAVE_DRI3_MODIFIERS
   if (wsi_conn->has_present) {
      xcb_present_query_version_cookie_t ver_cookie;
      xcb_present_query_version_reply_t *ver_reply;

      ver_cookie = xcb_present_query_version(conn, 1, 2);
      ver_reply = xcb_present_query_version_reply(conn, ver_cookie, NULL);
      has_present_v1_2 = ver_reply != NULL &&
        (ver_reply->major_version > 1 || ver_reply->minor_version >= 2);
      free(ver_reply);
   }
#endif

   wsi_conn->has_xfixes = xfixes_reply->present != 0;
   if (wsi_conn->has_xfixes) {
      xcb_xfixes_query_version_cookie_t ver_cookie;
      xcb_xfixes_query_version_reply_t *ver_reply;

      ver_cookie = xcb_xfixes_query_version(conn, 6, 0);
      ver_reply = xcb_xfixes_query_version_reply(conn, ver_cookie, NULL);
      wsi_conn->has_xfixes = (ver_reply->major_version >= 2);
      free(ver_reply);
   }

   wsi_conn->is_xwayland = wsi_x11_detect_xwayland(conn, randr_reply,
                                                   xwl_reply);

   wsi_conn->has_dri3_modifiers = has_dri3_v1_2 && has_present_v1_2;
   wsi_conn->is_proprietary_x11 = false;
   if (amd_reply && amd_reply->present)
      wsi_conn->is_proprietary_x11 = true;
   if (nv_reply && nv_reply->present)
      wsi_conn->is_proprietary_x11 = true;

   wsi_conn->has_mit_shm = false;
   if (wsi_conn->has_dri3 && wsi_conn->has_present && wants_shm) {
      bool has_mit_shm = shm_reply->present != 0;

      xcb_shm_query_version_cookie_t ver_cookie;
      xcb_shm_query_version_reply_t *ver_reply;

      ver_cookie = xcb_shm_query_version(conn);
      ver_reply = xcb_shm_query_version_reply(conn, ver_cookie, NULL);

      has_mit_shm = ver_reply->shared_pixmaps;
      free(ver_reply);
      xcb_void_cookie_t cookie;
      xcb_generic_error_t *error;

      if (has_mit_shm) {
         cookie = xcb_shm_detach_checked(conn, 0);
         if ((error = xcb_request_check(conn, cookie))) {
            if (error->error_code != BadRequest)
               wsi_conn->has_mit_shm = true;
            free(error);
         }
      }
   }

   free(dri3_reply);
   free(pres_reply);
   free(randr_reply);
   free(xwl_reply);
   free(amd_reply);
   free(nv_reply);
   free(xfixes_reply);
   if (wants_shm)
      free(shm_reply);

   return wsi_conn;
}

static void
wsi_x11_connection_destroy(struct wsi_device *wsi_dev,
                           struct wsi_x11_connection *conn)
{
   vk_free(&wsi_dev->instance_alloc, conn);
}

static bool
wsi_x11_check_for_dri3(struct wsi_x11_connection *wsi_conn)
{
  if (wsi_conn->has_dri3)
    return true;
  if (!wsi_conn->is_proprietary_x11) {
    fprintf(stderr, "vulkan: No DRI3 support detected - required for presentation\n"
                    "Note: you can probably enable DRI3 in your Xorg config\n");
  }
  return false;
}

/**
 * Get internal struct representing an xcb_connection_t.
 *
 * This can allocate the struct but the caller does not own the struct. It is
 * deleted on wsi_x11_finish_wsi by the hash table it is inserted.
 *
 * If the allocation fails NULL is returned.
 */
static struct wsi_x11_connection *
wsi_x11_get_connection(struct wsi_device *wsi_dev,
                       xcb_connection_t *conn)
{
   struct wsi_x11 *wsi =
      (struct wsi_x11 *)wsi_dev->wsi[VK_ICD_WSI_PLATFORM_XCB];

   pthread_mutex_lock(&wsi->mutex);

   struct hash_entry *entry = _mesa_hash_table_search(wsi->connections, conn);
   if (!entry) {
      /* We're about to make a bunch of blocking calls.  Let's drop the
       * mutex for now so we don't block up too badly.
       */
      pthread_mutex_unlock(&wsi->mutex);

      struct wsi_x11_connection *wsi_conn =
         wsi_x11_connection_create(wsi_dev, conn);
      if (!wsi_conn)
         return NULL;

      pthread_mutex_lock(&wsi->mutex);

      entry = _mesa_hash_table_search(wsi->connections, conn);
      if (entry) {
         /* Oops, someone raced us to it */
         wsi_x11_connection_destroy(wsi_dev, wsi_conn);
      } else {
         entry = _mesa_hash_table_insert(wsi->connections, conn, wsi_conn);
      }
   }

   pthread_mutex_unlock(&wsi->mutex);

   return entry->data;
}

struct surface_format {
   VkFormat format;
   unsigned bits_per_rgb;
};

static const struct surface_format formats[] = {
   { VK_FORMAT_B8G8R8A8_SRGB,             8 },
   { VK_FORMAT_B8G8R8A8_UNORM,            8 },
   { VK_FORMAT_A2R10G10B10_UNORM_PACK32, 10 },
};

static const VkPresentModeKHR present_modes[] = {
   VK_PRESENT_MODE_IMMEDIATE_KHR,
   VK_PRESENT_MODE_MAILBOX_KHR,
   VK_PRESENT_MODE_FIFO_KHR,
   VK_PRESENT_MODE_FIFO_RELAXED_KHR,
};

static xcb_screen_t *
get_screen_for_root(xcb_connection_t *conn, xcb_window_t root)
{
   xcb_screen_iterator_t screen_iter =
      xcb_setup_roots_iterator(xcb_get_setup(conn));

   for (; screen_iter.rem; xcb_screen_next (&screen_iter)) {
      if (screen_iter.data->root == root)
         return screen_iter.data;
   }

   return NULL;
}

static xcb_visualtype_t *
screen_get_visualtype(xcb_screen_t *screen, xcb_visualid_t visual_id,
                      unsigned *depth)
{
   xcb_depth_iterator_t depth_iter =
      xcb_screen_allowed_depths_iterator(screen);

   for (; depth_iter.rem; xcb_depth_next (&depth_iter)) {
      xcb_visualtype_iterator_t visual_iter =
         xcb_depth_visuals_iterator (depth_iter.data);

      for (; visual_iter.rem; xcb_visualtype_next (&visual_iter)) {
         if (visual_iter.data->visual_id == visual_id) {
            if (depth)
               *depth = depth_iter.data->depth;
            return visual_iter.data;
         }
      }
   }

   return NULL;
}

static xcb_visualtype_t *
connection_get_visualtype(xcb_connection_t *conn, xcb_visualid_t visual_id)
{
   xcb_screen_iterator_t screen_iter =
      xcb_setup_roots_iterator(xcb_get_setup(conn));

   /* For this we have to iterate over all of the screens which is rather
    * annoying.  Fortunately, there is probably only 1.
    */
   for (; screen_iter.rem; xcb_screen_next (&screen_iter)) {
      xcb_visualtype_t *visual = screen_get_visualtype(screen_iter.data,
                                                       visual_id, NULL);
      if (visual)
         return visual;
   }

   return NULL;
}

static xcb_visualtype_t *
get_visualtype_for_window(xcb_connection_t *conn, xcb_window_t window,
                          unsigned *depth)
{
   xcb_query_tree_cookie_t tree_cookie;
   xcb_get_window_attributes_cookie_t attrib_cookie;
   xcb_query_tree_reply_t *tree;
   xcb_get_window_attributes_reply_t *attrib;

   tree_cookie = xcb_query_tree(conn, window);
   attrib_cookie = xcb_get_window_attributes(conn, window);

   tree = xcb_query_tree_reply(conn, tree_cookie, NULL);
   attrib = xcb_get_window_attributes_reply(conn, attrib_cookie, NULL);
   if (attrib == NULL || tree == NULL) {
      free(attrib);
      free(tree);
      return NULL;
   }

   xcb_window_t root = tree->root;
   xcb_visualid_t visual_id = attrib->visual;
   free(attrib);
   free(tree);

   xcb_screen_t *screen = get_screen_for_root(conn, root);
   if (screen == NULL)
      return NULL;

   return screen_get_visualtype(screen, visual_id, depth);
}

static bool
visual_has_alpha(xcb_visualtype_t *visual, unsigned depth)
{
   uint32_t rgb_mask = visual->red_mask |
                       visual->green_mask |
                       visual->blue_mask;

   uint32_t all_mask = 0xffffffff >> (32 - depth);

   /* Do we have bits left over after RGB? */
   return (all_mask & ~rgb_mask) != 0;
}

static bool
visual_supported(xcb_visualtype_t *visual)
{
   if (!visual)
      return false;

   return visual->_class == XCB_VISUAL_CLASS_TRUE_COLOR ||
          visual->_class == XCB_VISUAL_CLASS_DIRECT_COLOR;
}

VkBool32 wsi_get_physical_device_xcb_presentation_support(
    struct wsi_device *wsi_device,
    uint32_t queueFamilyIndex,
    xcb_connection_t *connection,
    xcb_visualid_t visual_id)
{
   struct wsi_x11_connection *wsi_conn =
      wsi_x11_get_connection(wsi_device, connection);

   if (!wsi_conn)
      return false;

   if (!wsi_device->sw) {
      if (!wsi_x11_check_for_dri3(wsi_conn))
         return false;
   }

   if (!visual_supported(connection_get_visualtype(connection, visual_id)))
      return false;

   return true;
}

VKAPI_ATTR VkBool32 VKAPI_CALL
wsi_GetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice physicalDevice,
                                               uint32_t queueFamilyIndex,
                                               xcb_connection_t *connection,
                                               xcb_visualid_t visual_id)
{
   VK_FROM_HANDLE(vk_physical_device, pdevice, physicalDevice);
   struct wsi_device *wsi_device = pdevice->wsi_device;

   return wsi_get_physical_device_xcb_presentation_support(wsi_device,
                                                           queueFamilyIndex,
                                                           connection,
                                                           visual_id);
}

VKAPI_ATTR VkBool32 VKAPI_CALL
wsi_GetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice physicalDevice,
                                                uint32_t queueFamilyIndex,
                                                Display *dpy,
                                                VisualID visualID)
{
   return wsi_GetPhysicalDeviceXcbPresentationSupportKHR(physicalDevice,
                                                         queueFamilyIndex,
                                                         XGetXCBConnection(dpy),
                                                         visualID);
}

static xcb_connection_t*
x11_surface_get_connection(VkIcdSurfaceBase *icd_surface)
{
   if (icd_surface->platform == VK_ICD_WSI_PLATFORM_XLIB)
      return XGetXCBConnection(((VkIcdSurfaceXlib *)icd_surface)->dpy);
   else
      return ((VkIcdSurfaceXcb *)icd_surface)->connection;
}

static xcb_window_t
x11_surface_get_window(VkIcdSurfaceBase *icd_surface)
{
   if (icd_surface->platform == VK_ICD_WSI_PLATFORM_XLIB)
      return ((VkIcdSurfaceXlib *)icd_surface)->window;
   else
      return ((VkIcdSurfaceXcb *)icd_surface)->window;
}

static VkResult
x11_surface_get_support(VkIcdSurfaceBase *icd_surface,
                        struct wsi_device *wsi_device,
                        uint32_t queueFamilyIndex,
                        VkBool32* pSupported)
{
   xcb_connection_t *conn = x11_surface_get_connection(icd_surface);
   xcb_window_t window = x11_surface_get_window(icd_surface);

   struct wsi_x11_connection *wsi_conn =
      wsi_x11_get_connection(wsi_device, conn);
   if (!wsi_conn)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   if (!wsi_device->sw) {
      if (!wsi_x11_check_for_dri3(wsi_conn)) {
         *pSupported = false;
         return VK_SUCCESS;
      }
   }

   if (!visual_supported(get_visualtype_for_window(conn, window, NULL))) {
      *pSupported = false;
      return VK_SUCCESS;
   }

   *pSupported = true;
   return VK_SUCCESS;
}

static uint32_t
x11_get_min_image_count(const struct wsi_device *wsi_device)
{
   if (wsi_device->x11.override_minImageCount)
      return wsi_device->x11.override_minImageCount;

   /* For IMMEDIATE and FIFO, most games work in a pipelined manner where the
    * can produce frames at a rate of 1/MAX(CPU duration, GPU duration), but
    * the render latency is CPU duration + GPU duration.
    *
    * This means that with scanout from pageflipping we need 3 frames to run
    * full speed:
    * 1) CPU rendering work
    * 2) GPU rendering work
    * 3) scanout
    *
    * Once we have a nonblocking acquire that returns a semaphore we can merge
    * 1 and 3. Hence the ideal implementation needs only 2 images, but games
    * cannot tellwe currently do not have an ideal implementation and that
    * hence they need to allocate 3 images. So let us do it for them.
    *
    * This is a tradeoff as it uses more memory than needed for non-fullscreen
    * and non-performance intensive applications.
    */
   return 3;
}

static VkResult
x11_surface_get_capabilities(VkIcdSurfaceBase *icd_surface,
                             struct wsi_device *wsi_device,
                             VkSurfaceCapabilitiesKHR *caps)
{
   xcb_connection_t *conn = x11_surface_get_connection(icd_surface);
   xcb_window_t window = x11_surface_get_window(icd_surface);
   xcb_get_geometry_cookie_t geom_cookie;
   xcb_generic_error_t *err;
   xcb_get_geometry_reply_t *geom;
   unsigned visual_depth;

   geom_cookie = xcb_get_geometry(conn, window);

   /* This does a round-trip.  This is why we do get_geometry first and
    * wait to read the reply until after we have a visual.
    */
   xcb_visualtype_t *visual =
      get_visualtype_for_window(conn, window, &visual_depth);

   if (!visual)
      return VK_ERROR_SURFACE_LOST_KHR;

   geom = xcb_get_geometry_reply(conn, geom_cookie, &err);
   if (geom) {
      VkExtent2D extent = { geom->width, geom->height };
      caps->currentExtent = extent;
      caps->minImageExtent = extent;
      caps->maxImageExtent = extent;
   }
   free(err);
   free(geom);
   if (!geom)
       return VK_ERROR_SURFACE_LOST_KHR;

   if (visual_has_alpha(visual, visual_depth)) {
      caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR |
                                      VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
   } else {
      caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR |
                                      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
   }

   caps->minImageCount = x11_get_min_image_count(wsi_device);
   /* There is no real maximum */
   caps->maxImageCount = 0;

   caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->maxImageArrayLayers = 1;
   caps->supportedUsageFlags =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
      VK_IMAGE_USAGE_SAMPLED_BIT |
      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
      VK_IMAGE_USAGE_STORAGE_BIT |
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;

   return VK_SUCCESS;
}

static VkResult
x11_surface_get_capabilities2(VkIcdSurfaceBase *icd_surface,
                              struct wsi_device *wsi_device,
                              const void *info_next,
                              VkSurfaceCapabilities2KHR *caps)
{
   assert(caps->sType == VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR);

   VkResult result =
      x11_surface_get_capabilities(icd_surface, wsi_device,
                                   &caps->surfaceCapabilities);

   if (result != VK_SUCCESS)
      return result;

   vk_foreach_struct(ext, caps->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR: {
         VkSurfaceProtectedCapabilitiesKHR *protected = (void *)ext;
         protected->supportsProtected = VK_FALSE;
         break;
      }

      default:
         /* Ignored */
         break;
      }
   }

   return result;
}

static bool
get_sorted_vk_formats(VkIcdSurfaceBase *surface, struct wsi_device *wsi_device,
                      VkFormat *sorted_formats, unsigned *count)
{
   xcb_connection_t *conn = x11_surface_get_connection(surface);
   xcb_window_t window = x11_surface_get_window(surface);
   xcb_visualtype_t *visual = get_visualtype_for_window(conn, window, NULL);
   if (!visual)
      return false;

   *count = 0;
   for (unsigned i = 0; i < ARRAY_SIZE(formats); i++) {
      if (formats[i].bits_per_rgb == util_bitcount(visual->red_mask) &&
          formats[i].bits_per_rgb == util_bitcount(visual->green_mask) &&
          formats[i].bits_per_rgb == util_bitcount(visual->blue_mask))
         sorted_formats[(*count)++] = formats[i].format;
   }

   if (wsi_device->force_bgra8_unorm_first) {
      for (unsigned i = 0; i < *count; i++) {
         if (sorted_formats[i] == VK_FORMAT_B8G8R8A8_UNORM) {
            sorted_formats[i] = sorted_formats[0];
            sorted_formats[0] = VK_FORMAT_B8G8R8A8_UNORM;
            break;
         }
      }
   }

   return true;
}

static VkResult
x11_surface_get_formats(VkIcdSurfaceBase *surface,
                        struct wsi_device *wsi_device,
                        uint32_t *pSurfaceFormatCount,
                        VkSurfaceFormatKHR *pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormatKHR, out,
                          pSurfaceFormats, pSurfaceFormatCount);

   unsigned count;
   VkFormat sorted_formats[ARRAY_SIZE(formats)];
   if (!get_sorted_vk_formats(surface, wsi_device, sorted_formats, &count))
      return VK_ERROR_SURFACE_LOST_KHR;

   for (unsigned i = 0; i < count; i++) {
      vk_outarray_append_typed(VkSurfaceFormatKHR, &out, f) {
         f->format = sorted_formats[i];
         f->colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }

   return vk_outarray_status(&out);
}

static VkResult
x11_surface_get_formats2(VkIcdSurfaceBase *surface,
                        struct wsi_device *wsi_device,
                        const void *info_next,
                        uint32_t *pSurfaceFormatCount,
                        VkSurfaceFormat2KHR *pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormat2KHR, out,
                          pSurfaceFormats, pSurfaceFormatCount);

   unsigned count;
   VkFormat sorted_formats[ARRAY_SIZE(formats)];
   if (!get_sorted_vk_formats(surface, wsi_device, sorted_formats, &count))
      return VK_ERROR_SURFACE_LOST_KHR;

   for (unsigned i = 0; i < count; i++) {
      vk_outarray_append_typed(VkSurfaceFormat2KHR, &out, f) {
         assert(f->sType == VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR);
         f->surfaceFormat.format = sorted_formats[i];
         f->surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }

   return vk_outarray_status(&out);
}

static VkResult
x11_surface_get_present_modes(VkIcdSurfaceBase *surface,
                              uint32_t *pPresentModeCount,
                              VkPresentModeKHR *pPresentModes)
{
   if (pPresentModes == NULL) {
      *pPresentModeCount = ARRAY_SIZE(present_modes);
      return VK_SUCCESS;
   }

   *pPresentModeCount = MIN2(*pPresentModeCount, ARRAY_SIZE(present_modes));
   typed_memcpy(pPresentModes, present_modes, *pPresentModeCount);

   return *pPresentModeCount < ARRAY_SIZE(present_modes) ?
      VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult
x11_surface_get_present_rectangles(VkIcdSurfaceBase *icd_surface,
                                   struct wsi_device *wsi_device,
                                   uint32_t* pRectCount,
                                   VkRect2D* pRects)
{
   xcb_connection_t *conn = x11_surface_get_connection(icd_surface);
   xcb_window_t window = x11_surface_get_window(icd_surface);
   VK_OUTARRAY_MAKE_TYPED(VkRect2D, out, pRects, pRectCount);

   vk_outarray_append_typed(VkRect2D, &out, rect) {
      xcb_generic_error_t *err = NULL;
      xcb_get_geometry_cookie_t geom_cookie = xcb_get_geometry(conn, window);
      xcb_get_geometry_reply_t *geom =
         xcb_get_geometry_reply(conn, geom_cookie, &err);
      free(err);
      if (geom) {
         *rect = (VkRect2D) {
            .offset = { 0, 0 },
            .extent = { geom->width, geom->height },
         };
      }
      free(geom);
      if (!geom)
          return VK_ERROR_SURFACE_LOST_KHR;
   }

   return vk_outarray_status(&out);
}

VkResult wsi_create_xcb_surface(const VkAllocationCallbacks *allocator,
				const VkXcbSurfaceCreateInfoKHR *pCreateInfo,
				VkSurfaceKHR *pSurface)
{
   VkIcdSurfaceXcb *surface;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR);

   surface = vk_alloc(allocator, sizeof *surface, 8,
                      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (surface == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   surface->base.platform = VK_ICD_WSI_PLATFORM_XCB;
   surface->connection = pCreateInfo->connection;
   surface->window = pCreateInfo->window;

   *pSurface = VkIcdSurfaceBase_to_handle(&surface->base);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
wsi_CreateXcbSurfaceKHR(VkInstance _instance,
                        const VkXcbSurfaceCreateInfoKHR *pCreateInfo,
                        const VkAllocationCallbacks *pAllocator,
                        VkSurfaceKHR *pSurface)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   const VkAllocationCallbacks *allocator;

   if (pAllocator)
     allocator = pAllocator;
   else
     allocator = &instance->alloc;

   return wsi_create_xcb_surface(allocator,
                                 pCreateInfo,
                                 pSurface);
}

VkResult wsi_create_xlib_surface(const VkAllocationCallbacks *allocator,
				 const VkXlibSurfaceCreateInfoKHR *pCreateInfo,
				 VkSurfaceKHR *pSurface)
{
   VkIcdSurfaceXlib *surface;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR);

   surface = vk_alloc(allocator, sizeof *surface, 8,
                      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (surface == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   surface->base.platform = VK_ICD_WSI_PLATFORM_XLIB;
   surface->dpy = pCreateInfo->dpy;
   surface->window = pCreateInfo->window;

   *pSurface = VkIcdSurfaceBase_to_handle(&surface->base);
   return VK_SUCCESS;
}

struct x11_image {
   struct wsi_image                          base;
   xcb_pixmap_t                              pixmap;
   xcb_xfixes_region_t                       update_region; /* long lived XID */
   xcb_xfixes_region_t                       update_area;   /* the above or None */
   bool                                      busy;
   bool                                      present_queued;
   struct xshmfence *                        shm_fence;
   uint32_t                                  sync_fence;
   uint32_t                                  serial;
   xcb_shm_seg_t                             shmseg;
   int                                       shmid;
   uint8_t *                                 shmaddr;
};

VKAPI_ATTR VkResult VKAPI_CALL
wsi_CreateXlibSurfaceKHR(VkInstance _instance,
                         const VkXlibSurfaceCreateInfoKHR *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkSurfaceKHR *pSurface)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   const VkAllocationCallbacks *allocator;

   if (pAllocator)
     allocator = pAllocator;
   else
     allocator = &instance->alloc;

   return wsi_create_xlib_surface(allocator,
                                  pCreateInfo,
                                  pSurface);
}

struct x11_swapchain {
   struct wsi_swapchain                        base;

   bool                                         has_dri3_modifiers;
   bool                                         has_mit_shm;

   xcb_connection_t *                           conn;
   xcb_window_t                                 window;
   xcb_gc_t                                     gc;
   uint32_t                                     depth;
   VkExtent2D                                   extent;

   xcb_present_event_t                          event_id;
   xcb_special_event_t *                        special_event;
   uint64_t                                     send_sbc;
   uint64_t                                     last_present_msc;
   uint32_t                                     stamp;
   atomic_int                                   sent_image_count;

   bool                                         has_present_queue;
   bool                                         has_acquire_queue;
   VkResult                                     status;
   bool                                         copy_is_suboptimal;
   struct wsi_queue                             present_queue;
   struct wsi_queue                             acquire_queue;
   pthread_t                                    queue_manager;

   struct x11_image                             images[0];
};
VK_DEFINE_NONDISP_HANDLE_CASTS(x11_swapchain, base.base, VkSwapchainKHR,
                               VK_OBJECT_TYPE_SWAPCHAIN_KHR)

/**
 * Update the swapchain status with the result of an operation, and return
 * the combined status. The chain status will eventually be returned from
 * AcquireNextImage and QueuePresent.
 *
 * We make sure to 'stick' more pessimistic statuses: an out-of-date error
 * is permanent once seen, and every subsequent call will return this. If
 * this has not been seen, success will be returned.
 */
static VkResult
_x11_swapchain_result(struct x11_swapchain *chain, VkResult result,
                      const char *file, int line)
{
   /* Prioritise returning existing errors for consistency. */
   if (chain->status < 0)
      return chain->status;

   /* If we have a new error, mark it as permanent on the chain and return. */
   if (result < 0) {
#ifndef NDEBUG
      fprintf(stderr, "%s:%d: Swapchain status changed to %s\n",
              file, line, vk_Result_to_str(result));
#endif
      chain->status = result;
      return result;
   }

   /* Return temporary errors, but don't persist them. */
   if (result == VK_TIMEOUT || result == VK_NOT_READY)
      return result;

   /* Suboptimal isn't an error, but is a status which sticks to the swapchain
    * and is always returned rather than success.
    */
   if (result == VK_SUBOPTIMAL_KHR) {
#ifndef NDEBUG
      if (chain->status != VK_SUBOPTIMAL_KHR) {
         fprintf(stderr, "%s:%d: Swapchain status changed to %s\n",
                 file, line, vk_Result_to_str(result));
      }
#endif
      chain->status = result;
      return result;
   }

   /* No changes, so return the last status. */
   return chain->status;
}
#define x11_swapchain_result(chain, result) \
   _x11_swapchain_result(chain, result, __FILE__, __LINE__)

static struct wsi_image *
x11_get_wsi_image(struct wsi_swapchain *wsi_chain, uint32_t image_index)
{
   struct x11_swapchain *chain = (struct x11_swapchain *)wsi_chain;
   return &chain->images[image_index].base;
}

/**
 * Process an X11 Present event. Does not update chain->status.
 */
static VkResult
x11_handle_dri3_present_event(struct x11_swapchain *chain,
                              xcb_present_generic_event_t *event)
{
   switch (event->evtype) {
   case XCB_PRESENT_CONFIGURE_NOTIFY: {
      xcb_present_configure_notify_event_t *config = (void *) event;

      if (config->width != chain->extent.width ||
          config->height != chain->extent.height)
         return VK_SUBOPTIMAL_KHR;

      break;
   }

   case XCB_PRESENT_EVENT_IDLE_NOTIFY: {
      xcb_present_idle_notify_event_t *idle = (void *) event;

      for (unsigned i = 0; i < chain->base.image_count; i++) {
         if (chain->images[i].pixmap == idle->pixmap) {
            chain->images[i].busy = false;
            chain->sent_image_count--;
            assert(chain->sent_image_count >= 0);
            if (chain->has_acquire_queue)
               wsi_queue_push(&chain->acquire_queue, i);
            break;
         }
      }

      break;
   }

   case XCB_PRESENT_EVENT_COMPLETE_NOTIFY: {
      xcb_present_complete_notify_event_t *complete = (void *) event;
      if (complete->kind == XCB_PRESENT_COMPLETE_KIND_PIXMAP) {
         unsigned i;
         for (i = 0; i < chain->base.image_count; i++) {
            struct x11_image *image = &chain->images[i];
            if (image->present_queued && image->serial == complete->serial)
               image->present_queued = false;
         }
         chain->last_present_msc = complete->msc;
      }

      VkResult result = VK_SUCCESS;
      switch (complete->mode) {
      case XCB_PRESENT_COMPLETE_MODE_COPY:
         if (chain->copy_is_suboptimal)
            result = VK_SUBOPTIMAL_KHR;
         break;
      case XCB_PRESENT_COMPLETE_MODE_FLIP:
         /* If we ever go from flipping to copying, the odds are very likely
          * that we could reallocate in a more optimal way if we didn't have
          * to care about scanout, so we always do this.
          */
         chain->copy_is_suboptimal = true;
         break;
#ifdef HAVE_DRI3_MODIFIERS
      case XCB_PRESENT_COMPLETE_MODE_SUBOPTIMAL_COPY:
         /* The winsys is now trying to flip directly and cannot due to our
          * configuration. Request the user reallocate.
          */
         result = VK_SUBOPTIMAL_KHR;
         break;
#endif
      default:
         break;
      }

      return result;
   }

   default:
      break;
   }

   return VK_SUCCESS;
}


static uint64_t wsi_get_absolute_timeout(uint64_t timeout)
{
   uint64_t current_time = os_time_get_nano();

   timeout = MIN2(UINT64_MAX - current_time, timeout);

   return current_time + timeout;
}

/**
 * Acquire a ready-to-use image directly from our swapchain. If all images are
 * busy wait until one is not anymore or till timeout.
 */
static VkResult
x11_acquire_next_image_poll_x11(struct x11_swapchain *chain,
                                uint32_t *image_index, uint64_t timeout)
{
   xcb_generic_event_t *event;
   struct pollfd pfds;
   uint64_t atimeout;
   while (1) {
      for (uint32_t i = 0; i < chain->base.image_count; i++) {
         if (!chain->images[i].busy) {
            /* We found a non-busy image */
            xshmfence_await(chain->images[i].shm_fence);
            *image_index = i;
            chain->images[i].busy = true;
            return x11_swapchain_result(chain, VK_SUCCESS);
         }
      }

      xcb_flush(chain->conn);

      if (timeout == UINT64_MAX) {
         event = xcb_wait_for_special_event(chain->conn, chain->special_event);
         if (!event)
            return x11_swapchain_result(chain, VK_ERROR_SURFACE_LOST_KHR);
      } else {
         event = xcb_poll_for_special_event(chain->conn, chain->special_event);
         if (!event) {
            int ret;
            if (timeout == 0)
               return x11_swapchain_result(chain, VK_NOT_READY);

            atimeout = wsi_get_absolute_timeout(timeout);

            pfds.fd = xcb_get_file_descriptor(chain->conn);
            pfds.events = POLLIN;
            ret = poll(&pfds, 1, timeout / 1000 / 1000);
            if (ret == 0)
               return x11_swapchain_result(chain, VK_TIMEOUT);
            if (ret == -1)
               return x11_swapchain_result(chain, VK_ERROR_OUT_OF_DATE_KHR);

            /* If a non-special event happens, the fd will still
             * poll. So recalculate the timeout now just in case.
             */
            uint64_t current_time = os_time_get_nano();
            if (atimeout > current_time)
               timeout = atimeout - current_time;
            else
               timeout = 0;
            continue;
         }
      }

      /* Update the swapchain status here. We may catch non-fatal errors here,
       * in which case we need to update the status and continue.
       */
      VkResult result = x11_handle_dri3_present_event(chain, (void *)event);
      /* Ensure that VK_SUBOPTIMAL_KHR is reported to the application */
      result = x11_swapchain_result(chain, result);
      free(event);
      if (result < 0)
         return result;
   }
}

/**
 * Acquire a ready-to-use image from the acquire-queue. Only relevant in fifo
 * presentation mode.
 */
static VkResult
x11_acquire_next_image_from_queue(struct x11_swapchain *chain,
                                  uint32_t *image_index_out, uint64_t timeout)
{
   assert(chain->has_acquire_queue);

   uint32_t image_index;
   VkResult result = wsi_queue_pull(&chain->acquire_queue,
                                    &image_index, timeout);
   if (result < 0 || result == VK_TIMEOUT) {
      /* On error, the thread has shut down, so safe to update chain->status.
       * Calling x11_swapchain_result with VK_TIMEOUT won't modify
       * chain->status so that is also safe.
       */
      return x11_swapchain_result(chain, result);
   } else if (chain->status < 0) {
      return chain->status;
   }

   assert(image_index < chain->base.image_count);
   xshmfence_await(chain->images[image_index].shm_fence);

   *image_index_out = image_index;

   return chain->status;
}

/**
 * Send image to X server via Present extension.
 */
static VkResult
x11_present_to_x11_dri3(struct x11_swapchain *chain, uint32_t image_index,
                        uint64_t target_msc)
{
   struct x11_image *image = &chain->images[image_index];

   assert(image_index < chain->base.image_count);

   uint32_t options = XCB_PRESENT_OPTION_NONE;

   int64_t divisor = 0;
   int64_t remainder = 0;

   struct wsi_x11_connection *wsi_conn =
      wsi_x11_get_connection((struct wsi_device*)chain->base.wsi, chain->conn);
   if (!wsi_conn)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   if (chain->base.present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ||
       (chain->base.present_mode == VK_PRESENT_MODE_MAILBOX_KHR &&
        wsi_conn->is_xwayland) ||
       chain->base.present_mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR)
      options |= XCB_PRESENT_OPTION_ASYNC;

#ifdef HAVE_DRI3_MODIFIERS
   if (chain->has_dri3_modifiers)
      options |= XCB_PRESENT_OPTION_SUBOPTIMAL;
#endif

   /* Poll for any available event and update the swapchain status. This could
    * update the status of the swapchain to SUBOPTIMAL or OUT_OF_DATE if the
    * associated X11 surface has been resized.
    */
   xcb_generic_event_t *event;
   while ((event = xcb_poll_for_special_event(chain->conn, chain->special_event))) {
      VkResult result = x11_handle_dri3_present_event(chain, (void *)event);
      /* Ensure that VK_SUBOPTIMAL_KHR is reported to the application */
      result = x11_swapchain_result(chain, result);
      free(event);
      if (result < 0)
         return result;
   }

   xshmfence_reset(image->shm_fence);

   ++chain->sent_image_count;
   assert(chain->sent_image_count <= chain->base.image_count);

   ++chain->send_sbc;
   image->present_queued = true;
   image->serial = (uint32_t) chain->send_sbc;

   xcb_void_cookie_t cookie =
      xcb_present_pixmap_checked(chain->conn,
                                 chain->window,
                                 image->pixmap,
                                 image->serial,
                                 0,                            /* valid */
                                 image->update_area,           /* update */
                                 0,                            /* x_off */
                                 0,                            /* y_off */
                                 XCB_NONE,                     /* target_crtc */
                                 XCB_NONE,
                                 image->sync_fence,
                                 options,
                                 target_msc,
                                 divisor,
                                 remainder, 0, NULL);
   xcb_generic_error_t *error = xcb_request_check(chain->conn, cookie);
   if (error) {
      free(error);
      return x11_swapchain_result(chain, VK_ERROR_SURFACE_LOST_KHR);
   }

   return x11_swapchain_result(chain, VK_SUCCESS);
}

/**
 * Send image to X server unaccelerated (software drivers).
 */
static VkResult
x11_present_to_x11_sw(struct x11_swapchain *chain, uint32_t image_index,
                      uint64_t target_msc)
{
   struct x11_image *image = &chain->images[image_index];

   xcb_void_cookie_t cookie;
   void *myptr = image->base.cpu_map;
   size_t hdr_len = sizeof(xcb_put_image_request_t);
   int stride_b = image->base.row_pitches[0];
   size_t size = (hdr_len + stride_b * chain->extent.height) >> 2;
   uint64_t max_req_len = xcb_get_maximum_request_length(chain->conn);

   if (size < max_req_len) {
      cookie = xcb_put_image(chain->conn, XCB_IMAGE_FORMAT_Z_PIXMAP,
                             chain->window,
                             chain->gc,
                             image->base.row_pitches[0] / 4,
                             chain->extent.height,
                             0,0,0,24,
                             image->base.row_pitches[0] * chain->extent.height,
                             image->base.cpu_map);
      xcb_discard_reply(chain->conn, cookie.sequence);
   } else {
      int num_lines = ((max_req_len << 2) - hdr_len) / stride_b;
      int y_start = 0;
      int y_todo = chain->extent.height;
      while (y_todo) {
         int this_lines = MIN2(num_lines, y_todo);
         cookie = xcb_put_image(chain->conn, XCB_IMAGE_FORMAT_Z_PIXMAP,
                                chain->window,
                                chain->gc,
                                image->base.row_pitches[0] / 4,
                                this_lines,
                                0,y_start,0,24,
                                this_lines * stride_b,
                                (const uint8_t *)myptr + (y_start * stride_b));
         xcb_discard_reply(chain->conn, cookie.sequence);
         y_start += this_lines;
         y_todo -= this_lines;
      }
   }

   chain->images[image_index].busy = false;
   xcb_flush(chain->conn);
   return x11_swapchain_result(chain, VK_SUCCESS);
}

/**
 * Send image to the X server for presentation at target_msc.
 */
static VkResult
x11_present_to_x11(struct x11_swapchain *chain, uint32_t image_index,
                   uint64_t target_msc)
{
   if (chain->base.wsi->sw && !chain->has_mit_shm)
      return x11_present_to_x11_sw(chain, image_index, target_msc);
   return x11_present_to_x11_dri3(chain, image_index, target_msc);
}

/**
 * Acquire a ready-to-use image from the swapchain.
 *
 * This means usually that the image is not waiting on presentation and that the
 * image has been released by the X server to be used again by the consumer.
 */
static VkResult
x11_acquire_next_image(struct wsi_swapchain *anv_chain,
                       const VkAcquireNextImageInfoKHR *info,
                       uint32_t *image_index)
{
   struct x11_swapchain *chain = (struct x11_swapchain *)anv_chain;
   uint64_t timeout = info->timeout;

   /* If the swapchain is in an error state, don't go any further. */
   if (chain->status < 0)
      return chain->status;

   if (chain->base.wsi->sw && !chain->has_mit_shm) {
      for (unsigned i = 0; i < chain->base.image_count; i++) {
         if (!chain->images[i].busy) {
            *image_index = i;
            chain->images[i].busy = true;
            xcb_generic_error_t *err;

            xcb_get_geometry_cookie_t geom_cookie = xcb_get_geometry(chain->conn, chain->window);
            xcb_get_geometry_reply_t *geom = xcb_get_geometry_reply(chain->conn, geom_cookie, &err);
            VkResult result = VK_SUCCESS;
            if (geom) {
               if (chain->extent.width != geom->width ||
                   chain->extent.height != geom->height)
                  result = VK_SUBOPTIMAL_KHR;
            } else {
               result = VK_ERROR_SURFACE_LOST_KHR;
            }
            free(err);
            free(geom);
            return result;
         }
      }
      return VK_NOT_READY;
   }

   if (chain->has_acquire_queue) {
      return x11_acquire_next_image_from_queue(chain, image_index, timeout);
   } else {
      return x11_acquire_next_image_poll_x11(chain, image_index, timeout);
   }
}

#define MAX_DAMAGE_RECTS 64

/**
 * Queue a new presentation of an image that was previously acquired by the
 * consumer.
 *
 * Note that in immediate presentation mode this does not really queue the
 * presentation but directly asks the X server to show it.
 */
static VkResult
x11_queue_present(struct wsi_swapchain *anv_chain,
                  uint32_t image_index,
                  const VkPresentRegionKHR *damage)
{
   struct x11_swapchain *chain = (struct x11_swapchain *)anv_chain;
   xcb_xfixes_region_t update_area = 0;

   /* If the swapchain is in an error state, don't go any further. */
   if (chain->status < 0)
      return chain->status;

   if (damage && damage->pRectangles && damage->rectangleCount > 0 &&
      damage->rectangleCount <= MAX_DAMAGE_RECTS) {
      xcb_rectangle_t rects[MAX_DAMAGE_RECTS];

      update_area = chain->images[image_index].update_region;
      for (unsigned i = 0; i < damage->rectangleCount; i++) {
         const VkRectLayerKHR *rect = &damage->pRectangles[i];
         assert(rect->layer == 0);
         rects[i].x = rect->offset.x;
         rects[i].y = rect->offset.y;
         rects[i].width = rect->extent.width;
         rects[i].height = rect->extent.height;
      }
      xcb_xfixes_set_region(chain->conn, update_area, damage->rectangleCount, rects);
   }
   chain->images[image_index].update_area = update_area;

   chain->images[image_index].busy = true;
   if (chain->has_present_queue) {
      wsi_queue_push(&chain->present_queue, image_index);
      return chain->status;
   } else {
      /* No present queue means immedate mode, so we present immediately. */
      return x11_present_to_x11(chain, image_index, 0);
   }
}

/**
 * Decides if an early wait on buffer fences before buffer submission is required. That is for:
 *   - Mailbox mode, as otherwise the latest image in the queue might not be fully rendered at
 *     present time, what could lead to missing a frame.
 *   - Immediate mode under Xwayland, as it works practically the same as mailbox mode using the
 *     mailbox mechanism of Wayland. Sending a buffer with fences not yet signalled can make the
 *     compositor miss a frame when compositing the final image with this buffer.
 *
 * Note though that early waits can be disabled in general on Xwayland by setting the
 * 'vk_xwayland_wait_ready' DRIConf option to false.
 */
static bool
x11_needs_wait_for_fences(const struct wsi_device *wsi_device,
                          struct wsi_x11_connection *wsi_conn,
                          VkPresentModeKHR present_mode)
{
   if (wsi_conn->is_xwayland && !wsi_device->x11.xwaylandWaitReady) {
      return false;
   }

   switch (present_mode) {
   case VK_PRESENT_MODE_MAILBOX_KHR:
      return true;
   case VK_PRESENT_MODE_IMMEDIATE_KHR:
      return wsi_conn->is_xwayland;
   default:
      return false;
   }
}

/**
 * The number of images that are not owned by X11:
 *  (1) in the ownership of the app, or
 *  (2) app to take ownership through an acquire, or
 *  (3) in the present queue waiting for the FIFO thread to present to X11.
 */
static unsigned x11_driver_owned_images(const struct x11_swapchain *chain)
{
   return chain->base.image_count - chain->sent_image_count;
}

/**
 * Our queue manager. Albeit called x11_manage_fifo_queues only directly
 * manages the present-queue and does this in general in fifo and mailbox presentation
 * modes (there is no present-queue in immediate mode with the exception of Xwayland).
 *
 * Runs in a separate thread, blocks and reacts to queued images on the
 * present-queue
 *
 * In mailbox mode the queue management is simplified since we only need to
 * pull new images from the present queue and can directly present them.
 *
 * In fifo mode images can only be presented one after the other. For that after
 * sending the image to the X server we wait until the image either has been
 * presented or released and only then pull a new image from the present-queue.
 */
static void *
x11_manage_fifo_queues(void *state)
{
   struct x11_swapchain *chain = state;
   struct wsi_x11_connection *wsi_conn =
      wsi_x11_get_connection((struct wsi_device*)chain->base.wsi, chain->conn);
   VkResult result = VK_SUCCESS;

   assert(chain->has_present_queue);

   u_thread_setname("WSI swapchain queue");

   while (chain->status >= 0) {
      /* We can block here unconditionally because after an image was sent to
       * the server (later on in this loop) we ensure at least one image is
       * acquirable by the consumer or wait there on such an event.
       */
      uint32_t image_index = 0;
      {
         MESA_TRACE_SCOPE("pull present queue");
         result = wsi_queue_pull(&chain->present_queue, &image_index, INT64_MAX);
         assert(result != VK_TIMEOUT);
      }

      if (result < 0) {
         goto fail;
      } else if (chain->status < 0) {
         /* The status can change underneath us if the swapchain is destroyed
          * from another thread.
          */
         return NULL;
      }

      /* Waiting for the GPU work to finish at this point in time is required in certain usage
       * scenarios. Otherwise we wait as usual in wsi_common_queue_present.
       */
      if (x11_needs_wait_for_fences(chain->base.wsi, wsi_conn,
                                    chain->base.present_mode)) {
         MESA_TRACE_SCOPE("wait fence");
         result = chain->base.wsi->WaitForFences(chain->base.device, 1,
                                        &chain->base.fences[image_index],
                                        true, UINT64_MAX);
         if (result != VK_SUCCESS) {
            result = VK_ERROR_OUT_OF_DATE_KHR;
            goto fail;
         }
      }

      uint64_t target_msc = 0;
      if (chain->has_acquire_queue)
         target_msc = chain->last_present_msc + 1;

      result = x11_present_to_x11(chain, image_index, target_msc);
      if (result < 0)
         goto fail;

      if (chain->has_acquire_queue) {
         MESA_TRACE_SCOPE("wait present");

         /* Assume this isn't a swapchain where we force 5 images, because those
          * don't end up with an acquire queue at the moment.
          */
         unsigned min_image_count = x11_get_min_image_count(chain->base.wsi);

         /* With drirc overrides some games have swapchain with less than
          * minimum number of images. */
         min_image_count = MIN2(min_image_count, chain->base.image_count);

         /* We always need to ensure that the app can have this number of images
          * acquired concurrently in between presents:
          * "VUID-vkAcquireNextImageKHR-swapchain-01802
          *  If the number of currently acquired images is greater than the difference
          *  between the number of images in swapchain and the value of
          *  VkSurfaceCapabilitiesKHR::minImageCount as returned by a call to
          *  vkGetPhysicalDeviceSurfaceCapabilities2KHR with the surface used to
          *  create swapchain, timeout must not be UINT64_MAX"
          */
         unsigned forward_progress_guaranteed_acquired_images =
            chain->base.image_count - min_image_count + 1;

         /* Wait for our presentation to occur and ensure we have at least one
          * image that can be acquired by the client afterwards. This ensures we
          * can pull on the present-queue on the next loop.
          */
         while (chain->images[image_index].present_queued ||
                /* If we have images in the present queue the outer loop won't block and a break
                 * here would end up at this loop again, otherwise a break here satisfies
                 * VUID-vkAcquireNextImageKHR-swapchain-01802 */
                x11_driver_owned_images(chain) < forward_progress_guaranteed_acquired_images) {

            xcb_generic_event_t *event =
               xcb_wait_for_special_event(chain->conn, chain->special_event);
            if (!event) {
               result = VK_ERROR_SURFACE_LOST_KHR;
               goto fail;
            }

            result = x11_handle_dri3_present_event(chain, (void *)event);
            /* Ensure that VK_SUBOPTIMAL_KHR is reported to the application */
            result = x11_swapchain_result(chain, result);
            free(event);
            if (result < 0)
               goto fail;
         }
      }
   }

fail:
   x11_swapchain_result(chain, result);
   if (chain->has_acquire_queue)
      wsi_queue_push(&chain->acquire_queue, UINT32_MAX);

   return NULL;
}

static uint8_t *
alloc_shm(struct wsi_image *imagew, unsigned size)
{
#ifdef HAVE_SYS_SHM_H
   struct x11_image *image = (struct x11_image *)imagew;
   image->shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | 0600);
   if (image->shmid < 0)
      return NULL;

   uint8_t *addr = (uint8_t *)shmat(image->shmid, 0, 0);
   /* mark the segment immediately for deletion to avoid leaks */
   shmctl(image->shmid, IPC_RMID, 0);

   if (addr == (uint8_t *) -1)
      return NULL;

   image->shmaddr = addr;
   return addr;
#else
   return NULL;
#endif
}

static VkResult
x11_image_init(VkDevice device_h, struct x11_swapchain *chain,
               const VkSwapchainCreateInfoKHR *pCreateInfo,
               const VkAllocationCallbacks* pAllocator,
               int display_fd,
               struct x11_image *image)
{
   xcb_void_cookie_t cookie;
   VkResult result;
   uint32_t bpp = 32;
   int fence_fd;

   result = wsi_create_image(&chain->base, &chain->base.image_info,
                             display_fd, &image->base);
   if (result != VK_SUCCESS)
      return result;

   image->update_region = xcb_generate_id(chain->conn);
   xcb_xfixes_create_region(chain->conn, image->update_region, 0, NULL);

   if (chain->base.wsi->sw) {
      if (!chain->has_mit_shm) {
         image->busy = false;
         return VK_SUCCESS;
      }

      image->shmseg = xcb_generate_id(chain->conn);

      xcb_shm_attach(chain->conn,
                     image->shmseg,
                     image->shmid,
                     0);
      image->pixmap = xcb_generate_id(chain->conn);
      cookie = xcb_shm_create_pixmap_checked(chain->conn,
                                             image->pixmap,
                                             chain->window,
                                             image->base.row_pitches[0] / 4,
                                             pCreateInfo->imageExtent.height,
                                             chain->depth,
                                             image->shmseg, 0);
      xcb_discard_reply(chain->conn, cookie.sequence);
      goto out_fence;
   }
   image->pixmap = xcb_generate_id(chain->conn);

#ifdef HAVE_DRI3_MODIFIERS
   if (image->base.drm_modifier != DRM_FORMAT_MOD_INVALID) {
      /* If the image has a modifier, we must have DRI3 v1.2. */
      assert(chain->has_dri3_modifiers);

      /* XCB requires an array of file descriptors but we only have one */
      int fds[4] = { -1, -1, -1, -1 };
      for (int i = 0; i < image->base.num_planes; i++) {
         fds[i] = os_dupfd_cloexec(image->base.dma_buf_fd);
         if (fds[i] == -1) {
            for (int j = 0; j < i; j++)
               close(fds[j]);

            return VK_ERROR_OUT_OF_HOST_MEMORY;
         }
      }

      cookie =
         xcb_dri3_pixmap_from_buffers_checked(chain->conn,
                                              image->pixmap,
                                              chain->window,
                                              image->base.num_planes,
                                              pCreateInfo->imageExtent.width,
                                              pCreateInfo->imageExtent.height,
                                              image->base.row_pitches[0],
                                              image->base.offsets[0],
                                              image->base.row_pitches[1],
                                              image->base.offsets[1],
                                              image->base.row_pitches[2],
                                              image->base.offsets[2],
                                              image->base.row_pitches[3],
                                              image->base.offsets[3],
                                              chain->depth, bpp,
                                              image->base.drm_modifier,
                                              fds);
   } else
#endif
   {
      /* Without passing modifiers, we can't have multi-plane RGB images. */
      assert(image->base.num_planes == 1);

      /* XCB will take ownership of the FD we pass it. */
      int fd = os_dupfd_cloexec(image->base.dma_buf_fd);
      if (fd == -1)
         return VK_ERROR_OUT_OF_HOST_MEMORY;

      cookie =
         xcb_dri3_pixmap_from_buffer_checked(chain->conn,
                                             image->pixmap,
                                             chain->window,
                                             image->base.sizes[0],
                                             pCreateInfo->imageExtent.width,
                                             pCreateInfo->imageExtent.height,
                                             image->base.row_pitches[0],
                                             chain->depth, bpp, fd);
   }

   xcb_discard_reply(chain->conn, cookie.sequence);

out_fence:
   fence_fd = xshmfence_alloc_shm();
   if (fence_fd < 0)
      goto fail_pixmap;

   image->shm_fence = xshmfence_map_shm(fence_fd);
   if (image->shm_fence == NULL)
      goto fail_shmfence_alloc;

   image->sync_fence = xcb_generate_id(chain->conn);
   xcb_dri3_fence_from_fd(chain->conn,
                          image->pixmap,
                          image->sync_fence,
                          false,
                          fence_fd);

   image->busy = false;
   xshmfence_trigger(image->shm_fence);

   return VK_SUCCESS;

fail_shmfence_alloc:
   close(fence_fd);

fail_pixmap:
   cookie = xcb_free_pixmap(chain->conn, image->pixmap);
   xcb_discard_reply(chain->conn, cookie.sequence);

   wsi_destroy_image(&chain->base, &image->base);

   return VK_ERROR_INITIALIZATION_FAILED;
}

static void
x11_image_finish(struct x11_swapchain *chain,
                 const VkAllocationCallbacks* pAllocator,
                 struct x11_image *image)
{
   xcb_void_cookie_t cookie;

   if (!chain->base.wsi->sw || chain->has_mit_shm) {
      cookie = xcb_sync_destroy_fence(chain->conn, image->sync_fence);
      xcb_discard_reply(chain->conn, cookie.sequence);
      xshmfence_unmap_shm(image->shm_fence);

      cookie = xcb_free_pixmap(chain->conn, image->pixmap);
      xcb_discard_reply(chain->conn, cookie.sequence);

      cookie = xcb_xfixes_destroy_region(chain->conn, image->update_region);
      xcb_discard_reply(chain->conn, cookie.sequence);
   }

   wsi_destroy_image(&chain->base, &image->base);
#ifdef HAVE_SYS_SHM_H
   if (image->shmaddr)
      shmdt(image->shmaddr);
#endif
}

static void
wsi_x11_get_dri3_modifiers(struct wsi_x11_connection *wsi_conn,
                           xcb_connection_t *conn, xcb_window_t window,
                           uint8_t depth, uint8_t bpp,
                           VkCompositeAlphaFlagsKHR vk_alpha,
                           uint64_t **modifiers_in, uint32_t *num_modifiers_in,
                           uint32_t *num_tranches_in,
                           const VkAllocationCallbacks *pAllocator)
{
   if (!wsi_conn->has_dri3_modifiers)
      goto out;

#ifdef HAVE_DRI3_MODIFIERS
   xcb_generic_error_t *error = NULL;
   xcb_dri3_get_supported_modifiers_cookie_t mod_cookie =
      xcb_dri3_get_supported_modifiers(conn, window, depth, bpp);
   xcb_dri3_get_supported_modifiers_reply_t *mod_reply =
      xcb_dri3_get_supported_modifiers_reply(conn, mod_cookie, &error);
   free(error);

   if (!mod_reply || (mod_reply->num_window_modifiers == 0 &&
                      mod_reply->num_screen_modifiers == 0)) {
      free(mod_reply);
      goto out;
   }

   uint32_t n = 0;
   uint32_t counts[2];
   uint64_t *modifiers[2];

   if (mod_reply->num_window_modifiers) {
      counts[n] = mod_reply->num_window_modifiers;
      modifiers[n] = vk_alloc(pAllocator,
                              counts[n] * sizeof(uint64_t),
                              8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!modifiers[n]) {
         free(mod_reply);
         goto out;
      }

      memcpy(modifiers[n],
             xcb_dri3_get_supported_modifiers_window_modifiers(mod_reply),
             counts[n] * sizeof(uint64_t));
      n++;
   }

   if (mod_reply->num_screen_modifiers) {
      counts[n] = mod_reply->num_screen_modifiers;
      modifiers[n] = vk_alloc(pAllocator,
                              counts[n] * sizeof(uint64_t),
                              8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (!modifiers[n]) {
	 if (n > 0)
            vk_free(pAllocator, modifiers[0]);
         free(mod_reply);
         goto out;
      }

      memcpy(modifiers[n],
             xcb_dri3_get_supported_modifiers_screen_modifiers(mod_reply),
             counts[n] * sizeof(uint64_t));
      n++;
   }

   for (int i = 0; i < n; i++) {
      modifiers_in[i] = modifiers[i];
      num_modifiers_in[i] = counts[i];
   }
   *num_tranches_in = n;

   free(mod_reply);
   return;
#endif
out:
   *num_tranches_in = 0;
}

static VkResult
x11_swapchain_destroy(struct wsi_swapchain *anv_chain,
                      const VkAllocationCallbacks *pAllocator)
{
   struct x11_swapchain *chain = (struct x11_swapchain *)anv_chain;
   xcb_void_cookie_t cookie;

   if (chain->has_present_queue) {
      chain->status = VK_ERROR_OUT_OF_DATE_KHR;
      /* Push a UINT32_MAX to wake up the manager */
      wsi_queue_push(&chain->present_queue, UINT32_MAX);
      pthread_join(chain->queue_manager, NULL);

      if (chain->has_acquire_queue)
         wsi_queue_destroy(&chain->acquire_queue);
      wsi_queue_destroy(&chain->present_queue);
   }

   for (uint32_t i = 0; i < chain->base.image_count; i++)
      x11_image_finish(chain, pAllocator, &chain->images[i]);

   xcb_unregister_for_special_event(chain->conn, chain->special_event);
   cookie = xcb_present_select_input_checked(chain->conn, chain->event_id,
                                             chain->window,
                                             XCB_PRESENT_EVENT_MASK_NO_EVENT);
   xcb_discard_reply(chain->conn, cookie.sequence);

   wsi_swapchain_finish(&chain->base);

   vk_free(pAllocator, chain);

   return VK_SUCCESS;
}

static void
wsi_x11_set_adaptive_sync_property(xcb_connection_t *conn,
                                   xcb_drawable_t drawable,
                                   uint32_t state)
{
   static char const name[] = "_VARIABLE_REFRESH";
   xcb_intern_atom_cookie_t cookie;
   xcb_intern_atom_reply_t* reply;
   xcb_void_cookie_t check;

   cookie = xcb_intern_atom(conn, 0, strlen(name), name);
   reply = xcb_intern_atom_reply(conn, cookie, NULL);
   if (reply == NULL)
      return;

   if (state)
      check = xcb_change_property_checked(conn, XCB_PROP_MODE_REPLACE,
                                          drawable, reply->atom,
                                          XCB_ATOM_CARDINAL, 32, 1, &state);
   else
      check = xcb_delete_property_checked(conn, drawable, reply->atom);

   xcb_discard_reply(conn, check.sequence);
   free(reply);
}

/**
 * Create the swapchain.
 *
 * Supports immediate, fifo and mailbox presentation mode.
 *
 */
static VkResult
x11_surface_create_swapchain(VkIcdSurfaceBase *icd_surface,
                             VkDevice device,
                             struct wsi_device *wsi_device,
                             const VkSwapchainCreateInfoKHR *pCreateInfo,
                             const VkAllocationCallbacks* pAllocator,
                             struct wsi_swapchain **swapchain_out)
{
   struct x11_swapchain *chain;
   xcb_void_cookie_t cookie;
   VkResult result;
   VkPresentModeKHR present_mode = wsi_swapchain_get_present_mode(wsi_device, pCreateInfo);

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);

   /* Get xcb connection from the icd_surface and from that our internal struct
    * representing it.
    */
   xcb_connection_t *conn = x11_surface_get_connection(icd_surface);
   struct wsi_x11_connection *wsi_conn =
      wsi_x11_get_connection(wsi_device, conn);
   if (!wsi_conn)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   /* Get number of images in our swapchain. This count depends on:
    * - requested minimal image count
    * - device characteristics
    * - presentation mode.
    */
   unsigned num_images = pCreateInfo->minImageCount;
   if (wsi_device->x11.strict_imageCount)
      num_images = pCreateInfo->minImageCount;
   else if (x11_needs_wait_for_fences(wsi_device, wsi_conn, present_mode))
      num_images = MAX2(num_images, 5);
   else if (wsi_device->x11.ensure_minImageCount)
      num_images = MAX2(num_images, x11_get_min_image_count(wsi_device));

   /* Check that we have a window up-front. It is an error to not have one. */
   xcb_window_t window = x11_surface_get_window(icd_surface);

   /* Get the geometry of that window. The bit depth of the swapchain will be fitted and the
    * chain's images extents should fit it for performance-optimizing flips.
    */
   xcb_get_geometry_reply_t *geometry =
      xcb_get_geometry_reply(conn, xcb_get_geometry(conn, window), NULL);
   if (geometry == NULL)
      return VK_ERROR_SURFACE_LOST_KHR;
   const uint32_t bit_depth = geometry->depth;
   const uint16_t cur_width = geometry->width;
   const uint16_t cur_height = geometry->height;
   free(geometry);

   /* Allocate the actual swapchain. The size depends on image count. */
   size_t size = sizeof(*chain) + num_images * sizeof(chain->images[0]);
   chain = vk_zalloc(pAllocator, size, 8,
                      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (chain == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   struct wsi_base_image_params *image_params = NULL;
   struct wsi_cpu_image_params cpu_image_params;
   struct wsi_drm_image_params drm_image_params;
   uint64_t *modifiers[2] = {NULL, NULL};
   uint32_t num_modifiers[2] = {0, 0};
   if (wsi_device->sw) {
      cpu_image_params = (struct wsi_cpu_image_params) {
         .base.image_type = WSI_IMAGE_TYPE_CPU,
         .alloc_shm = wsi_conn->has_mit_shm ? &alloc_shm : NULL,
      };
      image_params = &cpu_image_params.base;
   } else {
      drm_image_params = (struct wsi_drm_image_params) {
         .base.image_type = WSI_IMAGE_TYPE_DRM,
         .same_gpu = wsi_x11_check_dri3_compatible(wsi_device, conn),
      };
      if (wsi_device->supports_modifiers) {
         wsi_x11_get_dri3_modifiers(wsi_conn, conn, window, bit_depth, 32,
                                    pCreateInfo->compositeAlpha,
                                    modifiers, num_modifiers,
                                    &drm_image_params.num_modifier_lists,
                                    pAllocator);
         drm_image_params.num_modifiers = num_modifiers;
         drm_image_params.modifiers = (const uint64_t **)modifiers;
      }
      image_params = &drm_image_params.base;
   }

   int display_fd;
   if (wsi_device->sw) {
      display_fd = -1;
   } else {
      xcb_screen_iterator_t screen_iter =
         xcb_setup_roots_iterator(xcb_get_setup(conn));
      xcb_screen_t *screen = screen_iter.data;

      display_fd = wsi_dri3_open(conn, screen->root, None);
   }

   result = wsi_swapchain_init(wsi_device, &chain->base, device, pCreateInfo,
                               image_params, pAllocator, display_fd);

   for (int i = 0; i < ARRAY_SIZE(modifiers); i++)
      vk_free(pAllocator, modifiers[i]);

   if (result != VK_SUCCESS)
      goto fail_alloc;

   chain->base.destroy = x11_swapchain_destroy;
   chain->base.get_wsi_image = x11_get_wsi_image;
   chain->base.acquire_next_image = x11_acquire_next_image;
   chain->base.queue_present = x11_queue_present;
   chain->base.present_mode = present_mode;
   chain->base.image_count = num_images;
   chain->conn = conn;
   chain->window = window;
   chain->depth = bit_depth;
   chain->extent = pCreateInfo->imageExtent;
   chain->send_sbc = 0;
   chain->sent_image_count = 0;
   chain->last_present_msc = 0;
   chain->has_acquire_queue = false;
   chain->has_present_queue = false;
   chain->status = VK_SUCCESS;
   chain->has_dri3_modifiers = wsi_conn->has_dri3_modifiers;
   chain->has_mit_shm = wsi_conn->has_mit_shm;

   /* When images in the swapchain don't fit the window, X can still present them, but it won't
    * happen by flip, only by copy. So this is a suboptimal copy, because if the client would change
    * the chain extents X may be able to flip
    */
   if (chain->extent.width != cur_width || chain->extent.height != cur_height)
       chain->status = VK_SUBOPTIMAL_KHR;

   /* On a new swapchain this helper variable is set to false. Once we present it will have an
    * impact once we ever do at least one flip and go back to copying afterwards. It is presumed
    * that in this case here is a high likelihood X could do flips again if the client reallocates a
    * new swapchain.
    *
    * Note that we used to inheritted this property from 'pCreateInfo->oldSwapchain'. But when it
    * was true, and when the next present was completed with copying, we would return
    * VK_SUBOPTIMAL_KHR and hint the app to reallocate again for no good reason. If all following
    * presents on the surface were completed with copying because of some surface state change, we
    * would always return VK_SUBOPTIMAL_KHR no matter how many times the app had reallocated.
    *
    * Note also that is is questionable in general if that mechanism is really useful. It ist not
    * clear why on a change from flipping to copying we can assume a reallocation has a high chance
    * of making flips work again per se. In other words it is not clear why there is need for
    * another way to inform clients about suboptimal copies besides forwarding the
    * 'PresentOptionSuboptimal' complete mode.
    */
   chain->copy_is_suboptimal = false;

   /* For our swapchain we need to listen to following Present extension events:
    * - Configure: Window dimensions changed. Images in the swapchain might need
    *              to be reallocated.
    * - Complete: An image from our swapchain was presented on the output.
    * - Idle: An image from our swapchain is not anymore accessed by the X
    *         server and can be reused.
    */
   chain->event_id = xcb_generate_id(chain->conn);
   xcb_present_select_input(chain->conn, chain->event_id, chain->window,
                            XCB_PRESENT_EVENT_MASK_CONFIGURE_NOTIFY |
                            XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY |
                            XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY);

   /* Create an XCB event queue to hold present events outside of the usual
    * application event queue
    */
   chain->special_event =
      xcb_register_for_special_xge(chain->conn, &xcb_present_id,
                                   chain->event_id, NULL);

   /* Create the graphics context. */
   chain->gc = xcb_generate_id(chain->conn);
   if (!chain->gc) {
      /* FINISHME: Choose a better error. */
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail_register;
   }

   cookie = xcb_create_gc(chain->conn,
                          chain->gc,
                          chain->window,
                          XCB_GC_GRAPHICS_EXPOSURES,
                          (uint32_t []) { 0 });
   xcb_discard_reply(chain->conn, cookie.sequence);

   uint32_t image = 0;
   for (; image < chain->base.image_count; image++) {
      result = x11_image_init(device, chain, pCreateInfo, pAllocator,
                              display_fd, &chain->images[image]);
      if (result != VK_SUCCESS)
         goto fail_init_images;
   }

   if (display_fd >= 0) {
      close(display_fd);
      display_fd = -1;
   }

   /* Initialize queues for images in our swapchain. Possible queues are:
    * - Present queue: for images sent to the X server but not yet presented.
    * - Acquire queue: for images already presented but not yet released by the
    *                  X server.
    *
    * In general queues are not used on software drivers, otherwise which queues
    * are used depends on our presentation mode:
    * - Fifo: present and acquire
    * - Mailbox: present only
    * - Immediate: present when we wait on fences before buffer submission (Xwayland)
    */
   if ((chain->base.present_mode == VK_PRESENT_MODE_FIFO_KHR ||
        chain->base.present_mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR ||
        x11_needs_wait_for_fences(wsi_device, wsi_conn,
                                  chain->base.present_mode)) &&
       !chain->base.wsi->sw) {
      chain->has_present_queue = true;

      /* The queues have a length of base.image_count + 1 because we will
       * occasionally use UINT32_MAX to signal the other thread that an error
       * has occurred and we don't want an overflow.
       */
      int ret;
      ret = wsi_queue_init(&chain->present_queue, chain->base.image_count + 1);
      if (ret) {
         goto fail_init_images;
      }

      if (chain->base.present_mode == VK_PRESENT_MODE_FIFO_KHR ||
          chain->base.present_mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR) {
         chain->has_acquire_queue = true;

         ret = wsi_queue_init(&chain->acquire_queue, chain->base.image_count + 1);
         if (ret) {
            wsi_queue_destroy(&chain->present_queue);
            goto fail_init_images;
         }

         for (unsigned i = 0; i < chain->base.image_count; i++)
            wsi_queue_push(&chain->acquire_queue, i);
      }

      ret = pthread_create(&chain->queue_manager, NULL,
                           x11_manage_fifo_queues, chain);
      if (ret) {
         wsi_queue_destroy(&chain->present_queue);
         if (chain->has_acquire_queue)
            wsi_queue_destroy(&chain->acquire_queue);

         goto fail_init_images;
      }
   }

   assert(chain->has_present_queue || !chain->has_acquire_queue);

   /* It is safe to set it here as only one swapchain can be associated with
    * the window, and swapchain creation does the association. At this point
    * we know the creation is going to succeed. */
   wsi_x11_set_adaptive_sync_property(conn, window,
                                      wsi_device->enable_adaptive_sync);

   *swapchain_out = &chain->base;

   return VK_SUCCESS;

fail_init_images:
   for (uint32_t j = 0; j < image; j++)
      x11_image_finish(chain, pAllocator, &chain->images[j]);

fail_register:
   xcb_unregister_for_special_event(chain->conn, chain->special_event);

   wsi_swapchain_finish(&chain->base);

fail_alloc:
   vk_free(pAllocator, chain);

   if (display_fd >= 0)
     close(display_fd);

   return result;
}

VkResult
wsi_x11_init_wsi(struct wsi_device *wsi_device,
                 const VkAllocationCallbacks *alloc,
                 const struct driOptionCache *dri_options)
{
   struct wsi_x11 *wsi;
   VkResult result;

   wsi = vk_alloc(alloc, sizeof(*wsi), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!wsi) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   int ret = pthread_mutex_init(&wsi->mutex, NULL);
   if (ret != 0) {
      if (ret == ENOMEM) {
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
      } else {
         /* FINISHME: Choose a better error. */
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
      }

      goto fail_alloc;
   }

   wsi->connections = _mesa_hash_table_create(NULL, _mesa_hash_pointer,
                                              _mesa_key_pointer_equal);
   if (!wsi->connections) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail_mutex;
   }

   if (dri_options) {
      if (driCheckOption(dri_options, "vk_x11_override_min_image_count", DRI_INT)) {
         wsi_device->x11.override_minImageCount =
            driQueryOptioni(dri_options, "vk_x11_override_min_image_count");
      }
      if (driCheckOption(dri_options, "vk_x11_strict_image_count", DRI_BOOL)) {
         wsi_device->x11.strict_imageCount =
            driQueryOptionb(dri_options, "vk_x11_strict_image_count");
      }
      if (driCheckOption(dri_options, "vk_x11_ensure_min_image_count", DRI_BOOL)) {
         wsi_device->x11.ensure_minImageCount =
            driQueryOptionb(dri_options, "vk_x11_ensure_min_image_count");
      }
      wsi_device->x11.xwaylandWaitReady = true;
      if (driCheckOption(dri_options, "vk_xwayland_wait_ready", DRI_BOOL)) {
         wsi_device->x11.xwaylandWaitReady =
            driQueryOptionb(dri_options, "vk_xwayland_wait_ready");
      }
   }

   wsi->base.get_support = x11_surface_get_support;
   wsi->base.get_capabilities2 = x11_surface_get_capabilities2;
   wsi->base.get_formats = x11_surface_get_formats;
   wsi->base.get_formats2 = x11_surface_get_formats2;
   wsi->base.get_present_modes = x11_surface_get_present_modes;
   wsi->base.get_present_rectangles = x11_surface_get_present_rectangles;
   wsi->base.create_swapchain = x11_surface_create_swapchain;

   wsi_device->wsi[VK_ICD_WSI_PLATFORM_XCB] = &wsi->base;
   wsi_device->wsi[VK_ICD_WSI_PLATFORM_XLIB] = &wsi->base;

   return VK_SUCCESS;

fail_mutex:
   pthread_mutex_destroy(&wsi->mutex);
fail_alloc:
   vk_free(alloc, wsi);
fail:
   wsi_device->wsi[VK_ICD_WSI_PLATFORM_XCB] = NULL;
   wsi_device->wsi[VK_ICD_WSI_PLATFORM_XLIB] = NULL;

   return result;
}

void
wsi_x11_finish_wsi(struct wsi_device *wsi_device,
                   const VkAllocationCallbacks *alloc)
{
   struct wsi_x11 *wsi =
      (struct wsi_x11 *)wsi_device->wsi[VK_ICD_WSI_PLATFORM_XCB];

   if (wsi) {
      hash_table_foreach(wsi->connections, entry)
         wsi_x11_connection_destroy(wsi_device, entry->data);

      _mesa_hash_table_destroy(wsi->connections, NULL);

      pthread_mutex_destroy(&wsi->mutex);

      vk_free(alloc, wsi);
   }
}
