cmake_minimum_required(VERSION 3.16)

# Reuse the shared ST7789 driver that lives one level up (esp-idf/components/).
set(EXTRA_COMPONENT_DIRS "../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(demo02)
