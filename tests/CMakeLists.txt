cmake_minimum_required(VERSION 3.9)
project(riscv CXX)

option(RISCV_DEBUG "" ON)
option(RISCV_ICACHE "" OFF)
add_subdirectory(../lib lib)
target_compile_options(riscv PUBLIC "-g" "-Wall" "-Wextra" "-Wno-unused")

set(SOURCES
	custom.cpp
	main.cpp
	test_crashes.cpp
	test_rv32i.cpp
	test_rv32c.cpp
)

add_executable(tests ${SOURCES})
target_link_libraries(tests riscv)
set_target_properties(tests PROPERTIES CXX_STANDARD 17)

target_compile_options(riscv PUBLIC "-fsanitize=address,undefined")
target_link_libraries(tests "-fsanitize=address,undefined")
