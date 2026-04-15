#
# Print a message only if the `VERBOSE_OUTPUT` option is on
#

function(verbose_message content)
    if(${PROJECT_NAME}_VERBOSE_OUTPUT)
			message(STATUS ${content})
    endif()
endfunction()

#
# Add a target for formating the project using `clang-format` (i.e: cmake --build build --target clang-format)
#

function(add_clang_format_target)
    if(NOT ${PROJECT_NAME}_CLANG_FORMAT_BINARY)
			find_program(${PROJECT_NAME}_CLANG_FORMAT_BINARY clang-format)
    endif()

    if(${PROJECT_NAME}_CLANG_FORMAT_BINARY)
			file(GLOB_RECURSE FORMAT_SOURCES
				"${CMAKE_SOURCE_DIR}/src/*.cpp"
				"${CMAKE_SOURCE_DIR}/src/*.hpp"
				"${CMAKE_SOURCE_DIR}/src/*.h"
				"${CMAKE_SOURCE_DIR}/include/chem/*.cpp"
				"${CMAKE_SOURCE_DIR}/include/chem/*.hpp"
				"${CMAKE_SOURCE_DIR}/include/chem/*.h"
				"${CMAKE_SOURCE_DIR}/test/*.cpp"
				"${CMAKE_SOURCE_DIR}/test/*.hpp"
				"${CMAKE_SOURCE_DIR}/test/*.h"
			)
			list(FILTER FORMAT_SOURCES EXCLUDE REGEX ".*nlohmann.*")
			list(FILTER FORMAT_SOURCES EXCLUDE REGEX ".*generated.*")
			list(FILTER FORMAT_SOURCES EXCLUDE REGEX ".*spdlog.*")
			add_custom_target(clang-format
					COMMAND ${${PROJECT_NAME}_CLANG_FORMAT_BINARY}
					-i ${FORMAT_SOURCES}
					WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

			message(STATUS "Format the project using the `clang-format` target (i.e: cmake --build build --target clang-format).\n")
    endif()
endfunction()
