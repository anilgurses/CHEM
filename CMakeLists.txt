cmake_minimum_required(VERSION 3.12)

find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(GIT_TAG)
        string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" ACHEM_VERSION ${GIT_TAG})
    else()
        set(ACHEM_VERSION "0.0.0")
    endif()
else()
    set(ACHEM_VERSION "0.0.0")
endif()

project(
  "CHEM"
  VERSION ${ACHEM_VERSION}
  LANGUAGES C CXX)

add_compile_definitions(ACHEM_VERSION="${ACHEM_VERSION}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include(cmake/StandardSettings.cmake)
include(cmake/StaticAnalyzers.cmake)
include(cmake/Utils.cmake)
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

#TODO: Not needed anymore?
set(Boost_USE_MULTITHREADED ON)

option(CHEM_VM "Build for CHEM-VM" OFF)

option(ENABLE_DEBUG_LOG "Enable CHEM debug log" OFF)
option(ENABLE_PROFILING "Enable CHEM profiling" OFF)

if(ENABLE_DEBUG_LOG)
    add_definitions(-DENABLE_DEBUG_LOG)
    # Sometimes we need to debug the code without reducing the optimization level
    # set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ggdb -fno-omit-frame-pointer -fsanitize=address")
endif(ENABLE_DEBUG_LOG)

if(ENABLE_PROFILING)
    add_definitions(-DENABLE_PROFILING)
    message(STATUS "Profiling enabled")
endif(ENABLE_PROFILING)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "DEBUG")
endif()

message(STATUS "Started CMake for ${PROJECT_NAME} v${ACHEM_VERSION}...\n")

if(CMAKE_BUILD_TYPE STREQUAL "DEBUG")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ggdb -fno-omit-frame-pointer")
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w -O3 -ffast-math")

## SIMD
find_package(Intrinsics)
if(HAVE_AVX512)
  set(CMAKE_CXX_FLAGS
      "${CMAKE_CXX_FLAGS} -mavx512f -DLV_HAVE_AVX512"
  )
  message(STATUS "AVX512 Enabled")
elseif(HAVE_AVX2)
  set(CMAKE_CXX_FLAGS
      "${CMAKE_CXX_FLAGS} -mfpmath=sse -mavx2 -DLV_HAVE_AVX2"
  )
  message(STATUS "AVX2 Enabled")
elseif(HAVE_SSE)
  set(CMAKE_CXX_FLAGS
      "${CMAKE_CXX_FLAGS} -msse4.1 -DLV_HAVE_SSE")
  message(STATUS "SSE Enabled")
endif(HAVE_AVX2)

if(HAVE_FMA)
  set(CMAKE_CXX_FLAGS
      "${CMAKE_CXX_FLAGS} -mfma -DLV_HAVE_FMA"
  )
  message(STATUS "FMA Enabled")
endif(HAVE_FMA)


if(${PROJECT_NAME}_USE_ALT_NAMES)
  string(TOLOWER ${PROJECT_NAME} PROJECT_NAME_LOWERCASE)
  string(TOUPPER ${PROJECT_NAME} PROJECT_NAME_UPPERCASE)
else()
  set(PROJECT_NAME_LOWERCASE ${PROJECT_NAME})
  set(PROJECT_NAME_UPPERCASE ${PROJECT_NAME})
endif()

#
# Prevent building in the source directory
#

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(
    FATAL_ERROR
      "In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there.\n"
  )
endif()

include(cmake/SourcesAndHeaders.cmake)
# add_subdirectory(lib/SQLiteCpp)

# Find all headers and implementation files
add_executable(${PROJECT_NAME} ${exe_sources})

if(VERBOSE_OUTPUT)
    verbose_message("Found the following sources:")
    foreach(source IN LISTS exe_sources)
      verbose_message("* ${source}")
    endforeach()
endif()

if(${PROJECT_NAME}_ENABLE_UNIT_TESTING)
    add_library(${PROJECT_NAME}_LIB ${headers} ${sources})

    if(VERBOSE_OUTPUT)
      verbose_message("Found the following headers:")
      foreach(header IN LISTS headers)
        verbose_message("* ${header}")
      endforeach()
    endif()
endif()

set_target_properties(
  ${PROJECT_NAME}
  PROPERTIES ARCHIVE_OUTPUT_DIRECTORY
             "${CMAKE_BINARY_DIR}/lib/${CMAKE_BUILD_TYPE}"
             LIBRARY_OUTPUT_DIRECTORY
             "${CMAKE_BINARY_DIR}/lib/${CMAKE_BUILD_TYPE}"
             RUNTIME_OUTPUT_DIRECTORY
             "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}")

if(${PROJECT_NAME}_ENABLE_UNIT_TESTING)
  set_target_properties(
    ${PROJECT_NAME}_LIB
    PROPERTIES ARCHIVE_OUTPUT_DIRECTORY
               "${CMAKE_BINARY_DIR}/lib/${CMAKE_BUILD_TYPE}"
               LIBRARY_OUTPUT_DIRECTORY
               "${CMAKE_BINARY_DIR}/lib/${CMAKE_BUILD_TYPE}"
               OUTPUT_NAME ${PROJECT_NAME})
endif()

message(STATUS "Added all header and implementation files.\n")

include(CheckCXXCompilerFlag)

