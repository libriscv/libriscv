cmake_minimum_required(VERSION 3.14)
project(example LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(libriscv
  GIT_REPOSITORY https://github.com/fwsGonzo/libriscv
  GIT_TAG        master
  )
FetchContent_Declare(libfmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt
  GIT_TAG        master
  )

FetchContent_MakeAvailable(libriscv)
FetchContent_MakeAvailable(libfmt)

add_executable(example example.cpp script.cpp)
target_link_libraries(example riscv fmt)

add_executable(simple_example simple_example.cpp)
target_link_libraries(simple_example riscv fmt)
