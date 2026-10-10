cmake_minimum_required(VERSION 4.3)
project(soml-package LANGUAGES CXX)

set(SOML_INSTALL ON CACHE BOOL "Install SOML" FORCE)
add_subdirectory("${SOML_SOURCE_DIR}" soml)
