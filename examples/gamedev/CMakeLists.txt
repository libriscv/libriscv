cmake_minimum_required(VERSION 3.14)
project(example LANGUAGES CXX)

# Enable LTO
cmake_policy(SET CMP0069 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

include(CheckIPOSupported)
check_ipo_supported(RESULT supported OUTPUT error)
if (supported)
	message(STATUS "IPO / LTO enabled")
	set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# Some libriscv options that we want to enable
option(RISCV_32I "" OFF)
option(RISCV_64I "" ON)
option(RISCV_FLAT_RW_ARENA "" ON)

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

# Use at least C++20
set(CMAKE_CXX_STANDARD 20)

add_executable(example example.cpp script.cpp)
target_link_libraries(example riscv fmt)
