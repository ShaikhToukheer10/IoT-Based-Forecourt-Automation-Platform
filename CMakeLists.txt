cmake_minimum_required(VERSION 3.16)

# Change to "v2.0.0" when building the target firmware for patch generation
set(PROJECT_VER "v1.0.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(delta_fota)
