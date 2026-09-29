#!/usr/bin/env bash
# Usage: ./build.sh [release | debug | rdbi] [static | dynamic] [clean]
#                   [rebuild-sdl] [no-vulkan | rebuild-vulkan | vulkan-full]
#
# Everything is vendored: a fresh clone needs only a compiler, cmake and curl.
# Nothing is installed system-wide and nothing is read from outside the repo.
#
#   release        -> CMAKE_BUILD_TYPE=Release          (default)
#   debug          -> CMAKE_BUILD_TYPE=Debug
#   rdbi           -> CMAKE_BUILD_TYPE=RelWithDebInfo
#   static         -> lib/libVALO.a                     (default)
#   dynamic        -> lib/libVALO.dylib
#   clean          -> wipe this config's VALO build dir before building
#   rebuild-sdl    -> force SDL3 to be rebuilt (normally built once and cached)
#   no-vulkan      -> skip the Vulkan SDK entirely (software renderer only)
#   rebuild-vulkan -> wipe ext/vulkan and re-download the SDK
#   vulkan-full    -> keep the whole SDK instead of pruning it to ~520 MB
#
# SDL3 handling (vendored):
#   1. ext/SDL3 missing            -> git clone SDL_TAG from SDL_REPO into ext/SDL3
#   2. ext/SDL3-install missing    -> build SDL3 (static, PIC, Release) once
#                                     into build/sdl3, install to ext/SDL3-install,
#                                     copy libSDL3.a to lib/
#   3. otherwise                   -> SDL3 step is skipped entirely
#
# Vulkan handling (vendored, prebuilt - no source build):
#   ext/vulkan missing -> download the LunarG SDK for the host platform. macOS
#                         and Linux install/untar it into ext/vulkan; Windows
#                         unpacks the 7z payload straight out of the installer
#                         binary (its "core" component demands admin otherwise)
#                         and pulls headers from the matching Khronos tag. Then
#                         prune the
#                         tooling VALO never calls (2.6 GB -> ~520 MB).
#                         Gives loader + validation layers + glslc +
#                         glslangValidator + headers, and MoltenVK on macOS.
#   otherwise          -> skipped. Re-run with rebuild-vulkan to force.
#   Also writes ext/vulkan/env.sh: source it to point the loader at the
#   vendored ICD and layers, which are not in any system search path.
#
# header-only libs (vendored):
#   ext/stb/stb_image.h missing               -> download (pinned STB_COMMIT) into ext/stb
#   ext/tinyobjloader/tiny_obj_loader.h missing -> download (pinned TINYOBJ_TAG)
#   ext/cgltf/cgltf.h missing                 -> download (pinned CGLTF_TAG)
#   ext/vma/vk_mem_alloc.h missing            -> download (pinned VMA_TAG)
#   otherwise                                 -> skipped
#
# Outputs:
#   lib/                      libSDL3.a + libVALO.{a,dylib}
#   examples/bin/             one executable per examples/*.cpp
#   build/<type>-<linkage>/   VALO cmake cache + objects (one dir per config)
#   build/sdl3/               SDL3 cmake cache + objects
#   ext/SDL3-install/         SDL3 headers, lib, cmake config (consumed by find_package)
#   ext/vulkan/               Vulkan SDK (loader, layers, MoltenVK, glslc, headers)
#   ext/vulkan/env.sh         runtime env for the vendored loader/ICD/layers

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

# Vulkan SDK (LunarG, prebuilt). Vendored whole into ext/vulkan so a clone needs
# nothing installed system-wide: loader, MoltenVK (macOS ICD), validation layers,
# glslc/glslangValidator and the headers all come from here.
VULKAN_VERSION="${VULKAN_VERSION:-1.4.363.0}"
# Windows takes its headers straight from Khronos: only Bin/ is embedded in
# the LunarG installer binary, the rest is fetched by the installer at runtime.
VULKAN_HEADERS_TAG="${VULKAN_HEADERS_TAG:-vulkan-sdk-$VULKAN_VERSION}"
VULKAN_DIR="ext/vulkan"

# VMA is not in the SDK; single header, pinned tag
VMA_TAG="${VMA_TAG:-v3.3.0}"
VMA_URL="https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/$VMA_TAG/include/vk_mem_alloc.h"
VMA_DIR="ext/vma"

BUILD_TYPE="Release"
LINKAGE="static"
CLEAN=0
REBUILD_SDL=0
REBUILD_VULKAN=0
WITH_VULKAN=1
PRUNE_VULKAN=1

