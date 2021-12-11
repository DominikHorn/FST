cmake_minimum_required(VERSION 3.19)
project(mmphf_fst)

# ==== Dependencies ====
find_package(Threads REQUIRED)
set(THREADS_PREFER_PTHREAD_FLAG ON)

# ==== Library definition ====
add_library(${PROJECT_NAME} INTERFACE)
target_include_directories(${PROJECT_NAME} INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_compile_features(${PROJECT_NAME} INTERFACE cxx_std_17)
target_compile_options(${PROJECT_NAME} INTERFACE -mpopcnt -pthread)

# Make IDE friendly
target_sources(${PROJECT_NAME} INTERFACE mmphf_fst.hpp include/)

# Test code
get_directory_property(hasParent PARENT_DIRECTORY)
if (NOT hasParent)
  add_subdirectory(src)
endif()
