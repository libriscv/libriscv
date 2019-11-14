cmake_minimum_required(VERSION 3.9)
project(webapi C CXX)

option(LTO         "Enable interprocedural optimizations" ON)
option(NATIVE      "Enable native instructions" ON)

add_subdirectory(../lib lib)

set(SOURCES
	server.cpp
)

add_executable(webapi ${SOURCES})
set_target_properties(webapi PROPERTIES CXX_STANDARD 17)
target_link_libraries(webapi riscv pthread)
target_include_directories(webapi PRIVATE "cpp-httplib")
target_include_directories(webapi PRIVATE ${CMAKE_BINARY_DIR})

if (LTO)
	set_target_properties(riscv PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
	set_property(TARGET webapi PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
	if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
		target_link_libraries(webapi "-fuse-ld=lld")
	endif()
endif()
if (NATIVE)
	target_compile_options(riscv PUBLIC "-march=native")
endif()
target_compile_options(riscv PUBLIC -O2)
