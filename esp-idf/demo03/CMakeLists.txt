cmake_minimum_required(VERSION 3.16)

# Reuse the shared components (st7789, sd_card) one level up.
set(EXTRA_COMPONENT_DIRS "../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(demo03)
