#include "VulkanContext.hpp"

#ifdef VALO_HAS_VULKAN

#include <cstdio>
#include <cstring>
#include <vector>

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

bool VulkanContext::init(const bool validation) {
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

    return true;
}

void VulkanContext::shutdown() {
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

} // namespace valo

#endif // VALO_HAS_VULKAN