FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(SRCS ${app_sources})

# Linker wrap overrides — these must be here rather than in platformio.ini
# build_flags because PlatformIO does not propagate -Wl,--wrap to the
# ESP-IDF CMake link step.
target_link_options(${COMPONENT_LIB} INTERFACE
    -Wl,--wrap=bootloader_common_check_efuse_blk_validity
    -Wl,--wrap=panic_abort
    -Wl,--wrap=panic_print_backtrace
)