include(cmake/CompilerWarnings.cmake)
set_project_warnings(${PROJECT_NAME})

verbose_message(
  "Applied compiler warnings. Using standard ${CMAKE_CXX_STANDARD}.\n")

include(cmake/Doxygen.cmake)
include(cmake/Libs.cmake)

target_link_libraries(${PROJECT_NAME} PRIVATE ${privatelibs})

if(${PROJECT_NAME}_ENABLE_UNIT_TESTING)
  target_link_libraries(${PROJECT_NAME}_LIB PUBLIC ${privatelibs})
endif()

verbose_message("Successfully added all dependencies and linked against them.")

target_include_directories(
    ${PROJECT_NAME}
    PUBLIC include
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${chem_extension_include_dirs}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(${PROJECT_NAME}_ENABLE_UNIT_TESTING)
    target_include_directories(
      ${PROJECT_NAME}_LIB
      PUBLIC include
             ${CMAKE_CURRENT_SOURCE_DIR}/include
             ${chem_extension_include_dirs}
      PRIVATE
             ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()

message(STATUS "Finished setting up include directories.")

add_executable(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})

verbose_message("Project is now aliased as ${PROJECT_NAME}::${PROJECT_NAME}.\n")

#
# Benchmark executable
#
option(ENABLE_BENCHMARK "Build the achem_benchmark tool" OFF)
if(ENABLE_BENCHMARK)
  add_definitions(-DENABLE_BENCHMARK)
  message(STATUS "Benchmark timing instrumentation enabled")

  set(BENCHMARK_NAME "achem_benchmark")
  add_executable(${BENCHMARK_NAME}
    src/chem/benchmark/achem_benchmark.cpp
    ${sources}
  )

  target_link_libraries(${BENCHMARK_NAME} PRIVATE ${privatelibs})

  target_include_directories(
    ${BENCHMARK_NAME}
    PUBLIC include
           ${CMAKE_CURRENT_SOURCE_DIR}/include
           ${chem_extension_include_dirs}
    PRIVATE
           ${CMAKE_CURRENT_SOURCE_DIR}/src)

  set_target_properties(
    ${BENCHMARK_NAME}
    PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}")

  target_compile_definitions(${BENCHMARK_NAME} PRIVATE ENABLE_DEBUG_LOG ENABLE_PROFILING)
  
  install(
    TARGETS ${BENCHMARK_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  
  message(STATUS "Benchmark tool '${BENCHMARK_NAME}' will be built.")
endif()

# Format the project using the `clang-format` target (i.e: cmake --build build
# --target clang-format)
#

add_clang_format_target()

# Install library for easy downstream inclusion
#

include(GNUInstallDirs)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

option(INSTALL_PYCHEM "Install pychem module into Python site-packages" ON)
set(CHEM_PYTHON_SITELIB "${Python3_SITELIB}")
if(NOT CHEM_PYTHON_SITELIB)
  execute_process(
    COMMAND ${Python3_EXECUTABLE} -c "import sysconfig; print(sysconfig.get_path('purelib'))"
    OUTPUT_VARIABLE CHEM_PYTHON_SITELIB
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
endif()

set(CHEM_DEFAULT_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/configs/config.json")
install(
  TARGETS ${PROJECT_NAME}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(
  PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/utils/pychem.py
  DESTINATION ${CMAKE_INSTALL_BINDIR}
  RENAME pychem)

if(INSTALL_PYCHEM)
  install(
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/utils/pychem.py
    DESTINATION ${CHEM_PYTHON_SITELIB}/pychem
    RENAME __init__.py)
endif()


verbose_message(
  "Install targets successfully built."
)

install(CODE "
  set(_home \"\$ENV{HOME}\")
  if(NOT _home)
    message(WARNING \"HOME not set; skipping config install to user directory.\")
  else()
    set(_dest \"\${_home}/.config/CHEM\")
    file(MAKE_DIRECTORY \"\${_dest}\")
    file(COPY \"${CHEM_DEFAULT_CONFIG}\" DESTINATION \"\${_dest}\")
  endif()
")

message(STATUS "Finished building requirements for installing the package.\n")

# Post build steps. Installing the config file
# add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD COMMAND /bin/sh
# ${PROJECT_SOURCE_DIR}/create_config.sh ${PROJECT_SOURCE_DIR} ) endif()

# CPack configuration for Debian package generation
set(CPACK_PACKAGE_NAME "chem")
set(CPACK_PACKAGE_VENDOR "AERPAW")
set(CPACK_PACKAGE_CONTACT "agurses@ncsu.edu")
set(CPACK_PACKAGE_VERSION ${ACHEM_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "AERPAW Channel Emulator (CHEM)")
set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Anil Gurses")
set(
  CPACK_DEBIAN_PACKAGE_DEPENDS
  "libuhd-dev, libboost-system-dev, libboost-thread-dev, libboost-regex-dev, libsqlite3-0, libvolk2-dev, libmavsdk, libtbb-dev"
)
include(CPack)

#
# Unit testing setup
#

if(${PROJECT_NAME}_ENABLE_UNIT_TESTING)
  enable_testing()
  message(
    STATUS
      "Build unit tests for the project. Tests should always be found in the test folder\n"
  )
  add_subdirectory(test)
endif()
