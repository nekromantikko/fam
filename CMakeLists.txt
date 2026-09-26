cmake_minimum_required(VERSION 3.22.1)
project(fam LANGUAGES C VERSION 1.0.0)

option(FAM_BUILD_EXAMPLES "Build examples" OFF)
option(FAM_BUILD_TESTS "Build tests" OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    option(FAM_INSTALL "Generate install rules for fam" ON)
else()
    option(FAM_INSTALL "Generate install rules for fam" OFF)
endif()

add_library(fam src/apu.c src/player.c src/stream.c)

include(GNUInstallDirs)

target_include_directories(fam
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/internal
)
target_compile_definitions(fam PRIVATE FAM_BUILD)
target_compile_features(fam PUBLIC c_std_11)
set_target_properties(fam PROPERTIES C_VISIBILITY_PRESET hidden)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(fam PUBLIC FAM_SHARED)
    set_target_properties(fam PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )
endif()

# Installation

if(FAM_INSTALL)
    include(CMakePackageConfigHelpers)

    install(FILES README.md LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    configure_package_config_file(cmake/FamConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/cmake/fam/FamConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fam
    )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/cmake/fam/FamConfigVersion.cmake
        COMPATIBILITY SameMajorVersion
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/cmake/fam/FamConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/cmake/fam/FamConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fam
    )

    install(EXPORT fam-targets
        NAMESPACE fam::
        FILE FamTargets.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fam
    )

    install(TARGETS fam
        EXPORT fam-targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

if(FAM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(FAM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
