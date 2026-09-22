# Rollout Fabric - build, test, install and packaging rules.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Everything here is target based: no directory-global flags are set, so the
# library, the tools, the benchmarks and the tests each carry exactly the
# options they need and nothing used for the build can leak into the installed
# package.

cmake_minimum_required(VERSION 3.20)

project(RolloutFabric
        VERSION 1.0.0
        DESCRIPTION "Vendor-neutral rollout orchestration runtime for the Summon Software Labs Fabric OS"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(BUILD_SHARED_LIBS "Build rollout_fabric as a shared library instead of a static one" OFF)

option(RF_BUILD_TESTS "Build the Rollout Fabric test suite" ON)
option(RF_BUILD_TOOLS "Build the Rollout Fabric command line tools" ON)
option(RF_BUILD_EXAMPLES "Build the Rollout Fabric example programs" ON)
option(RF_BUILD_BENCHMARKS "Build the Rollout Fabric benchmarks" ON)
option(RF_WARNINGS_AS_ERRORS "Treat every compiler warning as an error" ON)
option(RF_SANITIZERS "Build with AddressSanitizer, and UndefinedBehaviorSanitizer on GCC and Clang" OFF)
option(RF_ANALYZE "Build with the MSVC static analyzer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CheckCXXCompilerFlag)
include(CTest)

set(RF_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/RolloutFabric"
    CACHE STRING "Install destination of the Rollout Fabric CMake package files")

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

function(rf_configure_target target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
    if(RF_WARNINGS_AS_ERRORS AND NOT RF_ANALYZE)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:/RTC1>)
    target_compile_definitions(${target} PRIVATE $<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=2>)
    target_compile_options(${target} PRIVATE
      $<$<CONFIG:Release>:/O2>
      $<$<CONFIG:Release>:/Gy>
      $<$<CONFIG:Release>:/Gw>)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    if(RF_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(MSVC AND RF_ANALYZE)
    target_compile_options(${target} PRIVATE /analyze /analyze:WX-)
  endif()
  if(RF_SANITIZERS)
    target_compile_options(${target} PRIVATE ${RF_SANITIZER_COMPILE_OPTIONS})
    target_link_options(${target} PRIVATE ${RF_SANITIZER_LINK_OPTIONS})
  endif()
endfunction()

function(rf_add_executable target)
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE rollout_fabric)
  rf_configure_target(${target})
endfunction()

# ---------------------------------------------------------------------------
# Sanitizers
# ---------------------------------------------------------------------------

if(RF_SANITIZERS)
  if(MSVC)
    check_cxx_compiler_flag("/fsanitize=address" RF_COMPILER_HAS_ASAN)
    if(NOT RF_COMPILER_HAS_ASAN)
      message(FATAL_ERROR
        "RF_SANITIZERS=ON cannot be used with ${CMAKE_CXX_COMPILER_ID} "
        "${CMAKE_CXX_COMPILER_VERSION}: a program built with /fsanitize=address does not "
        "compile and link here. Either the toolset predates AddressSanitizer support or the "
        "optional C++ AddressSanitizer component is not installed. Reconfigure with "
        "-DRF_SANITIZERS=OFF or install that component.")
    endif()
    set(RF_SANITIZER_COMPILE_OPTIONS /fsanitize=address /Zi)
    set(RF_SANITIZER_LINK_OPTIONS /fsanitize=address)
    message(STATUS "RolloutFabric: AddressSanitizer enabled (MSVC)")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    check_cxx_compiler_flag("-fsanitize=address,undefined" RF_COMPILER_HAS_ASAN_UBSAN)
    if(NOT RF_COMPILER_HAS_ASAN_UBSAN)
      message(FATAL_ERROR
        "RF_SANITIZERS=ON cannot be used with ${CMAKE_CXX_COMPILER_ID} "
        "${CMAKE_CXX_COMPILER_VERSION}: the sanitizer runtime is unavailable. "
        "Reconfigure with -DRF_SANITIZERS=OFF.")
    endif()
    set(RF_SANITIZER_COMPILE_OPTIONS -fsanitize=address,undefined -fno-omit-frame-pointer)
    set(RF_SANITIZER_LINK_OPTIONS -fsanitize=address,undefined)
    message(STATUS "RolloutFabric: AddressSanitizer and UndefinedBehaviorSanitizer enabled")
  else()
    message(FATAL_ERROR
      "RF_SANITIZERS=ON is supported with MSVC (/fsanitize=address) and with GCC or Clang "
      "(-fsanitize=address,undefined); the configured compiler is "
      "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}. Reconfigure with -DRF_SANITIZERS=OFF.")
  endif()
endif()

# ---------------------------------------------------------------------------
# rollout_fabric
# ---------------------------------------------------------------------------

set(RF_SOURCES
  src/version.cpp
  src/status.cpp
  src/digest.cpp
  src/codec.cpp
  src/ids.cpp
  src/time.cpp
  src/limits.cpp
  src/target.cpp
  src/selector.cpp
  src/policy.cpp
  src/plan.cpp
  src/evidence.cpp
  src/lifecycle.cpp
  src/attempt.cpp
  src/decision.cpp
  src/rollout.cpp
  src/journal.cpp
  src/persistence.cpp
  src/transport.cpp
  src/json.cpp
  src/plan_json.cpp
  src/protocol.cpp
  src/report.cpp
  src/adapters.cpp
  src/sim_adapters.cpp
  src/process.cpp
  src/process_adapter.cpp
  src/orchestrator.cpp
  src/orchestrator_gates.cpp
  src/orchestrator_advance.cpp
  src/orchestrator_dispatch.cpp
  src/orchestrator_recovery.cpp
  src/controller.cpp
  src/worker.cpp)

add_library(rollout_fabric ${RF_SOURCES})
add_library(SummonSoftwareLabs::rollout_fabric ALIAS rollout_fabric)

target_include_directories(rollout_fabric
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(rollout_fabric PUBLIC cxx_std_20)

if(WIN32)
  # Winsock for the framed transport, and the process APIs the supervisor uses.
  # PRIVATE: none of this belongs to the public interface.
  target_link_libraries(rollout_fabric PRIVATE ws2_32)
endif()

rf_configure_target(rollout_fabric)

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------

if(RF_BUILD_TOOLS)
  rf_add_executable(rolloutd tools/rolloutd.cpp)
  rf_add_executable(rf-worker tools/rf_worker.cpp)
  rf_add_executable(rollout-cli tools/rollout_cli.cpp)
  rf_add_executable(rf-inspect tools/rf_inspect.cpp)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------

if(RF_BUILD_EXAMPLES)
  file(GLOB RF_EXAMPLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp")
  foreach(example_source IN LISTS RF_EXAMPLE_SOURCES)
    get_filename_component(example_stem "${example_source}" NAME_WE)
    rf_add_executable(rf-example-${example_stem} "${example_source}")
  endforeach()
  if(NOT RF_EXAMPLE_SOURCES)
    message(STATUS "RolloutFabric: RF_BUILD_EXAMPLES is ON but examples/ has no program yet")
  endif()
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------

if(RF_BUILD_BENCHMARKS)
  rf_add_executable(rf-benchmarks benchmarks/benchmark_rollout.cpp)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(BUILD_TESTING AND RF_BUILD_TESTS)
  file(GLOB RF_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
  file(GLOB RF_TEST_SUPPORT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/support/*.cpp")

  if(RF_TEST_SUPPORT_SOURCES)
    add_library(rollout_fabric_test_support STATIC ${RF_TEST_SUPPORT_SOURCES})
    set_target_properties(rollout_fabric_test_support PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(rollout_fabric_test_support PUBLIC rollout_fabric)
    target_include_directories(rollout_fabric_test_support PUBLIC
      "${CMAKE_CURRENT_SOURCE_DIR}/tests"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/support")
    rf_configure_target(rollout_fabric_test_support)
  endif()

  foreach(test_source IN LISTS RF_TEST_SOURCES)
    get_filename_component(test_stem "${test_source}" NAME_WE)
    string(REGEX REPLACE "^test_" "" test_suffix "${test_stem}")
    set(test_target "rf-test-${test_suffix}")

    rf_add_executable(${test_target} "${test_source}")
    if(TARGET rollout_fabric_test_support)
      target_link_libraries(${test_target} PRIVATE rollout_fabric_test_support)
    endif()

    # The multi-process suites drive the real tools, so they must exist first.
    if(TARGET rolloutd AND TARGET rf-worker)
      add_dependencies(${test_target} rolloutd rf-worker)
    endif()

    if(test_stem STREQUAL "test_multiprocess" OR test_stem STREQUAL "test_restart_recovery"
       OR test_stem STREQUAL "test_transport_process" OR test_stem STREQUAL "test_rollback")
      add_test(NAME ${test_target}
               COMMAND ${test_target} process
                       --rolloutd $<TARGET_FILE:rolloutd>
                       --worker $<TARGET_FILE:rf-worker>
                       --cli $<TARGET_FILE:rollout-cli>
                       --inspect $<TARGET_FILE:rf-inspect>)
    else()
      add_test(NAME ${test_target} COMMAND ${test_target})
    endif()
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Installation and packaging
# ---------------------------------------------------------------------------

install(TARGETS rollout_fabric
        EXPORT RolloutFabricTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT RolloutFabricTargets
        NAMESPACE SummonSoftwareLabs::
        DESTINATION ${RF_INSTALL_CMAKEDIR})

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RolloutFabricConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/RolloutFabricConfig.cmake"
  INSTALL_DESTINATION "${RF_INSTALL_CMAKEDIR}"
  PATH_VARS CMAKE_INSTALL_INCLUDEDIR)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/RolloutFabricConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/RolloutFabricConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/RolloutFabricConfigVersion.cmake"
        DESTINATION ${RF_INSTALL_CMAKEDIR})

# The build tree is usable by a sibling project without installing first.
export(EXPORT RolloutFabricTargets
       NAMESPACE SummonSoftwareLabs::
       FILE "${CMAKE_CURRENT_BINARY_DIR}/RolloutFabricTargets.cmake")
