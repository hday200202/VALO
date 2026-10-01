#include "VulkanContext.hpp"

#ifdef VALO_HAS_VULKAN

#include <cstdio>
#include <cstring>
#include <vector>

#include <SDL3/SDL_vulkan.h>

namespace valo {

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*
) {
    const char* level = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "ERROR" : "WARN";
    std::fprintf(stderr, "[vulkan %s] %s\n", level, data->pMessage);
    return VK_FALSE;
}

bool hasLayer(const char* name) {
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> props(n);
    vkEnumerateInstanceLayerProperties(&n, props.data());
    for (const auto& p : props) if (!std::strcmp(p.layerName, name)) return true;
    return false;
}

} // namespace

bool VulkanContext::init(Window& window, const bool validation) {
    const char* validationLayer = "VK_LAYER_KHRONOS_validation";

    std::vector<const char*> layers;

    // Needed for MoltenVK
    std::vector<const char*> extensions = {
        VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
    };

    const bool useValidation = validation && hasLayer(validationLayer);
    if (validation && !useValidation)
        std::fprintf(stderr, "[vulkan] validation requested but not found - is ext/vulkan/env.sh sourced?\n");

    if (useValidation) {
        layers.push_back(validationLayer);
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkDebugUtilsMessengerCreateInfoEXT dbg{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,

        .messageSeverity = 
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,

        .messageType = 
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT     |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT  |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,

        .pfnUserCallback = debugCallback
    };

    VkApplicationInfo app{
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName   = "VALO",
        .apiVersion         = VK_API_VERSION_1_3
    };

    uint32_t sdlCount = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&sdlCount);

    for (uint32_t i = 0; sdlExts && i < sdlCount; ++i)
        extensions.push_back(sdlExts[i]);

    VkInstanceCreateInfo ci{
        .sType                      = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext                      = useValidation ? &dbg : nullptr,
        .flags                      = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
        .pApplicationInfo           = &app,
        .enabledLayerCount          = uint32_t(layers.size()),
        .ppEnabledLayerNames        = layers.data(),
        .enabledExtensionCount      = uint32_t(extensions.size()),
        .ppEnabledExtensionNames    = extensions.data()
    };

    if (vkCreateInstance(&ci, nullptr, &m_instance) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] vkCreateInstance failed\n");
        return false;
    }

    if (useValidation) {
        auto create = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT");
        if (create) create(m_instance, &dbg, nullptr, &m_debug);
    }

    if (!SDL_Vulkan_CreateSurface(window.sdlHandle(), m_instance, nullptr, &m_surface)) {
        std::fprintf(stderr, "[vulkan] SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        return false;
    }

    if (!pickPhysicalDevice()) return false;
    if (!createLogicalDevice()) return false;
    if (!createAllocator()) return false;

    return true;
}

void VulkanContext::shutdown() {
    if (m_allocator) {
        VmaTotalStatistics stats{};
        vmaCalculateStatistics(m_allocator, &stats);

        const VmaStatistics& t = stats.total.statistics;

        if (t.allocationCount > 0) {
            std::fprintf(
                stderr,
                "[vulkan] LEAK: %u allocation(s) still live at shutdown, %llu KiB "
                "across %u block(s)\n",
                t.allocationCount,
                (unsigned long long)(t.allocationBytes / 1024),
                t.blockCount
            );
        } else {
            std::fprintf(
                stderr, "[vulkan] vma: all allocations freed (%u block(s), %llu KiB reserved)\n",
                t.blockCount, (unsigned long long)(t.blockBytes / 1024)
            );
        }

        vmaDestroyAllocator(m_allocator);
        m_allocator = VK_NULL_HANDLE;
    }

    if (m_oneShotPool) {
        vkDestroyCommandPool(m_device, m_oneShotPool, nullptr);
        m_oneShotPool = VK_NULL_HANDLE;
    }

    if (m_device) {
        vkDeviceWaitIdle(m_device);
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }

    if (m_surface) { 
        SDL_Vulkan_DestroySurface(m_instance, m_surface, nullptr); 
        m_surface = VK_NULL_HANDLE;
    }

    if (m_debug) {
        auto destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy) destroy(m_instance, m_debug, nullptr);
        m_debug = VK_NULL_HANDLE;
    }

    if (m_instance) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }
}

