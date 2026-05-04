cmake_minimum_required(VERSION 3.22.1)
project(FamiLib)

option(FAMILIB_BUILD_EXAMPLES "Build examples" OFF)

if(FAMILIB_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
