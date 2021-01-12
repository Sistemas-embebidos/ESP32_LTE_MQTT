# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(SRCS ${app_sources})
list(APPEND EXTRA_COMPONENT_DIRS "esp_lte_modem/components/modem")
set(IDF_EXTRA_COMPONENT_DIRS ${EXTRA_COMPONENT_DIRS})