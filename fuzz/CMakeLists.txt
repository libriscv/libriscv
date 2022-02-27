cmake_minimum_required(VERSION 3.9)
project(riscv CXX)

# Instruction cache does not work without ELF loader (for now)
option(RISCV_ICACHE "" OFF)
add_subdirectory(../lib lib)
target_compile_options(riscv PUBLIC "-fsanitize=address,undefined")
target_compile_options(riscv PUBLIC -march=native -ggdb3 -O1)
target_compile_options(riscv PUBLIC "-fsanitize=fuzzer-no-link")

if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
	message(FATAL_ERROR "libfuzzer is part of the Clang compiler suite.")
endif()

set(SOURCES
	fuzz.cpp
)

function(add_fuzzer NAME ARCH MODE)
	add_executable(${NAME} ${SOURCES})
	target_link_libraries(${NAME} riscv)
	set_target_properties(${NAME} PROPERTIES CXX_STANDARD 17)
	target_link_libraries(${NAME} "-fsanitize=address,undefined")
	target_link_libraries(${NAME} "-fsanitize=fuzzer" "-fuse-ld=lld")
	target_compile_definitions(${NAME} PRIVATE RISCV_ARCH=${ARCH})
	target_compile_definitions(${NAME} PRIVATE ${MODE}=1)
endfunction()

add_fuzzer(vmfuzzer32  4   FUZZ_VM)
add_fuzzer(vmfuzzer64  8   FUZZ_VM)
add_fuzzer(vmfuzzer128 16  FUZZ_VM)

add_fuzzer(elffuzzer32  4   FUZZ_ELF)
add_fuzzer(elffuzzer64  8   FUZZ_ELF)
add_fuzzer(elffuzzer128 16  FUZZ_ELF)
