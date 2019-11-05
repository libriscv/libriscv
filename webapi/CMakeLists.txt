cmake_minimum_required(VERSION 3.9)
project(webapi C CXX)

option(LTO         "Enable interprocedural optimizations" ON)
option(NATIVE      "Enable native instructions" ON)

add_subdirectory(../lib lib)

add_custom_command(
	COMMAND cp ${CMAKE_SOURCE_DIR}/index.html webpage.html
	COMMAND xxd -i webpage.html > webpage.h
	VERBATIM
	OUTPUT  webpage.h
	DEPENDS ${CMAKE_SOURCE_DIR}/index.html
)
add_custom_target(webpage_generate ALL DEPENDS webpage.h)

set(SOURCES
	server.cpp
)

add_executable(webapi ${SOURCES})
add_dependencies(webapi webpage_generate)
set_target_properties(webapi PROPERTIES CXX_STANDARD 17)
target_link_libraries(webapi riscv pthread)
target_include_directories(webapi PRIVATE "cpp-httplib")
target_include_directories(webapi PRIVATE ${CMAKE_BINARY_DIR})

if (LTO)
	set_target_properties(riscv PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
	set_property(TARGET webapi PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
if (NATIVE)
	target_compile_options(riscv PUBLIC "-march=native")
endif()
target_compile_options(riscv PUBLIC -O2)