bool VulkanContext::pickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0) {
        std::fprintf(stderr, "[vulkan] no physical devices\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    int bestScore = -1;

    for (auto dev : devices) {
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, exts.data());

        bool hasSwapchain = false, hasPortability = false;
        for (const auto& ext : exts) {
            if (!std::strcmp(ext.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME))   hasSwapchain    = true;
            if (!std::strcmp(ext.extensionName, "VK_KHR_portability_subset"))       hasPortability  = true;
        }
        if (!hasSwapchain) continue;

        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qCount, families.data());

        uint32_t graphics = UINT32_MAX, present = UINT32_MAX;
        for (uint32_t i = 0; i < qCount; ++i) {
            const bool canGraphics = families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT;

            VkBool32 canPresent = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, m_surface, &canPresent);

            if (canGraphics && canPresent) { graphics = present = i; break; }

            if (canGraphics && graphics == UINT32_MAX) graphics = i;
            if (canPresent  && present  == UINT32_MAX) present  = i;
        }

        if (graphics == UINT32_MAX || present == UINT32_MAX) continue;

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(dev, &props);

        int score = 0;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)   score += 1000;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 100;
        if (graphics == present)                                        score += 10;

        if (score > bestScore) {
            bestScore           = score;
            m_physical          = dev;
            m_graphicsFamily    = graphics;
            m_presentFamily     = present;
            m_portabilitySubset = hasPortability;
        }
    }

    if (m_physical == VK_NULL_HANDLE) {
        std::fprintf(stderr, "[vulkan] no device with graphics + present + swapchain\n");
        return false;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_physical, &props);
    std::fprintf(
        stderr, "[vulkan] using %s (graphics q=%u, present q=%u)\n", 
        props.deviceName, m_graphicsFamily, m_presentFamily
    );
    return true;
}

bool VulkanContext::createLogicalDevice() {
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    const float priority = 1.f;

    uint32_t families[2] = { m_graphicsFamily, m_presentFamily };
    const uint32_t familyCount = (m_graphicsFamily == m_presentFamily) ? 1 : 2;

    for (uint32_t i = 0; i < familyCount; ++i) {
        VkDeviceQueueCreateInfo q{
            .sType              = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex   = families[i],
            .queueCount         = 1,
            .pQueuePriorities   = &priority
        };

        queueInfos.push_back(q);
    }

    std::vector<const char*> deviceExts = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    if (m_portabilitySubset) deviceExts.push_back("VK_KHR_portability_subset");

    VkPhysicalDeviceVulkan13Features f13{
        .sType              = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2   = VK_TRUE,
        .dynamicRendering   = VK_TRUE
    };

    VkPhysicalDeviceFeatures2 features {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &f13,
        .features = { .fillModeNonSolid = VK_TRUE }
    };

    VkDeviceCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features,
        .queueCreateInfoCount = uint32_t(queueInfos.size()),
        .pQueueCreateInfos = queueInfos.data(),
        .enabledExtensionCount = uint32_t(deviceExts.size()),
        .ppEnabledExtensionNames = deviceExts.data(),
    };

    if (vkCreateDevice(m_physical, &ci, nullptr, &m_device) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] vkCreateDevice failed\n");
        return false;
    }

    vkGetDeviceQueue(m_device, m_graphicsFamily, 0, &m_graphicsQueue);
    vkGetDeviceQueue(m_device, m_presentFamily,  0, &m_presentQueue);
    return true;
}


bool VulkanContext::createAllocator() {
    const VmaAllocatorCreateInfo ai{
        .physicalDevice     = m_physical,
        .device             = m_device,
        .instance           = m_instance,
        .vulkanApiVersion   = VK_API_VERSION_1_3
    };

    if (vmaCreateAllocator(&ai, &m_allocator) != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] vmaCreateAllocator failed\n");
        return false;
    }

    return true;
}


VkCommandBuffer VulkanContext::beginOneShot() {
    if (!m_oneShotPool) {
        const VkCommandPoolCreateInfo pi{
            .sType              = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags              = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex   = m_graphicsFamily
        };

        if (vkCreateCommandPool(m_device, &pi, nullptr, &m_oneShotPool) != VK_SUCCESS) {
            std::fprintf(stderr, "[vulkan] one-shot command pool failed\n");
            return VK_NULL_HANDLE;
        }
    }

    const VkCommandBufferAllocateInfo ai{
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = m_oneShotPool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(m_device, &ai, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;

    const VkCommandBufferBeginInfo bi{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

bool VulkanContext::endOneShot(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);

    const VkCommandBufferSubmitInfo cmdInfo{
        .sType          = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer  = cmd
    };

    const VkSubmitInfo2 submit{
        .sType                  = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos    = &cmdInfo
    };

    const VkResult r = vkQueueSubmit2(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphicsQueue);
    vkFreeCommandBuffers(m_device, m_oneShotPool, 1, &cmd);

    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[vulkan] one-shot submit failed (%d)\n", r);
        return false;
    }
    return true;
}

} // namespace valo

#endif // VALO_HAS_VULKAN