for arg in "$@"; do
    case "$(echo "$arg" | tr '[:upper:]' '[:lower:]')" in
        release)        BUILD_TYPE="Release" ;;
        debug)          BUILD_TYPE="Debug" ;;
        rdbi)           BUILD_TYPE="RelWithDebInfo" ;;
        static)         LINKAGE="static" ;;
        dynamic|shared) LINKAGE="dynamic" ;;
        clean)          CLEAN=1 ;;
        rebuild-sdl)    REBUILD_SDL=1 ;;
        no-vulkan)      WITH_VULKAN=0 ;;
        rebuild-vulkan) REBUILD_VULKAN=1 ;;
        vulkan-full)    PRUNE_VULKAN=0 ;;
        -h|--help)
            sed -n '2,30p' "$0"
            exit 0
            ;;
        *)
            echo "error: unknown argument '$arg'" >&2
            echo "usage: ./build.sh [release | debug | rdbi] [static | dynamic] [clean]" >&2
            echo "                  [rebuild-sdl] [no-vulkan | rebuild-vulkan | vulkan-full]" >&2
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
# 1c. Vendor the Vulkan SDK (prebuilt, ~520 MB installed after pruning)
# ---------------------------------------------------------------------------
# The SDK is the only practical source of desktop validation layers: Khronos
# publishes prebuilt layers for Android only, and building them from source
# pulls SPIRV-Tools + glslang and takes 15+ minutes. Everything lands under
# ext/vulkan so nothing has to be installed system-wide.
#
# macOS/Windows ship a Qt Installer Framework binary, which installs headlessly
# into an arbitrary --root. Linux is a plain tarball.

# A progress bar is useful at a terminal and megabytes of carriage returns in a
# redirected log, so only ask for one when stderr is a tty.
if [[ -t 2 ]]; then VK_CURL_PROGRESS=(--progress-bar); else VK_CURL_PROGRESS=(-sS); fi

vulkan_platform_paths() {
    # sets VK_SDK_ROOT (the dir holding bin/lib/include/share) for the host
    case "$(uname -s)" in
        Darwin)              VK_SDK_ROOT="$VULKAN_DIR/macOS" ;;
        Linux)               VK_SDK_ROOT="$VULKAN_DIR/$VULKAN_VERSION/x86_64" ;;
        MINGW*|MSYS*|CYGWIN*)VK_SDK_ROOT="$VULKAN_DIR" ;;
        *)                   VK_SDK_ROOT="" ;;
    esac
}

vulkan_need_7z() {
    # The payload is a plain 7z stream appended to the installer .exe. The msys
    # p7zip package ships wrappers pointing at binaries it does not install, so
    # probe for one that actually runs before falling back to installing 7zip.
    local c
    for c in "${MSYSTEM_PREFIX:-/ucrt64}/bin/7z.exe" 7z 7za 7zz; do
        if command -v "$c" >/dev/null 2>&1 && "$c" i >/dev/null 2>&1; then
            VK_7Z="$c"; return
        fi
    done
    echo "==> no working 7z found, installing mingw-w64-ucrt-x86_64-7zip"
    # no -Sy: refreshing the db without a full -Syu is the partial-upgrade trap
    pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-7zip >/dev/null 2>&1 || true
    VK_7Z="${MSYSTEM_PREFIX:-/ucrt64}/bin/7z.exe"
    if ! "$VK_7Z" i >/dev/null 2>&1; then
        echo "error: need 7z to unpack the Vulkan SDK; run 'pacman -Syu' then retry" >&2
        exit 1
    fi
}

vulkan_prune() {
    # Roughly 2.6 GB as installed; the bulk is tooling VALO never calls.
    # libshaderc_combined.a alone is 1.4 GB and only matters if you statically
    # link shaderc into your app instead of shelling out to glslc.
    local root="$1"
    rm -rf "$VULKAN_DIR/Applications" "$VULKAN_DIR/MaintenanceTool.app" \
           "$VULKAN_DIR/installerResources" "$VULKAN_DIR/tmpMaintenanceToolApp" \
           "$VULKAN_DIR/Demos"
    rm -f  "$root/lib/libshaderc_combined.a" \
           "$root/lib/libspirv-cross-"*.a \
           "$root/lib/libVulkanSafeStruct.a" \
           "$root/lib/libVulkanLayerSettings.a" \
           "$root/lib/libslang"* \
           "$root/lib/libdxcompiler."* "$root/lib/libdxil."* 2>/dev/null || true
    rm -rf "$root/lib/slang-standard-module-"* 2>/dev/null || true
    rm -f  "$root/bin/gfxrecon-"* "$root/bin/slangc" "$root/bin/vkconfig" 2>/dev/null || true

    # Windows unpack lands in Bin/ with different casing and a lot of ballast
    rm -rf "$root/Bin/slang-standard-module-"* 2>/dev/null || true
    rm -f  "$root/Bin/"*.pdb "$root/Bin/dxc.exe" "$root/Bin/dxcompiler.dll" \
           "$root/Bin/dxil.dll" "$root/Bin/slangc.exe" "$root/Bin/slang"*.dll \
           "$root/Bin/gfxrecon"* "$root/Bin/vkconfig"* 2>/dev/null || true
}

