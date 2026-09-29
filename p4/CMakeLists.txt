# The foc312 remote on Waveshare ESP32-P4 touch boards (notes/p4-remote.md). Board: the 5" (ESP32-P4-WIFI6-Touch-LCD-5).
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
add_compile_options(-Wno-format)
project(foc312_p4)
