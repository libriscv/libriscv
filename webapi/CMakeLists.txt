cmake_minimum_required(VERSION 3.9)
project(webapi CXX)

option(LTO         "Enable interprocedural optimizations" ON)
option(NATIVE      "Enable native instructions" ON)

add_subdirectory(../lib lib)
target_compile_options(riscv PUBLIC -std=c++17)

set(EMUDIR "../emulator/src")
set(SOURCES
	server.cpp
	${EMUDIR}/linux.cpp
	${EMUDIR}/syscalls.cpp
	${EMUDIR}/threads.cpp
)

add_executable(webapi ${SOURCES})
target_link_libraries(webapi riscv pthread m)
target_include_directories(webapi PRIVATE "cpp-httplib")
target_include_directories(webapi PRIVATE ${CMAKE_BINARY_DIR})
target_include_directories(webapi PRIVATE ${CMAKE_SOURCE_DIR}/../emulator/src)

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
target_compile_options(riscv PUBLIC -Ofast)