if [[ "$WITH_VULKAN" == "1" ]]; then
    if [[ "$REBUILD_VULKAN" == "1" ]]; then
        echo "==> rebuild-vulkan requested, removing $VULKAN_DIR"
        rm -rf "$VULKAN_DIR"
    fi

    vulkan_platform_paths
    if [[ -z "$VK_SDK_ROOT" ]]; then
        echo "==> WARNING: unrecognised platform $(uname -s), skipping the Vulkan SDK" >&2
    elif [[ -d "$VK_SDK_ROOT/include/vulkan" ]]; then
        echo "==> Vulkan SDK present in $VK_SDK_ROOT"
    else
        VK_TMP="build/vulkan-download"
        mkdir -p "$VK_TMP"
        echo "==> Vulkan SDK $VULKAN_VERSION not found, downloading (this is a few hundred MB, once)"

        case "$(uname -s)" in
            Darwin)
                VK_ARCHIVE="$VK_TMP/vulkan_sdk.zip"
                [[ -f "$VK_ARCHIVE" ]] || curl -fL "${VK_CURL_PROGRESS[@]}" \
                    "https://sdk.lunarg.com/sdk/download/$VULKAN_VERSION/mac/vulkan_sdk.zip" -o "$VK_ARCHIVE"
                rm -rf "$VK_TMP/unpacked"
                unzip -q -o "$VK_ARCHIVE" -d "$VK_TMP/unpacked"
                # unpacked/<name>.app/Contents/MacOS/<binary> is 4 levels down
                VK_INSTALLER="$(find "$VK_TMP/unpacked" -maxdepth 5 -type f -perm -u+x -path '*.app/Contents/MacOS/*' | head -1)"
                if [[ -z "$VK_INSTALLER" ]]; then
                    echo "error: no installer binary inside vulkan_sdk.zip" >&2; exit 1
                fi
                echo "==> installing headlessly into $VULKAN_DIR"
                "$VK_INSTALLER" --root "$PWD/$VULKAN_DIR" \
                    --accept-licenses --default-answer --confirm-command install >/dev/null
                ;;
            Linux)
                VK_ARCHIVE="$VK_TMP/vulkan_sdk.tar.xz"
                [[ -f "$VK_ARCHIVE" ]] || curl -fL "${VK_CURL_PROGRESS[@]}" \
                    "https://sdk.lunarg.com/sdk/download/$VULKAN_VERSION/linux/vulkan_sdk.tar.xz" -o "$VK_ARCHIVE"
                mkdir -p "$VULKAN_DIR"
                tar -xf "$VK_ARCHIVE" -C "$VULKAN_DIR"
                ;;
            MINGW*|MSYS*|CYGWIN*)
                # The Windows SDK is a Qt Installer Framework bundle whose
                # "core" component is flagged Always Installed and demands
                # elevation regardless of --root, so it cannot be driven into a
                # local prefix the way the macOS one can - it extracts fine and
                # then rolls the whole thing back with "Cannot elevate access
                # rights while running from command line".
                #
                # Nothing about that is needed here: the payload is a 7z stream
                # appended to the binary, so unpack it directly. No installer,
                # no elevation, nothing touched outside ext/.
                VK_ARCHIVE="$VK_TMP/vulkan_sdk.exe"
                [[ -f "$VK_ARCHIVE" ]] || curl -fL "${VK_CURL_PROGRESS[@]}" \
                    "https://sdk.lunarg.com/sdk/download/$VULKAN_VERSION/windows/vulkan_sdk.exe" -o "$VK_ARCHIVE"

                vulkan_need_7z
                echo "==> unpacking the SDK payload into $VULKAN_DIR (no installer, no admin)"
                mkdir -p "$VULKAN_DIR"
                "$VK_7Z" x -y -o"$VULKAN_DIR" "$VK_ARCHIVE" >/dev/null

                if [[ ! -d "$VULKAN_DIR/Bin" ]]; then
                    echo "error: unpacking $VK_ARCHIVE produced no Bin/" >&2; exit 1
                fi

                # Only Bin/ rides inside the binary (glslc, glslangValidator,
                # spirv-*, and every layer .dll beside its .json manifest). The
                # headers are a separate component the installer would have
                # fetched, so take them from the matching Khronos tag instead.
                echo "==> downloading Vulkan headers ($VULKAN_HEADERS_TAG)"
                VK_HDR="$VK_TMP/vulkan-headers.tar.gz"
                [[ -f "$VK_HDR" ]] || curl -fL "${VK_CURL_PROGRESS[@]}" \
                    "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/$VULKAN_HEADERS_TAG.tar.gz" \
                    -o "$VK_HDR"
                rm -rf "$VK_TMP/headers"; mkdir -p "$VK_TMP/headers"
                tar -xf "$VK_HDR" -C "$VK_TMP/headers" --strip-components=1
                mkdir -p "$VULKAN_DIR/Include"
                cp -r "$VK_TMP/headers/include/." "$VULKAN_DIR/Include/"

                # The loader itself ships with the GPU driver rather than the
                # SDK, so copy it in to keep ext/ self-contained. mingw links
                # against a bare .dll directly, no import library needed.
                mkdir -p "$VULKAN_DIR/Lib"
                for VK_LOADER in /c/Windows/System32/vulkan-1.dll "$SYSTEMROOT/System32/vulkan-1.dll"; do
                    if [[ -f "$VK_LOADER" ]]; then
                        cp -f "$VK_LOADER" "$VULKAN_DIR/Lib/vulkan-1.dll"
                        echo "==> vendored the loader from $VK_LOADER"
                        break
                    fi
                done
                if [[ ! -f "$VULKAN_DIR/Lib/vulkan-1.dll" ]]; then
                    echo "==> WARNING: no vulkan-1.dll found; install a GPU driver with Vulkan support" >&2
                fi
                ;;
        esac

        vulkan_platform_paths
        if [[ ! -d "$VK_SDK_ROOT/include/vulkan" ]]; then
            echo "error: Vulkan SDK install did not produce $VK_SDK_ROOT/include/vulkan" >&2
            exit 1
        fi

        [[ "$PRUNE_VULKAN" == "1" ]] && vulkan_prune "$VK_SDK_ROOT"
        rm -rf "$VK_TMP"
        echo "==> Vulkan SDK installed to $VK_SDK_ROOT ($(du -sh "$VULKAN_DIR" | cut -f1))"
    fi

    # VMA: single header, not shipped with the SDK
    mkdir -p "$VMA_DIR"
    if [[ ! -f "$VMA_DIR/vk_mem_alloc.h" ]]; then
        echo "==> vk_mem_alloc.h not found in $VMA_DIR, downloading ($VMA_TAG)"
        curl -fsSL "$VMA_URL" -o "$VMA_DIR/vk_mem_alloc.h"
    else
        echo "==> vk_mem_alloc.h present in $VMA_DIR"
    fi

    # env.sh: the loader only finds a vendored ICD and vendored layers through
    # these, since they are not in any system search path.
    if [[ -n "$VK_SDK_ROOT" && -d "$VK_SDK_ROOT/include/vulkan" ]]; then
        VK_ABS="$PWD/$VK_SDK_ROOT"
        cat > "$VULKAN_DIR/env.sh" <<ENVEOF
