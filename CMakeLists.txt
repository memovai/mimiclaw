# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mimiclaw)

# Pre-flash a valid SPIFFS image so first boot does not need runtime formatting.
spiffs_create_partition_image(spiffs spiffs_data FLASH_IN_PROJECT)
