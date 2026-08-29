cmake_minimum_required(VERSION 3.22.1)
project(fam)

option(FAM_BUILD_EXAMPLES "Build examples" OFF)
option(FAM_BUILD_TESTS "Build tests" OFF)
option(FAM_BUILD_CONVERTERS "Build format converters" ON)

# The runtime library: everything needed to load and play .fam data.
add_library(fam STATIC src/apu.c src/player.c src/io.c)
target_include_directories(fam PUBLIC include PRIVATE internal)

# Format converters, for the content authoring / cooking phase. Kept in a separate target so a
# game shipping fam at runtime links only the player and never pulls in converter code.
if(FAM_BUILD_CONVERTERS)
    add_library(fam_convert STATIC convert/vgm.c)
    # `internal` is PRIVATE on fam, so it does not propagate through the link; the PUBLIC
    # `include` dir does.
    target_include_directories(fam_convert PRIVATE internal)
    target_link_libraries(fam_convert PUBLIC fam)
endif()

if(FAM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(FAM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
