# Host build of the platform-independent modules and their test.
# This is a separate project from the firmware: configure it on its own.
cmake_minimum_required(VERSION 3.16)
project(powercanary_host_test C)

set(CMAKE_C_STANDARD 11)
enable_testing()

add_executable(host_test
    host_test.c
    ../main/power_state.c
    ../main/megatec.c
    ../main/ef_proto.c)
target_include_directories(host_test PRIVATE ../main)
if(MSVC)
    target_compile_options(host_test PRIVATE /W4)
else()
    target_compile_options(host_test PRIVATE -Wall -Wextra)
endif()

add_test(NAME host_test COMMAND host_test)
