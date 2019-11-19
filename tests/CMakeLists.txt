cmake_minimum_required(VERSION 3.9)
project(riscv CXX)

add_subdirectory(../lib lib)

set(SOURCES
	custom.cpp
	main.cpp
)

add_executable(tests ${SOURCES})
target_link_libraries(tests riscv)
set_target_properties(tests PROPERTIES CXX_STANDARD 17)
