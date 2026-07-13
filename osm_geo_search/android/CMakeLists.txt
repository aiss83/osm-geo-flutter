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
