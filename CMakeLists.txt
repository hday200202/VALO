cmake_minimum_required(VERSION 3.24)

project(VALO
    VERSION 0.1.0
    DESCRIPTION "VALO software renderer library"
    LANGUAGES C CXX
)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
# VALO_SHARED=ON  -> lib/libVALO.dylib (dynamic)
# VALO_SHARED=OFF -> lib/libVALO.a     (static, default)
option(VALO_SHARED          "Build VALO as a shared library" OFF)
option(VALO_BUILD_EXAMPLES  "Build the example programs in examples/" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)   # keep clang/gcc colors when output is piped through ninja
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# All library artifacts land in lib/ (source-tree relative, not build-dir
# relative), so the same folder always holds the latest build.
set(VALO_LIB_DIR "${CMAKE_SOURCE_DIR}/lib")

# ---------------------------------------------------------------------------
# SDL3 (vendored + prebuilt by build.sh -> ext/SDL3-install, lib/libSDL3.a)
# ---------------------------------------------------------------------------
# build.sh clones ext/SDL3 if missing and builds/installs it once into
# ext/SDL3-install. We only consume the exported static target here, so
# SDL is never recompiled per VALO configuration.
set(VALO_SDL3_PREFIX "${CMAKE_SOURCE_DIR}/ext/SDL3-install" CACHE PATH
    "Install prefix of the prebuilt SDL3")
list(APPEND CMAKE_PREFIX_PATH "${VALO_SDL3_PREFIX}")
find_package(SDL3 REQUIRED CONFIG COMPONENTS SDL3-static)

# ---------------------------------------------------------------------------
# stb (vendored header-only, fetched by build.sh into ext/stb)
# ---------------------------------------------------------------------------
# SYSTEM so stb's own warnings don't show up under -Wall -Wextra -Wpedantic.
# Exactly one .cpp must #define STB_IMAGE_IMPLEMENTATION before including it.
set(VALO_STB_DIR "${CMAKE_SOURCE_DIR}/ext/stb")
if(NOT EXISTS "${VALO_STB_DIR}/stb_image.h")
    message(FATAL_ERROR "ext/stb/stb_image.h not found - run ./build.sh (it fetches vendored headers)")
endif()

# tinyobjloader: same treatment. One .cpp must #define TINYOBJLOADER_IMPLEMENTATION.
set(VALO_TINYOBJ_DIR "${CMAKE_SOURCE_DIR}/ext/tinyobjloader")
if(NOT EXISTS "${VALO_TINYOBJ_DIR}/tiny_obj_loader.h")
    message(FATAL_ERROR "ext/tinyobjloader/tiny_obj_loader.h not found - run ./build.sh")
endif()

# cgltf: same treatment. One .cpp must #define CGLTF_IMPLEMENTATION.
set(VALO_CGLTF_DIR "${CMAKE_SOURCE_DIR}/ext/cgltf")
if(NOT EXISTS "${VALO_CGLTF_DIR}/cgltf.h")
    message(FATAL_ERROR "ext/cgltf/cgltf.h not found - run ./build.sh")
endif()

# ---------------------------------------------------------------------------
# Vulkan (vendored prebuilt SDK in ext/vulkan, fetched by build.sh)
# ---------------------------------------------------------------------------
# Detected, never required: with no ext/vulkan the software renderer builds
# exactly as before. The include dir goes on the VALO target so it lands in
# compile_commands.json, which is what clangd and the VS Code C/C++ extension
# read - that is the whole of the intellisense setup.
if(APPLE)
    set(VALO_VULKAN_ROOT "${CMAKE_SOURCE_DIR}/ext/vulkan/macOS")
elseif(WIN32)
    set(VALO_VULKAN_ROOT "${CMAKE_SOURCE_DIR}/ext/vulkan")
else()
    file(GLOB VALO_VULKAN_CANDIDATES "${CMAKE_SOURCE_DIR}/ext/vulkan/*/x86_64")
    list(GET VALO_VULKAN_CANDIDATES 0 VALO_VULKAN_ROOT)
endif()

set(VALO_VMA_DIR "${CMAKE_SOURCE_DIR}/ext/vma")

