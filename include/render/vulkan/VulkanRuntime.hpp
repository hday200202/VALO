#pragma once

#if defined(__APPLE__) && defined(VALO_HAS_VULKAN)
#include <SDL3/SDL.h>
#include <cstdlib>
#include <filesystem>

namespace valo {

// Run before SDL creates a Vulkan window, and before the loader enumerates
// drivers. These are development SDK defaults; explicit caller settings win.
inline void configureVulkanRuntime() {
    static const bool configured = [] {
        if (std::filesystem::exists(VALO_VULKAN_LOADER_PATH))
            SDL_SetHintWithPriority(SDL_HINT_VULKAN_LIBRARY,
                VALO_VULKAN_LOADER_PATH, SDL_HINT_DEFAULT);

        if (!std::getenv("VK_DRIVER_FILES") && !std::getenv("VK_ICD_FILENAMES")
            && !std::getenv("VK_ADD_DRIVER_FILES")
            && std::filesystem::exists(VALO_VULKAN_ICD_PATH))
            setenv("VK_ADD_DRIVER_FILES", VALO_VULKAN_ICD_PATH, 0);

        if (!std::getenv("VK_LAYER_PATH") && !std::getenv("VK_ADD_LAYER_PATH")
            && std::filesystem::exists(VALO_VULKAN_LAYER_PATH))
            setenv("VK_ADD_LAYER_PATH", VALO_VULKAN_LAYER_PATH, 0);
        return true;
    }();
    (void)configured;
}

} // namespace valo
#endif
