cmake_minimum_required(VERSION 2.8)
project(NXWEB)

SET (CMAKE_C_FLAGS    "-std=c99 -march=x86-64 -m64 -Winline")
SET (CMAKE_CXX_FLAGS  "-fopenmp -std=c++1y -march=x86-64 -m64 -fno-omit-frame-pointer -fPIC -I/usr/local/include -Wall -Wextra -Woverloaded-virtual -Wno-unused-local-typedefs")
set (CMAKE_C_FLAGS          "${CMAKE_C_FLAGS}" CACHE STRING "c flags")
set (CMAKE_CXX_FLAGS        "${CMAKE_CXX_FLAGS}" CACHE STRING "c++ flags")

SET (CMAKE_EXPORT_COMPILE_COMMANDS 1)
set (CMAKE_EXPORT_COMPILE_COMMANDS "${CMAKE_EXPORT_COMPILE_COMMANDS}" CACHE STRING "export compile_commands.json")

IF(NOT CMAKE_BUILD_TYPE)
  SET(CMAKE_BUILD_TYPE Release CACHE STRING
      "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel."
      FORCE)
ENDIF(NOT CMAKE_BUILD_TYPE)

set (CMAKE_BUILD_TYPE "${CMAKE_BUILD_TYPE}" CACHE STRING "build type")
SET (CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

find_package(Boost COMPONENTS system REQUIRED)
find_package(GFlags REQUIRED)
find_package(Glog REQUIRED)
find_package(Gperftools)
find_library(NXWEB_LIBRARY NAMES libnxweb.a nxweb)

include_directories(
  ${Boost_INCLUDE_DIR}
  ${NXWEB_SOURCE_DIR}
  ${GFLAGS_INCLUDE_DIRS}
  ${GLOG_INCLUDE_DIRS}
)

add_library(handler handler.cpp)
target_link_libraries(handler
  ${GFLAGS_LIBRARIES}
  ${GLOG_LIBRARIES}
)

add_executable(main main.cpp config.c)
target_link_libraries(main
  handler
  ${Boost_LIBRARIES}
  ${GPERFTOOLS_TCMALLOC_AND_PROFILER}
  ${NXWEB_LIBRARY}
  # ${ZLIB_LIBRARIES}
  dl
)

