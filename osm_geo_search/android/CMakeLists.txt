cmake_minimum_required(VERSION 3.18.1)
project(osm_geo_search LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(osm_geo_search SHARED
  ../native/osm_geo_search.c
)

target_include_directories(osm_geo_search PRIVATE
  ../native
)

# ICU — Android NDK provides headers (v76.1) + libicu.so stub (API 31+).
# The NDK toolchain file adds sysroot/usr/include to the include path
# automatically, so the NDK's ucasemap.h (with _76 suffixed symbols)
# is used instead of homebrew's.
find_library(icu_lib icu)
target_link_libraries(osm_geo_search ${icu_lib} log)
