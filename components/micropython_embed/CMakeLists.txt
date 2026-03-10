# MicroPython Embed Component for ESP-IDF
#
# Before building, generate the MicroPython embed files:
#   ./scripts/build_micropython_embed.sh
#
# See: https://docs.micropython.org/en/latest/develop/embed.html

set(MP_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated")

if(EXISTS "${MP_GENERATED_DIR}/micropython_embed.c")
    idf_component_register(
        SRCS "${MP_GENERATED_DIR}/micropython_embed.c"
        INCLUDE_DIRS "${MP_GENERATED_DIR}" "port"
    )
    # Suppress warnings in generated MicroPython code
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-all -Wno-extra)
else()
    # Register component with headers only so CMake config succeeds.
    # Linking will fail until the embed files are generated.
    idf_component_register(INCLUDE_DIRS "port")
    message(WARNING
        "MicroPython embed files not found in ${MP_GENERATED_DIR}.\n"
        "Run: ./scripts/build_micropython_embed.sh\n"
        "Then rebuild with: idf.py build")
endif()
