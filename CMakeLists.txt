cmake_minimum_required(VERSION 3.20)
project(xmux LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "x64 build required. Please set CMake generator to 64-bit.")
endif()

set(CMAKE_BUILD_PARALLEL_LEVEL 10)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(BIN_DIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# === Collect source files ===
file(GLOB_RECURSE xmux_SRC CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/*.cpp
)

# === Define binary ===
add_executable(xmux ${xmux_SRC})

target_include_directories(xmux PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

if(UNIX)
	set(CLEAR_COMMAND clear)
elseif(WIN32)
	set(CLEAR_COMMAND cls)
endif()

add_custom_command(TARGET xmux POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BIN_DIR}"
	COMMAND ${CLEAR_COMMAND}
    
    VERBATIM
)

add_custom_target(run_xmux ALL
	COMMAND ${CLEAR_COMMAND}
    COMMAND ${CMAKE_COMMAND} -E cmake_echo_color --yellow "Binary built"
)
