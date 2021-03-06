cmake_minimum_required(VERSION 3.11.0)
project(programs CXX)

add_executable(heaptest "heaptest.cpp")
target_include_directories(heaptest PUBLIC "../include")
