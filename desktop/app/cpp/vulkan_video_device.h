// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Included after Api, inside the codec backend's private namespace.

template<class T> T vkInfo(VkStructureType type) { T value{}; value.sType = type; return value; }

struct VulkanSurface {
    int width = 0, height = 0;
    VkImage images[2]{};
    VkDeviceMemory memory[2]{};
    VkSemaphore ready{};
    VkFence copied{}, retired{};
    VkCommandBuffer command{};
    AVFrame* source = nullptr;
    bool leased = false;
    bool released = false;
};

struct VulkanVideoDevice {
    const Api& functions = api();
    Library loader{dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL)};
    PFN_vkGetInstanceProcAddr getInstance = nullptr;
    PFN_vkGetDeviceProcAddr getDevice = nullptr;
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue copyQueue{}, renderQueue{};
    uint32_t graphics = 0;
    AVBufferRef* avdevice = nullptr;
    VkCommandPool commands{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkPhysicalDeviceFeatures2 features = vkInfo<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    VkPhysicalDeviceVulkan11Features f11 = vkInfo<VkPhysicalDeviceVulkan11Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    VkPhysicalDeviceVulkan12Features f12 = vkInfo<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    VkPhysicalDeviceVulkan13Features f13 = vkInfo<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    std::mutex mutex;
    std::vector<std::unique_ptr<VulkanSurface>> surfaces;
    const char* extensions[4] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME};

    template<typename T> T instanceFunction(const char* name) {
        auto fn = reinterpret_cast<T>(getInstance(instance, name));
        if (!fn) throw std::runtime_error(std::string("Missing Vulkan instance function: ") + name);
        return fn;
    }
    template<typename T> T deviceFunction(const char* name) {
        auto fn = reinterpret_cast<T>(getDevice(device, name));
        if (!fn) throw std::runtime_error(std::string("Missing Vulkan device function: ") + name);
        return fn;
    }
    static void check(VkResult result, const char* operation) {
        if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": VkResult=" + std::to_string(result));
    }

    void initialize(VkInstance supplied) {
        if (device) {
            if (instance != supplied) throw std::runtime_error("Vulkan instance changed after video initialization");
            return;
        }
        if (!loader) throw std::runtime_error("libvulkan.so.1 is required");
        getInstance = symbol<PFN_vkGetInstanceProcAddr>(loader.get(), "vkGetInstanceProcAddr");
        instance = supplied;
        auto enumerate = instanceFunction<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
        uint32_t count = 0;
        check(enumerate(instance, &count, nullptr), "Enumerate Vulkan GPUs");
        if (!count) throw std::runtime_error("No Vulkan GPU is available");
        std::vector<VkPhysicalDevice> devices(count);
        check(enumerate(instance, &count, devices.data()), "Enumerate Vulkan GPUs");
        auto properties = instanceFunction<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties");
        physical = devices[0];
        for (auto gpu : devices) {
            VkPhysicalDeviceProperties p{}; properties(gpu, &p);
            if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { physical = gpu; break; }
        }
        VkPhysicalDeviceProperties p{}; properties(physical, &p);
        if (p.apiVersion < VK_API_VERSION_1_3) throw std::runtime_error("Vulkan 1.3 is required on the selected GPU");
        auto enumerateExt = instanceFunction<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
        count = 0; check(enumerateExt(physical, nullptr, &count, nullptr), "Enumerate Vulkan extensions");
        std::vector<VkExtensionProperties> available(count);
        check(enumerateExt(physical, nullptr, &count, available.data()), "Enumerate Vulkan extensions");
        for (const auto* required : extensions) {
            if (std::none_of(available.begin(), available.end(), [&](const auto& e) { return std::strcmp(e.extensionName, required) == 0; }))
                throw std::runtime_error(std::string(p.deviceName) + " lacks required " + required);
        }
        auto queueProperties = instanceFunction<PFN_vkGetPhysicalDeviceQueueFamilyProperties2>("vkGetPhysicalDeviceQueueFamilyProperties2");
        count = 0; queueProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties2> queues(count);
        std::vector<VkQueueFamilyVideoPropertiesKHR> video(count);
        for (uint32_t i = 0; i < count; ++i) {
            video[i] = vkInfo<VkQueueFamilyVideoPropertiesKHR>(VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR);
            queues[i] = vkInfo<VkQueueFamilyProperties2>(VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2);
            queues[i].pNext = &video[i];
        }
        queueProperties(physical, &count, queues.data());
        int graphicsFamily = -1, decodeFamily = -1;
        for (uint32_t i = 0; i < count; ++i) {
            const auto& q = queues[i].queueFamilyProperties;
            if (graphicsFamily < 0 && q.queueCount >= 2 && (q.queueFlags & VK_QUEUE_GRAPHICS_BIT)
                && (q.queueFlags & VK_QUEUE_COMPUTE_BIT)) graphicsFamily = int(i);
            if (decodeFamily < 0 && (q.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR)
                && (video[i].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR)) decodeFamily = int(i);
        }
        if (graphicsFamily < 0 || decodeFamily < 0)
            throw std::runtime_error("Vulkan video requires H.264 decode and two graphics/compute queues");
        graphics = uint32_t(graphicsFamily);
        features.pNext = &f11; f11.pNext = &f12; f12.pNext = &f13;
        instanceFunction<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(physical, &features);
        if (!f12.timelineSemaphore || !f13.synchronization2)
            throw std::runtime_error("Vulkan timeline semaphores and synchronization2 are required");
        float priorities[2] = {1.f, 1.f};
        VkDeviceQueueCreateInfo qc[2]{};
        qc[0] = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, graphics, 2, priorities};
        uint32_t queueCount = 1;
        if (uint32_t(decodeFamily) != graphics)
            qc[queueCount++] = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, uint32_t(decodeFamily), 1, priorities};
        VkDeviceCreateInfo create = vkInfo<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
        create.pNext = &features;
        create.queueCreateInfoCount = queueCount; create.pQueueCreateInfos = qc;
        create.enabledExtensionCount = 4; create.ppEnabledExtensionNames = extensions;
        check(instanceFunction<PFN_vkCreateDevice>("vkCreateDevice")(physical, &create, nullptr, &device), "Create shared Vulkan video device");
        getDevice = instanceFunction<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        auto getQueue = deviceFunction<PFN_vkGetDeviceQueue>("vkGetDeviceQueue");
        getQueue(device, graphics, 0, &copyQueue);
        getQueue(device, graphics, 1, &renderQueue);
        instanceFunction<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical, &memory);
        avdevice = functions.device_alloc(AV_HWDEVICE_TYPE_VULKAN);
        if (!avdevice) throw std::bad_alloc();
        auto* ctx = reinterpret_cast<AVHWDeviceContext*>(avdevice->data);
        auto* vk = static_cast<AVVulkanDeviceContext*>(ctx->hwctx);
        vk->get_proc_addr = getInstance; vk->inst = instance; vk->phys_dev = physical; vk->act_dev = device;
        vk->device_features = features;
        vk->enabled_dev_extensions = extensions; vk->nb_enabled_dev_extensions = 4;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 39, 100)
        vk->qf[0] = {graphicsFamily, 1, VkQueueFlagBits(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT), VkVideoCodecOperationFlagBitsKHR(0)};
        vk->nb_qf = 1;
        if (decodeFamily == graphicsFamily) {
            vk->qf[0].flags = VkQueueFlagBits(vk->qf[0].flags | VK_QUEUE_VIDEO_DECODE_BIT_KHR);
            vk->qf[0].video_caps = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
        } else {
            vk->qf[vk->nb_qf++] = {decodeFamily, 1, VK_QUEUE_VIDEO_DECODE_BIT_KHR, VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR};
        }
