cmake_minimum_required(VERSION 3.1)
project(riscv C CXX)

add_subdirectory(lib)

set(SOURCES
	src/main.cpp
)

add_executable(remu ${SOURCES})
target_link_libraries(remu riscv)
set_target_properties(remu PROPERTIES CXX_STANDARD 17)
