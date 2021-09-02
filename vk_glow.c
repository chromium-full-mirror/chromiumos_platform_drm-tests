/*
 * Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <vulkan/vulkan.h>

#include "bs_drm.h"

// Used for double-buffering.
struct frame {
	struct gbm_bo *bo;
	uint32_t drm_fb_id;
	VkDeviceMemory vk_memory;
	VkImage vk_image;
	VkImageView vk_image_view;
	VkFramebuffer vk_framebuffer;
	VkCommandBuffer vk_cmd_buf;
};

#define check_vk_success(result, vk_func) \
	__check_vk_success(__FILE__, __LINE__, __func__, (result), (vk_func))

static void __check_vk_success(const char *file, int line, const char *func, VkResult result,
			       const char *vk_func)
{
	if (result == VK_SUCCESS)
		return;

	bs_debug_print("ERROR", func, file, line, "%s failed with VkResult(%d)", vk_func, result);
	exit(EXIT_FAILURE);
}

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec,
			      void *data)
{
	bool *waiting_for_flip = data;
	*waiting_for_flip = false;
}

// Add child to the pNext chain of parent.
static void chain_add(void *parent, void *child)
{
	VkBaseOutStructure *p = parent;
	VkBaseOutStructure *c = child;

	c->pNext = p->pNext;
	p->pNext = c;
}

// Choose a physical device that supports Vulkan 1.1 or later. Exit on failure.
VkPhysicalDevice choose_physical_device(VkInstance inst)
{
	uint32_t n_phys_devs;
	VkResult res;

	res = vkEnumeratePhysicalDevices(inst, &n_phys_devs, NULL);
	check_vk_success(res, "vkEnumeratePhysicalDevices");

	if (n_phys_devs == 0) {
		fprintf(stderr, "No available VkPhysicalDevices\n");
		exit(EXIT_FAILURE);
	}

	VkPhysicalDevice phys_devs[n_phys_devs];
	res = vkEnumeratePhysicalDevices(inst, &n_phys_devs, phys_devs);
	check_vk_success(res, "vkEnumeratePhysicalDevices");

	// Print information about all available devices. This helps debugging
	// when bringing up Vulkan on a new system.
	printf("Available VkPhysicalDevices:\n");

	uint32_t physical_device_idx = 0;
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	for (uint32_t i = 0; i < n_phys_devs; ++i) {
		VkPhysicalDeviceProperties props;

		vkGetPhysicalDeviceProperties(phys_devs[i], &props);

		printf("    VkPhysicalDevice %u:\n", i);
		printf("	apiVersion: %u.%u.%u\n", VK_VERSION_MAJOR(props.apiVersion),
		       VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
		printf("	driverVersion: %u\n", props.driverVersion);
		printf("	vendorID: 0x%x\n", props.vendorID);
		printf("	deviceID: 0x%x\n", props.deviceID);
		printf("	deviceName: %s\n", props.deviceName);
		printf("	pipelineCacheUUID: %x%x%x%x-%x%x-%x%x-%x%x-%x%x%x%x%x%x\n",
		       props.pipelineCacheUUID[0], props.pipelineCacheUUID[1],
		       props.pipelineCacheUUID[2], props.pipelineCacheUUID[3],
		       props.pipelineCacheUUID[4], props.pipelineCacheUUID[5],
		       props.pipelineCacheUUID[6], props.pipelineCacheUUID[7],
		       props.pipelineCacheUUID[8], props.pipelineCacheUUID[9],
		       props.pipelineCacheUUID[10], props.pipelineCacheUUID[11],
		       props.pipelineCacheUUID[12], props.pipelineCacheUUID[13],
		       props.pipelineCacheUUID[14], props.pipelineCacheUUID[15]);
		if (physical_device == VK_NULL_HANDLE &&
				VK_VERSION_MAJOR(props.apiVersion) >= 1 &&
				VK_VERSION_MINOR(props.apiVersion) >= 1) {
			physical_device_idx = i;
			physical_device = phys_devs[i];
		}
	}

	if (physical_device == VK_NULL_HANDLE) {
		bs_debug_error("unable to find a suitable physical device");
		exit(EXIT_FAILURE);
	}
	printf("Chose VkPhysicalDevice %d\n", physical_device_idx);
	fflush(stdout);
	return physical_device;
}

// Return the index of a graphics-enabled queue family. Return UINT32_MAX on
// failure.
uint32_t choose_gfx_queue_family(VkPhysicalDevice phys_dev)
{
	uint32_t family_idx = UINT32_MAX;
	VkQueueFamilyProperties *props = NULL;
	uint32_t n_props = 0;

	vkGetPhysicalDeviceQueueFamilyProperties(phys_dev, &n_props, NULL);

	props = calloc(sizeof(props[0]), n_props);
	if (!props) {
		bs_debug_error("out of memory");
		exit(EXIT_FAILURE);
	}

	vkGetPhysicalDeviceQueueFamilyProperties(phys_dev, &n_props, props);

	// Choose the first graphics queue.
	for (uint32_t i = 0; i < n_props; ++i) {
		if ((props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && props[i].queueCount > 0) {
			family_idx = i;
			break;
		}
	}

	free(props);
	return family_idx;
}

// Fail the test if the image is unsupported.
static void require_image_support(VkPhysicalDevice phys_dev, VkFormat format,
				  uint64_t drm_format_mod, uint32_t width, uint32_t height)
{
	VkResult res;

	VkFormatProperties2 format_props = {
		.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
	};

	VkDrmFormatModifierPropertiesListEXT mod_props_list = {
		.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
	};

	chain_add(&format_props, &mod_props_list);

	vkGetPhysicalDeviceFormatProperties2(phys_dev, format, &format_props);

	if (mod_props_list.drmFormatModifierCount == 0) {
		bs_debug_error("Vulkan does not support VkFormat(%d), "
			       "drmFormatModifier=0x%"PRIx64, format, drm_format_mod);
		exit(EXIT_FAILURE);
	}

	mod_props_list.pDrmFormatModifierProperties = alloca(mod_props_list.drmFormatModifierCount *
							     sizeof(mod_props_list.pDrmFormatModifierProperties[0]));

	vkGetPhysicalDeviceFormatProperties2(phys_dev, format, &format_props);

	const VkDrmFormatModifierPropertiesEXT *mod_props = NULL;

	for (uint32_t i = 0; i < mod_props_list.drmFormatModifierCount; ++i) {
		const VkDrmFormatModifierPropertiesEXT *tmp_props =
			&mod_props_list.pDrmFormatModifierProperties[i];

		if (tmp_props->drmFormatModifier == drm_format_mod) {
			mod_props = tmp_props;
			break;
		}

	}

	if (!mod_props) {
		bs_debug_error("Vulkan does not support VkFormat(%d), "
			       "drmFormatModifier=0x%"PRIx64, format, drm_format_mod);
		exit(EXIT_FAILURE);
	}

	if (!(mod_props->drmFormatModifierTilingFeatures &
			VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
		bs_debug_error("Vulkan supports VkFormat(%d), drmFormatModifier=0x%"PRIx64", "
			       "but lacks required features", format, drm_format_mod);
		exit(EXIT_FAILURE);
	}

	if (mod_props->drmFormatModifierPlaneCount != 1) {
		bs_debug_error("FINISHME: support drmFormatModifierPlaneCount > 1");
		exit(EXIT_FAILURE);
	}

	VkPhysicalDeviceImageFormatInfo2 image_format_info2 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
		.format = format,
		.type = VK_IMAGE_TYPE_2D,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
	};

	VkPhysicalDeviceImageDrmFormatModifierInfoEXT image_mod_info = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
		.drmFormatModifier = drm_format_mod,
	};

	chain_add(&image_format_info2, &image_mod_info);

	VkPhysicalDeviceExternalImageFormatInfo external_image_format_info = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};

	chain_add(&image_format_info2, &external_image_format_info);

	VkImageFormatProperties2 image_format_props2 = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
	};

	VkImageFormatProperties *image_format_props = &image_format_props2.imageFormatProperties;

	VkExternalImageFormatProperties external_image_format_props = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
	};

	chain_add(&image_format_props2, &external_image_format_props);

	res = vkGetPhysicalDeviceImageFormatProperties2(phys_dev, &image_format_info2,
							&image_format_props2);

	if (res == VK_ERROR_FORMAT_NOT_SUPPORTED) {
		bs_debug_error("vkGetPhysicalDeviceFormatProperties2 does not support image");
		exit(EXIT_FAILURE);
	}

	check_vk_success(res, "vkGetPhysicalDeviceFormatProperties2");

	if (image_format_props->maxExtent.width < width ||
	    image_format_props->maxExtent.height < height ||
	    !(image_format_props->sampleCounts & VK_SAMPLE_COUNT_1_BIT) ||
	    !(external_image_format_props.externalMemoryProperties.externalMemoryFeatures &
	      VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
		bs_debug_error("vkGetPhysicalDeviceFormatProperties2 does not support image");
		exit(EXIT_FAILURE);
	}

	if (external_image_format_props.externalMemoryProperties.externalMemoryFeatures &
	    VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) {
		bs_debug_error("FINISHME: support VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT");
		exit(EXIT_FAILURE);
	}
}

VkImage create_image(VkPhysicalDevice phys_dev, VkDevice dev, struct gbm_bo *bo, VkFormat format,
		     uint64_t drm_format_mod)
{
	VkImage image;
	VkResult res;

	VkImageCreateInfo base_create_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = format,
		.extent = (VkExtent3D){
			gbm_bo_get_width(bo),
			gbm_bo_get_height(bo), 1
		},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = 1,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VkImageDrmFormatModifierExplicitCreateInfoEXT mod_create_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
		.drmFormatModifier = drm_format_mod,
		.drmFormatModifierPlaneCount = 1,
		.pPlaneLayouts = (VkSubresourceLayout[]) {
			{
				.offset = gbm_bo_get_offset(bo, 0),
				.size = 0, // ignored
				.rowPitch = gbm_bo_get_stride_for_plane(bo, 0),
			},
		},
	};

	chain_add(&base_create_info, &mod_create_info);

	VkExternalMemoryImageCreateInfo external_create_info = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};

	chain_add(&base_create_info, &external_create_info);

	res = vkCreateImage(dev, &base_create_info, NULL, &image);
	check_vk_success(res, "vkCreateImage");

	return image;
}

bool bind_image_bo(VkDevice dev,
		   VkImage image,
		   struct gbm_bo *bo,
		   VkDeviceMemory *mems)
{
	PFN_vkGetMemoryFdPropertiesKHR bs_vkGetMemoryFdPropertiesKHR =
		(void *)vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");
	if (bs_vkGetMemoryFdPropertiesKHR == NULL) {
		bs_debug_error("vkGetDeviceProcAddr(\"vkGetMemoryFdPropertiesKHR\") failed");
		return false;
	}

	int prime_fd = gbm_bo_get_fd(bo);
	if (prime_fd < 0) {
		bs_debug_error("failed to get prime fd for gbm_bo");
		return false;
	}

	VkMemoryFdPropertiesKHR fd_props = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
	};
	bs_vkGetMemoryFdPropertiesKHR(dev,
			VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
			prime_fd, &fd_props);

	/* get image memory requirements */
	const VkImageMemoryRequirementsInfo2 mem_reqs_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
		.image = image,
	};
	VkMemoryRequirements2 mem_reqs = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
	};
	vkGetImageMemoryRequirements2(dev, &mem_reqs_info, &mem_reqs);

	const uint32_t memory_type_bits = fd_props.memoryTypeBits &
		mem_reqs.memoryRequirements.memoryTypeBits;
	if (!memory_type_bits) {
		bs_debug_error("no valid memory type");
		close(prime_fd);
		return false;
	}

	const VkMemoryDedicatedAllocateInfo memory_dedicated_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
		.image = image,
	};
	const VkImportMemoryFdInfoKHR memory_fd_info = {
		.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
		.pNext = &memory_dedicated_info,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		.fd = prime_fd,
	};
	VkResult res = vkAllocateMemory(dev,
		&(VkMemoryAllocateInfo){
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = &memory_fd_info,
		.allocationSize = mem_reqs.memoryRequirements.size,

		// Simply choose the first available memory type.  We
		// need neither performance nor mmap, so all memory
		// types are equally good.
		.memoryTypeIndex = ffs(memory_type_bits) - 1,
		},
		/*pAllocator*/ NULL, mems);
	check_vk_success(res, "vkAllocateMemory");

	res = vkBindImageMemory(dev, image, mems[0], 0);
	check_vk_success(res, "vkBindImageMemory");

	return true;
}

