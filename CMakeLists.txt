cmake_minimum_required(VERSION 3.22.1)
project(fam)

option(FAM_BUILD_EXAMPLES "Build examples" OFF)
option(FAM_BUILD_TESTS "Build tests" OFF)

# NOTE: Static unless BUILD_SHARED_LIBS is on. A shared build needs FAM_SHARED for everyone
# including the headers, and FAM_BUILD only while compiling fam itself
add_library(fam src/apu.c src/player.c src/stream.c)
target_include_directories(fam PUBLIC include PRIVATE internal)
target_compile_definitions(fam PRIVATE FAM_BUILD)
target_compile_features(fam PUBLIC c_std_11)
set_target_properties(fam PROPERTIES C_VISIBILITY_PRESET hidden)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(fam PUBLIC FAM_SHARED)
endif()

if(FAM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(FAM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
