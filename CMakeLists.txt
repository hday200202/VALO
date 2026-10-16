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
option(VALO_BUILD_EXAMPLES  "Build the example programs in examples/" ${PROJECT_IS_TOP_LEVEL})
option(VALO_BUILD_VALOC     "Build valoc, the C++ to GLSL shader translator" ${PROJECT_IS_TOP_LEVEL})
# OFF ignores ext/vulkan even when it is present, so ./build.sh no-vulkan
# produces a genuinely Vulkan-free build rather than just skipping the download.
option(VALO_VULKAN          "Use the vendored Vulkan SDK if present" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)   # keep clang/gcc colors when output is piped through ninja
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Top-level, all library artifacts land in lib/ (source-tree relative, not
# build-dir relative), so the same folder always holds the latest build. As a
# subproject the parent decides where outputs go.
set(VALO_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib")

# ---------------------------------------------------------------------------
# SDL3 (vendored + prebuilt by build.sh -> ext/SDL3-install, lib/libSDL3.a)
# ---------------------------------------------------------------------------
# build.sh clones ext/SDL3 if missing and builds/installs it once into
# ext/SDL3-install. We only consume the exported static target here, so
# SDL is never recompiled per VALO configuration.
# A parent that already has SDL3::SDL3-static keeps it, so there is exactly one
# SDL in the process.
set(VALO_SDL3_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/ext/SDL3-install" CACHE PATH
    "Install prefix of the prebuilt SDL3")
if(NOT TARGET SDL3::SDL3-static)
    list(APPEND CMAKE_PREFIX_PATH "${VALO_SDL3_PREFIX}")
    find_package(SDL3 REQUIRED CONFIG COMPONENTS SDL3-static)
endif()

# ---------------------------------------------------------------------------
# stb (vendored header-only, fetched by build.sh into ext/stb)
# ---------------------------------------------------------------------------
# SYSTEM so stb's own warnings don't show up under -Wall -Wextra -Wpedantic.
# VALO's software/RenderTarget.cpp defines STB_IMAGE_IMPLEMENTATION,
# TINYOBJLOADER_IMPLEMENTATION and CGLTF_IMPLEMENTATION, so a project linking
# VALO must not define any of them itself, or it gets duplicate symbols.
set(VALO_STB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ext/stb")
if(NOT EXISTS "${VALO_STB_DIR}/stb_image.h")
    message(FATAL_ERROR "ext/stb/stb_image.h not found - run ./build.sh (it fetches vendored headers)")
endif()

# tinyobjloader: same treatment. One .cpp must #define TINYOBJLOADER_IMPLEMENTATION.
set(VALO_TINYOBJ_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ext/tinyobjloader")
if(NOT EXISTS "${VALO_TINYOBJ_DIR}/tiny_obj_loader.h")
    message(FATAL_ERROR "ext/tinyobjloader/tiny_obj_loader.h not found - run ./build.sh")
endif()

# cgltf: same treatment. One .cpp must #define CGLTF_IMPLEMENTATION.
set(VALO_CGLTF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ext/cgltf")
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
    set(VALO_VULKAN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ext/vulkan/macOS")
elseif(WIN32)
    set(VALO_VULKAN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ext/vulkan")
else()
    file(GLOB VALO_VULKAN_CANDIDATES "${CMAKE_CURRENT_SOURCE_DIR}/ext/vulkan/*/x86_64")
    list(GET VALO_VULKAN_CANDIDATES 0 VALO_VULKAN_ROOT)
endif()

set(VALO_VMA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ext/vma")

if(VALO_VULKAN AND VALO_VULKAN_ROOT AND EXISTS "${VALO_VULKAN_ROOT}/include/vulkan/vulkan.h")
    set(VALO_HAS_VULKAN ON)
    if(WIN32)
        find_library(VALO_VULKAN_LIB NAMES vulkan-1 PATHS "${VALO_VULKAN_ROOT}/Lib" NO_DEFAULT_PATH)
        # No import library is unpacked from the SDK payload. mingw can link the
        # loader .dll directly; MSVC cannot, so it gets one made from the .dll's
        # exports with the toolchain's own dumpbin and lib.
        if(NOT VALO_VULKAN_LIB AND EXISTS "${VALO_VULKAN_ROOT}/Lib/vulkan-1.dll")
            if(MSVC)
                set(VALO_VULKAN_IMPLIB "${CMAKE_CURRENT_BINARY_DIR}/vulkan-1.lib")
                get_filename_component(VALO_MSVC_TOOLS "${CMAKE_LINKER}" DIRECTORY)
                execute_process(
                    COMMAND "${VALO_MSVC_TOOLS}/dumpbin.exe" /nologo /exports "${VALO_VULKAN_ROOT}/Lib/vulkan-1.dll"
                    OUTPUT_VARIABLE VALO_VULKAN_EXPORTS)
                string(REGEX MATCHALL "[ \t](vk[A-Za-z0-9_]+)\r?\n" VALO_VULKAN_NAMES "${VALO_VULKAN_EXPORTS}")
                set(VALO_VULKAN_DEF "LIBRARY vulkan-1\nEXPORTS\n")
                foreach(name IN LISTS VALO_VULKAN_NAMES)
                    string(STRIP "${name}" name)
                    string(APPEND VALO_VULKAN_DEF "    ${name}\n")
                endforeach()
                file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/vulkan-1.def" "${VALO_VULKAN_DEF}")
                execute_process(
                    COMMAND "${VALO_MSVC_TOOLS}/lib.exe" /nologo /machine:x64
                            "/def:${CMAKE_CURRENT_BINARY_DIR}/vulkan-1.def" "/out:${VALO_VULKAN_IMPLIB}"
                    RESULT_VARIABLE VALO_IMPLIB_RESULT OUTPUT_QUIET)
                if(VALO_IMPLIB_RESULT EQUAL 0)
                    set(VALO_VULKAN_LIB "${VALO_VULKAN_IMPLIB}" CACHE FILEPATH "" FORCE)
                endif()
            else()
                set(VALO_VULKAN_LIB "${VALO_VULKAN_ROOT}/Lib/vulkan-1.dll" CACHE FILEPATH "" FORCE)
            endif()
        endif()
    else()
        # SDK 1.4.3xx moved the loader down into lib/VulkanLoader/lib; older
        # layouts keep it directly in lib/, so both are searched.
        find_library(VALO_VULKAN_LIB NAMES vulkan
            PATHS "${VALO_VULKAN_ROOT}/lib/VulkanLoader/lib" "${VALO_VULKAN_ROOT}/lib"
            NO_DEFAULT_PATH)
        # A vendored SDK that ships no loader still leaves a usable system one,
        # and the loader ABI is stable (libvulkan.so.1), so this is safe.
        if(NOT VALO_VULKAN_LIB)
            find_library(VALO_VULKAN_LIB NAMES vulkan)
        endif()
    endif()
    message(STATUS "VALO: vendored Vulkan SDK at ${VALO_VULKAN_ROOT}")
    message(STATUS "VALO: vulkan loader ${VALO_VULKAN_LIB}")

    # The backend compiles whether or not a loader turns up, so a miss here
    # surfaces only as a wall of undefined vk* symbols at link time. Say so now.
    if(NOT VALO_VULKAN_LIB)
        message(FATAL_ERROR
            "VALO: Vulkan headers found at ${VALO_VULKAN_ROOT}, but no loader to link against.\n"
            "Fix one of:\n"
            "  - install a system Vulkan loader (vulkan-icd-loader / libvulkan1)\n"
            "  - ./build.sh rebuild-vulkan   to re-fetch the vendored SDK\n"
            "  - ./build.sh no-vulkan        to build the software renderer only")
    endif()
else()
    set(VALO_HAS_VULKAN OFF)
    if(NOT VALO_VULKAN)
        message(STATUS "VALO: Vulkan disabled (VALO_VULKAN=OFF) - software renderer only")
    else()
        message(STATUS "VALO: no vendored Vulkan SDK (run ./build.sh to fetch it) - software renderer only")
    endif()
endif()

# ---------------------------------------------------------------------------
# VALO library: everything under include/ is the library
# ---------------------------------------------------------------------------
file(GLOB_RECURSE VALO_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.c"
)
file(GLOB_RECURSE VALO_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.inl"
)

if(VALO_SOURCES)
    if(VALO_SHARED)
        add_library(VALO SHARED ${VALO_SOURCES} ${VALO_HEADERS})
    else()
        add_library(VALO STATIC ${VALO_SOURCES} ${VALO_HEADERS})
    endif()
    target_include_directories(VALO PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
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
        $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall -Wextra -Wpedantic -Wno-missing-field-initializers>
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
    )
    # the headers are UTF-8, and MSVC otherwise reads them in the system code page
    target_compile_options(VALO PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    set_target_properties(VALO PROPERTIES
        OUTPUT_NAME VALO
        VERSION     ${PROJECT_VERSION}
        SOVERSION   ${PROJECT_VERSION_MAJOR}
    )
    if(PROJECT_IS_TOP_LEVEL)
        set_target_properties(VALO PROPERTIES
            ARCHIVE_OUTPUT_DIRECTORY "${VALO_LIB_DIR}"
            LIBRARY_OUTPUT_DIRECTORY "${VALO_LIB_DIR}"
            RUNTIME_OUTPUT_DIRECTORY "${VALO_LIB_DIR}"
        )
    endif()
else()
    # No .cpp/.c files under include/ yet -> nothing to compile into a library.
    # Expose VALO as a header-only INTERFACE target so examples still build.
    message(STATUS "VALO: no source files under include/, building as header-only INTERFACE target")
    add_library(VALO INTERFACE ${VALO_HEADERS})
    target_include_directories(VALO INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
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
# Shaders: the runtime fallback's paths, and build-time embedding
# ---------------------------------------------------------------------------
find_program(VALO_GLSLC NAMES glslc HINTS "${VALO_VULKAN_ROOT}/Bin" "${VALO_VULKAN_ROOT}/bin")

set(VALO_SHADER_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include/render/vulkan" CACHE INTERNAL "")
set(VALO_VALOC_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples"
    "${VALO_STB_DIR}"
    "${VALO_TINYOBJ_DIR}"
    "${VALO_CGLTF_DIR}"
    "${VALO_SDL3_PREFIX}/include"
    CACHE INTERNAL ""
)

if(VALO_SOURCES)
    set(VALO_DEFINITION_SCOPE PRIVATE)
else()
    set(VALO_DEFINITION_SCOPE INTERFACE)
endif()

if(VALO_GLSLC)
    set(VALO_GLSLC_PATH "${VALO_GLSLC}")
else()
    set(VALO_GLSLC_PATH "glslc")
endif()

# the fallback translates and compiles at runtime; baked in so it does not
# depend on the directory the app was launched from
target_compile_definitions(VALO ${VALO_DEFINITION_SCOPE}
    "VALO_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\""
    "VALO_GLSLC_PATH=\"${VALO_GLSLC_PATH}\""
)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ValoEmbedShaders.cmake")

# ---------------------------------------------------------------------------
# valoc - the C++ shader to GLSL translator
#
# Optional on purpose: only shader authors need it. A shader embedded with
# valo_embed_shaders has its GLSL committed, so it builds without libclang;
# the rest translate at runtime, which needs valoc.
# ---------------------------------------------------------------------------
# the toolchain prefix, so an MSYS2 / Homebrew libclang is found without
# the caller having to pass CMAKE_PREFIX_PATH
get_filename_component(VALO_TOOLCHAIN_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(VALO_TOOLCHAIN_ROOT "${VALO_TOOLCHAIN_BIN}" DIRECTORY)

find_library(VALO_LIBCLANG
    NAMES clang libclang
    HINTS "${VALO_TOOLCHAIN_ROOT}/lib" "/opt/homebrew/opt/llvm/lib" "/usr/local/opt/llvm/lib"
)

find_path(VALO_LIBCLANG_INC clang-c/Index.h
    HINTS "${VALO_TOOLCHAIN_ROOT}/include" "/opt/homebrew/opt/llvm/include" "/usr/local/opt/llvm/include"
)

if(VALO_BUILD_VALOC AND VALO_LIBCLANG AND VALO_LIBCLANG_INC)
    add_executable(valoc "${CMAKE_CURRENT_SOURCE_DIR}/tools/valoc/main.cpp")
    target_include_directories(valoc SYSTEM PRIVATE "${VALO_LIBCLANG_INC}")
    target_link_libraries(valoc PRIVATE "${VALO_LIBCLANG}")
    set_target_properties(valoc PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")

    # libclang cannot parse C++ without its own builtin headers (stddef.h,
    # mm_malloc.h and friends), and they do not live on any normal include
    # path - so the location is baked in rather than left to the caller.
    # Derived from libclang's own location, not the compiler's: the compiler may
    # be Apple clang in /usr/bin while libclang comes from Homebrew, and the
    # builtin headers have to match the libclang actually doing the parsing.
    get_filename_component(VALO_LIBCLANG_DIR "${VALO_LIBCLANG}" DIRECTORY)
    file(GLOB VALO_CLANG_RESOURCE_DIRS
        "${VALO_LIBCLANG_DIR}/clang/*/include"
        "${VALO_TOOLCHAIN_ROOT}/lib/clang/*/include"
    )
    if(VALO_CLANG_RESOURCE_DIRS)
        list(GET VALO_CLANG_RESOURCE_DIRS 0 VALO_CLANG_RESOURCE)
        # -resource-dir wants the parent of include/, and letting clang place the
        # builtins itself keeps them after libc++ in the search order. Passing the
        # include dir as -I instead puts it first, and <cstddef> then finds the
        # wrong stddef.h.
        get_filename_component(VALO_CLANG_RESOURCE "${VALO_CLANG_RESOURCE}" DIRECTORY)
        target_compile_definitions(valoc PRIVATE "VALOC_CLANG_RESOURCE=\"${VALO_CLANG_RESOURCE}\"")
        message(STATUS "valoc: clang resource dir at ${VALO_CLANG_RESOURCE}")
    else()
        message(WARNING "valoc: clang builtin headers not found - parsing will fail")
    endif()

    # The shader headers include <vector>, <cmath> and so on, so libclang needs
    # the same system include path the compiler uses. CMake already computed it
    # for this toolchain; passing it through means valoc does not have to guess
    # at SDK locations or shell out to the compiler at runtime.
    set(VALO_VALOC_SYSINC "")
    foreach(dir IN LISTS CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
        set(VALO_VALOC_SYSINC "${VALO_VALOC_SYSINC}${dir}|")
    endforeach()
    target_compile_definitions(valoc PRIVATE "VALOC_SYSTEM_INCLUDES=\"${VALO_VALOC_SYSINC}\"")

    message(STATUS "valoc: libclang at ${VALO_LIBCLANG}")
elseif(VALO_BUILD_VALOC)
    message(STATUS "valoc: libclang not found - the shader translator will not be built")
endif()

# ---------------------------------------------------------------------------
# image-diff
# ---------------------------------------------------------------------------
if(PROJECT_IS_TOP_LEVEL)
    add_executable(image-diff "${CMAKE_CURRENT_SOURCE_DIR}/tools/image-diff.cpp")
    target_link_libraries(image-diff PRIVATE VALO::VALO)
    set_target_properties(image-diff PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(VALO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
