# The following lines of boilerplate have to be in your project's
# CMakeLists in your exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# Local component directories (e.g. the ili9341 TFT driver).
# MUST be set before project() so IDF picks up the extra component roots.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(firmware)
