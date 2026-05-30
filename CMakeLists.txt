cmake_minimum_required(VERSION 3.27)

project(Ufw VERSION 0.0.1 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Always emit compile_commands.json — consumed by tools/lint.py, clangd, and IDEs.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -----------------------------------------------------------------------------
# Options

option(UFW_USE_CCACHE        "Use ccache as compiler launcher"        OFF)
option(UFW_ENABLE_CLANG_TIDY "Run clang-tidy on first-party targets"  ON)
option(UFW_BUILD_PYTHON      "Build the nanobind Python control-plane module" OFF)

# Sanitizers default ON in Debug, OFF otherwise; user may override with
# -DUFW_ENABLE_SANITIZERS=ON/OFF on the configure line.
if(NOT DEFINED UFW_ENABLE_SANITIZERS)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(UFW_ENABLE_SANITIZERS ON  CACHE BOOL "Enable AddressSanitizer + UBSan")
    else()
        set(UFW_ENABLE_SANITIZERS OFF CACHE BOOL "Enable AddressSanitizer + UBSan")
    endif()
endif()

if(UFW_USE_CCACHE)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        message(STATUS "ccache enabled (${CCACHE_PROGRAM})")
        set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
        set(CMAKE_C_COMPILER_LAUNCHER   ${CCACHE_PROGRAM})
    else()
        message(WARNING "UFW_USE_CCACHE=ON but ccache not found in PATH")
    endif()
endif()

# clang-tidy serves two consumers: the per-target integration below
# (UFW_ENABLE_CLANG_TIDY → CXX_CLANG_TIDY on first-party targets) and the
# standalone `tidy` / `tidy-fix` targets, which also lint template-only headers
# that a .cpp TU never instantiates. Find it unconditionally for both.
find_program(CLANG_TIDY_PROGRAM NAMES clang-tidy)

set(UFW_CLANG_TIDY_COMMAND "")
if(UFW_ENABLE_CLANG_TIDY)
    if(CLANG_TIDY_PROGRAM)
        message(STATUS "clang-tidy enabled (${CLANG_TIDY_PROGRAM})")
        set(UFW_CLANG_TIDY_COMMAND "${CLANG_TIDY_PROGRAM}")
    else()
        message(STATUS "clang-tidy not found, skipping per-target integration")
    endif()
endif()

# Standalone lint harness — single source of truth in tools/lint.py.
find_program(PYTHON3_PROGRAM NAMES python3 python)
if(CLANG_TIDY_PROGRAM AND PYTHON3_PROGRAM)
    add_custom_target(tidy
        COMMAND ${PYTHON3_PROGRAM} ${PROJECT_SOURCE_DIR}/tools/lint.py
                --build-dir ${PROJECT_BINARY_DIR} --clang-tidy ${CLANG_TIDY_PROGRAM}
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} USES_TERMINAL
        COMMENT "clang-tidy: first-party TUs + standalone headers")
    add_custom_target(tidy-fix
        COMMAND ${PYTHON3_PROGRAM} ${PROJECT_SOURCE_DIR}/tools/lint.py
                --build-dir ${PROJECT_BINARY_DIR} --clang-tidy ${CLANG_TIDY_PROGRAM} --fix
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} USES_TERMINAL
        COMMENT "clang-tidy --fix: first-party TUs + standalone headers")
endif()

# -----------------------------------------------------------------------------
# Interface libraries: warnings (first-party only) and sanitizers (uniform
# across launcher + plugins, required by the dlopen plugin model).

add_library(ufw_warnings INTERFACE)
add_library(ufw::warnings ALIAS ufw_warnings)
if(NOT WIN32)
    target_compile_options(ufw_warnings INTERFACE
        -pedantic -Wall -Wextra -Werror)
endif()

add_library(ufw_sanitizers INTERFACE)
add_library(ufw::sanitizers ALIAS ufw_sanitizers)
if(UFW_ENABLE_SANITIZERS)
    message(STATUS "sanitizers enabled (asan + ubsan)")
    target_compile_options(ufw_sanitizers INTERFACE
        -fsanitize=address -fsanitize=undefined)
    target_link_options(ufw_sanitizers INTERFACE
        -fsanitize=address -fsanitize=undefined)
endif()

# -----------------------------------------------------------------------------
# Install layout

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${PROJECT_BINARY_DIR}/install"
        CACHE PATH "Project installation prefix" FORCE)
endif()

set(INSTALL_LIB_DIR     ${CMAKE_INSTALL_LIBDIR})
set(INSTALL_BIN_DIR     ${CMAKE_INSTALL_BINDIR})
set(INSTALL_INCLUDE_DIR ${CMAKE_INSTALL_INCLUDEDIR})
if(WIN32 AND NOT CYGWIN)
    set(INSTALL_CMAKE_DIR CMake)
else()
    set(INSTALL_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/Ufw)
endif()

set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
if(UNIX AND NOT APPLE)
    set(CMAKE_INSTALL_RPATH "$ORIGIN/../${INSTALL_LIB_DIR}")
endif()

# -----------------------------------------------------------------------------
# Subprojects

include_directories("${PROJECT_SOURCE_DIR}" "${PROJECT_BINARY_DIR}")

enable_testing()

add_subdirectory(ufw/topics)
add_subdirectory(ufw/app)
add_subdirectory(tests)
add_subdirectory(benchmarks)

if(UFW_BUILD_PYTHON)
    add_subdirectory(ufw/py)
endif()

# -----------------------------------------------------------------------------
# Package config / install exports

configure_package_config_file(
    cmake/UfwConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/UfwConfig.cmake
    INSTALL_DESTINATION ${INSTALL_CMAKE_DIR})

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/UfwConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/UfwConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/UfwConfigVersion.cmake
    DESTINATION ${INSTALL_CMAKE_DIR}
    COMPONENT dev)

install(EXPORT UfwTargets
    NAMESPACE ufw::
    DESTINATION ${INSTALL_CMAKE_DIR}
    COMPONENT dev)
