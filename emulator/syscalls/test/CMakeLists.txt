cmake_minimum_required(VERSION 3.11.0)
project(programs CXX)

add_subdirectory("${CMAKE_SOURCE_DIR}/../../../lib/EASTL" EASTL)
target_compile_definitions(EASTL PUBLIC EASTL_EXCEPTIONS_ENABLED=1)

add_executable(heaptest "heaptest.cpp")
target_include_directories(heaptest PUBLIC "../include")
target_link_libraries(heaptest EASTL)
