#!/usr/bin/env bash
# Usage: ./build.sh [release | debug | rdbi] [static | dynamic] [clean] [rebuild-sdl]
#
#   release      -> CMAKE_BUILD_TYPE=Release          (default)
#   debug        -> CMAKE_BUILD_TYPE=Debug
#   rdbi         -> CMAKE_BUILD_TYPE=RelWithDebInfo
#   static       -> lib/libVALO.a                     (default)
#   dynamic      -> lib/libVALO.dylib
#   clean        -> wipe this config's VALO build dir before building
#   rebuild-sdl  -> force SDL3 to be rebuilt (normally built once and cached)
#
# SDL3 handling (vendored):
#   1. ext/SDL3 missing            -> git clone SDL_TAG from SDL_REPO into ext/SDL3
#   2. ext/SDL3-install missing    -> build SDL3 (static, PIC, Release) once
#                                     into build/sdl3, install to ext/SDL3-install,
#                                     copy libSDL3.a to lib/
#   3. otherwise                   -> SDL3 step is skipped entirely
#
# header-only libs (vendored):
#   ext/stb/stb_image.h missing               -> download (pinned STB_COMMIT) into ext/stb
#   ext/tinyobjloader/tiny_obj_loader.h missing -> download (pinned TINYOBJ_TAG)
#   ext/cgltf/cgltf.h missing                 -> download (pinned CGLTF_TAG)
#   otherwise                                 -> skipped
#
# Outputs:
#   lib/                      libSDL3.a + libVALO.{a,dylib}
#   examples/bin/             one executable per examples/*.cpp
#   build/<type>-<linkage>/   VALO cmake cache + objects (one dir per config)
#   build/sdl3/               SDL3 cmake cache + objects
#   ext/SDL3-install/         SDL3 headers, lib, cmake config (consumed by find_package)

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

SDL_REPO="${SDL_REPO:-https://github.com/libsdl-org/SDL.git}"
SDL_TAG="${SDL_TAG:-release-3.2.10}"

SDL_SRC_DIR="ext/SDL3"
SDL_BUILD_DIR="build/sdl3"
SDL_INSTALL_DIR="ext/SDL3-install"
SDL_CONFIG_FILE="$SDL_INSTALL_DIR/lib/cmake/SDL3/SDL3Config.cmake"

# stb: header-only, so we just fetch the single file (pinned commit)
STB_COMMIT="${STB_COMMIT:-2c980bb59875b0d32144a71867fbdebb2f77cd20}"
STB_BASE_URL="https://raw.githubusercontent.com/nothings/stb/$STB_COMMIT"
STB_DIR="ext/stb"
STB_HEADERS=(stb_image.h)

# tinyobjloader: same deal, single header at a pinned release tag
TINYOBJ_TAG="${TINYOBJ_TAG:-v1.0.6}"
TINYOBJ_URL="https://raw.githubusercontent.com/tinyobjloader/tinyobjloader/$TINYOBJ_TAG/tiny_obj_loader.h"
TINYOBJ_DIR="ext/tinyobjloader"

# cgltf: single-header glTF 2.0 loader, pinned release tag
CGLTF_TAG="${CGLTF_TAG:-v1.15}"
CGLTF_URL="https://raw.githubusercontent.com/jkuhlmann/cgltf/$CGLTF_TAG/cgltf.h"
CGLTF_DIR="ext/cgltf"

BUILD_TYPE="Release"
LINKAGE="static"
CLEAN=0
REBUILD_SDL=0

for arg in "$@"; do
    case "$(echo "$arg" | tr '[:upper:]' '[:lower:]')" in
        release)        BUILD_TYPE="Release" ;;
        debug)          BUILD_TYPE="Debug" ;;
        rdbi)           BUILD_TYPE="RelWithDebInfo" ;;
        static)         LINKAGE="static" ;;
        dynamic|shared) LINKAGE="dynamic" ;;
        clean)          CLEAN=1 ;;
        rebuild-sdl)    REBUILD_SDL=1 ;;
        -h|--help)
            sed -n '2,24p' "$0"
            exit 0
            ;;
        *)
            echo "error: unknown argument '$arg'" >&2
            echo "usage: ./build.sh [release | debug | rdbi] [static | dynamic] [clean] [rebuild-sdl]" >&2
            exit 1
            ;;
    esac
done

GENERATOR_ARGS=()
if command -v ninja >/dev/null 2>&1; then
    GENERATOR_ARGS=(-G Ninja)
fi

JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"

