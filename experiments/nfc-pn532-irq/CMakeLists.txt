cmake_minimum_required(VERSION 3.16)
set(IDF_TARGET esp32s3)
# El driver vive en firmware/components/orbita_nfc (una sola copia): el banco lo usa desde ahi.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/orbita_nfc")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(orbita_nfc_lab)
