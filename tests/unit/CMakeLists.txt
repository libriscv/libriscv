cmake_minimum_required(VERSION 3.10)
project(libriscv_unittests CXX)

set(CMAKE_CXX_FLAGS "-Wall -Wextra -ggdb3")

option(RISCV_ICACHE "" ON)
add_subdirectory(../../lib lib)

add_subdirectory(../Catch2 Catch2)

enable_testing()

function(add_unit_test NAME)
	add_executable(${NAME}
		${ARGN}
		codebuilder.cpp
	)
	target_link_libraries(${NAME} riscv Catch2WithMain)
	add_test(
		NAME test_${NAME}
		COMMAND ${NAME}
	)
endfunction()

add_unit_test(basic basic.cpp)
