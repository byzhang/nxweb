SET (CMAKE_CXX_FLAGS  "-fopenmp -std=c++1y -march=x86-64 -m64 -fno-omit-frame-pointer -fPIC -I/usr/local/include -Wall -Wextra -Woverloaded-virtual -Wno-unused-local-typedefs")
set (CMAKE_CXX_FLAGS        "${CMAKE_CXX_FLAGS}" CACHE STRING "c++ flags")

SET (CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)

find_package(BLAZE REQUIRED)
find_package(Boost COMPONENTS system REQUIRED)
find_package(GFlags REQUIRED)
find_package(Glog REQUIRED)
find_package(Gperftools)

include_directories(
  ${BLAZE_INCLUDE_DIR}
  ${Boost_INCLUDE_DIR}
  ${GFLAGS_INCLUDE_DIRS}
  ${GLOG_INCLUDE_DIRS}
  ${PROJECT_SOURCE_DIR}/src/include
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
  ${PROJECT_BINARY_DIR}/src/lib/libnxweb.a
  ${EXTRA_LIBS}
  dl
)

