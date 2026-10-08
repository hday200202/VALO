# valo_embed_shaders(<target> SHADERS a.vert.hpp a.frag.hpp ... [INCLUDES dir ...])
#
# Compiles each shader to SPIR-V at build time and links it into <target>, so the
# Vulkan backend creates the program from memory: no glslc, no valoc and no
# files beside the executable at runtime. A path ending in .hpp is a C++ shader
# whose GLSL sits beside it with the .hpp stripped; anything else is GLSL.
#
# valoc runs first, only when it was built and the GLSL is stale. Without it the
# GLSL has to exist already. INCLUDES are extra -I paths for valoc, for shaders
# that include headers outside VALO.

set(VALO_EMBED_SPIRV_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/EmbedSpirv.cmake" CACHE INTERNAL "")

function(valo_embed_shaders target)
    cmake_parse_arguments(ARG "" "" "SHADERS;INCLUDES" ${ARGN})

    if(NOT VALO_GLSLC)
        message(FATAL_ERROR "valo_embed_shaders(${target}): glslc not found - fetch the Vulkan SDK with ./build.sh")
    endif()

    set(valoc_includes)
    foreach(dir IN LISTS VALO_VALOC_INCLUDES ARG_INCLUDES)
        list(APPEND valoc_includes -I "${dir}")
    endforeach()

    foreach(shader IN LISTS ARG_SHADERS)
        get_filename_component(source "${shader}" ABSOLUTE)

        if(source MATCHES "\\.hpp$")
            string(REGEX REPLACE "\\.hpp$" "" glsl "${source}")
        else()
            set(glsl "${source}")
        endif()

        get_filename_component(name "${glsl}" NAME)
        set(outdir "${CMAKE_CURRENT_BINARY_DIR}/valo_spirv/${target}")
        set(spv "${outdir}/${name}.spv")
        set(cpp "${outdir}/${name}.cpp")

        set(translate)
        set(byproducts BYPRODUCTS "${spv}")
        set(depends "${source}" "${VALO_SHADER_INCLUDE_DIR}/valo.glsl")

        # a byproduct, so a missing GLSL reruns the rule instead of failing it
        if(TARGET valoc AND source MATCHES "\\.hpp$")
            set(translate COMMAND $<TARGET_FILE:valoc> "${source}" -o "${glsl}" ${valoc_includes} --if-stale)
            list(APPEND byproducts "${glsl}")
            list(APPEND depends valoc)
        elseif(EXISTS "${glsl}")
            list(APPEND depends "${glsl}")
        else()
            message(FATAL_ERROR "valo_embed_shaders(${target}): ${glsl} does not exist and valoc was not built to make it")
        endif()

        add_custom_command(
            OUTPUT  "${cpp}"
            ${byproducts}
            ${translate}
            # glslc will not create its output directory, and the Makefile
            # generator does not create it for custom command outputs either
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${outdir}"
            COMMAND "${VALO_GLSLC}" -I "${VALO_SHADER_INCLUDE_DIR}" "${glsl}" -o "${spv}"
            COMMAND "${CMAKE_COMMAND}" -DINPUT=${spv} -DOUTPUT=${cpp} -DKEY=${source} -P "${VALO_EMBED_SPIRV_SCRIPT}"
            DEPENDS ${depends}
            COMMENT "Embedding ${name}"
            VERBATIM
        )

        target_sources(${target} PRIVATE "${cpp}")
    endforeach()
endfunction()
