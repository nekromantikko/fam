cmake_minimum_required(VERSION 3.22.1)
project(fam)

option(FAM_BUILD_EXAMPLES "Build examples" OFF)
option(FAM_BUILD_TESTS "Build tests" OFF)

add_library(fam STATIC src/apu.c src/player.c)
target_include_directories(fam PUBLIC include PRIVATE internal)

if(FAM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(FAM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