# Generated by build.sh - source this before running anything that uses Vulkan.
export VULKAN_SDK="$VK_ABS"
ENVEOF
        case "$(uname -s)" in
            MINGW*|MSYS*|CYGWIN*)
                # unpacked layout: tools and layer manifests both live in Bin/
                cat >> "$VULKAN_DIR/env.sh" <<'ENVEOF'
export PATH="$VULKAN_SDK/Bin:$PATH"
export VK_ADD_LAYER_PATH="$VULKAN_SDK/Bin"
export VK_LAYER_PATH="$VULKAN_SDK/Bin"
ENVEOF
                ;;
            *)
                cat >> "$VULKAN_DIR/env.sh" <<'ENVEOF'
export PATH="$VULKAN_SDK/bin:$PATH"
export VK_ADD_LAYER_PATH="$VULKAN_SDK/share/vulkan/explicit_layer.d"
export VK_LAYER_PATH="$VULKAN_SDK/share/vulkan/explicit_layer.d"
ENVEOF
                ;;
        esac
        if [[ "$(uname -s)" == "Darwin" ]]; then
            cat >> "$VULKAN_DIR/env.sh" <<'ENVEOF'
export VK_ICD_FILENAMES="$VULKAN_SDK/share/vulkan/icd.d/MoltenVK_icd.json"
export DYLD_LIBRARY_PATH="$VULKAN_SDK/lib:${DYLD_LIBRARY_PATH:-}"
ENVEOF
        elif [[ "$(uname -s)" == "Linux" ]]; then
            cat >> "$VULKAN_DIR/env.sh" <<'ENVEOF'
export LD_LIBRARY_PATH="$VULKAN_SDK/lib:${LD_LIBRARY_PATH:-}"
ENVEOF
        fi
        echo "==> wrote $VULKAN_DIR/env.sh"
    fi
else
    echo "==> no-vulkan requested, skipping the Vulkan SDK"
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