if(VALO_VULKAN_ROOT AND EXISTS "${VALO_VULKAN_ROOT}/include/vulkan/vulkan.h")
    set(VALO_HAS_VULKAN ON)
    if(WIN32)
        find_library(VALO_VULKAN_LIB NAMES vulkan-1 PATHS "${VALO_VULKAN_ROOT}/Lib" NO_DEFAULT_PATH)
        # No import library is unpacked from the SDK payload, but mingw can
        # link the loader .dll directly, so fall back to the vendored copy.
        if(NOT VALO_VULKAN_LIB AND EXISTS "${VALO_VULKAN_ROOT}/Lib/vulkan-1.dll")
            set(VALO_VULKAN_LIB "${VALO_VULKAN_ROOT}/Lib/vulkan-1.dll" CACHE FILEPATH "" FORCE)
        endif()
    else()
        find_library(VALO_VULKAN_LIB NAMES vulkan PATHS "${VALO_VULKAN_ROOT}/lib" NO_DEFAULT_PATH)
    endif()
    message(STATUS "VALO: vendored Vulkan SDK at ${VALO_VULKAN_ROOT}")
    message(STATUS "VALO: vulkan loader ${VALO_VULKAN_LIB}")
else()
    set(VALO_HAS_VULKAN OFF)
    message(STATUS "VALO: no vendored Vulkan SDK (run ./build.sh to fetch it) - software renderer only")
endif()

# ---------------------------------------------------------------------------
# VALO library: everything under include/ is the library
# ---------------------------------------------------------------------------
file(GLOB_RECURSE VALO_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/include/*.cpp"
    "${CMAKE_SOURCE_DIR}/include/*.cc"
    "${CMAKE_SOURCE_DIR}/include/*.c"
)
file(GLOB_RECURSE VALO_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/include/*.hpp"
    "${CMAKE_SOURCE_DIR}/include/*.h"
    "${CMAKE_SOURCE_DIR}/include/*.inl"
)

if(VALO_SOURCES)
    if(VALO_SHARED)
        add_library(VALO SHARED ${VALO_SOURCES} ${VALO_HEADERS})
    else()
        add_library(VALO STATIC ${VALO_SOURCES} ${VALO_HEADERS})
    endif()
    target_include_directories(VALO PUBLIC "${CMAKE_SOURCE_DIR}/include")
    target_include_directories(VALO SYSTEM PUBLIC "${VALO_STB_DIR}" "${VALO_TINYOBJ_DIR}" "${VALO_CGLTF_DIR}")
    target_link_libraries(VALO PUBLIC SDL3::SDL3-static)
    if(VALO_HAS_VULKAN)
        target_include_directories(VALO SYSTEM PUBLIC "${VALO_VULKAN_ROOT}/include" "${VALO_VMA_DIR}")
        target_compile_definitions(VALO PUBLIC VALO_HAS_VULKAN=1)
        if(VALO_VULKAN_LIB)
            target_link_libraries(VALO PUBLIC "${VALO_VULKAN_LIB}")
        endif()
    endif()
    target_compile_options(VALO PRIVATE
        $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall -Wextra -Wpedantic>
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
    )
    set_target_properties(VALO PROPERTIES
        OUTPUT_NAME VALO
        VERSION     ${PROJECT_VERSION}
        SOVERSION   ${PROJECT_VERSION_MAJOR}
        ARCHIVE_OUTPUT_DIRECTORY "${VALO_LIB_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${VALO_LIB_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${VALO_LIB_DIR}"
    )
else()
    # No .cpp/.c files under include/ yet -> nothing to compile into a library.
    # Expose VALO as a header-only INTERFACE target so examples still build.
    message(STATUS "VALO: no source files under include/, building as header-only INTERFACE target")
    add_library(VALO INTERFACE ${VALO_HEADERS})
    target_include_directories(VALO INTERFACE "${CMAKE_SOURCE_DIR}/include")
    target_include_directories(VALO SYSTEM INTERFACE "${VALO_STB_DIR}" "${VALO_TINYOBJ_DIR}" "${VALO_CGLTF_DIR}")
    target_link_libraries(VALO INTERFACE SDL3::SDL3-static)
    if(VALO_HAS_VULKAN)
        target_include_directories(VALO SYSTEM INTERFACE "${VALO_VULKAN_ROOT}/include" "${VALO_VMA_DIR}")
        target_compile_definitions(VALO INTERFACE VALO_HAS_VULKAN=1)
        if(VALO_VULKAN_LIB)
            target_link_libraries(VALO INTERFACE "${VALO_VULKAN_LIB}")
        endif()
    endif()
endif()

add_library(VALO::VALO ALIAS VALO)

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(VALO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
