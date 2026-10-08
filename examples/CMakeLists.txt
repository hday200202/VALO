# Each .cpp directly in examples/ becomes its own executable in examples/bin/.
file(GLOB VALO_EXAMPLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

foreach(src IN LISTS VALO_EXAMPLE_SOURCES)
    get_filename_component(name "${src}" NAME_WE)
    add_executable(${name} "${src}")
    target_link_libraries(${name} PRIVATE VALO::VALO)
    target_compile_options(${name} PRIVATE
        $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall -Wextra -Wpedantic -Wno-missing-field-initializers>
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
    )
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
        # Dynamic builds: let the example find lib/libVALO.dylib at runtime
        BUILD_RPATH "${VALO_LIB_DIR}"
    )
endforeach()

# compiled in, so clipstack --vulkan needs no shader files, glslc or valoc at runtime
if(VALO_HAS_VULKAN)
    valo_embed_shaders(clipstack SHADERS shaders/roundrect.vert.hpp shaders/roundrect.frag.hpp)
endif()
