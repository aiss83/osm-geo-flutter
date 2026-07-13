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

# ICU — headers from homebrew, library from Android NDK
target_include_directories(osm_geo_search PRIVATE
  /opt/homebrew/opt/icu4c@78/include
)

# NDK provides a unified libicu.so stub (API 31+).
find_library(icu_lib icu)
target_link_libraries(osm_geo_search ${icu_lib} log)