#else
        vk->queue_family_index = graphicsFamily; vk->nb_graphics_queues = 1;
        vk->queue_family_comp_index = graphicsFamily; vk->nb_comp_queues = 1;
        vk->queue_family_tx_index = graphicsFamily; vk->nb_tx_queues = 1;
        vk->queue_family_decode_index = decodeFamily; vk->nb_decode_queues = 1;
        vk->queue_family_encode_index = -1; vk->nb_encode_queues = 0;
#endif
        functions.check(functions.device_init(avdevice), "Initialize shared FFmpeg Vulkan device");
        VkCommandPoolCreateInfo pool = vkInfo<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        pool.queueFamilyIndex = graphics; pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(deviceFunction<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device, &pool, nullptr, &commands), "Create Vulkan video command pool");
    }

    uint32_t memoryType(uint32_t bits) {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return i;
        throw std::runtime_error("No Vulkan device-local image memory type");
    }

    VulkanSurface* acquire(int width, int height) {
        auto status = deviceFunction<PFN_vkGetFenceStatus>("vkGetFenceStatus");
        for (auto& item : surfaces) {
            auto& s = *item;
            if (s.released) {
                const auto copy = status(device, s.copied), retire = status(device, s.retired);
                if (copy != VK_NOT_READY) check(copy, "Poll Vulkan copy fence");
                if (retire != VK_NOT_READY) check(retire, "Poll Vulkan retirement fence");
                if (copy == VK_SUCCESS && retire == VK_SUCCESS) {
                    functions.frame_free(&s.source);
                    s.leased = s.released = false;
                }
            }
            if (!s.leased && s.width == width && s.height == height) {
                VkFence fences[] = {s.copied, s.retired};
                check(deviceFunction<PFN_vkResetFences>("vkResetFences")(device, 2, fences), "Reset Vulkan video fences");
                if (s.ready) deviceFunction<PFN_vkDestroySemaphore>("vkDestroySemaphore")(device, s.ready, nullptr);
                VkSemaphoreCreateInfo sem = vkInfo<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
                check(deviceFunction<PFN_vkCreateSemaphore>("vkCreateSemaphore")(device, &sem, nullptr, &s.ready), "Create video-ready semaphore");
                s.leased = true;
                return &s;
            }
        }
        // Repeated window resizing must not retain every historical resolution.
        // Both queues completed, and this lease is no longer referenced by a
        // material. Old Qt image views may await deferred destruction, but no
        // submitted command can still use their image (vkDestroyImage-01000).
        surfaces.erase(std::remove_if(surfaces.begin(), surfaces.end(), [&](auto& item) {
            auto& s = *item;
            if (s.leased || (s.width == width && s.height == height)) return false;
            deviceFunction<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(device, commands, 1, &s.command);
            deviceFunction<PFN_vkDestroyFence>("vkDestroyFence")(device, s.copied, nullptr);
            deviceFunction<PFN_vkDestroyFence>("vkDestroyFence")(device, s.retired, nullptr);
            if (s.ready) deviceFunction<PFN_vkDestroySemaphore>("vkDestroySemaphore")(device, s.ready, nullptr);
            for (int i = 0; i < 2; ++i) {
                deviceFunction<PFN_vkDestroyImage>("vkDestroyImage")(device, s.images[i], nullptr);
                deviceFunction<PFN_vkFreeMemory>("vkFreeMemory")(device, s.memory[i], nullptr);
            }
            return true;
        }), surfaces.end());
        if (surfaces.size() >= 64) throw std::runtime_error("Vulkan video surface pool exhausted (64 retained surfaces)");
        auto s = std::make_unique<VulkanSurface>(); s->width = width; s->height = height;
        for (int plane = 0; plane < 2; ++plane) {
            VkImageCreateInfo image = vkInfo<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
            image.imageType = VK_IMAGE_TYPE_2D; image.format = plane ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_R8_UNORM;
            image.extent = {uint32_t(plane ? width / 2 : width), uint32_t(plane ? height / 2 : height), 1};
            image.mipLevels = image.arrayLayers = 1; image.samples = VK_SAMPLE_COUNT_1_BIT;
            image.tiling = VK_IMAGE_TILING_OPTIMAL; image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            check(deviceFunction<PFN_vkCreateImage>("vkCreateImage")(device, &image, nullptr, &s->images[plane]), "Create shared video image");
            VkMemoryRequirements requirements{};
            deviceFunction<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(device, s->images[plane], &requirements);
            VkMemoryAllocateInfo allocate = vkInfo<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
            allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = memoryType(requirements.memoryTypeBits);
            check(deviceFunction<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &allocate, nullptr, &s->memory[plane]), "Allocate shared video memory");
            check(deviceFunction<PFN_vkBindImageMemory>("vkBindImageMemory")(device, s->images[plane], s->memory[plane], 0), "Bind shared video image");
        }
        VkFenceCreateInfo fence = vkInfo<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO); fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(deviceFunction<PFN_vkCreateFence>("vkCreateFence")(device, &fence, nullptr, &s->copied), "Create copy fence");
        check(deviceFunction<PFN_vkCreateFence>("vkCreateFence")(device, &fence, nullptr, &s->retired), "Create retirement fence");
        VkCommandBufferAllocateInfo allocate = vkInfo<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        allocate.commandPool = commands; allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount = 1;
        check(deviceFunction<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device, &allocate, &s->command), "Allocate video command buffer");
        surfaces.push_back(std::move(s));
        return acquire(width, height);
    }

    VulkanSurface* present(AVFrame* picture) {
        std::lock_guard guard(mutex);
        if (!device || picture->format != AV_PIX_FMT_VULKAN || !picture->hw_frames_ctx)
            throw std::runtime_error("Mirroring requires an owned Vulkan decoder frame");
        auto* frames = reinterpret_cast<AVHWFramesContext*>(picture->hw_frames_ctx->data);
        auto* owner = static_cast<AVVulkanDeviceContext*>(frames->device_ctx->hwctx);
        if (owner->act_dev != device || frames->sw_format != AV_PIX_FMT_NV12)
            throw std::runtime_error("Vulkan frame device/format does not match the Qt video device");
        auto* vk = reinterpret_cast<AVVkFrame*>(picture->data[0]);
        auto* fctx = static_cast<AVVulkanFramesContext*>(frames->hwctx);
        const int width = picture->width - int(picture->crop_left + picture->crop_right);
        const int height = picture->height - int(picture->crop_top + picture->crop_bottom);
        if (width <= 0 || height <= 0 || width % 2 || height % 2 || picture->crop_left % 2 || picture->crop_top % 2)
            throw std::runtime_error("Vulkan NV12 crop must have even dimensions");
        auto* surface = acquire(width, height);
        surface->source = functions.frame_clone(picture);
        if (!surface->source) throw std::bad_alloc();
        check(deviceFunction<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(surface->command, 0), "Reset video commands");
        VkCommandBufferBeginInfo begin = vkInfo<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO); begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(deviceFunction<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(surface->command, &begin), "Begin video copy");
        if (!fctx->lock_frame || !fctx->unlock_frame || !owner->lock_queue || !owner->unlock_queue)
            throw std::runtime_error("FFmpeg Vulkan synchronization callbacks are missing");
        fctx->lock_frame(frames, vk);
        struct Unlock { AVVulkanFramesContext* f; AVHWFramesContext* c; AVVkFrame* v; ~Unlock(){ f->unlock_frame(c,v); } } unlock{fctx,frames,vk};
        const bool multiplane = !vk->img[1];
        const int sources = multiplane ? 1 : 2;
        std::vector<VkImageMemoryBarrier2> barriers;
        for (int i = 0; i < sources; ++i) {
            if (vk->queue_family[i] != VK_QUEUE_FAMILY_IGNORED && vk->queue_family[i] != graphics)
                throw std::runtime_error("Vulkan decoder images must permit the shared graphics queue");
            VkImageMemoryBarrier2 b = vkInfo<VkImageMemoryBarrier2>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
            b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            b.oldLayout = vk->layout[i]; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = vk->img[i]; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            barriers.push_back(b);
        }
        for (int i = 0; i < 2; ++i) {
            VkImageMemoryBarrier2 b = vkInfo<VkImageMemoryBarrier2>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
            b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = surface->images[i]; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            barriers.push_back(b);
        }
        VkDependencyInfo dependency = vkInfo<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
        dependency.imageMemoryBarrierCount = uint32_t(barriers.size()); dependency.pImageMemoryBarriers = barriers.data();
        auto barrier = deviceFunction<PFN_vkCmdPipelineBarrier2>("vkCmdPipelineBarrier2");
        barrier(surface->command, &dependency);
        for (int i = 0; i < 2; ++i) {
            VkImageCopy copy{};
            copy.srcSubresource = {multiplane ? (i ? VK_IMAGE_ASPECT_PLANE_1_BIT : VK_IMAGE_ASPECT_PLANE_0_BIT) : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.srcOffset = {int32_t(picture->crop_left / (i ? 2 : 1)), int32_t(picture->crop_top / (i ? 2 : 1)), 0};
            copy.extent = {uint32_t(i ? width/2 : width), uint32_t(i ? height/2 : height), 1};
            deviceFunction<PFN_vkCmdCopyImage>("vkCmdCopyImage")(surface->command, vk->img[multiplane ? 0 : i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                surface->images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        }
        barriers.clear();
        for (int i = 0; i < 2; ++i) {
            VkImageMemoryBarrier2 b = vkInfo<VkImageMemoryBarrier2>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
            b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = surface->images[i]; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; barriers.push_back(b);
        }
        dependency.imageMemoryBarrierCount = 2; dependency.pImageMemoryBarriers = barriers.data();
        barrier(surface->command, &dependency);
        check(deviceFunction<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(surface->command), "End video copy");
        VkSemaphoreSubmitInfo waits[2]{}, signals[3]{};
        for (int i = 0; i < sources; ++i) {
            waits[i] = vkInfo<VkSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO); waits[i].semaphore = vk->sem[i];
            waits[i].value = vk->sem_value[i]; waits[i].stageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            signals[i] = waits[i]; signals[i].value++; signals[i].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }
        signals[sources] = vkInfo<VkSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO); signals[sources].semaphore = surface->ready;
        signals[sources].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo command = vkInfo<VkCommandBufferSubmitInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO); command.commandBuffer = surface->command;
        VkSubmitInfo2 submit = vkInfo<VkSubmitInfo2>(VK_STRUCTURE_TYPE_SUBMIT_INFO_2); submit.waitSemaphoreInfoCount = uint32_t(sources); submit.pWaitSemaphoreInfos = waits;
        submit.commandBufferInfoCount = 1; submit.pCommandBufferInfos = &command;
        submit.signalSemaphoreInfoCount = uint32_t(sources + 1); submit.pSignalSemaphoreInfos = signals;
        owner->lock_queue(frames->device_ctx, graphics, 0);
        VkResult result = deviceFunction<PFN_vkQueueSubmit2>("vkQueueSubmit2")(copyQueue, 1, &submit, surface->copied);
        owner->unlock_queue(frames->device_ctx, graphics, 0);
        check(result, "Submit Vulkan video copy");
        for (int i = 0; i < sources; ++i) {
            vk->layout[i] = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; vk->access[i] = VK_ACCESS_TRANSFER_READ_BIT; vk->sem_value[i]++;
        }
        return surface;
    }

    void release(VulkanSurface* surface) {
        std::lock_guard guard(mutex);
        if (!surface || surface->released) return;
        VkSubmitInfo2 retire = vkInfo<VkSubmitInfo2>(VK_STRUCTURE_TYPE_SUBMIT_INFO_2);
        check(deviceFunction<PFN_vkQueueSubmit2>("vkQueueSubmit2")(renderQueue, 1, &retire, surface->retired), "Retire Vulkan video surface");
        surface->released = true;
    }
};

inline VulkanVideoDevice& sharedVulkanVideoDevice() { static auto* device = new VulkanVideoDevice; return *device; }