int main(int argc, char **argv)
{
	const uint32_t drm_format = DRM_FORMAT_XBGR8888;
	const VkFormat format = VK_FORMAT_A8B8G8R8_UNORM_PACK32;
	const uint64_t drm_format_mod = DRM_FORMAT_MOD_LINEAR;
	bs_debug_warning("assume display supports DRM_FORMAT_XBGR8888 without querying plane "
			 "properties");

	VkInstance inst;
	VkPhysicalDevice phys_dev;
	uint32_t gfx_queue_family_idx;
	VkDevice dev;
	VkQueue gfx_queue;
	VkRenderPass pass;
	VkCommandPool cmd_pool;
	VkResult res;
	struct frame frames[2];
	int err;

	int dev_fd = bs_drm_open_main_display();
	if (dev_fd < 0) {
		bs_debug_error("failed to open display device");
		exit(EXIT_FAILURE);
	}

	struct gbm_device *gbm = gbm_create_device(dev_fd);
	if (!gbm) {
		bs_debug_error("failed to create gbm_device");
		exit(EXIT_FAILURE);
	}

	struct bs_drm_pipe pipe = { 0 };
	if (!bs_drm_pipe_make(dev_fd, &pipe)) {
		bs_debug_error("failed to make drm pipe");
		exit(EXIT_FAILURE);
	}

	drmModeConnector *connector = drmModeGetConnector(dev_fd, pipe.connector_id);
	if (!connector) {
		bs_debug_error("drmModeGetConnector failed");
		exit(EXIT_FAILURE);
	}

	drmModeModeInfo *mode = &connector->modes[0];

	res = vkCreateInstance(
	    &(VkInstanceCreateInfo){
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo =
		    &(VkApplicationInfo){
			.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
			.apiVersion = VK_MAKE_VERSION(1, 1, 0),
		    },
	    },
	    /*pAllocator*/ NULL, &inst);
	check_vk_success(res, "vkCreateInstance");

	phys_dev = choose_physical_device(inst);

	gfx_queue_family_idx = choose_gfx_queue_family(phys_dev);
	if (gfx_queue_family_idx == UINT32_MAX) {
		bs_debug_error(
		    "VkPhysicalDevice exposes no VkQueueFamilyProperties "
		    "with graphics");
		exit(EXIT_FAILURE);
	}

	const char *required_extensions[] = {
		"VK_KHR_external_memory_fd",
		"VK_KHR_image_format_list",
		"VK_EXT_external_memory_dma_buf",
		"VK_EXT_image_drm_format_modifier",
		"VK_EXT_queue_family_foreign",
	};
	uint32_t extension_count;
	vkEnumerateDeviceExtensionProperties(phys_dev, NULL, &extension_count, NULL);
	VkExtensionProperties available_extensions[extension_count];
	vkEnumerateDeviceExtensionProperties(phys_dev, NULL, &extension_count, available_extensions);
	for (uint32_t i = 0; i < BS_ARRAY_LEN(required_extensions); i++) {
		uint32_t j;
		for (j = 0; j < extension_count; j++) {
			if (strcmp(required_extensions[i], available_extensions[j].extensionName) == 0) {
				break;
			}
		}
		if (j == extension_count) {
			bs_debug_error("unsupported device extension: %s", required_extensions[i]);
			exit(EXIT_FAILURE);
		}
	}

	require_image_support(phys_dev, format, drm_format_mod, mode->hdisplay, mode->vdisplay);

	res = vkCreateDevice(phys_dev,
			     &(VkDeviceCreateInfo){
				 .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
				 .queueCreateInfoCount = 1,
				 .pQueueCreateInfos =
				     (VkDeviceQueueCreateInfo[]){
					 {
					     .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
					     .queueFamilyIndex = gfx_queue_family_idx,
					     .queueCount = 1,
					     .pQueuePriorities = (float[]){ 1.0f },

					 },
				     },
				 .enabledExtensionCount = BS_ARRAY_LEN(required_extensions),
				 .ppEnabledExtensionNames = required_extensions,
			     },
			     /*pAllocator*/ NULL, &dev);
	check_vk_success(res, "vkCreateDevice");

	vkGetDeviceQueue(dev, gfx_queue_family_idx, /*queueIndex*/ 0, &gfx_queue);

	res = vkCreateCommandPool(dev,
				  &(VkCommandPoolCreateInfo){
				      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
				      .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
					       VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
				      .queueFamilyIndex = gfx_queue_family_idx,
				  },
				  /*pAllocator*/ NULL, &cmd_pool);
	check_vk_success(res, "vkCreateCommandPool");

	res = vkCreateRenderPass(
	    dev,
	    &(VkRenderPassCreateInfo){
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments =
		    (VkAttachmentDescription[]){
			{
			    .format = format,
			    .samples = 1,
			    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			    .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
			},
		    },
		.subpassCount = 1,
		.pSubpasses =
		    (VkSubpassDescription[]){
			{
			    .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
			    .colorAttachmentCount = 1,
			    .pColorAttachments =
				(VkAttachmentReference[]){
				    {
					.attachment = 0,
					.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				    },
				},
			},
		    },
	    },
	    /*pAllocator*/ NULL, &pass);
	check_vk_success(res, "vkCreateRenderPass");

	for (int i = 0; i < BS_ARRAY_LEN(frames); ++i) {
		struct frame *fr = &frames[i];

		fr->bo = gbm_bo_create_with_modifiers(gbm, mode->hdisplay, mode->vdisplay,
						      drm_format, &drm_format_mod, 1);
		if (fr->bo == NULL) {
			bs_debug_error("failed to create framebuffer's gbm_bo");
			return 1;
		}

		fr->drm_fb_id = bs_drm_fb_create_gbm(fr->bo);
		if (fr->drm_fb_id == 0) {
			bs_debug_error("failed to create drm framebuffer id");
			return 1;
		}

		fr->vk_image = create_image(phys_dev, dev, fr->bo, format, drm_format_mod);
		if (fr->vk_image == VK_NULL_HANDLE) {
			bs_debug_error("failed to create VkImage");
			exit(EXIT_FAILURE);
		}

		if (!bind_image_bo(dev, fr->vk_image, fr->bo, &fr->vk_memory)) {
			bs_debug_error("failed to bind bo to image");
			exit(EXIT_FAILURE);
		}

		res = vkCreateImageView(dev,
					&(VkImageViewCreateInfo){
					    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
					    .image = fr->vk_image,
					    .viewType = VK_IMAGE_VIEW_TYPE_2D,
					    .format = format,
					    .components =
						(VkComponentMapping){
						    .r = VK_COMPONENT_SWIZZLE_IDENTITY,
						    .b = VK_COMPONENT_SWIZZLE_IDENTITY,
						    .g = VK_COMPONENT_SWIZZLE_IDENTITY,
						    .a = VK_COMPONENT_SWIZZLE_IDENTITY,
						},
					    .subresourceRange =
						(VkImageSubresourceRange){
						    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
						    .baseMipLevel = 0,
						    .levelCount = 1,
						    .baseArrayLayer = 0,
						    .layerCount = 1,
						},
					},
					/*pAllocator*/ NULL, &fr->vk_image_view);
		check_vk_success(res, "vkCreateImageView");

		res = vkCreateFramebuffer(dev,
					  &(VkFramebufferCreateInfo){
					      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
					      .renderPass = pass,
					      .attachmentCount = 1,
					      .pAttachments = (VkImageView[]){ fr->vk_image_view },
					      .width = mode->hdisplay,
					      .height = mode->vdisplay,
					      .layers = 1,
					  },
					  /*pAllocator*/ NULL, &fr->vk_framebuffer);
		check_vk_success(res, "vkCreateFramebuffer");

		res = vkAllocateCommandBuffers(
		    dev,
		    &(VkCommandBufferAllocateInfo){
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = cmd_pool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		    },
		    &fr->vk_cmd_buf);
		check_vk_success(res, "vkAllocateCommandBuffers");
	}

	// We set the screen mode using framebuffer 0. Then the first page flip
	// waits on framebuffer 1.
	err = drmModeSetCrtc(dev_fd, pipe.crtc_id, frames[0].drm_fb_id,
			     /*x*/ 0, /*y*/ 0, &pipe.connector_id, /*connector_count*/ 1, mode);
	if (err) {
		bs_debug_error("drmModeSetCrtc failed: %d", err);
		exit(EXIT_FAILURE);
	}

	// We set an upper bound on the render loop so we can run this in
	// from a testsuite.
	for (int i = 1; i < 500; ++i) {
		struct frame *fr = &frames[i % BS_ARRAY_LEN(frames)];

		// vkBeginCommandBuffer implicity resets the command buffer due
		// to VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT.
		res = vkBeginCommandBuffer(fr->vk_cmd_buf,
					   &(VkCommandBufferBeginInfo){
					       .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
					       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
					   });
		check_vk_success(res, "vkBeginCommandBuffer");

		// Transfer ownership of the dma-buf from DRM to Vulkan.
		vkCmdPipelineBarrier(
				fr->vk_cmd_buf,
				// srcStageMask is ignored when acquiring ownership, but various
				// validation layers complain when you pass 0 here, so we just set
				// it to the same as dstStageMask.
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				0, 0, NULL, 0, NULL,
				1,
				&(VkImageMemoryBarrier){
					.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					.pNext = NULL,
					.srcAccessMask = 0, /** ignored for transfers */
					.dstAccessMask = 0, /** ignored for transfers */
					.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
					.newLayout = VK_IMAGE_LAYOUT_UNDEFINED,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
					.dstQueueFamilyIndex = gfx_queue_family_idx,
					.image = fr->vk_image,
					.subresourceRange = (VkImageSubresourceRange){
				    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				    .baseMipLevel = 0,
				    .levelCount = 1,
				    .baseArrayLayer = 0,
				    .layerCount = 1,
					},
				});

		// Cycle along the circumference of the RGB color wheel.
		VkClearValue clear_color = {
			.color =
			    {
				.float32 =
				    {
					0.5f + 0.5f * sinf(2 * M_PI * i / 240.0f),
					0.5f + 0.5f * sinf(2 * M_PI * i / 240.0f +
							   (2.0f / 3.0f * M_PI)),
					0.5f + 0.5f * sinf(2 * M_PI * i / 240.0f +
							   (4.0f / 3.0f * M_PI)),
					1.0f,
				    },
			    },
		};

		vkCmdBeginRenderPass(fr->vk_cmd_buf,
				     &(VkRenderPassBeginInfo){
					 .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
					 .renderPass = pass,
					 .framebuffer = fr->vk_framebuffer,
					 .renderArea =
					     (VkRect2D){
						 .offset = { 0, 0 },
						 .extent = { mode->hdisplay, mode->vdisplay },
					     },
					 .clearValueCount = 1,
					 .pClearValues = (VkClearValue[]){ clear_color },
				     },
				     VK_SUBPASS_CONTENTS_INLINE);
		vkCmdEndRenderPass(fr->vk_cmd_buf);

		// Transfer ownership of the dma-buf from Vulkan to DRM.
		vkCmdPipelineBarrier(
				fr->vk_cmd_buf,
				VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
				// dstStageMask is ignored when releasing ownership, but various
				// validation layers complain when you pass 0 here, so we just set
				// it to the same as srcStageMask.
				VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
				0, 0, NULL, 0, NULL,
				1,
				&(VkImageMemoryBarrier){
					.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					.pNext = NULL,
					.srcAccessMask = 0, /** ignored for transfers */
					.dstAccessMask = 0, /** ignored for transfers */
					.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
					.newLayout = VK_IMAGE_LAYOUT_GENERAL,
					.srcQueueFamilyIndex = gfx_queue_family_idx,
					.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
					.image = fr->vk_image,
					.subresourceRange = (VkImageSubresourceRange){
				    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				    .baseMipLevel = 0,
				    .levelCount = 1,
				    .baseArrayLayer = 0,
				    .layerCount = 1,
					},
				});
		res = vkEndCommandBuffer(fr->vk_cmd_buf);
		check_vk_success(res, "vkEndCommandBuffer");

		res =
		    vkQueueSubmit(gfx_queue,
				  /*submitCount*/ 1,
				  (VkSubmitInfo[]){
				      {
					  .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
					  .commandBufferCount = 1,
					  .pCommandBuffers = (VkCommandBuffer[]){ fr->vk_cmd_buf },
				      },
				  },
				  VK_NULL_HANDLE);
		check_vk_success(res, "vkQueueSubmit");

		res = vkQueueWaitIdle(gfx_queue);
		check_vk_success(res, "vkQueueWaitIdle");

		bool waiting_for_flip = true;
		err = drmModePageFlip(dev_fd, pipe.crtc_id, fr->drm_fb_id, DRM_MODE_PAGE_FLIP_EVENT,
				      &waiting_for_flip);
		if (err) {
			bs_debug_error("failed page flip: error=%d", err);
			exit(EXIT_FAILURE);
		}

		while (waiting_for_flip) {
			drmEventContext ev_ctx = {
				.version = DRM_EVENT_CONTEXT_VERSION,
				.page_flip_handler = page_flip_handler,
			};

			fd_set fds;
			FD_ZERO(&fds);
			FD_SET(dev_fd, &fds);

			int n_fds = select(dev_fd + 1, &fds, NULL, NULL, NULL);
			if (n_fds < 0) {
				bs_debug_error("select() failed on page flip: %s", strerror(errno));
				exit(EXIT_FAILURE);
			} else if (n_fds == 0) {
				bs_debug_error("select() timeout on page flip");
				exit(EXIT_FAILURE);
			}

			err = drmHandleEvent(dev_fd, &ev_ctx);
			if (err) {
				bs_debug_error(
				    "drmHandleEvent failed while "
				    "waiting for page flip: error=%d",
				    err);
				exit(EXIT_FAILURE);
			}
		}
	}

	return 0;
}
