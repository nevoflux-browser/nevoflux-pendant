cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
set(PARTITION_CSV_PATH "${CMAKE_CURRENT_LIST_DIR}/../../partitions.csv")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nevoflux_pendant)