# ---------------------------------------------------------------------------
# 1. Vendor SDL3 source
# ---------------------------------------------------------------------------
if [[ ! -f "$SDL_SRC_DIR/CMakeLists.txt" ]]; then
    echo "==> SDL3 not found in $SDL_SRC_DIR, cloning $SDL_TAG from $SDL_REPO"
    git clone --depth 1 --branch "$SDL_TAG" "$SDL_REPO" "$SDL_SRC_DIR"
else
    echo "==> SDL3 source present in $SDL_SRC_DIR"
fi

# ---------------------------------------------------------------------------
# 1b. Vendor stb headers
# ---------------------------------------------------------------------------
mkdir -p "$STB_DIR"
for hdr in "${STB_HEADERS[@]}"; do
    if [[ ! -f "$STB_DIR/$hdr" ]]; then
        echo "==> $hdr not found in $STB_DIR, downloading (stb @ ${STB_COMMIT:0:12})"
        curl -fsSL "$STB_BASE_URL/$hdr" -o "$STB_DIR/$hdr"
    else
        echo "==> $hdr present in $STB_DIR"
    fi
done

mkdir -p "$TINYOBJ_DIR"
if [[ ! -f "$TINYOBJ_DIR/tiny_obj_loader.h" ]]; then
    echo "==> tiny_obj_loader.h not found in $TINYOBJ_DIR, downloading ($TINYOBJ_TAG)"
    curl -fsSL "$TINYOBJ_URL" -o "$TINYOBJ_DIR/tiny_obj_loader.h"
else
    echo "==> tiny_obj_loader.h present in $TINYOBJ_DIR"
fi

mkdir -p "$CGLTF_DIR"
if [[ ! -f "$CGLTF_DIR/cgltf.h" ]]; then
    echo "==> cgltf.h not found in $CGLTF_DIR, downloading ($CGLTF_TAG)"
    curl -fsSL "$CGLTF_URL" -o "$CGLTF_DIR/cgltf.h"
else
    echo "==> cgltf.h present in $CGLTF_DIR"
fi

# ---------------------------------------------------------------------------
# 2. Build SDL3 once (static, PIC so a dynamic VALO can link it)
# ---------------------------------------------------------------------------
if [[ "$REBUILD_SDL" == "1" ]]; then
    echo "==> rebuild-sdl requested, removing $SDL_BUILD_DIR and $SDL_INSTALL_DIR"
    rm -rf "$SDL_BUILD_DIR" "$SDL_INSTALL_DIR"
fi

if [[ ! -f "$SDL_CONFIG_FILE" || ! -f "lib/libSDL3.a" ]]; then
    echo "==> building SDL3 (this only happens once)"
    cmake -S "$SDL_SRC_DIR" -B "$SDL_BUILD_DIR" \
        "${GENERATOR_ARGS[@]}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PWD/$SDL_INSTALL_DIR" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DSDL_SHARED=OFF \
        -DSDL_STATIC=ON \
        -DSDL_TEST_LIBRARY=OFF \
        -DSDL_TESTS=OFF \
        -DSDL_EXAMPLES=OFF \
        -DSDL_INSTALL_DOCS=OFF
    cmake --build "$SDL_BUILD_DIR" --parallel "$JOBS"
    cmake --install "$SDL_BUILD_DIR"
    mkdir -p lib
    cp -f "$SDL_INSTALL_DIR/lib/libSDL3.a" lib/libSDL3.a
    echo "==> SDL3 installed to $SDL_INSTALL_DIR, static lib copied to lib/libSDL3.a"
else
    echo "==> SDL3 already built ($SDL_INSTALL_DIR), skipping"
fi

# ---------------------------------------------------------------------------
# 3. Build VALO + examples
# ---------------------------------------------------------------------------
VALO_SHARED="OFF"
[[ "$LINKAGE" == "dynamic" ]] && VALO_SHARED="ON"

BUILD_DIR="build/$(echo "$BUILD_TYPE" | tr '[:upper:]' '[:lower:]')-$LINKAGE"

if [[ "$CLEAN" == "1" ]]; then
    echo "==> cleaning $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

echo "==> configuring VALO  type=$BUILD_TYPE  linkage=$LINKAGE  dir=$BUILD_DIR"
cmake -S . -B "$BUILD_DIR" \
    "${GENERATOR_ARGS[@]}" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DVALO_SHARED="$VALO_SHARED"

echo "==> building VALO"
cmake --build "$BUILD_DIR" --parallel "$JOBS"

# Keep a stable compile_commands.json at the root for clangd / IDEs.
ln -sf "$BUILD_DIR/compile_commands.json" compile_commands.json

echo
echo "==> done"
echo "    libs:     $(ls lib 2>/dev/null | grep -v '^\.' | tr '\n' ' ')"
echo "    examples: $(ls examples/bin 2>/dev/null | grep -v '^\.' | tr '\n' ' ')"
