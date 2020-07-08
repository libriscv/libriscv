cmake_minimum_required(VERSION 3.9)
project(riscv CXX)

#option(RISCV_DEBUG "" ON)
add_subdirectory(../lib lib)

if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
	message(FATAL_ERROR "libfuzzer is part of the Clang compiler suite.")
endif()

set(SOURCES
	fuzz.cpp
)

add_executable(fuzzer ${SOURCES})
target_link_libraries(fuzzer riscv)
set_target_properties(fuzzer PROPERTIES CXX_STANDARD 17)

target_compile_options(riscv PUBLIC "-fsanitize=address,undefined")
target_link_libraries(fuzzer "-fsanitize=address,undefined")
target_compile_options(riscv PUBLIC -march=native -g -O2)

target_compile_options(riscv PUBLIC "-fsanitize=fuzzer-no-link")
target_link_libraries(fuzzer "-fsanitize=fuzzer" "-fuse-ld=lld")
