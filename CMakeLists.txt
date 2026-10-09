cmake_minimum_required(VERSION 3.22)

project(Diagnostics VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)

add_library(diagnostics STATIC src/Diagnostics.cpp)
add_library(Diagnostics::Diagnostics ALIAS diagnostics)
set_target_properties(diagnostics PROPERTIES
    EXPORT_NAME Diagnostics
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(diagnostics PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(diagnostics PUBLIC cxx_std_17)
target_compile_options(diagnostics PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(diagnostics PRIVATE Threads::Threads)

# The viewer is optional; importing the C++ library does not require Python.
find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_custom_target(diagnostics_viewer
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Tools/diagnostics_viewer.py"
        USES_TERMINAL
        COMMENT "Starting the Diagnostics viewer (Ctrl+C to stop)"
        VERBATIM
    )
endif()

option(DIAGNOSTICS_BUILD_TESTS "Build the diagnostics module tests" OFF)
if(DIAGNOSTICS_BUILD_TESTS)
    enable_testing()
    add_executable(diagnostics_tests tests/DiagnosticsTests.cpp)
    target_link_libraries(diagnostics_tests PRIVATE Diagnostics::Diagnostics)
    add_test(NAME diagnostics_tests COMMAND diagnostics_tests)
    if(Python3_Interpreter_FOUND)
        add_test(NAME diagnostics_viewer_tests
            COMMAND "${Python3_EXECUTABLE}" -m unittest discover
                -s "${CMAKE_CURRENT_SOURCE_DIR}/Tools" -p test_diagnostics_viewer.py
        )
    endif()
endif()

set(diagnostics_package_directory "${CMAKE_INSTALL_LIBDIR}/cmake/Diagnostics")
configure_package_config_file(cmake/DiagnosticsConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/DiagnosticsConfig.cmake"
    INSTALL_DESTINATION "${diagnostics_package_directory}"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/DiagnosticsConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)
install(TARGETS diagnostics EXPORT DiagnosticsTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT DiagnosticsTargets NAMESPACE Diagnostics::
    DESTINATION "${diagnostics_package_directory}"
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/DiagnosticsConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/DiagnosticsConfigVersion.cmake"
    DESTINATION "${diagnostics_package_directory}"
)
install(FILES Tools/diagnostics_viewer.py Tools/diagnostics_viewer.html
    DESTINATION "${CMAKE_INSTALL_DATADIR}/diagnostics/Tools"
)
