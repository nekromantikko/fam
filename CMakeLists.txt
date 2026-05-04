cmake_minimum_required(VERSION 3.22.1)
project(FamiLib)

option(FAMILIB_BUILD_EXAMPLES "Build examples" OFF)
option(FAMILIB_BUILD_TESTS "Build tests" OFF)

add_library(familib STATIC src/familib.c)
target_include_directories(familib PUBLIC include)

if(FAMILIB_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(FAMILIB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
