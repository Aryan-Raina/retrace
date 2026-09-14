cmake_minimum_required(VERSION 3.20)

project(retrace
        VERSION 0.1.0
        DESCRIPTION "Restricted deterministic record-replay debugger for single-threaded x86-64 Linux programs"
        LANGUAGES CXX C)  # C is only for the test fixtures

# retrace is built directly on ptrace(2) and on x86-64 register layout.
# There is no portability layer and none is planned, so fail loudly at
# configure time rather than producing confusing compile errors later.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "retrace targets Linux only (found: ${CMAKE_SYSTEM_NAME})")
endif()
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(FATAL_ERROR "retrace targets x86-64 only (found: ${CMAKE_SYSTEM_PROCESSOR})")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(RETRACE_WERROR "Treat compiler warnings as errors" ON)

# One place to define the warning set, so the binary and the tests are held to
# the same standard.
add_library(retrace_warnings INTERFACE)
target_compile_options(retrace_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    $<$<BOOL:${RETRACE_WERROR}>:-Werror>
)

add_executable(retrace
    src/breakpoint.cpp
    src/log.cpp
    src/main.cpp
    src/process.cpp
    src/replay.cpp
    src/syscalls.cpp
    src/trace.cpp
)
target_link_libraries(retrace PRIVATE retrace_warnings)

include(GNUInstallDirs)
install(TARGETS retrace RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

enable_testing()

# Target programs for the acceptance tests. Built plainly: they are meant to
# misbehave in specific ways, not to be exemplary code.
set(RETRACE_FIXTURE_DIR ${CMAKE_BINARY_DIR}/fixtures)
foreach(fixture exit_ok exit_code crash echo_args catch_usr1 reader)
    add_executable(fixture_${fixture} tests/fixtures/${fixture}.c)
    set_target_properties(fixture_${fixture} PROPERTIES
        OUTPUT_NAME ${fixture}
        RUNTIME_OUTPUT_DIRECTORY ${RETRACE_FIXTURE_DIR})
endforeach()

# Built -no-pie so the address nm reports is the address it runs at, which is
# what --break takes.
add_executable(fixture_counter tests/fixtures/counter.c)
set_target_properties(fixture_counter PROPERTIES
    OUTPUT_NAME counter
    RUNTIME_OUTPUT_DIRECTORY ${RETRACE_FIXTURE_DIR}
    POSITION_INDEPENDENT_CODE OFF
    LINK_FLAGS "-no-pie")

add_test(NAME acceptance
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_acceptance.sh
                 $<TARGET_FILE:retrace> ${RETRACE_FIXTURE_DIR})
