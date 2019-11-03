cmake_minimum_required(VERSION 3.9)
project(riscv C CXX)

option(RISCV_DEBUG "" ON)
add_subdirectory(../lib lib)

set(SOURCES
	fuzz.cpp
)
add_executable(fuzzer ${SOURCES})
target_link_libraries(fuzzer riscv)
set_target_properties(fuzzer PROPERTIES CXX_STANDARD 17)

#set_target_properties(riscv PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
#set_property(TARGET fuzzer PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
target_compile_options(riscv PUBLIC "-fsanitize=address,undefined")
target_link_libraries(fuzzer "-fsanitize=address,undefined")
target_compile_options(riscv PUBLIC -march=native -g -O2)

target_compile_options(riscv PUBLIC "-fsanitize=fuzzer-no-link")
target_link_libraries(fuzzer "-fsanitize=fuzzer" "-fuse-ld=lld")
