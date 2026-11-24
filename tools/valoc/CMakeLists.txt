cmake_minimum_required(VERSION 3.24)

# valoc - the C++ shader to GLSL translator. A project of its own so VALO never
# depends on it or on libclang: ./build.sh valoc builds it into bin/.
project(valoc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# the toolchain prefix, so an MSYS2 / Homebrew libclang is found without
# the caller having to pass CMAKE_PREFIX_PATH
get_filename_component(VALOC_TOOLCHAIN_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(VALOC_TOOLCHAIN_ROOT "${VALOC_TOOLCHAIN_BIN}" DIRECTORY)

find_library(VALOC_LIBCLANG
    NAMES clang libclang
    HINTS "${VALOC_TOOLCHAIN_ROOT}/lib" "/opt/homebrew/opt/llvm/lib" "/usr/local/opt/llvm/lib"
)

find_path(VALOC_LIBCLANG_INC clang-c/Index.h
    HINTS "${VALOC_TOOLCHAIN_ROOT}/include" "/opt/homebrew/opt/llvm/include" "/usr/local/opt/llvm/include"
)

if(NOT VALOC_LIBCLANG OR NOT VALOC_LIBCLANG_INC)
    message(FATAL_ERROR "valoc: libclang not found (MSYS2: pacman -S mingw-w64-ucrt-x86_64-clang)")
endif()

add_executable(valoc "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
target_include_directories(valoc SYSTEM PRIVATE "${VALOC_LIBCLANG_INC}")
target_link_libraries(valoc PRIVATE "${VALOC_LIBCLANG}")
set_target_properties(valoc PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../../bin")

# libclang cannot parse C++ without its own builtin headers (stddef.h,
# mm_malloc.h and friends), and they do not live on any normal include
# path - so the location is baked in rather than left to the caller.
# Derived from libclang's own location, not the compiler's: the compiler may
# be Apple clang in /usr/bin while libclang comes from Homebrew, and the
# builtin headers have to match the libclang actually doing the parsing.
get_filename_component(VALOC_LIBCLANG_DIR "${VALOC_LIBCLANG}" DIRECTORY)
file(GLOB VALOC_CLANG_RESOURCE_DIRS
    "${VALOC_LIBCLANG_DIR}/clang/*/include"
    "${VALOC_TOOLCHAIN_ROOT}/lib/clang/*/include"
)
if(VALOC_CLANG_RESOURCE_DIRS)
    list(GET VALOC_CLANG_RESOURCE_DIRS 0 VALOC_CLANG_RESOURCE)
    # -resource-dir wants the parent of include/, and letting clang place the
    # builtins itself keeps them after libc++ in the search order. Passing the
    # include dir as -I instead puts it first, and <cstddef> then finds the
    # wrong stddef.h.
    get_filename_component(VALOC_CLANG_RESOURCE "${VALOC_CLANG_RESOURCE}" DIRECTORY)
    target_compile_definitions(valoc PRIVATE "VALOC_CLANG_RESOURCE=\"${VALOC_CLANG_RESOURCE}\"")
    message(STATUS "valoc: clang resource dir at ${VALOC_CLANG_RESOURCE}")
else()
    message(WARNING "valoc: clang builtin headers not found - parsing will fail")
endif()

# The shader headers include <vector>, <cmath> and so on, so libclang needs
# the same system include path the compiler uses. CMake already computed it
# for this toolchain; passing it through means valoc does not have to guess
# at SDK locations or shell out to the compiler at runtime.
set(VALOC_SYSINC "")
foreach(dir IN LISTS CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
    set(VALOC_SYSINC "${VALOC_SYSINC}${dir}|")
endforeach()
target_compile_definitions(valoc PRIVATE "VALOC_SYSTEM_INCLUDES=\"${VALOC_SYSINC}\"")

message(STATUS "valoc: libclang at ${VALOC_LIBCLANG}")
