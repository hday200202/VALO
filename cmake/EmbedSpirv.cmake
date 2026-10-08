# cmake -DINPUT=<file.spv> -DOUTPUT=<file.cpp> -DKEY=<path createProgram is given> -P EmbedSpirv.cmake
#
# Turns one SPIR-V binary into a .cpp that registers it with valo::registerSpirv,
# so the Vulkan backend can create the program without touching the disk.

file(READ "${INPUT}" HEX HEX)

string(LENGTH "${HEX}" HEX_LENGTH)
math(EXPR REMAINDER "${HEX_LENGTH} % 8")
if(HEX_LENGTH EQUAL 0 OR NOT REMAINDER EQUAL 0)
    message(FATAL_ERROR "${INPUT} is not a whole number of 32-bit words")
endif()

# SPIR-V is little-endian on disk, so each word's bytes come out reversed
string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1u, " WORDS "${HEX}")
set(WORD "0x........u, ")
string(REGEX REPLACE "(${WORD}${WORD}${WORD}${WORD}${WORD}${WORD}${WORD}${WORD})" "\\1\n    " WORDS "${WORDS}")
string(REPLACE ", \n" ",\n" WORDS "${WORDS}")
string(REGEX REPLACE "[ \n]+$" "" WORDS "${WORDS}")

file(TO_CMAKE_PATH "${KEY}" KEY)

file(WRITE "${OUTPUT}"
"// generated from ${INPUT} - do not edit
#include <render/SpirvRegistry.hpp>

namespace {

const uint32_t SPIRV[] = {
    ${WORDS}
};

VALO_EMBED_SPIRV(SPIRV, \"${KEY}\")

} // namespace
")
