# cmake -DINPUT=<file.spv> -DOUTPUT=<file.cpp> -DKEY=<path createProgram is given>
#       -DSYMBOL=<registration function name> -P EmbedSpirv.cmake
#
# Turns one SPIR-V binary into a .cpp that registers it with valo::registerSpirv,
# so the Vulkan backend can create the program without touching the disk.
#
# Registration happens two ways. A static initializer does it for an executable,
# whose objects are always linked. A static library's objects are only linked
# when something references them, so the file also defines SYMBOL - an ordinary
# function the per-target index (see ValoEmbedShaders.cmake) calls, which is
# what pulls this object in. Either path registers once.

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

} // namespace

void ${SYMBOL}() {
    [[maybe_unused]] static const bool registered =
        ::valo::registerSpirv(\"${KEY}\", SPIRV, sizeof(SPIRV) / sizeof(SPIRV[0]));
}

namespace {

[[maybe_unused]] const bool autoRegistered = (${SYMBOL}(), true);

} // namespace
")
