cmake_minimum_required(VERSION 3.11.0)
project(programs CXX)

add_executable(heaptest "heaptest.cpp")
set_target_properties(heaptest PROPERTIES CXX_STANDARD 17)
target_include_directories(heaptest PUBLIC "../../../lib")
