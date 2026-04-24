cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xteink-writer-firmware)
add_compile_options(-Wno-error=int-in-bool-context)
